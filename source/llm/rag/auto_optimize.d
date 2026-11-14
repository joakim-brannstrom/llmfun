/// One-shot background auto-optimize check for the primary RAG database.
///
/// At spawn the actor arms one delayed self-send one interval (config, default 15 min) after startup.
/// The check opens the primary read/write database, reads the write counter (OptimizeCounterTbl) and, at or above the threshold (config, default 100), runs the sqlite-vec optimize command and resets the counter.
/// The actor then exits: the automatic path compacts at most once per process run, and read-only CI-published secondaries are never touched.
module llm.rag.auto_optimize;

import logger = std.logger;
import core.time : Duration, msecs, minutes;

import my.actor.behavior : ActorRef;
import my.actor.channel : dynDelayedSend;
import my.actor.common : ExitReason;
import my.actor.msg : delay, sendExit;
import my.path : AbsolutePath;

import llm.rag.database : Database, openDatabase, optimize, timeout;
import llm.rag.optimize_counter : readOptimizeCounter, resetOptimizeCounter;

class AutoOptimizeActor {
    private {
        AbsolutePath dbPath_;
        long threshold_;
        long intervalMin_;
        string model_;
        long embedDims_;
        Duration busyTimeout_;
        ActorRef self_;
    }

    /// All knobs are constructor arguments - the spawn site reads them from the layered config, so the actor never touches llm.config.
    /// The busy timeout is passed through to openDatabase, so a user-configured busyTimeoutMs is honored exactly as on the manual `rag --optimize` path.
    this(AbsolutePath dbPath, long threshold, long intervalMin, string model,
            long embedDims, Duration busyTimeout = 500.msecs) {
        dbPath_ = dbPath;
        threshold_ = threshold;
        intervalMin_ = intervalMin;
        model_ = model;
        embedDims_ = embedDims;
        busyTimeout_ = busyTimeout;
    }

    /// Arm the single check with a delayed self-send, one interval after spawn.
    /// The wait sits in the actor's delay queue: no pool worker is occupied and no thread sleeps through the interval.
    /// Pending delayed entries are cleared at actor shutdown, so a System.shutdown before the check fires never stalls behind it.
    /// If the instance exits first, nothing is lost: the next process run re-checks.
    void onSpawn(ActorRef self) @safe {
        self_ = self;
        logger.tracef("auto-optimize actor started (threshold=%s, interval=%s min)",
                threshold_, intervalMin_);
        dynDelayedSend(self_.address, delay(intervalMin_.minutes), "check");
    }

    /// The single message method: run the check, then terminate.
    void check() {
        checkAndMaybeOptimize();
        sendExit(self_.address, ExitReason.userShutdown);
    }

    /// Open the primary database read/write, read the write counter and, when it is at or above the threshold, run optimize and reset the counter.
    ///
    /// A failed optimize (exception / SpinSqlTimeout) leaves the counter: the next process run's check sees it and retries.
    /// A failed open is logged and skipped. The handle is closed before returning.
    private void checkAndMaybeOptimize() nothrow {
        import std.exception : collectException;
        import std.sumtype : match;
        import miniorm : spinSql;
        import my.optional : hasValue, None;

        try {
            auto dbOpt = openDatabase(dbPath_, model_, embedDims_, readOnly: false,
                    busyTimeout: busyTimeout_);
            if (!hasValue(dbOpt)) {
                logger.warningf("auto-optimize: skipping '%s' (cannot be opened read-write)",
                        dbPath_.toString);
                return;
            }
            auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
            scope (exit)
                db.destroy;

            const count = spinSql!(() => readOptimizeCounter(db.db))(timeout);
            logger.tracef("auto-optimize check: counter=%s, threshold=%s", count, threshold_);
            if (count < threshold_)
                return;

            logger.infof("auto-optimize triggered (count=%s, threshold=%s)", count, threshold_);
            optimizeAndReset(db, timeout);
        } catch (Exception e) {
            logger.errorf("auto-optimize check failed: %s", e.msg).collectException;
        }
    }

    /// Run the optimize command inside a bounded spin, then reset the counter.
    /// The spin ceiling is a parameter only so tests can bound a broken database fast; production passes the 30 s `timeout`.
    /// A failed optimize deliberately leaves the counter (the next process run's check retries); the reset itself is best-effort - a lost count only delays a run.
    private void optimizeAndReset(ref Database db, Duration spinCeiling) nothrow {
        import std.datetime.stopwatch : StopWatch;
        import std.exception : collectException;
        import miniorm : spinSql, SpinSqlTimeout;

        try {
            StopWatch sw;
            sw.start;
            spinSql!(() => optimize(db.db))(spinCeiling);
            logger.infof("auto-optimize: optimized '%s' in %s ms", dbPath_,
                    sw.peek.total!"msecs");
            resetOptimizeCounter(db.db);
        } catch (SpinSqlTimeout e) {
            logger.errorf("auto-optimize: '%s' still locked after %s of retries - giving up",
                    dbPath_, spinCeiling).collectException;
        } catch (Exception e) {
            logger.errorf("auto-optimize: optimize on '%s' failed: %s",
                    dbPath_, e.msg).collectException;
        }
    }
}

