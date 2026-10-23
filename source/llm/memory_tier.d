/// Background actor enforcing the memory tier lifecycle. under every
/// `memoryArea` dir the subdirs `short/` (default 7 d), `long/` (default 100 d)
/// and `archive/` (cold, unreachable) hold the memories. One fixed-order pass
/// - flat-layout upgrade (self-no-op once done) -> reconcile -> promoteAged ->
/// demoteAged - runs at startup (via onSpawn, on a repeating self-tick;
/// 0 = startup-only). The diff-based reconcile enforces the three invariants
/// so every crash window self-heals on the next pass with no recovery code.
/// Failures are logged and skipped. The actor emits no TUI messages and makes
/// no LLM calls.
module llm.memory_tier;

import logger = std.logger;
import std.algorithm : filter, map;
import std.datetime : Clock, Duration, dur;
import std.file : exists, isFile, mkdirRecurse, readText, remove, rename;
import std.path : stripExtension;
import std.sumtype : match;

import llm.config : LlmConfig, memoryArchiveDir, memoryLongDir, memoryShortDir;
import llm.rag.rag : add, Document, Offset, Origin, RAG;
import my.actor : ActorRef;
import my.path : AbsolutePath, Path;

final class MemoryTierActor {
    private {
        LlmConfig llmConf_;
        RAG rag_;
    }

    /// Owner/message pattern: the ctor stores config only - no I/O, no
    /// sends (it runs on the caller's thread; the actor has no address yet).
    this(LlmConfig llmConf, RAG rag) @safe nothrow {
        llmConf_ = llmConf;
        rag_ = rag;
    }

    /// The first pass runs HERE, on the actor's own thread, and the
    /// repeating tick is armed from here - so program startup latency is
    /// unchanged.
    void onSpawn(ActorRef self) {
        runPass();
        const interval = passInterval();
        if (armsRepeatingTick(interval))
            self.scheduleRepeating(interval, "runPass");
    }

    private bool armsRepeatingTick(Duration interval) const @safe nothrow {
        return interval > Duration.zero;
    }

    /// Internal: the repeating-tick interval; zero arms no tick
    /// (tierPassIntervalHours = 0 => startup-only pass).
    private Duration passInterval() const @safe nothrow {
        return llmConf_.tierPassIntervalHours > 0
            ? dur!"hours"(llmConf_.tierPassIntervalHours) : Duration.zero;
    }

    // TODO: make this function nothrow
    /// One full pass: the one-time flat-layout upgrade (self-no-op once no
    /// flat *.md files remain), then reconcile -> promoteAged -> demoteAged.
    /// Public for tests and for the periodic tick: the framework dispatches
    /// the tick by this exact method name.
    void runPass() {
        if (rag_ is null || rag_.isPrimaryInMemory || llmConf_.noMemory)
            return;
        // The flat-layout upgrade completes BEFORE the phases touch
        // the tier dirs - a just-adopted flat file is processed exactly once
        // (adopted and indexed here, then seen by reconcile as a normal long/
        // file). The upgrade self-no-ops once no flat *.md files remain, so
        // periodic re-runs only pay a shallow dir scan - no firstPass flag.
        // A failure is logged and skipped; a per-file failure is retried
        // by the NEXT pass while the file remains flat (a one-shot flag would
        // strand an unreachable flat file until restart).
        try {
            upgradeFlatLayout();
        } catch (Exception e) {
            logger.warningf("memory tier flat-layout upgrade failed: %s", e.msg);
        }
        // An exception escaping a phase must never kill the actor - the
        // periodic tick is dispatched by name, so a thrown runPass would
        // shutdown this actor silently (no onException hook here) and tier
        // enforcement would stop until restart. mdFilesUnder deliberately
        // does NOT swallow FileException: a failed tier scan must abort the
        // reconcile phase BEFORE the purge removes anything, not degrade into an
        // empty longFiles set (which would mass-remove memory sources).
        foreach (phase; [&reconcile, &promoteAged, &demoteAged]) {
            try {
                phase();
            } catch (Exception e) {
                logger.warningf("memory tier pass failed: %s", e.msg);
            }
        }
    }

