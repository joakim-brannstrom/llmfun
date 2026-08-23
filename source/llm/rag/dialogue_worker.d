/// Dialogue indexing worker: a std.concurrency actor that owns its own embedder
/// and indexes evicted raw dialogue into per-session RAG databases.
///
/// The worker runs on its own thread (spawned by DialogueIndex in Task 5) and
/// never shares its embedder or its batch-size state with the agent thread. It
/// creates its OWN embedder from the EmbedConfig on its thread, opens a
/// per-session write connection (WAL), and indexes each episode through the
/// shared addToDatabase seam (which does the per-thread nBatchCache
/// adaptation). All communication is via value messages (DiJob / DiDrain /
/// DiDrained / DiDegraded); there is no shared state and no lock. The thread
/// runs for the lifetime of the process (in-flight episodes are lost at exit --
/// a documented hole).
module llm.rag.dialogue_worker;

import logger = std.logger;
import std.algorithm : filter;
import std.array : empty;
import std.concurrency : Tid, receive, send, OwnerTerminated;
import std.datetime : SysTime;
import std.exception : collectException;
import std.range : empty;
import std.sumtype : match;
import std.typecons : Tuple;

import my.optional;
import my.path : AbsolutePath;

import llm.common.config : EmbedConfig, RemoteEmbedConfig, ServerConfig;
import llm.common.embedder : Embedder, EmbedderFactory, createEmbedder, EmbedResult, EmbedError;
import llm.config : RagConfig;
import llm.rag.database : Database, openDatabase, Source, SourceChecksum, SourceId;
import llm.rag.rag : Document, Origin, Topic, addToDatabase;
import llm.session.types : SessionId, isValidId;

/// A single evicted dialogue episode to be indexed verbatim.
///
/// `topicName` is the fully-encoded Topic name (built by DialogueIndex via the
/// episode codec); the worker uses it directly as the Document origin, so it is
/// never parsed or re-encoded here.
struct DiEpisode {
    string topicName;
    string text;
    long turnEnd;
}

/// One compression checkpoint's worth of episodes for a single session.
///
/// `episodes` must be an `immutable` dynamic array: a mutable `DiEpisode[]`
/// field makes the message fail std.concurrency's `hasLocalAliasing` static
/// assert (it would alias the sender's thread-local array buffer). The strings
/// are `char[]` (already immutable) and the worker dup's what it needs.
struct DiJob {
    string sessionId;
    immutable(DiEpisode[]) episodes;
}

/// Request the worker to report back to `replyTo` once its mailbox has drained.
/// This is the synchronization point for tests (and for disposal) so a reader
/// can know that a preceding DiJob has been fully processed.
struct DiDrain {
    Tid replyTo;
}

/// Sent back to DiDrain.replyTo once the mailbox has been drained.
struct DiDrained {
}

/// Sent to the owner when the worker cannot create its embedder. Indexing is
/// then disabled for the process lifetime (documented hole, no recovery); the
/// worker still answers DiDrain so disposal never hangs.
struct DiDegraded {
    immutable string reason;
}