version (unittest) {
    import core.thread : Thread;
    import core.time : msecs, seconds;
    import std.conv : to;
    import std.datetime.stopwatch : AutoStart, StopWatch;
    import std.exception : collectException;
    import std.file : exists, mkdirRecurse, rmdirRecurse;
    import std.path : absolutePath;
    import std.sumtype : match;
    import miniorm : Miniorm;
    import my.actor.channel : dynSend;
    import my.actor.mailbox : WeakAddress;
    import my.actor.msg : monitor;
    import my.actor.system : makeSystem;
    import my.actor.system_msg : DownMsg;
    import my.optional : hasValue, None;
    import llm.common.embedder : Embedder, EmbedResult;
    import llm.config : RagConfig;
    import llm.rag.rag : addToDatabase, Document, Origin, Topic;

    /// Deterministic test embedder: FNV-1a 64-bit over the input, expanded into 8 float dimensions.
    /// The model/dims pair must match every openDatabase call in these tests, or the wipe-on-mismatch path silently rebuilds the database under test.
    private class TestEmbedder : Embedder {
        override string modelName() {
            return "test";
        }

        override long dimensions() {
            return 8;
        }

        override bool supportsTokenization() {
            return false;
        }

        override int batchSize() {
            return 1;
        }

        override EmbedResult embedQuery(string text) {
            return embed(text);
        }

        override EmbedResult embedDocument(string text) {
            return embed(text);
        }

        EmbedResult embed(string text) {
            ulong h = 14695981039346656037;
            foreach (byte b; text) {
                h ^= b;
                h *= 1099511628211;
            }
            auto vec = new float[8];
            foreach (i, ref f; vec) {
                f = cast(float)((h >> (8 * i)) & 0xFF) / 255.0;
            }
            return EmbedResult(vec);
        }

        EmbedResult embed(int[] tokens) {
            char[] text = new char[tokens.length];
            foreach (i, ref c; text)
                c = cast(char)(tokens[i] & 0x7F);
            return embed(cast(string) text);
        }

        override EmbedResult embedQuery(int[] tokens) {
            return embed(tokens);
        }

        override EmbedResult embedDocument(int[] tokens) {
            return embed(tokens);
        }

        override int[] tokenize(string text, bool addSpecial) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override void destroy() {
        }
    }

    private AbsolutePath makeScratchDir(long line = __LINE__) {
        auto dir = ("llmfun_test/rag_auto_optimize/" ~ line.to!string).AbsolutePath;
        mkdirRecurse(dir);
        return dir;
    }

    /// Build a scratch database at `path` and perform `writes` top-level adds (distinct topics, so each add counts 1 in the optimize counter).
    /// The database handle is returned OPEN: callers close it themselves (that is the point of several tests below).
    private Database buildDb(AbsolutePath path, long writes) {
        auto dbOpt = openDatabase(path, "test", 8, readOnly: false);
        assert(dbOpt.hasValue, "openDatabase must succeed on a fresh scratch dir");
        auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);

        foreach (i; 0 .. writes) {
            size_t nBatch;
            auto doc = Document(origin: Origin(Topic("t" ~ i.to!string)), data: "sample text "
                    ~ i.to!string ~ " for the counter test");
            auto res = addToDatabase(db, new TestEmbedder(), doc,
                    RagConfig(windowOverlapPercent: 10), nBatch);
            assert(res.chunks > 0, "each add must index at least one chunk");
        }
        return db;
    }

    private Database openScratch(AbsolutePath path) {
        auto dbOpt = openDatabase(path, "test", 8, readOnly: false);
        assert(dbOpt.hasValue, "openDatabase must succeed on the scratch db");
        return dbOpt.match!((Database d) => d, (None _) => Database.init);
    }

    /// Number of open file descriptors whose target is `file` (procfs).
    /// 0 after a clean sqlite close, so it observes the actor closing its database handle.
    private size_t openFdCount(string file) {
        import std.file : dirEntries, readLink, SpanMode;

        const absFile = absolutePath(file);
        size_t n;
        foreach (e; dirEntries("/proc/self/fd", SpanMode.shallow)) {
            try {
                if (absolutePath(readLink(e.name)) == absFile)
                    n++;
            } catch (Exception ex) {
                // an fd closed between listing and readLink: not open, skip
            }
        }
        return n;
    }

    /// Strict counter read for the smoke test: unlike readOptimizeCounter (nothrow, swallows errors and returns 0) this throws on a failed read,
    /// so a read error during the actor's optimize can never masquerade as a reset-to-0 and end the poll loop early (false-pass guard).
    private long counterStrict(ref Miniorm db) {
        auto stmt = db.prepare("SELECT count FROM OptimizeCounterTbl");
        foreach (ref r; stmt.get.execute) {
            return r.peek!long(0);
        }
        assert(0, "OptimizeCounterTbl must have exactly one row");
    }

    /// Shared flag set by ProbeActor when its monitored actor dies.
    private final class ExitProbe {
        shared bool down;
    }

    /// Test observer actor: records the DownMsg of the monitored actor, so the one-shot sendExit-after-check termination is observed, not just inspected.
    /// It defines no message methods - onDownMessage is a hook.
    private class ProbeActor {
        private ExitProbe probe_;

        this(ExitProbe probe) {
            probe_ = probe;
        }

        void onDownMessage(DownMsg msg) @safe nothrow {
            probe_.down = true;
        }
    }
}