    /// Reconcile disk vs the RAG index: re-adds every long/ file,
    /// removes RAG memory entries whose topic is not in long/, removes
    /// stale long/ duplicates of short/ topics. Archive/ is never scanned.
    private void reconcile() {
        // Topic -> owning long/ file, across every memoryArea dir.
        // longFiles is the global membership set; longFilesByArea
        // keys the same data per area for the duplicate check.
        Path[string] longFiles;
        Path[string][string] longFilesByArea;
        foreach (area; llmConf_.memoryArea) {
            foreach (f; mdFilesUnder(memoryLongDir(area))) {
                longFiles[topicOf(f)] = f;
                longFilesByArea[area.toString][topicOf(f)] = f;
            }
        }

        foreach (topic, f; longFiles) {
            try {
                rag_.add(Document(Origin(f), readText(f), Offset.init), llmConf_.ragConfig);
            } catch (Exception e) {
                logger.warningf("reconcile: failed to index memory '%s': %s", topic, e.msg);
            }
        }

        foreach (src; rag_.db.getSources()) {
            src.origin.match!((Path p) {
                try {
                    if (isMemoryAreaPath(p) && topicOf(p) !in longFiles)
                        rag_.db.removeSource(src.origin);
                } catch (Exception e) {
                    logger.warningf("reconcile: failed to remove stale memory source '%s': %s",
                        p, e.msg);
                }
            }, (_) {});
        }

        // A topic present under both short/ and long/ is a second-write
        // duplicate: the fresh short-term copy wins (fresh mtime). Remove the
        // long/ file together with its stale RAG source. The
        // duplicate check is per-area: with several memoryArea dirs, a topic
        // duplicated across DIFFERENT areas is two distinct memories, not a
        // second write - only the same area's long/ file loses.
        foreach (area; llmConf_.memoryArea) {
            const areaLongFiles = longFilesByArea.get(area.toString, null);
            foreach (f; mdFilesUnder(memoryShortDir(area))) {
                const topic = topicOf(f);
                if (auto stale = topic in areaLongFiles) {
                    try {
                        remove(*stale);
                        rag_.db.removeSource(Origin(*stale));
                        logger.infof("reconcile: memory '%s' exists in both tiers; the short-term copy wins",
                                topic);
                    } catch (Exception e) {
                        logger.warningf("reconcile: failed to remove duplicate memory '%s': %s",
                                topic, e.msg);
                    }
                }
            }
        }
    }

    /// Promote aged short-term memories - RAG FIRST with the long/
    /// DESTINATION origin, then rename short/ -> long/. A step failure is
    /// logged and skipped; the next pass heals it.
    private void promoteAged() {
        foreach (area; llmConf_.memoryArea) {
            foreach (f; mdFilesUnder(memoryShortDir(area))) {
                const topic = topicOf(f);
                try {
                    if (ageOf(f) <= dur!"days"(llmConf_.shortTermDays))
                        continue;

                    rag_.add(Document(Origin(memoryLongDir(area) ~ (topic ~ ".md")),
                            readText(f), Offset.init), llmConf_.ragConfig);
                    rename(f, memoryLongDir(area) ~ (topic ~ ".md"));
                    logger.infof("promoted memory '%s' to long-term", topic);
                } catch (Exception e) {
                    logger.warningf("promoteAged: skipped memory '%s': %s", topic, e.msg);
                }
            }
        }
    }

    /// Mirror image of promoteAged: RAG-remove FIRST, then rename
    /// long/ -> archive/. The file is never deleted - demotion is
    /// write-once cold storage.
    private void demoteAged() {
        foreach (area; llmConf_.memoryArea) {
            foreach (f; mdFilesUnder(memoryLongDir(area))) {
                const topic = topicOf(f);
                try {
                    if (ageOf(f) <= dur!"days"(llmConf_.longTermDays))
                        continue;

                    rag_.db.removeSource(Origin(f));
                    rename(f, memoryArchiveDir(area) ~ (topic ~ ".md"));
                    logger.infof("demoted memory '%s' to archive", topic);
                } catch (Exception e) {
                    logger.warningf("demoteAged: skipped memory '%s': %s", topic, e.msg);
                }
            }
        }
    }

    // TODO: make this function nothrow
    /// One-time adoption of pre-tier flat memories: `*.md`.
    private void upgradeFlatLayout() {
        foreach (area; llmConf_.memoryArea) {
            foreach (f; mdFilesUnder(area)) {
                const topic = topicOf(f);
                try {
                    // Retire a pre-tier index of this file
                    rag_.db.removeSource(Origin(f));

                    const toShort = ageOf(f) <= dur!"days"(llmConf_.shortTermDays);
                    const dest = toShort ? memoryShortDir(area) ~ (
                            topic ~ ".md") : memoryLongDir(area) ~ (topic ~ ".md");

                    // A same-topic tier file means a stale post-tier flat
                    // twin (reachable only via manual tampering)
                    if (exists(dest) && isFile(dest)) {
                        remove(f);
                        logger.warningf("flat memory '%s' duplicates a tier file;" ~ " the flat copy was removed",
                                topic);
                        continue;
                    }

                    rename(f, dest);
                    if (!toShort) {
                        // Destination-origin rule: the indexed origin is
                        // the long/ DESTINATION path.
                        rag_.add(Document(Origin(dest), readText(dest),
                                Offset.init), llmConf_.ragConfig);
                    }
                    logger.infof("adopted pre-tier memory '%s' into %s", topic,
                            toShort ? "short-term" : "long-term");
                } catch (Exception e) {
                    logger.warningf("flat adoption failed for %s: %s", f, e.msg);
                }
            }
        }
    }