/// Actor entry point. Runs on its own thread (spawn with all arguments as
/// values). Creates and owns its own embedder; it never touches the agent's.
/// `embedderFactory` is a DI seam: when non-null the worker builds its
/// embedder from it instead of the process-wide factory registry, so unit
/// tests never race each other on the shared "remote" factory slot.
void dialogueWorker(Tid ownerTid, AbsolutePath dialogueDir, EmbedConfig embedConfig,
        RagConfig dialogueRagCfg, EmbedderFactory embedderFactory) {
    // Worker-local state. Single-threaded, so no locks. nBatchCache is owned
    // here (per-thread) and passed by reference into the addToDatabase seam.
    size_t nBatchCache;
    Database[string] dbs; // per-session write connections (lazy)

    // Create our OWN embedder on this thread. There is NO shared embedder
    // between this worker and the agent thread. Tests inject a factory
    // (embedderFactory != null); production goes through the registry.
    Embedder embedder;
    bool degraded;
    {
        try {
            embedder = embedderFactory is null ? createEmbedder(embedConfig) : embedderFactory(
                    embedConfig);
        } catch (Exception e) {
            embedder = null;
            logger.warningf("dialogue worker: embedder factory threw: %s", e.msg);
        }
        degraded = embedder is null;
    }
    if (degraded) {
        logger.warning(
                "dialogue worker: no embedder available; indexing disabled for process lifetime");
        send(ownerTid, DiDegraded("no embedder available"));
    }

    // Open (or reuse) the WAL write connection for a session. WAL is applied
    // once per opened connection so the agent thread can read committed
    // episodes concurrently (D6). Returns true on success.
    // NOTE: openDatabase returns None (not an error) when the parent dir is
    // missing or unwritable -- the DialogueIndex constructor mkdirRecurse's the
    // directory, so this only fails on an unwritable directory.
    bool ensureDb(string sessionId) {
        // Defense in depth: onCheckpoint validates, but the worker must never
        // be a path-traversal vector for a future caller that bypasses it.
        if (sessionId.empty || !isValidId(SessionId(sessionId))) {
            logger.warningf("dialogue worker: refusing invalid session '%s'", sessionId);
            return false;
        }
        if (sessionId in dbs)
            return true;
        auto path = (dialogueDir ~ (sessionId ~ ".db")).AbsolutePath;
        auto dbOpt = openDatabase(path, embedder.modelName(),
                embedder.dimensions(), readOnly: false);
        return dbOpt.match!((Database d) {
            try {
                d.run("PRAGMA journal_mode=WAL;");
            } catch (Exception e) {
                logger.warningf("dialogue worker: WAL pragma failed on '%s': %s", path, e.msg);
            }
            dbs[sessionId] = d;
            return true;
        }, (None _) {
            logger.warningf("dialogue worker: cannot open session DB '%s', skipping", path);
            return false;
        });
    }

    // Rebuild the FTS5 index of one session (fails soft: chunks are committed
    // either way; only text search is affected, and only until a later job's
    // rebuild). The external-content FtsChunksTbl is not auto-synced with
    // TextChunkTbl; this is the same seam the RAG add tools use.
    void rebuildFts(string sessionId) {
        try {
            dbs[sessionId].fts5Rebuild;
        } catch (Exception e) {
            logger.warningf("dialogue worker: fts5 rebuild failed for '%s': %s", sessionId, e.msg);
        }
    }

    // Index one checkpoint's episodes. D7 observability: exactly ONE tracef
    // per completed job (session id, episode/chunk/failure counts, turn
    // range min turnStart - max turnEnd); per-chunk logging is forbidden
    // (N3) and the episode text is never logged. Episodes are single-turn
    // (DialogueIndex groups by turnId, so each DiEpisode has turnStart ==
    // turnEnd), hence min(ep.turnEnd) over the job IS its min turnStart.
    void indexJob(DiJob job) {
        if (degraded || job.sessionId.empty || !ensureDb(job.sessionId))
            return;
        long jobMinTurn;
        long jobMaxTurn;
        size_t successes;
        size_t totalChunks;
        size_t failures;
        foreach (ep; job.episodes.filter!(a => !a.text.empty)) {
            // B1 (FX2): a turn split across two compressions re-arrives under
            // the same topic; merge the existing episode text with the new
            // piece instead of replacing it (topic name and turn range stay
            // unchanged). Read failures degrade to the plain piece (N3).
            string pieceText = ep.text;
            try {
                auto existingOpt = dbs[job.sessionId].getSource(Origin(Topic(ep.topicName)));
                if (hasValue(existingOpt)) {
                    auto existing = existingOpt.match!(a => a.value,
                            (None _) => Tuple!(Source, "src", SourceId, "id").init);
                    auto oldText = dbs[job.sessionId].sourceText(existing.id);
                    if (!oldText.empty) {
                        pieceText = oldText ~ "\n" ~ ep.text;
                        logger.tracef("dialogue worker: merged episode '%s' in '%s' (%s + %s chars)",
                                ep.topicName, job.sessionId, oldText.length, ep.text.length);
                    }
                }
            } catch (Exception e) {
                logger.warningf("dialogue worker: cannot read existing episode '%s' in '%s', replacing: %s",
                        ep.topicName, job.sessionId, e.msg);
            }
            auto doc = Document(origin: Origin(Topic(ep.topicName)), data: pieceText);

            try {
                // B4: the topic name is the dedup salt, so identical text
                // re-arriving under a different topic is not deduped away.
                auto res = addToDatabase(dbs[job.sessionId], embedder, doc,
                        dialogueRagCfg, nBatchCache, ep.topicName);
                totalChunks += res.chunks;
                ++successes;
            } catch (Exception e) {
                logger.warningf("dialogue worker: failed to index episode '%s' in '%s': %s",
                        ep.topicName, job.sessionId, e.msg);
                ++failures;
            }
            if (jobMinTurn == 0 || ep.turnEnd < jobMinTurn)
                jobMinTurn = ep.turnEnd;
            if (ep.turnEnd > jobMaxTurn)
                jobMaxTurn = ep.turnEnd;
        }
        // B3 (FX3): every job that committed at least one chunk ends with a
        // synchronous FTS5 rebuild, so committed chunks are never left
        // text-invisible (documented hole: a crash between the last chunk
        // commit and this rebuild - design R5).
        if (totalChunks > 0)
            rebuildFts(job.sessionId);
        logger.tracef("dialogue worker: session '%s' indexed %s episodes (%s chunks, %s failures, turns %s-%s)",
                job.sessionId, successes, totalChunks, failures, jobMinTurn, jobMaxTurn);
    }

    // Checkpoint + close all write connections. Leaves the session DBs in a
    // clean, sidecar-free state for the next process's read-only opens (normal
    // exit path; an abnormal death still leaves WAL sidecars, which SQLite
    // recovers on the next read-write open).
    void drainAndClose() {
        foreach (sessionId, ref db; dbs) {
            try {
                db.run("PRAGMA wal_checkpoint(TRUNCATE);");
            } catch (Exception e) {
                logger.warningf("dialogue worker: wal_checkpoint failed for '%s': %s",
                        sessionId, e.msg);
            }
            db.destroy;
        }
        dbs = null;
    }

    // Unbounded mailbox, no backpressure (documented hole). The loop never
    // ends; the thread dies with the process (in-flight episodes lost).
    bool running = true;
    while (running) {
        try {
            receive((DiJob j) {
                try {
                    indexJob(j);
                } catch (Exception e) {
                    logger.errorf("dialogue worker: indexJob threw: %s", e.msg);
                }
            }, (DiDrain d) {
                // A drained mailbox leaves clean, sidecar-free DBs behind
                // (checkpoint + close); FTS indexes were already rebuilt
                // per job (B3).
                drainAndClose();
                send(d.replyTo, DiDrained());
            });
        } catch (OwnerTerminated e) {
            running = false;
            logger.trace("dialogue worker: owner terminated");
        } catch (Exception e) {
            running = false;
            logger.warningf("dialogue worker: unexpected error, stopping: %s", e.msg);
        }
    }

    foreach (db; dbs.byKeyValue) {
        db.value.destroy;
    }
    dbs = null;
}

// ===========================================================================
// Tests
// ===========================================================================