@("checkAndMaybeOptimize below threshold leaves the counter and closes the handle")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    auto path = (dir ~ "p.db").AbsolutePath;

    auto db = buildDb(path, 1); // counter 1, below the threshold 2
    db.destroy; // release the test's own handle before the check

    auto a = new AutoOptimizeActor(path, 2, 1, "test", 8);
    a.checkAndMaybeOptimize();

    // Handle closedness is observed before opening the verification connection.
    assert(openFdCount(path.toString) == 0,
            "the check must close the database handle before returning");

    auto db2 = openScratch(path);
    scope (exit)
        db2.destroy;
    assert(readOptimizeCounter(db2.db) == 1,
            "a below-threshold check must not touch the counter, got " ~ readOptimizeCounter(db2.db)
                .to!string);
}

@("checkAndMaybeOptimize at or above the threshold optimizes and resets the counter")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    auto path = (dir ~ "p.db").AbsolutePath;

    auto db = buildDb(path, 3); // counter 3, above the threshold 2
    db.destroy;

    auto a = new AutoOptimizeActor(path, 2, 1, "test", 8);
    a.checkAndMaybeOptimize();

    auto db2 = openScratch(path);
    scope (exit)
        db2.destroy;
    assert(readOptimizeCounter(db2.db) == 0,
            "a triggered check must run optimize and reset the counter, got " ~ readOptimizeCounter(db2.db)
                .to!string);
}

@("a failed optimize leaves the counter for the next process run's check")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    auto path = (dir ~ "p.db").AbsolutePath;

    auto db = buildDb(path, 3); // counter 3, above the threshold 2
    // Break the write path without touching the on-disk counter: query_only makes the optimize INSERT fail immediately (SQLITE_READONLY).
    // That way the bounded spin hits its ceiling fast instead of hanging on a lock.
    db.run("PRAGMA query_only = ON");
    scope (exit)
        db.run("PRAGMA query_only = OFF");

    auto a = new AutoOptimizeActor(path, 2, 1, "test", 8);
    a.optimizeAndReset(db, 200.msecs); // bounded spin; the SpinSqlTimeout is swallowed

    db.run("PRAGMA query_only = OFF");
    assert(readOptimizeCounter(db.db) == 3,
            "a failed optimize must NOT reset the counter, got " ~ readOptimizeCounter(db.db)
                .to!string);
}

@("a failed open is logged and skipped, touching no counter")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    // The parent directory of the database does not exist: openDatabase returns none.
    // The check must skip without throwing or creating one.
    auto path = (dir ~ "no_such_dir" ~ "p.db").AbsolutePath;
    auto a = new AutoOptimizeActor(path, 2, 1, "test", 8);
    a.checkAndMaybeOptimize();

    // Pin the skip: openDatabase must bail at the directory check before the sqlite file is created.
    // The skipped run must leave no database behind.
    assert(!path.toString.exists, "a failed open must not create the database file");
}

@("a spawned actor runs its check on its own connection and exits without leaking")
unittest {
    auto sys = makeSystem;
    scope (exit)
        sys.shutdown;
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    auto path = (dir ~ "p.db").AbsolutePath;

    // The test holds its own connection open while the actor opens a second one to the same file: safe under rollback-journal locking.
    auto db = buildDb(path, 3); // counter 3, above the threshold 2
    scope (exit)
        db.destroy;

    // The interval is a constructor argument: 1 minute here, so the delayed self-send stays parked for the whole test and the check is driven directly.
    // After the check the actor sends exit; the pending delayed entry is cleared at actor shutdown, so shutdown never stalls.
    auto actor = sys.spawn!AutoOptimizeActor(path, 2, 1, "test", 8);
    // The check is the shutdown (one-shot): a monitor observes the actor's death, so the sendExit-after-check is a tested fact, not inspection.
    // The monitor is registered before the check is triggered: a late monitor would never see the DownMsg.
    auto probe = new ExitProbe();
    auto observer = sys.spawn!ProbeActor(probe);
    monitor(observer.weakRef, actor.weakRef);
    dynSend(actor.weakRef, "check");

    auto sw = StopWatch(AutoStart.yes);
    while (counterStrict(db.db) != 0 && sw.peek < 5.seconds) {
        Thread.sleep(10.msecs);
    }
    assert(counterStrict(db.db) == 0,
            "the spawned actor must run its check (optimize + reset) on its own connection, counter is " ~ counterStrict(
                db.db).to!string);

    sw.reset();
    while (!probe.down && sw.peek < 5.seconds) {
        Thread.sleep(10.msecs);
    }
    assert(probe.down, "the actor must terminate via sendExit after its check");
}