    /// True iff `p` lies under some memoryArea dir or one of its tier subdirs.
    /// Note: AbsolutePath resolves relative paths against the process CWD,
    /// so this is same-CWD-sensitive: the default memoryArea is itself
    /// relative, so the actor and the agent must run in the same CWD.
    private bool isMemoryAreaPath(Path p) const {
        foreach (area; llmConf_.memoryArea) {
            const dir = AbsolutePath(p).dirName;
            if (dir == AbsolutePath(area) || dir == AbsolutePath(memoryShortDir(area))
                    || dir == AbsolutePath(memoryLongDir(area))
                    || dir == AbsolutePath(memoryArchiveDir(area)))
                return true;
        }
        return false;
    }
}

/// Basename minus the `.md` extension: the memory topic of a tier file.
private string topicOf(Path f) @safe {
    return f.baseName.stripExtension;
}

/// The `*.md` files directly under `dir` (shallow). Only short/ and long/ are
/// ever scanned; archive/ never is.
private Path[] mdFilesUnder(Path dir) @safe {
    import std.array : array;
    import std.file : SpanMode, dirEntries;
    import std.algorithm.searching : endsWith;

    if (!dir.exists)
        return [];

    // std.file.dirEntries uses dip1000-dependent instantiations: without
    // -preview=dip1000 the returned range and its destructor are @system.
    // Confine the scan to one @trusted scope and materialize the names
    // eagerly so the lazy range never escapes into @safe code (llm.vfs).
    auto names = () @trusted {
        return dirEntries(dir, SpanMode.shallow).filter!(f => f.isFile)
            .map!(f => Path(f.name))
            .filter!(f => f.toString.endsWith(".md"))
            .array;
    }();

    return names;
}

/// File age: now minus the file's mtime. Rename preserves mtime, so the
/// age of a promoted memory survives the move.
private Duration ageOf(Path f) @safe {
    import std.file : timeLastModified;

    return Clock.currTime - timeLastModified(f);
}

version (unittest) {
    import std.datetime : SysTime;
    import std.file : getTimes, mkdirRecurse, setTimes, write;
    import std.path : absolutePath;

    import llm.config : RagDatabaseConfig;
    import llm.test_util : TestArea, TestEmbedder, testArea;

    private SysTime makeOld(Path f, long days) @safe {
        SysTime accessTime, modifiedTime;
        getTimes(f, accessTime, modifiedTime);
        modifiedTime = Clock.currTime - dur!"days"(days);
        setTimes(f, accessTime, modifiedTime);
        return modifiedTime;
    }

    /// A tier fixture: tier dirs under a private temp area, a real file-backed
    /// RAG over them, and the actor wired to both. The RAG is registered in
    /// the TestArea so cleanup destroys it before the dir is removed
    /// (d2sqlite3's debug build asserts in the GC finalizer otherwise).
    private struct TierFixture {
        // Points at the caller's TestArea: a by-value copy would make addRag
        // register the RAG in the fixture's copy only, so the test's
        // scope(exit) testDir.cleanup() would see an empty rags list and leave
        // the RAG (and its sqlite statements) to the GC finalizer, which
        // aborts the test binary (d2sqlite3 ensureNotInGC).
        TestArea* testDir;
        Path area;
        LlmConfig conf;
        RAG rag;
        MemoryTierActor actor;

        this(ref TestArea testDir_, uint shortTermDays = 7, uint longTermDays = 100,
                bool inMemoryRag = false) {
            testDir = &testDir_;
            area = testDir_ ~ "memory";
            foreach (sub; ["short", "long", "archive"])
                mkdirRecurse((area ~ sub).toString);

            conf = LlmConfig();
            conf.memoryArea = [area];
            conf.shortTermDays = shortTermDays;
            conf.longTermDays = longTermDays;
            conf.tierPassIntervalHours = 0;

            // A nonexistent primary dir makes openDatabase fall back to an
            // in-memory RAG (isPrimaryInMemory true) - the gating fixture.
            rag = new RAG(new TestEmbedder(), RagDatabaseConfig(inMemoryRag
                    ? Path("/nonexistent_dir/x.db") : area ~ "rag.sqlite3", "test rag"), null);
            testDir.addRag(rag);
            actor = new MemoryTierActor(conf, rag);
        }

        void cleanup() {
            try {
                rag.destroy;
            } catch (Exception) {
            }
            testDir.cleanup();
        }

        /// Write a memory file into the `tier` subdir (short/long/archive).
        void writeMem(string tier, string topic, string content) @safe {
            write((area ~ tier ~ (topic ~ ".md")).toString, content);
        }

        /// Write a pre-tier flat memory file directly under the area dir.
        void writeFlat(string topic, string content) @safe {
            write((area ~ (topic ~ ".md")).toString, content);
        }

        Path flatFile(string topic) @safe pure nothrow {
            return area ~ (topic ~ ".md");
        }

        Path memFile(string tier, string topic) @safe pure nothrow {
            return area ~ tier ~ (topic ~ ".md");
        }
    }
}