version (unittest) {
    import std.file : mkdirRecurse;
    import llm.test_util : TestArea, testArea;

    // --- Test embedder factories.
    // Defined at module scope (not nested in a unittest) as plain
    // `Embedder f(EmbedConfig)` functions: each test passes one by reference
    // into dialogueWorker (the DI seam), and the address of a module-scope
    // function (`&f`) converts to the plain EmbedderFactory pointer.

    /// Non-thread-safe embedder for the concurrency-isolation test (test 1): it
    /// writes the input into a per-instance scratch buffer, sleeps to force
    /// concurrent overlap, then reads it back.
    private class NtsEmbedder : Embedder {
        private char[] scratch;

        this() {
            scratch = new char[64];
        }

        override void destroy() {
        }

        override string modelName() {
            return "nts";
        }

        override long dimensions() {
            return 16;
        }

        override bool supportsTokenization() {
            return false;
        }

        override EmbedResult embed(string text) {
            import core.thread : Thread;
            import core.time : dur;
            import std.algorithm : min;

            int n = min(cast(int) text.length, 16);
            scratch[0 .. n] = text[0 .. n];
            scope (exit)
                Thread.sleep(50.dur!"msecs"); // force overlap
            auto v = new float[16];
            foreach (i, ref f; v)
                f = i < n ? cast(float) scratch[i] : 0f;
            return EmbedResult(v);
        }

        override EmbedResult embed(int[] tokens) {
            return EmbedResult(EmbedError("no tokens"));
        }

        override int[] tokenize(string text) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            return 1024;
        }
    }

    /// Deterministic embedder for the worker integration test (test 2): returns an
    /// all-ones 8-dim vector; a small batchSize forces multiple chunks.
    private class WkEmbedder : Embedder {
        override void destroy() {
        }

        override string modelName() {
            return "wk";
        }

        override long dimensions() {
            return 8;
        }

        override bool supportsTokenization() {
            return false;
        }

        override EmbedResult embed(string text) {
            auto v = new float[8];
            foreach (ref f; v)
                f = 1.0f;
            return EmbedResult(v);
        }

        override EmbedResult embed(int[] tokens) {
            return EmbedResult(EmbedError("no tokens"));
        }

        override int[] tokenize(string text) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            return 16;
        } // small -> several chunks
    }

    /// Embedder for the per-episode failure test (test d): returns a valid
    /// all-ones 8-dim vector for normal text, but THROWS for any text carrying
    /// the poison marker. A throw (as opposed to an EmbedError) propagates out
    /// of addToDatabase so the worker's per-episode catch is exercised for
    /// exactly that episode.
    private class PoisonedEmbedder : Embedder {
        override void destroy() {
        }

        override string modelName() {
            return "wk";
        }

        override long dimensions() {
            return 8;
        }

        override bool supportsTokenization() {
            return false;
        }

        override EmbedResult embed(string text) {
            import std.string : indexOf;

            if (text.indexOf("POISON") >= 0)
                throw new Exception("poisoned text (per-episode failure test)");
            auto v = new float[8];
            foreach (ref f; v)
                f = 1.0f;
            return EmbedResult(v);
        }

        override EmbedResult embed(int[] tokens) {
            return EmbedResult(EmbedError("no tokens"));
        }

        override int[] tokenize(string text) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            return 16;
        }
    }

    // Plain-function embedder factories for the spawn-based tests: each test
    // injects its own factory into dialogueWorker, so no test touches the
    // process-wide "remote" factory slot (a parallel test swapping that slot
    // while another test's worker thread was starting used to race the worker
    // onto the wrong embedder - or onto a degraded one - for its whole life).
    private Embedder wkEmbedderFactory(EmbedConfig config) {
        return new WkEmbedder();
    }

    private Embedder ntsEmbedderFactory(EmbedConfig config) {
        return new NtsEmbedder();
    }

    private Embedder poisonedEmbedderFactory(EmbedConfig config) {
        return new PoisonedEmbedder();
    }

    /// D7 test seam: a Logger that captures formatted messages so a test can
    /// assert on emitted log lines. Installed via the std.logger `sharedLog`
    /// swap (same pattern as TuiLogger in llm.tui); thread-safe because the
    /// worker thread logs concurrently with the test thread.
    private class D7LogCapture : logger.Logger {
        import core.sync.mutex;
        import std.array : Appender;

        private {
            Appender!(string[]) lines;
            Mutex mtx;
        }

        this(const logger.LogLevel lvl = logger.LogLevel.all) {
            super(lvl);
            this.mtx = new Mutex;
        }

        override void writeLogMsg(ref LogEntry payload) @trusted {
            mtx.lock_nothrow();
            scope (exit)
                mtx.unlock_nothrow();
            lines.put(payload.msg);
        }

        string[] takeLines() {
            mtx.lock_nothrow();
            scope (exit)
                mtx.unlock_nothrow();
            auto tmp = lines[];
            lines.clear();
            return tmp;
        }
    }
}

/// Per-worker result of the isolation test (test 1). Carried back to the main
/// thread; std.concurrency cannot send a local float[] result.
private struct NtsDone {
    int idx;
    bool ok;
}

/// Test-1 worker (runs on its own thread via spawn). Creates its OWN embedder
/// from the injected factory (the per-thread guarantee), embeds its text, and
/// self-verifies the result matches the input -- a shared non-thread-safe
/// instance would corrupt its scratch buffer under concurrent overlap and fail
/// this check. Module-scope so spawn can take its address: a core.thread
/// closure in a foreach captures the loop index by reference, so every thread
/// would see the final value of i.
private void ntsIsolationWorker(int idx, string text, EmbedConfig cfg,
        EmbedderFactory factory, Tid doneTid) {
    auto emb = factory(cfg);
    if (emb is null) {
        send(doneTid, NtsDone(idx, false));
        return;
    }
    auto v = emb.embed(text).match!((float[] a) => a, (EmbedError e) => null);
    bool ok = (v !is null && v.length == 16);
    if (ok)
        foreach (j; 0 .. 16)
            if (v[j] != (j < text.length ? cast(float) text[j] : 0f))
                ok = false;
    send(doneTid, NtsDone(idx, ok));
}