@("promoteAged: RAG add FIRST with the long/ destination origin, then rename; mtime preserved")
unittest {
    import std.conv : to;
    import std.file : timeLastModified;

    auto testDir = testArea("promote_order");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);
    immutable content = "short-term memory content that is long enough to chunk into "
        ~ "several pieces for the RAG indexer to store: a b c d e f g h i j k l m n o p";
    fx.writeMem("short", "alpha", content);
    immutable oldMtime = makeOld(fx.memFile("short", "alpha"), fx.conf.shortTermDays + 1);

    fx.actor.runPass();

    assert(fx.memFile("long", "alpha").exists, "promoted file missing");
    assert(!fx.memFile("short", "alpha").exists, "source file must be renamed away");
    assert(readText(fx.memFile("long", "alpha")) == content, "content must survive");
    // The RAG entry's origin is the long/ DESTINATION path.
    auto sources = fx.rag.db.getSources;
    assert(sources.length == 1, "exactly one RAG source expected, got " ~ sources.length.to!string);
    sources[0].origin.match!((Path p) {
        assert(p == AbsolutePath(fx.memFile("long", "alpha")),
            "origin must be the long/ destination, got " ~ p.toString);
    }, (_) { assert(false, "origin must be a Path"); });
    // The rename preserved the mtime (the age survives promotion).
    assert(timeLastModified(fx.memFile("long",
            "alpha")) - oldMtime < dur!"seconds"(5), "rename must preserve the mtime");
}

@("demoteAged: RAG source removed FIRST, then rename to archive/; the file stays on disk")
unittest {
    auto testDir = testArea("demote_order");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);
    immutable content = "long-term memory content that is long enough to chunk into "
        ~ "several pieces for the RAG indexer: a b c d e f g h i j k l m n o p q r s t u";
    fx.writeMem("long", "beta", content);
    makeOld(fx.memFile("long", "beta"), fx.conf.longTermDays + 1);

    fx.actor.runPass();

    assert(fx.memFile("archive", "beta").exists, "demotion must rename, never delete");
    assert(!fx.memFile("long", "beta").exists, "stale long/ file must be renamed away");
    assert(readText(fx.memFile("archive", "beta")) == content, "content must survive");
    // The demoted content is unreachable - no RAG entry remains, and
    // the archive is never re-indexed by the next pass.
    assert(fx.rag.db.getSources.length == 0, "the demoted RAG source must be gone");
    fx.actor.runPass(); // a rerun never resurrects it
    assert(fx.rag.db.getSources.length == 0);
}

@("reconcile: re-adds every long/ file; same-state re-add is a no-op")
unittest {
    auto testDir = testArea("reconcile_i1");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);
    fx.writeMem("long", "gamma", "long-term content that is long enough to chunk "
            ~ "into several pieces for the RAG indexer: a b c d e f g h");

    fx.actor.runPass();
    assert(fx.rag.db.getSources.length == 1, "missing long/ file must get indexed");
    fx.actor.runPass(); // same-state re-add: per-origin dedup makes this a no-op
    assert(fx.rag.db.getSources.length == 1, "same-state re-add must not add a source");
}

@("reconcile: removes RAG memory entries whose topic is not in long/, incl. after a simulated demotion crash; non-memory origins untouched")
unittest {
    import std.conv : to;

    auto testDir = testArea("reconcile_i2");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);
    fx.writeMem("long", "delta", "long-term content that is long enough to chunk "
            ~ "into several pieces for the RAG indexer: a b c d e f g h");

    fx.actor.runPass();
    assert(fx.rag.db.getSources.length == 1);

    // Simulate a demotion crash after removeSource, before rename: the long/
    // file moves to archive/ while the RAG entry stays.
    rename(fx.memFile("long", "delta"), fx.memFile("archive", "delta"));

    // A non-memory origin (a project file outside the memory areas) must
    // stay untouched.
    immutable foreignOrigin = Origin(Path("/etc/llmfun/foreign.md"));
    fx.rag.add(Document(foreignOrigin, "project content that is long enough " ~ "to chunk: a b c d e f g h",
            Offset.init), fx.conf.ragConfig);

    fx.actor.runPass();

    auto sources = fx.rag.db.getSources;
    assert(sources.length == 1,
            "the stale memory entry must be removed, " ~ "got " ~ sources.length.to!string);
    sources[0].origin.match!((Path p) {
        assert(p == AbsolutePath(Path("/etc/llmfun/foreign.md")),
            "the non-memory origin must survive, got " ~ p.toString);
    }, (_) { assert(false, "the surviving origin must be the foreign Path"); });
    assert(fx.memFile("archive", "delta").exists, "the archive file is untouched");
}

@("reconcile: duplicate short/+long/ topic - the short-term copy wins")
unittest {
    auto testDir = testArea("reconcile_i3");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);
    fx.writeMem("long", "eps", "OLD long-term content that is long enough to chunk "
            ~ "into several pieces for the RAG indexer: a b c d e f g h");
    fx.actor.runPass(); // index the long-term copy first
    assert(fx.rag.db.getSources.length == 1);

    fx.writeMem("short", "eps", "FRESH short-term content that is long enough to chunk "
            ~ "into several pieces for the RAG indexer: a b c d e f g h");

    fx.actor.runPass(); // short wins; long/ file + stale RAG source removed

    assert(!fx.memFile("long", "eps").exists, "the stale long/ file must be removed");
    assert(fx.memFile("short", "eps").exists, "the fresh short/ copy wins");
    assert(fx.rag.db.getSources.length == 0, "the stale long-term RAG source must be removed");
}