/// A non-thread-safe embedder that mimics LlamaEmbedder: it writes the input
/// into a per-instance pre-allocated scratch buffer, sleeps to force concurrent
/// overlap, then reads it back. A SINGLE instance shared across threads would
/// corrupt here; the per-thread instances produced by the worker pattern are
/// isolated.
unittest {
    import std.concurrency : spawn, thisTid;
    import std.format : format;

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "nts", dimensions: 16));

    immutable N = 4;
    auto texts = new string[N];
    foreach (i; 0 .. N) {
        string t;
        foreach (j; 0 .. 16)
            t ~= cast(char)('0' + ((i * 7 + j) % 10));
        texts[i] = t;
    }

    // Each worker embeds ITS OWN text with ITS OWN embedder and self-verifies
    // the result against the input. A shared non-thread-safe instance would
    // corrupt its scratch buffer under concurrent overlap and fail the check.
    // The float[] result is checked inside the worker (std.concurrency cannot
    // send local arrays) and only a bool verdict is carried back.
    auto doneTid = thisTid;
    foreach (i; 0 .. N)
        spawn(&ntsIsolationWorker, i, texts[i], cfg, &ntsEmbedderFactory, doneTid);

    int done;
    bool allOk = true;
    foreach (i; 0 .. N)
        receive((NtsDone d) {
            if (!d.ok)
                allOk = false;
            ++done;
        });
    assert(done == N, "expected %s workers to report, got %s".format(N, done));
    assert(allOk, "an embedder instance was not isolated (cross-thread corruption detected)");
}