@("gating: the pass is a no-op when rag is null, isPrimaryInMemory, or noMemory")
unittest {
    // rag null: runPass must not throw and must touch nothing.
    auto testDirNull = testArea("gating_rag_null");
    scope (exit)
        testDirNull.cleanup();
    auto fxNull = TierFixture(testDirNull, 7, 100, true);
    immutable content = "short-term memory content that is long enough to chunk "
        ~ "into several pieces for the RAG indexer: a b c d e f g h i j";
    fxNull.writeMem("short", "omega", content);
    makeOld(fxNull.memFile("short", "omega"), fxNull.conf.shortTermDays + 1);
    fxNull.actor.runPass();
    assert(fxNull.memFile("short", "omega").exists, "a null RAG must change nothing");

    // isPrimaryInMemory: the gating fixture's RAG is in-memory primary.
    assert(fxNull.rag.isPrimaryInMemory, "fixture RAG must be in-memory primary");
    fxNull.actor.runPass();
    assert(fxNull.memFile("short", "omega").exists, "an in-memory primary RAG must change nothing");

    // noMemory: a file-backed RAG but noMemory set still gates the pass.
    auto testDirNoMem = testArea("gating_no_memory");
    scope (exit)
        testDirNoMem.cleanup();
    auto fxNoMem = TierFixture(testDirNoMem);
    // The actor captures LlmConfig by value, so noMemory must be set on
    // the copy handed to a dedicated actor (the fixture's actor is ungated).
    LlmConfig noMemConf = fxNoMem.conf;
    noMemConf.noMemory = true;
    auto actorNoMem = new MemoryTierActor(noMemConf, fxNoMem.rag);
    scope (exit)
        actorNoMem.destroy;
    fxNoMem.writeMem("short", "sigma", content);
    makeOld(fxNoMem.memFile("short", "sigma"), fxNoMem.conf.shortTermDays + 1);
    actorNoMem.runPass();
    assert(fxNoMem.memFile("short", "sigma").exists, "noMemory must gate the whole pass");
    assert(fxNoMem.rag.db.getSources.length == 0, "nothing may be indexed");
}

@("runPass never throws when a tier dir is unreadable")
unittest {
    import core.sys.posix.sys.stat : chmod;
    import std.conv : octal;
    import std.string : toStringz;

    auto testDir = testArea("pass_unreadable");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);

    const blocked = fx.area ~ "long";
    chmod(toStringz(blocked.toString), 0u);
    scope (exit)
        chmod(toStringz(blocked.toString), octal!755);

    // reconcile's long/ scan throws FileException; demoteAged's scan
    // throws out of its foreach header. Both must be absorbed by
    // runPass, not escape into the framework's dispatcher.
    fx.actor.runPass();
}

@("passInterval: tierPassIntervalHours drives the tick; zero disables it")
unittest {
    auto testDir = testArea("pass_interval");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);

    // The actor captures LlmConfig by value, so the fixture's
    // tierPassIntervalHours = 0 is what passInterval reports.
    assert(fx.actor.passInterval == Duration.zero, "0 must mean startup-only");

    LlmConfig sixHours = fx.conf;
    sixHours.tierPassIntervalHours = 6;
    auto actor6 = new MemoryTierActor(sixHours, fx.rag);
    scope (exit)
        actor6.destroy;
    assert(actor6.passInterval == dur!"hours"(6),
            "the config default of 6 must map to a 6-hour tick");
}

// Crash-window rows as named test cases: each test seeds a
// post-crash state directly, reruns runPass(), and asserts the healed
// disk + RAG state (the assertions are the contract).

@("crash_row1_promotion_between_add_and_rename_heals_on_rerun")
unittest {
    import std.conv : to;

    auto testDir = testArea("crash_row1_promote_add");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);
    immutable content = "short-term memory content that is long enough to chunk into "
        ~ "several pieces for the RAG indexer: a b c d e f g h i j";
    fx.writeMem("short", "zeta", content);
    makeOld(fx.memFile("short", "zeta"), fx.conf.shortTermDays + 1);

    // Crash state: the RAG entry was added with the long/ DESTINATION
    // origin, the rename never happened.
    fx.rag.add(Document(Origin(fx.memFile("long", "zeta")), content,
            Offset.init), fx.conf.ragConfig);
    assert(fx.rag.db.getSources.length == 1);

    fx.actor.runPass();

    // The purge removes the stale entry (predicate: topic not in long/), then
    // promoteAged re-adds under the same long/ origin and renames - no
    // permanently stale entry survives.
    assert(fx.memFile("long", "zeta").exists, "the promoted file must exist");
    assert(!fx.memFile("short", "zeta").exists, "the source file must be renamed away");
    auto sources = fx.rag.db.getSources;
    assert(sources.length == 1, "exactly one RAG source expected, got " ~ sources.length.to!string);
    sources[0].origin.match!((Path p) {
        assert(p == AbsolutePath(fx.memFile("long", "zeta")),
            "the healed entry must carry the long/ destination origin, got " ~ p.toString);
    }, (_) { assert(false, "origin must be a Path"); });
    assert(readText(fx.memFile("long", "zeta")) == content, "content must survive");
}

@("crash_row2_promotion_rename_is_atomic_file_in_exactly_one_dir")
unittest {
    auto testDir = testArea("crash_row2_promote_rename");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);
    fx.writeMem("short", "eta", "short-term memory content that is long enough "
            ~ "to chunk into several pieces for the RAG indexer: a b c d e f g h");
    makeOld(fx.memFile("short", "eta"), fx.conf.shortTermDays + 1);

    fx.actor.runPass();

    // Row 2: the rename itself is atomic across sibling dirs - the topic file
    // exists in exactly one dir, never in both.
    assert(fx.memFile("long", "eta").exists, "the renamed file must exist");
    assert(!fx.memFile("short", "eta").exists, "no short/ copy may survive");
}

@("crash_row3_demotion_between_removeSource_and_rename_heals_via_i1")
unittest {
    auto testDir = testArea("crash_row3_demote_remove");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);
    fx.writeMem("long", "theta", "long-term memory content that is long enough to chunk "
            ~ "into several pieces for the RAG indexer: a b c d e f g h");

    fx.actor.runPass(); // index the long-term copy first (file is still fresh)
    assert(fx.rag.db.getSources.length == 1);

    // Crash state: the RAG source was removed, the rename to
    // archive/ never happened - the long/ file is unindexed.
    fx.rag.db.removeSource(Origin(fx.memFile("long", "theta")));
    assert(fx.rag.db.getSources.length == 0);

    // Now age it: the heal rerun must both re-add and re-demote.
    makeOld(fx.memFile("long", "theta"), fx.conf.longTermDays + 1);
    fx.actor.runPass();

    // The re-add heals the long/ file; the rerun re-demotes it.
    assert(!fx.memFile("long", "theta").exists, "the rerun re-demoted the long/ file");
    assert(fx.memFile("archive", "theta").exists, "demotion must rename, never delete");
    assert(fx.rag.db.getSources.length == 0, "the re-added source is demoted again");
}

@("crash_row4_demotion_after_rename_heals_via_i2")
unittest {
    auto testDir = testArea("crash_row4_demote_rename");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);
    fx.writeMem("long", "iota", "long-term memory content that is long enough to chunk "
            ~ "into several pieces for the RAG indexer: a b c d e f g h");
    fx.actor.runPass(); // index the long-term copy first (file is still fresh)
    assert(fx.rag.db.getSources.length == 1);
    makeOld(fx.memFile("long", "iota"), fx.conf.longTermDays + 1);

    // Crash state: the rename happened, the long/ file lives in
    // archive/ while the RAG entry still points at the long/ path.
    rename(fx.memFile("long", "iota"), fx.memFile("archive", "iota"));
    assert(fx.rag.db.getSources.length == 1, "the stale entry survives the crash window");

    fx.actor.runPass();

    // The purge: the predicate is "topic not in long/".
    assert(fx.rag.db.getSources.length == 0, "the stale entry must be removed");
    assert(fx.memFile("archive", "iota").exists, "the archive file is untouched");
    fx.actor.runPass(); // the heal is idempotent
    assert(fx.rag.db.getSources.length == 0);
}

@("crash_row7_actor_death_mid_pass_rerun_completes")
unittest {
    // Row 7: an actor death mid-pass leaves a partial pass state (here seeded
    // directly, combining the promote-side, demote-side and reconcile-side
    // crash windows); the next runPass() is idempotent and completes the work -
    // all steps are diff-based, no recovery code.
    auto testDir = testArea("crash_row7_partial_pass");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);
    immutable content = "short-term memory content that is long enough to chunk into "
        ~ "several pieces for the RAG indexer: a b c d e f g h i j";

    // Promote-side partial state: short/ file + RAG entry with
    // the long/ destination origin, rename not done.
    fx.writeMem("short", "kappa", content);
    makeOld(fx.memFile("short", "kappa"), fx.conf.shortTermDays + 1);
    fx.rag.add(Document(Origin(fx.memFile("long", "kappa")), content,
            Offset.init), fx.conf.ragConfig);

    // Demote-side partial state: an indexed long/ file whose
    // RAG source is already gone (removeSource done, rename not).
    fx.writeMem("long", "lambda", "long-term memory content that is long enough to chunk "
            ~ "into several pieces for the RAG indexer: a b c d e f g h");
    makeOld(fx.memFile("long", "lambda"), fx.conf.longTermDays + 1);
    fx.rag.add(Document(Origin(fx.memFile("long", "lambda")),
            readText(fx.memFile("long", "lambda")), Offset.init), fx.conf.ragConfig);
    fx.rag.db.removeSource(Origin(fx.memFile("long", "lambda")));
    assert(fx.rag.db.getSources.length == 1, "only kappa's entry remains");

    // Reconcile-side partial state: a stale RAG entry whose
    // topic file already lives in archive/.
    fx.writeMem("long", "mu", "long-term memory content that is long enough to chunk "
            ~ "into several pieces for the RAG indexer: a b c d e f g h");
    fx.rag.add(Document(Origin(fx.memFile("long", "mu")),
            readText(fx.memFile("long", "mu")), Offset.init), fx.conf.ragConfig);
    rename(fx.memFile("long", "mu"), fx.memFile("archive", "mu"));
    assert(fx.rag.db.getSources.length == 2, "kappa + lambda + mu entries expected");

    fx.actor.runPass();

    // The rerun completes everything: kappa is promoted, lambda is demoted,
    // mu's stale entry is purged.
    assert(fx.memFile("long", "kappa").exists, "kappa must be promoted");
    assert(!fx.memFile("short", "kappa").exists, "kappa's short/ copy renamed away");
    assert(!fx.memFile("long", "lambda").exists, "lambda must be demoted");
    assert(fx.memFile("archive", "lambda").exists, "lambda must land in archive/");
    assert(!fx.memFile("long", "mu").exists, "mu's long/ file was renamed away");
    assert(fx.memFile("archive", "mu").exists, "mu's archive file is untouched");
    assert(fx.rag.db.getSources.length == 1, "only kappa's long/ entry remains");
    fx.actor.runPass(); // the healed state is stable under rerun
    assert(fx.rag.db.getSources.length == 1);
}