/// Worker integration: spawn the actor, feed it one DiJob, drain it (the
/// mailbox is FIFO so the drain proves the job was processed), and verify the
/// per-session DB was created and contains indexed chunks.
unittest {
    import core.thread : Thread;
    import std.concurrency : spawn, send, receiveTimeout, thisTid;
    import core.time : dur;
    import std.file : mkdirRecurse, rmdirRecurse, tempDir;
    import std.path : buildPath;
    import std.conv : to;
    import std.format : format;

    auto tmpDir = testArea("worker_integration");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    // Spawn the worker on its own thread; it creates its own embedder.
    auto tid = spawn(&dialogueWorker, thisTid, tmpDir.workArea, cfg, ragCfg, &wkEmbedderFactory);
    Thread.sleep(150.dur!"msecs"); // let the worker start up

    // One episode. topicName is opaque to the worker (it never parses topic
    // names - the codec round-trip is exercised by dialogue_index tests), but
    // the session id must pass the worker's own D12 validation.
    string sid = "20240101-120000-abcd";
    string topicName = "d_20240101_120000_abcd__t11_11__1000";
    string episodeText;
    foreach (i; 0 .. 12)
        episodeText ~= "turn " ~ i.to!string ~ " content line\n";

    send(tid, DiJob(sid, [DiEpisode(topicName.idup, episodeText.idup, 11)]));
    send(tid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(30.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain its mailbox");

    // Verify the per-session DB has indexed chunks.
    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "per-session DB was not created");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    long countChunks(ref Database d) {
        static immutable sql = "SELECT count(*) FROM TextChunkTbl;";
        auto stmt = d.prepare(sql);
        foreach (ref r; stmt.get.execute)
            return r.peek!long(0);
        return -1;
    }

    auto rowCount = countChunks(db);
    assert(rowCount >= 2, "expected >= 2 indexed chunks, got %s".format(rowCount));

    // FTS5 must be rebuilt by the worker so text search sees the new chunks.
    auto hits = db.queryTextSearch("content", 10);
    assert(hits !is null && hits.length >= 1,
            "expected FTS text search hits after indexing, got none");
    assert(hits[0].text.length > 0, "hit must carry the verbatim chunk text");
}

/// B4 (FX1): identical episode text under two different topics must both be
/// indexed. The topic name is the dedup salt, so the two salted identities
/// differ; with content-only dedup the second episode would be dropped as a
/// duplicate and only one source would exist.
unittest {
    import core.thread : Thread;
    import std.concurrency : spawn, send, receiveTimeout, thisTid;
    import core.time : dur;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.conv : to;
    import std.format : format;

    auto tmpDir = testArea("dialogue_dedup");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto tid = spawn(&dialogueWorker, thisTid, tmpDir.workArea, cfg, ragCfg, &wkEmbedderFactory);
    Thread.sleep(150.dur!"msecs"); // let the worker start up

    // One job, two episodes: identical text, different topics (turns 11 and 12).
    string sid = "20240101-120000-abcd";
    string episodeText;
    foreach (i; 0 .. 12)
        episodeText ~= "turn " ~ i.to!string ~ " content line\n";
    send(tid, DiJob(sid, [
        DiEpisode("d_20240101_120000_abcd__t11_11__1000", episodeText.idup, 11),
        DiEpisode("d_20240101_120000_abcd__t12_12__1000", episodeText.idup, 12),
    ]));
    send(tid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(30.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain its mailbox");

    // Both topics must be indexed as distinct sources (content-only dedup
    // would leave exactly one).
    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "per-session DB was not created");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;
    assert(db.getSources().length == 2,
            "identical text under two topics must index two sources, got %s".format(
                db.getSources().length));
}

/// B1 (FX2): a turn split across two compressions re-arrives under the same
/// topic. The worker merges the existing episode text with the new piece
/// (instead of replacing it): after both jobs, one source holds "old\nnew"
/// verbatim and both pieces are text-searchable.
unittest {
    import core.thread : Thread;
    import std.concurrency : spawn, send, receiveTimeout, thisTid;
    import core.time : dur;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.format : format;

    auto tmpDir = testArea("dialogue_splitmerge");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto tid = spawn(&dialogueWorker, thisTid, tmpDir.workArea, cfg, ragCfg, &wkEmbedderFactory);
    Thread.sleep(150.dur!"msecs"); // let the worker start up

    // Two jobs, same topic (a turn split across two compression checkpoints).
    string sid = "20240101-120000-abcd";
    string topicName = "d_20240101_120000_abcd__t11_11__1000";
    send(tid, DiJob(sid, [
        DiEpisode(topicName.idup, "SPLITUSER exact question alpha".idup, 11)
    ]));
    send(tid, DiJob(sid, [
        DiEpisode(topicName.idup, "SPLITASST answer beta".idup, 11)
    ]));
    send(tid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(30.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain its mailbox");

    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "per-session DB was not created");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    // Exactly one source: the merge replaced the first piece's source, it did
    // not add a second one.
    assert(db.getSources().length == 1,
            "split-turn merge must keep exactly one source, got %s".format(db.getSources().length));

    auto opt = db.getSource(Origin(Topic(topicName)));
    assert(hasValue(opt), "merged source must be findable by topic");
    auto id = opt.match!(a => a.id, (None _) => SourceId.init);
    auto text = db.sourceText(id);
    assert(text == "SPLITUSER exact question alpha\nSPLITASST answer beta",
            "merged text must be old\nnew verbatim, got '%s'".format(text));

    // Both pieces must be text-searchable (the FTS index covers the merge).
    auto h1 = db.queryTextSearch("SPLITUSER", 10);
    assert(h1 !is null && h1.length >= 1, "old piece not text-searchable after the merge");
    auto h2 = db.queryTextSearch("SPLITASST", 10);
    assert(h2 !is null && h2.length >= 1, "new piece not text-searchable after the merge");
}

/// B1 (FX2): Database.sourceText reconstructs a multi-chunk source exactly -
/// the leading-overlap stripping removes the sliding-window overlap without
/// duplicating or dropping any text.
unittest {
    import core.thread : Thread;
    import std.concurrency : spawn, send, receiveTimeout, thisTid;
    import core.time : dur;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.conv : to;
    import std.format : format;

    auto tmpDir = testArea("dialogue_multichunk");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto tid = spawn(&dialogueWorker, thisTid, tmpDir.workArea, cfg, ragCfg, &wkEmbedderFactory);
    Thread.sleep(150.dur!"msecs"); // let the worker start up

    string sid = "20240101-120000-abcd";
    string topicName = "d_20240101_120000_abcd__t11_11__1000";
    string episodeText;
    foreach (i; 0 .. 12)
        episodeText ~= "turn " ~ i.to!string ~ " content line\n";
    send(tid, DiJob(sid, [DiEpisode(topicName.idup, episodeText.idup, 11)]));
    send(tid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(30.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain its mailbox");

    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "per-session DB was not created");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    // Precondition: the episode produced >= 2 chunks (overlapping windows).
    long rowCount = -1;
    {
        auto stmt = db.prepare("SELECT count(*) FROM TextChunkTbl;");
        foreach (ref r; stmt.get.execute)
            rowCount = r.peek!long(0);
    }
    assert(rowCount >= 2, "expected >= 2 chunks for the round-trip test, got %s".format(rowCount));

    auto opt = db.getSource(Origin(Topic(topicName)));
    assert(hasValue(opt), "source must be findable by topic");
    auto id = opt.match!(a => a.id, (None _) => SourceId.init);
    auto text = db.sourceText(id);
    assert(text == episodeText,
            "sourceText must round-trip the original text exactly (no duplicated overlap, no lost text)");
}

/// B1 (FX2): a pre-existing source with no chunks (empty reconstruction) must
/// not poison the merge - the plain piece is indexed, replacing the empty
/// source (one source, no throw out of the job).
unittest {
    import core.thread : Thread;
    import std.concurrency : spawn, send, receiveTimeout, thisTid;
    import core.time : dur;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.format : format;

    auto tmpDir = testArea("dialogue_emptysrc");
    scope (exit)
        tmpDir.cleanup();

    string sid = "20240101-120000-abcd";
    string topicName = "d_20240101_120000_abcd__t11_11__1000";

    // Seed the session DB with an empty source (no chunks) for the topic.
    auto seedOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: false);
    assert(seedOpt.hasValue, "seed DB must open");
    auto seed = seedOpt.match!((Database d) => d, (None _) => Database.init);
    seed.addSource(Source(origin: Origin(Topic(topicName)), checksum: 42.SourceChecksum,
            added: SysTime.init));
    seed.destroy;

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto tid = spawn(&dialogueWorker, thisTid, tmpDir.workArea, cfg, ragCfg, &wkEmbedderFactory);
    Thread.sleep(150.dur!"msecs"); // let the worker start up

    string piece = "EMPTYFALLBACK plain piece after an empty source";
    send(tid, DiJob(sid, [DiEpisode(topicName.idup, piece.idup, 11)]));
    send(tid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(30.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain its mailbox (read failure must not throw)");

    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "per-session DB was not created");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    assert(db.getSources().length == 1,
            "the empty source must be replaced, not duplicated (got %s)".format(
                db.getSources().length));
    auto h = db.queryTextSearch("EMPTYFALLBACK", 10);
    assert(h !is null && h.length >= 1, "the plain piece was not indexed");
}

/// B3 (FX3): two back-to-back jobs for one session (the second arriving well
/// inside the old 1 s coalescing window) must BOTH be text-searchable after
/// the drain - every job that committed chunks rebuilds the FTS index itself,
/// and the drain no longer flushes any deferred rebuilds.
unittest {
    import core.thread : Thread;
    import std.concurrency : spawn, send, receiveTimeout, thisTid;
    import core.time : dur;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.format : format;

    auto tmpDir = testArea("dialogue_ftsperjob");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto tid = spawn(&dialogueWorker, thisTid, tmpDir.workArea, cfg, ragCfg, &wkEmbedderFactory);
    Thread.sleep(150.dur!"msecs"); // let the worker start up

    // Two jobs back-to-back (different topics, unique tokens); the second
    // lands far inside the old 1 s coalescing window, so only a per-job
    // rebuild can make its token searchable before the drain's checkpoint+close.
    string sid = "20240101-120000-abcd";
    send(tid, DiJob(sid, [
        DiEpisode("d_20240101_120000_abcd__t11_11__1000".idup,
                "FTSJOBONE first job token here".idup, 11)
    ]));
    send(tid, DiJob(sid, [
        DiEpisode("d_20240101_120000_abcd__t12_12__1000".idup,
                "FTSJOBTWO second job token here".idup, 12)
    ]));
    send(tid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(30.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain its mailbox");

    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "per-session DB was not created");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    auto h1 = db.queryTextSearch("FTSJOBONE", 10);
    assert(h1 !is null && h1.length >= 1, "job 1 token not text-searchable");
    auto h2 = db.queryTextSearch("FTSJOBTWO", 10);
    assert(h2 !is null && h2.length >= 1,
            "job 2 token not text-searchable (per-job rebuild missing)");
}

/// N4: result carried back from the concurrent read-only reader (test 3) to the
/// main thread. std.concurrency cannot send a Database, so the reader reports a
/// verdict: the committed chunk count it observed and any lock/validity errors.
private struct WkRdResult {
    long count; // committed chunk count observed (-1 = never read a valid count)
    int errors; // lock/validity errors hit while polling
    bool ready; // true = handshake (read-only connection open), false = final verdict
}

/// N4: concurrent read-only reader (test 3). Opens the session DB file READ-ONLY
/// while the worker holds its WAL write connection, and polls the committed chunk
/// count until it observes the worker's second commit (count >= 4) or times out.
/// It sends a `ready` handshake once its connection is open (so the main thread
/// commits the second job while the reader is definitely open -- a true
/// concurrent writer+reader overlap) and a final `ready=false` verdict at the end.
/// Module-scope so spawn can take its address.
private void readerProbe(in string dbPath, Tid outTid) {
    import core.thread : Thread;
    import core.time : dur;
    import std.string : indexOf;

    // Phase 1: open the DB read-only. openDatabase(readOnly) returns None
    // immediately when the file does not exist yet (no 5 s retry), so poll until
    // the worker has created the file + schema.
    Database db = Database.init;
    bool opened = false;
    foreach (i; 0 .. 300) {
        auto dbOpt = openDatabase(dbPath.AbsolutePath, "wk", 8, readOnly: true);
        if (dbOpt.hasValue) {
            db = dbOpt.match!((Database d) => d, (None _) => Database.init);
            opened = true;
            break;
        }
        Thread.sleep(50.dur!"msecs");
    }

    // Handshake: the read-only connection is open (or we give up below). Let the
    // main thread commit the second job while we hold the connection open, so the
    // reader and writer genuinely coexist (N4).
    send(outTid, WkRdResult(-1, 0, true));

    if (!opened) {
        send(outTid, WkRdResult(-1, 1, false)); // never managed to open
        return;
    }
    scope (exit)
        db.destroy;

    // Phase 2: poll the committed chunk count until it reaches >= 4 (both jobs'
    // chunks are visible through this read-only connection) or time out.
    long maxCount = -1;
    int errors = 0;
    bool everValid = false;
    foreach (i; 0 .. 300) {
        long n = -1;
        try {
            auto stmt = db.prepare("SELECT count(*) FROM TextChunkTbl;");
            foreach (ref r; stmt.get.execute)
                n = r.peek!long(0);
        } catch (Exception e) {
            // A "database is locked" here is the N4 violation; any other error
            // (e.g. the table is not committed yet) is transient -- keep polling.
            string m = (e.msg is null) ? "" : e.msg;
            if (m.indexOf("locked") >= 0)
                errors++;
        }
        if (n >= 0) {
            everValid = true;
            if (n > maxCount)
                maxCount = n;
            if (maxCount >= 4)
                break;
        }
        Thread.sleep(50.dur!"msecs");
    }
    if (!everValid)
        errors += 2; // never saw a valid committed count through the reader
    send(outTid, WkRdResult(maxCount, errors, false)); // final verdict
}

/// Embedder factory for the degraded-worker test (test c): always throws, so
/// it throws on the worker thread when injected and the worker enters the
/// degraded path (embedder is null). Module-scope so its address can be passed
/// to dialogueWorker as the DI seam (a plain function pointer).
private Embedder throwingEmbedderFactory(EmbedConfig config) {
    throw new Exception("simulated embedder creation failure (degraded-worker test)");
}

/// N4: WAL lets the worker (single writer) and a concurrent read-only reader
/// coexist on the same session DB file without "database is locked". The reader
/// opens the file read-only while the worker holds its WAL write connection, must
/// observe the worker's first commit, and must then see the worker's second
/// commit land (polling for visibility) without a lock error. This follows the
/// production pattern: the worker drains exactly once, at dispose, AFTER all its
/// DiJobs -- it does not checkpoint mid-stream.
unittest {
    import core.thread : Thread;
    import std.concurrency : spawn, send, receiveTimeout, thisTid;
    import core.time : dur;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.conv : to;
    import std.format : format;

    auto tmpDir = testArea("dialogue_rw");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto tid = spawn(&dialogueWorker, thisTid, tmpDir.workArea, cfg, ragCfg, &wkEmbedderFactory);
    Thread.sleep(150.dur!"msecs");

    string sid = "20240101-120000-abcd";
    string topicName = "d_20240101_120000_abcd__t11_11__1000";
    string episodeText;
    foreach (i; 0 .. 12)
        episodeText ~= "turn " ~ i.to!string ~ " content line\n";

    // Writer: first job (fire-and-forget, like production onCheckpoint). This
    // opens the worker's WAL connection and creates the schema + first commit.
    send(tid, DiJob(sid, [DiEpisode(topicName.idup, episodeText.idup, 11)]));

    // Concurrent read-only reader on the same file (N4).
    string dbPath = (tmpDir ~ (sid ~ ".db")).idup;
    spawn(&readerProbe, dbPath, thisTid);

    // Wait for the reader's handshake: its read-only connection is now open.
    WkRdResult readyMsg;
    bool gotReady = false;
    receiveTimeout(30.dur!"seconds", (WkRdResult r) {
        readyMsg = r;
        gotReady = true;
    });
    assert(gotReady, "reader did not report its read-only connection is open");
    assert(readyMsg.ready, "first reader message must be the ready handshake");

    // Writer: second job committed WHILE the reader connection is open. With WAL
    // the reader keeps running and will see this commit; without WAL it would hit
    // "database is locked" (caught and reported by the reader).
    string episodeText2;
    foreach (i; 0 .. 12)
        episodeText2 ~= "turn " ~ i.to!string ~ " content line two\n";
    send(tid, DiJob(sid, [DiEpisode(topicName.idup, episodeText2.idup, 12)]));

    // Wait for the reader's final verdict (it polled until it saw both commits).
    WkRdResult finMsg;
    bool gotFin = false;
    receiveTimeout(30.dur!"seconds", (WkRdResult r) { finMsg = r; gotFin = true; });
    assert(gotFin, "reader final verdict not received");
    assert(!finMsg.ready, "second reader message must be the final verdict");
    assert(finMsg.errors == 0,
            "read-only reader hit a lock/validity error while the writer was committing: %s".format(
                finMsg.errors));
    assert(finMsg.count >= 4,
            "reader did not observe both commits (expected >= 4 chunks, saw %s)".format(
                finMsg.count));

    // Single final drain (production dispose pattern): flush dirty FTS and close
    // the write connection. The reader is closed now, so the checkpoint is clean.
    send(tid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(30.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain its mailbox");

    // Independent verification: a fresh read-only connection sees all committed
    // chunks and the file is in WAL mode.
    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "per-session DB missing");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;
    long total = -1;
    {
        auto stmt = db.prepare("SELECT count(*) FROM TextChunkTbl;");
        foreach (ref r; stmt.get.execute)
            total = r.peek!long(0);
    }
    assert(total >= 4, "fresh connection sees < 4 committed chunks: %s".format(total));
    string journal = "";
    {
        auto stmt = db.prepare("PRAGMA journal_mode;");
        foreach (ref r; stmt.get.execute)
            journal = r.peek!string(0);
    }
    assert(journal == "wal", "expected wal journal mode, got '%s'".format(journal));
}

/// Resilience (N3, writer side): a pre-corrupted session DB must not crash the
/// worker or starve other sessions. The corrupted session's job is skipped
/// (openDatabase keeps failing and returns none), but a second session's job in
/// the same mailbox is still indexed, and the worker still answers DiDrain.
unittest {
    import core.thread : Thread;
    import std.concurrency : spawn, send, receiveTimeout, thisTid;
    import core.time : dur;
    import std.file : mkdirRecurse, rmdirRecurse, write;
    import std.conv : to;
    import std.format : format;

    auto tmpDir = testArea("dialogue_corrupt");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    // Pre-corrupt session A's DB file with junk bytes so openDatabase fails.
    string sidA = "20240101-120000-abcd";
    string sidB = "20240101-120001-ef01";
    ubyte[] junk;
    foreach (i; 0 .. 128)
        junk ~= cast(ubyte)(i + 1);
    write((tmpDir ~ (sidA ~ ".db")).AbsolutePath, junk);

    auto tid = spawn(&dialogueWorker, thisTid, tmpDir.workArea, cfg, ragCfg, &wkEmbedderFactory);
    Thread.sleep(150.dur!"msecs");

    // Job for the corrupted session A (skipped), then a valid session B.
    string epA;
    foreach (i; 0 .. 12)
        epA ~= "turn " ~ i.to!string ~ " content line\n";
    send(tid, DiJob(sidA, [
        DiEpisode("d_20240101_120000_abcd__t11_11__1000", epA.idup, 11)
    ]));
    string epB;
    foreach (i; 0 .. 12)
        epB ~= "session B turn " ~ i.to!string ~ " line\n";
    send(tid, DiJob(sidB, [
        DiEpisode("d_20240101_120001_ef01__t11_11__1000", epB.idup, 11)
    ]));
    send(tid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(60.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain after a corrupted session DB (crashed or hung)");

    // Session B's DB must have indexed chunks (other session's state is intact).
    auto dbOpt = openDatabase((tmpDir ~ (sidB ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "session B DB missing (worker aborted after the corrupted session)");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;
    long rowCount = -1;
    {
        auto stmt = db.prepare("SELECT count(*) FROM TextChunkTbl;");
        foreach (ref r; stmt.get.execute)
            rowCount = r.peek!long(0);
    }
    assert(rowCount >= 2, "session B should have >= 2 indexed chunks, got %s".format(rowCount));
}

/// Resilience (N3, degraded worker): when the injected embedder factory
/// throws, the worker degrades gracefully -- it sends DiDegraded exactly
/// once, skips every DiJob, and still answers DiDrain so disposal never hangs.
unittest {
    import core.thread : Thread;
    import std.concurrency : spawn, send, receiveTimeout, thisTid;
    import core.time : dur;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.conv : to;
    import std.format : format;

    auto tmpDir = testArea("dialogue_degraded");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto tid = spawn(&dialogueWorker, thisTid, tmpDir.workArea, cfg, ragCfg,
            &throwingEmbedderFactory);
    Thread.sleep(150.dur!"msecs"); // let the worker start up and degrade

    // Two DiJobs (both must be skipped) and a DiDrain (must be answered).
    string sid = "20240101-120000-abcd";
    string ep;
    foreach (i; 0 .. 12)
        ep ~= "turn " ~ i.to!string ~ " content line\n";
    send(tid, DiJob(sid, [
        DiEpisode("d_20240101_120000_abcd__t11_11__1000", ep.idup, 11)
    ]));
    send(tid, DiJob(sid, [
        DiEpisode("d_20240101_120000_abcd__t12_12__1000", ep.idup, 12)
    ]));
    send(tid, DiDrain(thisTid));

    int degradedCount = 0;
    bool drained = false;
    foreach (i; 0 .. 10) {
        if (degradedCount >= 1 && drained)
            break;
        receiveTimeout(5.dur!"seconds", (DiDegraded _) { ++degradedCount; }, (DiDrained _) {
            drained = true;
        });
    }
    assert(degradedCount == 1,
            "degraded worker must send DiDegraded exactly once, got %s".format(degradedCount));
    assert(drained, "degraded worker did not answer DiDrain (disposal would hang)");
}

/// Resilience (N3, per-episode): a poisoned episode (the embedder THROWS for
/// that text) must not abort the rest of the job. The bad episode is skipped
/// (warningf + ++failures) and the other episodes in the SAME job are still
/// indexed.
unittest {
    import core.thread : Thread;
    import std.concurrency : spawn, send, receiveTimeout, thisTid;
    import core.time : dur;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.format : format;

    auto tmpDir = testArea("dialogue_poison");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto tid = spawn(&dialogueWorker, thisTid, tmpDir.workArea, cfg, ragCfg,
            &poisonedEmbedderFactory);
    Thread.sleep(150.dur!"msecs");

    string sid = "20240101-120000-abcd";
    // Three episodes in one job: the first is poisoned (the embedder throws for
    // it); the other two are normal and must still be indexed.
    string poison = "POISON marker this episode must fail to index";
    string good1 = "alpha episode one normal content here";
    string good2 = "beta episode two normal content here";
    send(tid, DiJob(sid, [
        DiEpisode("d_20240101_120000_abcd__t1_1__1000", poison.idup, 1),
        DiEpisode("d_20240101_120000_abcd__t2_2__1000", good1.idup, 2),
        DiEpisode("d_20240101_120000_abcd__t3_3__1000", good2.idup, 3),
    ]));
    send(tid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(30.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain after a poisoned episode (crashed)");

    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "session DB was not created");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    // The normal episodes must be indexed (the bad episode did not abort them).
    auto h1 = db.queryTextSearch("alpha", 10);
    assert(h1 !is null && h1.length >= 1,
            "good episode 1 was not indexed (the bad episode aborted the job)");
    auto h2 = db.queryTextSearch("beta", 10);
    assert(h2 !is null && h2.length >= 1,
            "good episode 2 was not indexed (the bad episode aborted the job)");

    // The poisoned episode must NOT be indexed (its embedding threw).
    auto hp = db.queryTextSearch("POISON", 10);
    assert(hp is null || hp.length == 0,
            "poisoned episode was indexed despite the embedder throwing");
}

/// D7: one completed DiJob must produce exactly ONE worker trace line carrying
/// all D7 fields (session id, episode/chunk/failure counts, turn range
/// min turnStart - max turnEnd), with no per-chunk spam. Captures the line via
/// the std.logger sharedLog seam (restored on exit, with the global log level
/// raised to trace for the duration only). The capture is process-global and
/// parallel worker tests emit their own D7 lines, so this test uses a unique
/// per-process session id and anchors the capture filter on its quoted form.
unittest {
    import core.thread : Thread;
    import std.concurrency : spawn, send, receiveTimeout, thisTid;
    import core.time : dur;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.algorithm : canFind;
    import std.string : startsWith;
    import std.conv : to;
    import std.format : format;

    auto tmpDir = testArea("dialogue_d7");
    scope (exit)
        tmpDir.cleanup();

    // Capture all log output (the worker thread's included) and enable trace
    // level for the D7 line; both are restored on exit.
    auto prevLog = logger.sharedLog;
    auto prevLevel = logger.globalLogLevel;
    auto cap = cast(shared) new D7LogCapture();
    logger.sharedLog = cap;
    logger.globalLogLevel = logger.LogLevel.trace;
    scope (exit) {
        logger.globalLogLevel = prevLevel;
        logger.sharedLog = prevLog;
    }

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto tid = spawn(&dialogueWorker, thisTid, tmpDir.workArea, cfg, ragCfg, &wkEmbedderFactory);
    Thread.sleep(150.dur!"msecs"); // let the worker start up

    // One job with two single-turn episodes (turns 3 and 7): the D7 line must
    // report the range 3-7 (min turnStart - max turnEnd over the job).
    // Unique per-process session id (valid IdPattern form: YYYYMMDD-HHMMSS-
    // 4hex): the capture filter anchors on its quoted form, which no parallel
    // test's line can match. A module-scope counter plus the wall clock make
    // collisions with the fixed sids other tests use (~"abcd") negligible.
    static uint d7Seq;
    long nowMillis = Clock.currTime.toUnixTime * 1000;
    uint salt = cast(uint)(nowMillis) * 2654435761u + ++d7Seq;
    string sid = format("20240101-120000-%04x", salt & 0xFFFFu);
    string hex = sid[16 .. $];
    string ep3;
    foreach (i; 0 .. 12)
        ep3 ~= "episode three turn " ~ i.to!string ~ " line\n";
    string ep7;
    foreach (i; 0 .. 12)
        ep7 ~= "episode seven turn " ~ i.to!string ~ " line\n";
    send(tid, DiJob(sid, [
        DiEpisode("d_20240101_120000_" ~ hex ~ "__t3_3__1000", ep3.idup, 3),
        DiEpisode("d_20240101_120000_" ~ hex ~ "__t7_7__1000", ep7.idup, 7),
    ]));
    send(tid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(30.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain its mailbox");

    // The drain proves the job completed, so the D7 line is already captured.
    // Anchor on this test's quoted session id: parallel worker tests share
    // this global capture and log their own D7 lines (their own sids).
    string[] d7;
    foreach (l; (cast() cap).takeLines())
        if (l.startsWith("dialogue worker: session '" ~ sid ~ "'"))
            d7 ~= l;
    assert(d7.length == 1,
            "expected exactly one D7 trace line per completed job, got %s: %s".format(d7.length,
                d7));
    string line = d7[0];
    assert(line.canFind(sid), "D7 line missing session id: " ~ line);
    assert(line.canFind("2 episodes"), "D7 line missing episode count: " ~ line);
    assert(line.canFind("chunks,"), "D7 line missing chunk count: " ~ line);
    assert(line.canFind("0 failures"), "D7 line missing failure count: " ~ line);
    assert(line.canFind("turns 3-7"), "D7 line missing the 3-7 turn range: " ~ line);

    // No per-chunk spam: several chunks were indexed (verified below) yet
    // exactly one D7 line was emitted.
    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "session DB missing");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;
    long rowCount = -1;
    {
        auto stmt = db.prepare("SELECT count(*) FROM TextChunkTbl;");
        foreach (ref r; stmt.get.execute)
            rowCount = r.peek!long(0);
    }
    assert(rowCount >= 2, "expected >= 2 indexed chunks, got %s".format(rowCount));
}