@("periodic_pass_promotes_after_startup_write")
unittest {
    import std.conv : to;

    auto testDir = testArea("periodic_pass");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);
    immutable content = "short-term memory content that is long enough to chunk into "
        ~ "several pieces for the RAG indexer: a b c d e f g h i j";
    fx.writeMem("short", "nu", content);

    // The startup pass: a freshly written memory is too young to promote.
    fx.actor.runPass();
    assert(fx.memFile("short", "nu").exists, "too young: still short-term");
    assert(fx.rag.db.getSources.length == 0, "too young: not indexed yet");

    // A later periodic pass: the same actor object, no process restart.
    makeOld(fx.memFile("short", "nu"), fx.conf.shortTermDays + 1);
    fx.actor.runPass();

    assert(!fx.memFile("short", "nu").exists, "the aged file must be renamed away");
    assert(fx.memFile("long", "nu").exists, "the memory must be promoted");
    assert(readText(fx.memFile("long", "nu")) == content, "content must survive");
    auto sources = fx.rag.db.getSources;
    assert(sources.length == 1, "exactly one RAG source expected, got " ~ sources.length.to!string);
    sources[0].origin.match!((Path p) {
        assert(p == AbsolutePath(fx.memFile("long", "nu")),
            "the indexed origin must be the long/ destination, got " ~ p.toString);
    }, (_) { assert(false, "origin must be a Path"); });
}

@("startup_only_when_interval_zero: no repeating tick is armed")
unittest {
    auto testDir = testArea("startup_only_tick");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir); // tierPassIntervalHours = 0

    assert(!fx.actor.armsRepeatingTick(fx.actor.passInterval()), "0 must arm no repeating tick");
    // onSpawn with an inert self handle: the startup pass runs, and with the
    // interval at zero nothing can be armed through it.
    fx.actor.onSpawn(ActorRef(null));

    LlmConfig sixHours = fx.conf;
    sixHours.tierPassIntervalHours = 6;
    auto actor6 = new MemoryTierActor(sixHours, fx.rag);
    scope (exit)
        actor6.destroy;
    assert(actor6.armsRepeatingTick(actor6.passInterval()), "an interval > 0 must arm the tick");
    // The tick dispatches by method name: the target must exist.
    static assert(__traits(hasMember, MemoryTierActor, "runPass"),
            "the tick target runPass must exist");
}

@("no_tui_messages_during_pass: the actor carries no TUI send hook")
unittest {
    // Structural: the ctor takes only the config and the RAG, and no TUI
    // channel or send delegate exists anywhere on the type. A delegate param
    // would not accept RAG.init (a null class reference), and the negative
    // case pins the exact arity - no extra defaulted params.
    static assert(!__traits(hasMember, MemoryTierActor, "sendChatMessage"),
            "the actor must not carry a TUI send hook");
    static assert(is(typeof(new MemoryTierActor(LlmConfig(), RAG.init))),
            "the ctor must take only the config and the RAG");
    static assert(!is(typeof(new MemoryTierActor(LlmConfig(), RAG.init, 0))),
            "no ctor params beyond the config and the RAG");

    // Deliberate smoke test: a full pass with nothing TUI-wired completes -
    // everything it emits goes to the logger only.
    auto testDir = testArea("no_tui_pass");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);
    fx.actor.runPass();
}

@("flat_upgrade: a fresh flat file adopts into short/ disk-only")
unittest {
    auto testDir = testArea("flat_upgrade_fresh");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);
    immutable content = "short-term memory content that is long enough to chunk into "
        ~ "several pieces for the RAG indexer: a b c d e f g h i j";
    fx.writeFlat("omega", content);

    fx.actor.runPass();

    assert(!fx.flatFile("omega").exists, "the flat file must be moved away");
    assert(fx.memFile("short", "omega").exists, "a fresh flat file must land in short/");
    assert(readText(fx.memFile("short", "omega")) == content, "content must survive");
    assert(fx.rag.db.getSources.length == 0, "a fresh adoption stays disk-only");
}

@(
        "flat_upgrade: an aged flat file adopts into long/ and is indexed there under the long/ destination origin")
unittest {
    import std.conv : to;

    auto testDir = testArea("flat_upgrade_aged");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);
    immutable content = "long-term memory content that is long enough to chunk into "
        ~ "several pieces for the RAG indexer: a b c d e f g h";
    fx.writeFlat("theta", content);
    makeOld(fx.flatFile("theta"), fx.conf.shortTermDays + 1);

    fx.actor.runPass();

    assert(!fx.flatFile("theta").exists, "the flat file must be moved away");
    assert(fx.memFile("long", "theta").exists, "an aged flat file must land in long/");
    assert(readText(fx.memFile("long", "theta")) == content, "content must survive");
    auto sources = fx.rag.db.getSources;
    assert(sources.length == 1, "exactly one RAG source expected, got " ~ sources.length.to!string);
    sources[0].origin.match!((Path p) {
        assert(p == AbsolutePath(fx.memFile("long", "theta")),
            "the indexed origin must be the long/ destination, got " ~ p.toString);
    }, (_) { assert(false, "origin must be a Path"); });
}

@("flat_upgrade runs before the first reconcile: an aged flat file plus its superseded pre-tier flat-origin RAG entry heal on the first pass")
unittest {
    import std.conv : to;

    auto testDir = testArea("flat_upgrade_stale_entry");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);
    immutable content = "short-term memory content that is long enough to chunk into "
        ~ "several pieces for the RAG indexer: a b c d e f g h i j";
    fx.writeFlat("kappa", content);
    makeOld(fx.flatFile("kappa"), fx.conf.shortTermDays + 1);
    // Pre-tier state: the flat file was indexed at write time under its flat
    // path.
    fx.rag.add(Document(Origin(fx.flatFile("kappa")), content, Offset.init), fx.conf.ragConfig);
    assert(fx.rag.db.getSources.length == 1);

    fx.actor.runPass(); // ONE pass: upgrade -> reconcile -> promote -> demote

    // The upgrade retired the superseded flat-origin entry and re-indexed the
    // adopted file under its long/ destination origin; the reconcile purge then
    // had nothing stale to purge - the upgrade ran FIRST.
    assert(fx.memFile("long", "kappa").exists, "the flat file must be adopted");
    assert(!fx.flatFile("kappa").exists, "the flat path must be renamed away");
    auto sources = fx.rag.db.getSources;
    assert(sources.length == 1,
            "the superseded pre-tier entry must be retired, got " ~ sources.length.to!string);
    sources[0].origin.match!((Path p) {
        assert(p == AbsolutePath(fx.memFile("long", "kappa")),
            "the surviving entry must carry the long/ destination origin, got " ~ p.toString);
    }, (_) { assert(false, "origin must be a Path"); });
    assert(!fx.memFile("archive", "kappa").exists, "archive must be untouched");
    fx.actor.runPass(); // the healed state is stable under rerun (idempotent)
    assert(fx.rag.db.getSources.length == 1);
    assert(fx.memFile("long", "kappa").exists);
}

@("flat_upgrade: a per-file failure is logged and skipped; the other flat files still adopt")
unittest {
    import core.sys.posix.sys.stat : mkdir;
    import std.conv : octal;
    import std.string : toStringz;

    auto testDir = testArea("flat_upgrade_skip");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);
    fx.writeFlat("stuck", "content that never adopts: a b c d e f g h");
    // rename(f, short/stuck.md) fails while the destination is a directory.
    mkdir(toStringz(fx.memFile("short", "stuck").toString), octal!755);
    fx.writeFlat("moves", "short-term memory content that is long enough to chunk "
            ~ "into several pieces for the RAG indexer: a b c d e f g h");

    fx.actor.runPass();

    assert(fx.memFile("short", "moves").exists, "the other flat file must still adopt");
    assert(fx.flatFile("stuck").exists, "the failed file stays flat for the next pass to retry");
    assert(fx.rag.db.getSources.length == 0, "nothing may be indexed");
}

@("runPass never throws when the flat-layout upgrade scan fails (unreadable area dir)")
unittest {
    import core.sys.posix.sys.stat : chmod;
    import std.conv : octal;
    import std.string : toStringz;

    auto testDir = testArea("upgrade_scan_unreadable");
    scope (exit)
        testDir.cleanup();
    auto fx = TierFixture(testDir);

    const blocked = fx.area;
    chmod(toStringz(blocked.toString), 0u);
    scope (exit)
        chmod(toStringz(blocked.toString), octal!755);

    fx.actor.runPass();
}
