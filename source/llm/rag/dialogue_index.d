/// DialogueIndex: per-session dialogue database manager and async indexing coordinator.
///
/// The agent thread owns a single DialogueIndex instance for the process lifetime.
/// It spawns the dialogue worker (Task 4) on its own thread and coordinates:
///   - onCheckpoint: the CheckpointListener that fires on every compression
///     checkpoint, filtering evicted dialogue and enqueuing it for indexing.
///   - query: opens the session DB read-only, dispatches the appropriate search,
///     and post-filters results by episode metadata and turn-age window.
///   - maxTurn / hasHistory: cold-start queries that open the DB read-only.
///   - dispose: best-effort drain of the worker mailbox (idempotent, never throws).
///
/// There is no shared state between the agent thread and the worker thread.
/// All communication is via value messages (DiJob / DiDrain / DiDiDrained / DiDegraded).
module llm.rag.dialogue_index;

import core.time : dur, Duration;
import std.algorithm : max;
import std.concurrency : Tid, spawn, send, receiveTimeout, thisTid, OwnerTerminated;
import std.conv : to;
import std.datetime : SysTime, Clock, DateTime;
import std.exception : collectException;
import std.file : mkdirRecurse;
import std.format : format;
import std.json : JSONType;
import std.range : empty;
import std.string : replace, indexOf, startsWith;
import std.sumtype : match;

import logger = std.logger;

import my.path : AbsolutePath;
import my.optional;

import llm.chat : Chat, Message, ToolMessage, ToolResponse, Role, turnIdOf, dialogueOf;
import llm.common.config : EmbedConfig, RemoteEmbedConfig;
import llm.common.embedder : Embedder, EmbedError, EmbedderFactory;
import llm.config : RagConfig, ToolLimits;
import llm.rag.database : Database, openDatabase, SourceMatch, Search;
import llm.rag.dialogue_worker : DiJob, DiEpisode, DiDrain, DiDrained, DiDegraded, dialogueWorker;
import llm.rag.rag : Origin, Topic;
import llm.session.types : SessionId, isValidId;
import llm.summary_agent : SummaryAgent;
import llm.tool_call : Context;

// ===========================================================================
// DialogueContext interface (Task 6)
// ===========================================================================
//
// Defined here (in llm.rag.dialogue_index) rather than in llm.tool_call to
// avoid a circular import: llm.agent.context imports this module for
// DialogueIndex, and the tool_call modules import llm.agent.context.

interface DialogueContext : Context {
    DialogueIndex getDialogueIndex();
    string currentSessionId();
    ToolLimits getToolLimits() @safe;
}

// Convenience aliases for the nested types
alias CompressionCheckpoint = SummaryAgent.CompressionCheckpoint;
alias CheckpointListener = SummaryAgent.CheckpointListener;

/// Metadata parsed from an episode's topic name.
struct EpisodeMeta {
    string sessionId;
    long turnStart;
    long turnEnd;
    long epochMillis;
}

/// A single dialogue match from a query.
struct DialogueMatch {
    EpisodeMeta episode;
    string text;
    double rank;
}

/// Result of a dialogue query.
///
/// Contract: when `hasHistory` is false, `message` is either a graceful
/// no-history notice (N3 - a success from the tool's perspective) or an
/// engine error whose message starts with "error:" (embed failure, missing
/// embedder). The tool maps the "error:" prefix to `success: false`; all
/// other messages are returned as `success: true`. Keep this prefix
/// convention when adding new failure paths.
// TODO: change the contract to an encoded boolean so "success: true" is not
// tied to what is in message. This reduces the implicit coupling of the text
// content and the status.
struct DialogueQueryResult {
    DialogueMatch[] matches;
    bool hasHistory;
    string message;
}

/// Encode a topic name: d_<sess>__t<turnStart>_<turnEnd>__<epochMillis>
/// Session hyphens are replaced with underscores.
string encodeTopicName(string sessionId, long turnStart, long turnEnd, long epochMillis) @safe {
    string safeSess = replace(sessionId, "-", "_");
    return "d_" ~ safeSess ~ "__t" ~ turnStart.to!string ~ "_"
        ~ turnEnd.to!string ~ "__" ~ epochMillis.to!string;
}

/// Decode a topic name back into its components.
/// Returns none if the name does not match the expected format.
Optional!EpisodeMeta decodeTopicName(string topicName) @safe {
    if (!topicName.startsWith("d_"))
        return none!EpisodeMeta();

    auto rest = topicName[2 .. $]; // strip "d_"
    auto firstDq = indexOf(rest, "__");
    if (firstDq == -1)
        return none!EpisodeMeta();

    string safeSess = rest[0 .. firstDq];
    string sessionId = replace(safeSess, "_", "-");

    auto rest2 = rest[firstDq + 2 .. $];
    auto secondDq = indexOf(rest2, "__");
    if (secondDq == -1)
        return none!EpisodeMeta();

    string turnPart = rest2[0 .. secondDq];
    if (!turnPart.startsWith("t"))
        return none!EpisodeMeta();

    auto us = indexOf(turnPart, "_");
    if (us == -1)
        return none!EpisodeMeta();

    long turnStart;
    long turnEnd;
    try {
        turnStart = turnPart[1 .. us].to!long;
        turnEnd = turnPart[us + 1 .. $].to!long;
    } catch (Exception) {
        return none!EpisodeMeta();
    }

    string epochStr = rest2[secondDq + 2 .. $];
    long epochMillis;
    try {
        epochMillis = epochStr.to!long;
    } catch (Exception) {
        return none!EpisodeMeta();
    }

    return some(EpisodeMeta(sessionId, turnStart, turnEnd, epochMillis));
}

/// Candidate headroom for DB-level queries: the maxTurnAge window and the
/// unparseable-topic filter can drop candidates, so the DB is asked for more
/// than topK and the result is capped AFTER post-filtering (plan 5.2 step 3).
immutable long CandidateHeadroom = 100;

/// F10: true if the entry is a merged compression summary marker.
private bool isSummaryMarker(const Chat.MessageT msg) @safe {
    return msg.match!((const Message m) {
        if (m.saveData.type != JSONType.object)
            return false;
        try {
            return ("summary_turn_start" in m.saveData) !is null;
        } catch (Exception) {
            logger.trace("isSummaryMarker: saveData lookup failed on Message");
        }
        return false;
    }, (const ToolMessage m) {
        if (m.saveData.type != JSONType.object)
            return false;
        try {
            return ("summary_turn_start" in m.saveData) !is null;
        } catch (Exception) {
            logger.trace("isSummaryMarker: saveData lookup failed on ToolMessage");
        }
        return false;
    }, (_) => false);
}

/// Extract the episode text piece from a single dialogue entry.
/// User queries → content, assistant → content, taskDone → final answer.
/// Thinking/reasoning text is excluded.
private string episodePiece(const Chat.MessageT entry) @safe {
    return entry.match!((const Message m) {
        if ((m.role == Role.user && m.isUserQuery) || (m.role == Role.assistant && !m.content.empty)) {
            return m.content;
        }
        return "";
    }, (const ToolMessage m) {
        if (m.isFinalAnswer()) {
            return m.getFinalAnswer();
        }
        return "";
    }, (_) => "");
}

/// Per-session dialogue database manager and async indexing coordinator.
///
/// The agent thread owns one instance for the process lifetime. It spawns the
/// dialogue worker on its own thread (Task 4) and coordinates checkpoint-driven
/// indexing and read-only queries. There is no shared state between threads.
class DialogueIndex {
    private {
        Tid workerTid;
        AbsolutePath dialogueDir;
        EmbedConfig embedConfig;
        RagConfig dialogueRagCfg;
        bool disposed;
    }

    /// Create a DialogueIndex, ensuring the directory exists and spawning the worker.
    /// `embedderFactory` (default null) is passed to the worker thread, which uses
    /// it to create its own embedder instead of consulting the process-global
    /// factory registry; null means the worker resolves the embedder from the
    /// registry (createEmbedder) as before.
    this(AbsolutePath dialogueDir, EmbedConfig embedConfig,
            RagConfig dialogueRagCfg, EmbedderFactory embedderFactory = null) {
        import std.file : exists;

        if (!dialogueDir.toString.exists) {
            try {
                mkdirRecurse(dialogueDir.toString);
            } catch (Exception e) {
                logger.warningf("DialogueIndex: cannot create dir '%s': %s", dialogueDir, e.msg);
            }
        }
        this.dialogueDir = dialogueDir;
        this.embedConfig = embedConfig;
        this.dialogueRagCfg = dialogueRagCfg;
        this.workerTid = spawn(&dialogueWorker, thisTid, dialogueDir,
                embedConfig, dialogueRagCfg, embedderFactory);
        logger.tracef("DialogueIndex: spawned worker for dir '%s'", dialogueDir);
    }

    /// CheckpointListener: fires on every compression checkpoint.
    ///
    /// Refuses empty/invalid session ids (log only). Filters the evicted
    /// dialogue through the A4 classifier (dialogueOf) and the F10 exclusion
    /// (summary markers and turnId==0). Groups by turnId into episodes,
    /// encodes topic names, and sends a DiJob to the worker.
    ///
    /// Must never throw (fire-and-forget; errors are logged and the
    /// compression proceeds). Must return quickly (no I/O, no blocking).
    /// Emits exactly one trace per sent DiJob (D7/N3: the per-checkpoint
    /// counts and turn range are logged by the worker, not here).
    void onCheckpoint(const CompressionCheckpoint cp) nothrow {
        try {
            string sid = cp.sessionId;
            if (sid.empty || !isValidId(SessionId(sid))) {
                logger.warningf("DialogueIndex: refusing checkpoint for invalid session '%s'", sid);
                return;
            }

            // Combine evicted messages and apply the A4 dialogue filter.
            auto combined = cp.evictedSummarized ~ cp.evictedInPlace;
            auto dialogue = dialogueOf(combined);

            // F10: drop summary markers and turnId==0 entries.
            Chat.MessageT[] filtered;
            foreach (entry; dialogue) {
                if (turnIdOf(entry) == 0)
                    continue;
                if (isSummaryMarker(entry))
                    continue;
                filtered ~= entry;
            }

            if (filtered.length == 0)
                return;

            // Group entries by turnId into episodes. Messages of the same turn
            // can be split across the evictedSummarized/evictedInPlace boundary,
            // so pieces are collected per turnId (first-occurrence order) and
            // merged - a split turn must not produce two episodes with the
            // same topic name (the second add would replace the first).
            // Epoch milliseconds (second precision is fine for metadata).
            // NOTE: epochMillis uses Clock.currTime (checkpoint-handling time) rather than
            // cp.timestamp (the stamped eviction time). The two differ by microseconds at
            // most. Known deviation from D2; intentionally not worth fixing (user decision,
            // 2026-09-06 review).
            long epochMillis = Clock.currTime.toUnixTime * 1000;
            long[] turnOrder;
            size_t[long] turnIndex; // turnId -> index in turnOrder
            string[] epTexts;
            foreach (entry; filtered) {
                long tid = turnIdOf(entry);
                string piece = episodePiece(entry);
                if (piece.empty)
                    continue;
                if (auto p = tid in turnIndex) {
                    epTexts[*p] ~= "\n" ~ piece;
                } else {
                    turnIndex[tid] = turnOrder.length;
                    turnOrder ~= tid;
                    epTexts ~= piece;
                }
            }
            DiEpisode[] episodes;
            foreach (i, tid; turnOrder) {
                string topicName = encodeTopicName(sid, tid, tid, epochMillis);
                episodes ~= DiEpisode(topicName.idup, epTexts[i].idup, tid);
            }

            if (episodes.length == 0)
                return;

            // Send to worker (fire-and-forget, non-blocking).
            immutable(DiEpisode[]) immEps = cast(immutable(DiEpisode[])) episodes;
            send(workerTid, DiJob(sid, immEps));
            logger.tracef("DialogueIndex: sent %s episodes for session '%s'", episodes.length, sid);
        } catch (Exception e) {
            logger.errorf("checkpoint failure: %s", e.msg).collectException;
        }
    }

    /// Query the session's dialogue DB read-only.
    ///
    /// Dispatches: queryCombineSemanticText (both), queryTextSearch (text only),
    /// querySemantic (vector only). Post-filters: parses the Topic origin into
    /// EpisodeMeta, drops unparseable topics, applies the maxTurnAge window
    /// (0 or negative: no age filtering; a positive N keeps matches with
    /// turnEnd >= maxTurn - N, where maxTurn is the newest indexed turnEnd),
    /// and caps at topK.
    ///
    /// `qEmbedder` is the AGENT's RAG embedder (never the worker's): it supplies
    /// the model name/dimensions for the read-only DB open (matching the values
    /// the worker wrote) and embeds `vectorQuery`. On EmbedError the query
    /// falls back to text-only when `textQuery` is present, else returns a
    /// graceful error result. `topK` is clamped to >= 1 (never a crash source).
    ///
    /// Never throws: DB open failure → no-history result; parse failures →
    /// entry dropped. An invalid session id (path-traversal guard, defense in
    /// depth on top of the tool-level validation) is treated as no-history.
    DialogueQueryResult query(Embedder qEmbedder, SessionId sessionId,
            string textQuery, string vectorQuery, long topK = 10, long maxTurnAge = 0) {
        if (qEmbedder is null) {
            logger.warning("DialogueIndex: query without an embedder");
            return DialogueQueryResult(null, false, "error: dialogue query requires an embedder");
        }
        if (!isValidId(sessionId)) {
            logger.warningf("DialogueIndex: refusing query for invalid session '%s'",
                    sessionId.to!string);
            // Defense in depth: the tool validates first, so this is
            // unreachable from queryDialogueHistory. Returned as a no-history
            // notice (not an "error:" message) on purpose - an invalid id
            // must never leak into a DB path.
            return DialogueQueryResult(null, false,
                    "No dialogue history indexed for this session yet.");
        }
        if (topK < 1)
            topK = 1; // clamp: negative/zero topK must never slice with a bad bound
        auto dbOpt = openDatabase((dialogueDir ~ (sessionId.to!string ~ ".db"))
                .AbsolutePath, qEmbedder.modelName(), qEmbedder.dimensions(), readOnly: true);

        if (!hasValue(dbOpt)) {
            return DialogueQueryResult(null, false,
                    "No dialogue history indexed for this session yet.");
        }

        auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
        scope (exit)
            db.destroy();

        // Check if the DB has any sources
        size_t sourceCount;
        try {
            sourceCount = db.getSources.length;
        } catch (Exception e) {
            logger.warningf("getSources failed: %s", e.msg);
            return DialogueQueryResult(null, false,
                    "No dialogue history indexed for this session yet.");
        }
        if (sourceCount == 0) {
            return DialogueQueryResult(null, false,
                    "No dialogue history indexed for this session yet.");
        }

        // Embed the vector query with the caller-supplied (agent) embedder.
        float[] embedded;
        bool embedFailed;
        string embedError;
        if (!vectorQuery.empty) {
            qEmbedder.embedQuery(vectorQuery).match!((float[] v) { embedded = v; }, (EmbedError e) {
                embedFailed = true;
                embedError = e.errorMsg;
                logger.tracef("DialogueIndex.query: embed failed: %s", e.errorMsg);
            });
        }

        // Query the DB with candidate headroom: the maxTurnAge window and the
        // unparseable-topic filter can drop candidates, so topK is applied
        // AFTER post-filtering (plan 5.2 step 3), not by the DB LIMIT alone.
        long dbLimit = max(topK * 10, CandidateHeadroom);

        // Dispatch the appropriate search
        SourceMatch[] raw;
        if (!textQuery.empty && !embedFailed && !vectorQuery.empty) {
            raw = db.queryCombineSemanticText(Search(embedded), textQuery, dbLimit);
        } else if (!textQuery.empty) {
            raw = db.queryTextSearch(textQuery, dbLimit);
        } else if (!embedFailed && !vectorQuery.empty) {
            raw = db.querySemantic(Search(embedded), dbLimit);
        } else if (!vectorQuery.empty) {
            return DialogueQueryResult(null, false,
                    "error: could not embed vectorQuery: " ~ embedError);
        } else {
            return DialogueQueryResult(null, false, "No query provided.");
        }

        // Compute maxTurn for the age window
        long maxTurn = 0;
        if (maxTurnAge > 0) {
            maxTurn = computeMaxTurn(db);
        }

        // Post-filter: parse topic, apply age window
        DialogueMatch[] matches;
        foreach (sm; raw) {
            auto origin = sm.origin;
            auto metaOpt = origin.match!((Topic t) => decodeTopicName(t.name),
                    (_) => none!EpisodeMeta());

            if (!hasValue(metaOpt))
                continue; // drop unparseable topics

            auto meta = metaOpt.match!((EpisodeMeta m) => m, (None _) => EpisodeMeta.init);

            // Apply maxTurnAge window
            if (maxTurnAge > 0 && maxTurn > 0) {
                if (meta.turnEnd < maxTurn - maxTurnAge)
                    continue;
            }

            matches ~= DialogueMatch(meta, sm.text, sm.rank);
        }

        // Cap at topK (after filtering)
        if (matches.length > topK)
            matches = matches[0 .. topK];

        return DialogueQueryResult(matches, true, "");
    }

    /// Returns the highest indexed turnEnd for a session (0 if none).
    ///
    /// Opens the session DB read-only (model/dimensions come from `qEmbedder`,
    /// the agent's RAG embedder - the values the worker wrote), parses the
    /// Topic names from the sources table, and returns the max turnEnd. Works
    /// after process restart (cold start) because it reads the DB, not memory.
    /// Never throws: missing DB or corrupt DB → 0.
    long maxTurn(Embedder qEmbedder, SessionId sessionId) {
        if (qEmbedder is null || !isValidId(sessionId))
            return 0;
        auto dbOpt = openDatabase((dialogueDir ~ (sessionId.to!string ~ ".db"))
                .AbsolutePath, qEmbedder.modelName(), qEmbedder.dimensions(), readOnly: true);
        if (!hasValue(dbOpt))
            return 0;
        auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
        scope (exit)
            db.destroy();
        return computeMaxTurn(db);
    }

    /// Returns whether the session's DB has at least one source.
    ///
    /// Never throws: missing DB or corrupt DB → false.
    bool hasHistory(Embedder qEmbedder, SessionId sessionId) {
        if (qEmbedder is null || !isValidId(sessionId))
            return false;
        auto dbOpt = openDatabase((dialogueDir ~ (sessionId.to!string ~ ".db"))
                .AbsolutePath, qEmbedder.modelName(), qEmbedder.dimensions(), readOnly: true);
        if (!hasValue(dbOpt))
            return false;
        auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
        scope (exit)
            db.destroy();
        try {
            return db.getSources.length > 0;
        } catch (Exception e) {
            logger.warningf("hasHistory failed: %s", e.msg);
            return false;
        }
    }

    /// Best-effort drain of the worker mailbox. Idempotent (guarded by a flag).
    /// Never throws (catches OwnerTerminated). Returns without waiting if the
    /// worker is already gone. After the wait, consumes any DiDrained that may
    /// have arrived late (e.g. after a timeout) so it cannot be mistaken for a
    /// future drain's reply by a later consumer of this thread's mailbox.
    void dispose() {
        if (disposed)
            return;
        disposed = true;
        try {
            send(workerTid, DiDrain(thisTid));
            receiveTimeout(5.dur!"seconds", (DiDrained _) {});
            // Consume a possible late/duplicate DiDrained (zero-time poll).
            receiveTimeout(Duration.zero, (DiDrained _) {});
        } catch (OwnerTerminated) {
            logger.trace("DialogueIndex.dispose: worker already terminated");
        } catch (Exception e) {
            logger.warningf("DialogueIndex.dispose: error: %s", e.msg);
        }
    }

    /// Compute the max turnEnd from a database's sources. Never throws:
    /// a corrupt DB (getSources failure) yields 0.
    private static long computeMaxTurn(ref Database db) nothrow {
        try {
            long mt = 0;
            foreach (src; db.getSources) {
                src.origin.match!((Topic t) {
                    auto metaOpt = decodeTopicName(t.name);
                    if (hasValue(metaOpt)) {
                        auto m = metaOpt.match!((EpisodeMeta x) => x, (None _) => EpisodeMeta.init);
                        if (m.turnEnd > mt)
                            mt = m.turnEnd;
                    }
                }, (_) {});
            }
            return mt;
        } catch (Exception e) {
            logger.warningf("computeMaxTurn failed: %s", e.msg).collectException;
            return 0;
        }
    }
}

version (unittest) {
    import std.file : exists;
    import std.json : JSONValue;
    import std.path : buildPath;
    import core.thread : Thread;
    import llm.common.embedder : Embedder, EmbedResult, EmbedError;
    import llm.common.config : ServerConfig;
    import llm.test_util : TestArea, testArea;

    /// Deterministic test embedder: returns an all-ones 8-dim vector.
    private class DiTestEmbedder : Embedder {
        override void destroy() {
        }

        override string modelName() {
            return "di-test";
        }

        override long dimensions() {
            return 8;
        }

        override bool supportsTokenization() {
            return false;
        }

        override EmbedResult embedQuery(string text) {
            return embed(text);
        }

        override EmbedResult embedDocument(string text) {
            return embed(text);
        }

        EmbedResult embed(string text) {
            auto v = new float[8];
            foreach (ref f; v)
                f = 1.0f;
            return EmbedResult(v);
        }

        override EmbedResult embedQuery(int[] tokens) {
            return embed(tokens);
        }

        override EmbedResult embedDocument(int[] tokens) {
            return embed(tokens);
        }

        EmbedResult embed(int[] tokens) {
            return EmbedResult(EmbedError("no tokens"));
        }

        override int[] tokenize(string text) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            return 64;
        }
    }

    /// Factory that injects the test embedder into the worker thread (instead
    /// of the process-global factory registry, which parallel tests race on).
    private Embedder diTestFactory(EmbedConfig config) {
        return new DiTestEmbedder();
    }

    /// Helper: create a temp dir and return config.
    private struct TestSetup {
        TestArea tmpDir;
        EmbedConfig cfg;
        RagConfig ragCfg;
    }

    private TestSetup setupTest(string testName, string file = __FILE__, uint line = __LINE__) {
        auto tmpDir = testArea(testName, file, line);

        auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
                modelName: "di-test", dimensions: 8));
        auto ragCfg = RagConfig(windowOverlapPercent: 10);

        return TestSetup(tmpDir, cfg, ragCfg);
    }

    private void teardownTest(ref TestSetup s) {
        s.tmpDir.cleanup;
    }

    /// Query-path embedder: modelName/dimensions must match the worker's
    /// DiTestEmbedder so read-only DB opens succeed (Issue 2 review fix).
    private DiTestEmbedder qEmb() {
        return new DiTestEmbedder();
    }
}

// ------------------------------------------------------------------
// Codec tests
// ------------------------------------------------------------------

unittest {
    // Round-trip: encode then decode
    auto encoded = encodeTopicName("20240101-120000-abcd", 5, 5, 1700000000000);
    assert(encoded == "d_20240101_120000_abcd__t5_5__1700000000000", "unexpected encode: " ~ encoded);

    auto decoded = decodeTopicName(encoded);
    assert(hasValue(decoded));
    auto decMeta = decoded.match!((EpisodeMeta m) => m, (None _) => EpisodeMeta.init);
    assert(decMeta.sessionId == "20240101-120000-abcd");
    assert(decMeta.turnStart == 5);
    assert(decMeta.turnEnd == 5);
    assert(decMeta.epochMillis == 1700000000000);
}

unittest {
    // Decode rejects malformed names
    assert(!hasValue(decodeTopicName("")));
    assert(!hasValue(decodeTopicName("no_prefix")));
    assert(!hasValue(decodeTopicName("d_")));
    assert(!hasValue(decodeTopicName("d_s__t__")));
    assert(!hasValue(decodeTopicName("d_s__t5_5__"))); // missing epoch
    assert(!hasValue(decodeTopicName("d_s__x5_5__123"))); // missing 't' prefix
}

unittest {
    // Multiple round-trips with different values
    foreach (i; 1 .. 20) {
        auto sid = format("20240101-%06d-%04x", i, i * 7);
        auto ts = i * 10;
        auto te = i * 10;
        auto epoch = 1700000000000L + i;
        auto enc = encodeTopicName(sid, ts, te, epoch);
        auto dec = decodeTopicName(enc);
        assert(hasValue(dec), "decode failed for: " ~ enc);
        auto dm = dec.match!((EpisodeMeta m) => m, (None _) => EpisodeMeta.init);
        assert(dm.sessionId == sid);
        assert(dm.turnStart == ts);
        assert(dm.turnEnd == te);
        assert(dm.epochMillis == epoch);
    }
}

// ------------------------------------------------------------------
// onCheckpoint rejection tests
// ------------------------------------------------------------------

unittest {
    auto s = setupTest("on_checkpoint_rejection_test");
    scope (exit)
        teardownTest(s);

    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
    scope (exit)
        di.dispose;
    Thread.sleep(100.dur!"msecs"); // let worker start

    // Empty session ID → refused
    auto cp1 = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: "",
            evictedSummarized: [
                Chat.MessageT(Message(Role.user, true, "test", ""))
    ], evictedPurged: null, evictedInPlace: null, turnStart: 1, turnEnd: 1,
            summaryText: "", originalLength: 1, newLength: 0, newContextSize: 0);
    di.onCheckpoint(cp1); // must not throw

    // Invalid session IDs → refused (path-traversal guard, plan cases)
    foreach (bad; [
        "invalid", "../etc/passwd", "not-an-id", "20240101-120000-zzzz"
    ]) {
        auto cp2 = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: bad,
                evictedSummarized: [
                    Chat.MessageT(Message(Role.user, true, "test", ""))
        ], evictedPurged: null, evictedInPlace: null, turnStart: 1, turnEnd: 1,
                summaryText: "", originalLength: 1, newLength: 0, newContextSize: 0);
        di.onCheckpoint(cp2); // must not throw
    }

    // Verify no DB was created for invalid sessions: hasHistory is false
    // for a never-indexed valid-format id, and no DB file exists for a
    // refused id (the path-traversal guard must prevent file creation).
    assert(!di.hasHistory(qEmb(), SessionId("20240101-120000-0000")),
            "invalid session should not have history");
    assert(!(s.tmpDir ~ "not-an-id.db").toString.exists,
            "refused session must not create a DB file");
    assert(!(s.tmpDir ~ "invalid.db").toString.exists, "refused session must not create a DB file");
}

// ------------------------------------------------------------------
// F10 filter test (synchronous: verify grouping logic)
// ------------------------------------------------------------------

unittest {
    // Summary marker: assistant message with summary_turn_start in saveData
    JSONValue sd1;
    sd1["summary_turn_start"] = 3;
    auto summaryMsg = Message(Role.assistant, false, "Summary of turns 1-3",
            "", JSONValue.init, sd1);
    summaryMsg.turnId = 3;
    assert(isSummaryMarker(Chat.MessageT(summaryMsg)), "expected summary marker to be detected");

    // Normal assistant message: no summary marker
    auto normalMsg = Message(Role.assistant, false, "Hello!", "");
    normalMsg.turnId = 5;
    assert(!isSummaryMarker(Chat.MessageT(normalMsg)),
            "normal message should not be a summary marker");

    // ToolMessage with summary marker
    JSONValue sd2;
    sd2["summary_turn_start"] = 1;
    auto toolMsg = ToolMessage("", JSONValue(JSONType.array), JSONValue.init, sd2);
    toolMsg.turnId = 1;
    assert(isSummaryMarker(Chat.MessageT(toolMsg)),
            "tool message with summary marker should be detected");
}

// ------------------------------------------------------------------
// End-to-end: onCheckpoint → worker indexes → query returns results
// ------------------------------------------------------------------

unittest {
    auto s = setupTest("on_checkpoint_end_to_end_with_worker_indexes");
    scope (exit)
        teardownTest(s);

    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
    scope (exit)
        di.dispose;
    Thread.sleep(100.dur!"msecs"); // let worker start

    string sid = "20240101-120000-abcd";

    // Build a checkpoint with one turn of dialogue
    auto userMsg = Message(Role.user, true, "What is the capital of France?", "");
    userMsg.turnId = 1;
    auto asstMsg = Message(Role.assistant, false, "The capital of France is Paris.", "");
    asstMsg.turnId = 1;

    auto cp = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: sid,
            evictedSummarized: [Chat.MessageT(userMsg), Chat.MessageT(asstMsg)],
            evictedPurged: null, evictedInPlace: null,
            turnStart: 1, turnEnd: 1, summaryText: "", originalLength: 2,
            newLength: 0, newContextSize: 0);

    di.onCheckpoint(cp);

    // Drain the worker to ensure the job is processed
    send(di.workerTid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(10.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain");

    // Verify hasHistory
    assert(di.hasHistory(qEmb(), SessionId(sid)), "session should have history after indexing");

    // Verify maxTurn
    assert(di.maxTurn(qEmb(), SessionId(sid)) == 1, "maxTurn should be 1");

    // Query with text search
    auto result = di.query(qEmb(), SessionId(sid), "capital France", "");
    assert(result.hasHistory, "query should find history");
    assert(result.matches.length > 0, "query should return matches");
    assert(result.matches[0].episode.turnEnd == 1, "match should have turnEnd 1");
    assert(result.matches[0].text.length > 0, "match should have text");
}

// ------------------------------------------------------------------
// maxTurnAge window test
// ------------------------------------------------------------------

unittest {
    auto s = setupTest("max_turn_age_window");
    scope (exit)
        teardownTest(s);

    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
    scope (exit)
        di.dispose;
    Thread.sleep(100.dur!"msecs");

    string sid = "20240101-120000-beef";

    // Index turn 1
    auto u1 = Message(Role.user, true, "First question alpha", "");
    u1.turnId = 1;
    auto a1 = Message(Role.assistant, false, "First answer beta", "");
    a1.turnId = 1;

    auto cp1 = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: sid,
            evictedSummarized: [Chat.MessageT(u1), Chat.MessageT(a1)],
            evictedPurged: null, evictedInPlace: null, turnStart: 1, turnEnd: 1,
            summaryText: "", originalLength: 2, newLength: 0, newContextSize: 0);
    di.onCheckpoint(cp1);

    // Index turn 10
    auto u10 = Message(Role.user, true, "Second question gamma", "");
    u10.turnId = 10;
    auto a10 = Message(Role.assistant, false, "Second answer delta", "");
    a10.turnId = 10;

    auto cp10 = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: sid,
            evictedSummarized: [Chat.MessageT(u10), Chat.MessageT(a10)],
            evictedPurged: null, evictedInPlace: null, turnStart: 10, turnEnd: 10,
            summaryText: "", originalLength: 2, newLength: 0, newContextSize: 0);
    di.onCheckpoint(cp10);

    // Drain
    send(di.workerTid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(10.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain");

    // maxTurn should be 10
    assert(di.maxTurn(qEmb(), SessionId(sid)) == 10,
            "maxTurn should be 10, got " ~ di.maxTurn(qEmb(), SessionId(sid)).to!string);

    // Query with maxTurnAge=5: should only include turn 10 (10 >= 10-5=5),
    // exclude turn 1 (1 < 5)
    auto result = di.query(qEmb(), SessionId(sid), "question", "", topK: 10, maxTurnAge: 5);
    assert(result.hasHistory);
    foreach (m; result.matches) {
        assert(m.episode.turnEnd >= 5,
                "match turnEnd " ~ m.episode.turnEnd.to!string
                ~ " should be >= 5 with maxTurnAge=5");
    }

    // Query with maxTurnAge=20: should include both turns
    auto result2 = di.query(qEmb(), SessionId(sid), "question", "", topK: 10, maxTurnAge: 20);
    assert(result2.hasHistory);
    bool foundTurn10 = false;
    foreach (m; result2.matches) {
        if (m.episode.turnEnd == 10)
            foundTurn10 = true;
    }
    assert(foundTurn10, "should find turn 10");
}

// ------------------------------------------------------------------
// No-history: query on a never-indexed session
// ------------------------------------------------------------------

unittest {
    auto s = setupTest("no_history");
    scope (exit)
        teardownTest(s);

    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
    scope (exit)
        di.dispose;
    Thread.sleep(100.dur!"msecs");

    string sid = "20240615-083000-1234"; // valid format, never indexed

    // hasHistory should be false
    assert(!di.hasHistory(qEmb(), SessionId(sid)), "never-indexed session should have no history");

    // maxTurn should be 0
    assert(di.maxTurn(qEmb(), SessionId(sid)) == 0, "never-indexed session should have maxTurn 0");

    // query should return no-history gracefully (no exception)
    auto result = di.query(qEmb(), SessionId(sid), "anything", "");
    assert(!result.hasHistory, "query on empty session should report no history");
    assert(result.message.length > 0, "should have an explanatory message");
    assert(result.matches.length == 0, "should have no matches");
}

// ------------------------------------------------------------------
// onCheckpoint with evictedInPlace (combined with evictedSummarized)
// ------------------------------------------------------------------

unittest {
    auto s = setupTest("on_checkpoint_eviced_in_place");
    scope (exit)
        teardownTest(s);

    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
    scope (exit)
        di.dispose;
    Thread.sleep(100.dur!"msecs");

    string sid = "20240301-090000-cafe";

    // Turn 1 in evictedSummarized, turn 2 in evictedInPlace
    auto u1 = Message(Role.user, true, "Question one", "");
    u1.turnId = 1;
    auto a1 = Message(Role.assistant, false, "Answer one", "");
    a1.turnId = 1;

    auto u2 = Message(Role.user, true, "Question two", "");
    u2.turnId = 2;
    auto a2 = Message(Role.assistant, false, "Answer two", "");
    a2.turnId = 2;

    auto cp = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: sid,
            evictedSummarized: [Chat.MessageT(u1), Chat.MessageT(a1)], evictedPurged: null, evictedInPlace: [
                Chat.MessageT(u2), Chat.MessageT(a2)
    ], turnStart: 1, turnEnd: 2, summaryText: "", originalLength: 4, newLength: 0,
            newContextSize: 0);

    di.onCheckpoint(cp);

    // Drain
    send(di.workerTid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(10.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain");

    // Both turns should be indexed
    assert(di.maxTurn(qEmb(), SessionId(sid)) == 2, "maxTurn should be 2");

    // Query should find both
    auto result = di.query(qEmb(), SessionId(sid), "question", "", topK: 10);
    assert(result.hasHistory);
    assert(result.matches.length >= 1, "should find at least one match");
}

// ------------------------------------------------------------------
// onCheckpoint excludes harness traffic (non-userQuery user messages)
// ------------------------------------------------------------------

unittest {
    auto s = setupTest("on_checkpoint_excludes_harness_traffic");
    scope (exit)
        teardownTest(s);

    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
    scope (exit)
        di.dispose;
    Thread.sleep(100.dur!"msecs");

    string sid = "20240401-100000-dead";

    // A harness nudge (user role but NOT userQuery) should be excluded by A4
    auto nudge = Message(Role.user, false, "System nudge continue", "");
    nudge.turnId = 1;

    // A real user query
    auto uq = Message(Role.user, true, "Real question here", "");
    uq.turnId = 1;

    // Assistant response
    auto ar = Message(Role.assistant, false, "Real answer here", "");
    ar.turnId = 1;

    auto cp = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: sid,
            evictedSummarized: [
                Chat.MessageT(nudge), Chat.MessageT(uq), Chat.MessageT(ar)
    ], evictedPurged: null, evictedInPlace: null, turnStart: 1, turnEnd: 1,
            summaryText: "", originalLength: 3, newLength: 0, newContextSize: 0);

    di.onCheckpoint(cp);

    // Drain
    send(di.workerTid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(10.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained);

    // The nudge text should NOT appear in the indexed content
    auto result = di.query(qEmb(), SessionId(sid), "nudge", "");
    // The nudge was excluded by A4, so "nudge" should not be found
    if (result.hasHistory && result.matches.length > 0) {
        foreach (m; result.matches) {
            assert(indexOf(m.text, "System nudge") == size_t.max,
                    "harness nudge should not be indexed");
        }
    }
}

// ------------------------------------------------------------------
// Episode grouping: taskDone included, thinking excluded, empty episode
// skipped, non-final ToolMessage and harness nudge excluded.
// ------------------------------------------------------------------

unittest {
    auto s = setupTest("episode_grouping");
    scope (exit)
        teardownTest(s);

    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
    scope (exit)
        di.dispose;
    Thread.sleep(100.dur!"msecs");

    string sid = "20240501-110000-face";

    // Turn 1: user query + assistant answer (with thinking) + taskDone.
    auto u1 = Message(Role.user, true, "Question one alpha", "");
    u1.turnId = 1;
    auto a1 = Message(Role.assistant, false, "Answer one beta", "HIDDEN REASONING gamma");
    a1.turnId = 1;
    JSONValue sd;
    sd["taskDoneAnswer"] = "FINAL ANSWER delta 777";
    auto td = ToolMessage("", JSONValue(JSONType.array), JSONValue.init, sd);
    td.turnId = 1;
    // Non-final ToolMessage (tool call, no taskDoneAnswer) - excluded by A4.
    auto tool = ToolMessage("", JSONValue(JSONType.array));
    tool.turnId = 1;
    // Harness nudge (user role, NOT userQuery) - excluded by A4.
    auto nudge = Message(Role.user, false, "NUDGE TEXT epsilon", "");
    nudge.turnId = 1;
    // Turn 2: user query with EMPTY content - episode text empty, skipped.
    auto u2 = Message(Role.user, true, "", "");
    u2.turnId = 2;

    auto cp = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: sid,
            evictedSummarized: [
                Chat.MessageT(nudge), Chat.MessageT(u1), Chat.MessageT(a1),
                Chat.MessageT(tool), Chat.MessageT(td), Chat.MessageT(u2)
    ], evictedPurged: null, evictedInPlace: null, turnStart: 1, turnEnd: 2,
            summaryText: "", originalLength: 6, newLength: 0, newContextSize: 0);
    di.onCheckpoint(cp);

    send(di.workerTid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(10.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain");

    // Turn 1's episode carries user query + assistant answer + taskDone
    // final answer in one verbatim chunk.
    auto res = di.query(qEmb(), SessionId(sid), "delta", "", topK: 10);
    assert(res.hasHistory);
    assert(res.matches.length > 0, "taskDone final answer must be indexed");
    assert(indexOf(res.matches[0].text, "Question one alpha") != size_t.max,
            "user query must be in the episode");
    assert(indexOf(res.matches[0].text, "Answer one beta") != size_t.max,
            "assistant answer must be in the episode");
    assert(indexOf(res.matches[0].text, "FINAL ANSWER delta 777") != size_t.max,
            "taskDone final answer must be in the episode");

    // Thinking text is never verbatim-searchable.
    auto think = di.query(qEmb(), SessionId(sid), "gamma", "");
    assert(think.matches.length == 0,
            "thinking text must not be indexed (got %s matches)".format(think.matches.length));

    // Harness nudge is not indexed (A4).
    auto nud = di.query(qEmb(), SessionId(sid), "epsilon", "");
    assert(nud.matches.length == 0,
            "harness nudge must not be indexed (got %s matches)".format(nud.matches.length));

    // The empty turn-2 episode was skipped: only turn 1 is indexed.
    assert(di.maxTurn(qEmb(), SessionId(sid)) == 1,
            "empty episode must be skipped, maxTurn should be 1");
}

// ------------------------------------------------------------------
// F10 end-to-end: summary markers and turnId==0 entries are NOT indexed;
// a genuine adjacent turn in the same checkpoint IS indexed.
// ------------------------------------------------------------------

unittest {
    auto s = setupTest("end_to_end_summary_markers");
    scope (exit)
        teardownTest(s);

    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
    scope (exit)
        di.dispose;
    Thread.sleep(100.dur!"msecs");

    string sid = "20240502-120000-beef";

    // (a) merged-summary-shaped entry: assistant Message with content and
    //     summary_turn_start/end markers (the buildMergedSummary shape).
    JSONValue sd;
    sd["summary_turn_start"] = 1;
    sd["summary_turn_end"] = 3;
    auto summaryMsg = Message(Role.assistant, false,
            "Summary zebra marker text", "", JSONValue.init, sd);
    summaryMsg.turnId = 3;

    // (b) legacy unstamped entry: userQuery true but turnId == 0.
    auto legacyMsg = Message(Role.user, true, "Legacy quokka unstamped text", "");
    legacyMsg.turnId = 0;

    // (c) genuine adjacent turn.
    auto u5 = Message(Role.user, true, "Genuine alpha turn text", "");
    u5.turnId = 5;
    auto a5 = Message(Role.assistant, false, "Genuine beta answer", "");
    a5.turnId = 5;

    auto cp = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: sid,
            evictedSummarized: [
                Chat.MessageT(summaryMsg), Chat.MessageT(legacyMsg),
                Chat.MessageT(u5), Chat.MessageT(a5)
    ], evictedPurged: null, evictedInPlace: null, turnStart: 1, turnEnd: 5,
            summaryText: "", originalLength: 4, newLength: 0, newContextSize: 0);
    di.onCheckpoint(cp);

    send(di.workerTid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(10.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain");

    // The summary marker text must not be queryable (F10).
    auto sm = di.query(qEmb(), SessionId(sid), "zebra", "");
    assert(sm.matches.length == 0,
            "merged summary must not be indexed (F10), got %s matches".format(sm.matches.length));
    // The legacy unstamped text must not be queryable (F10).
    auto lg = di.query(qEmb(), SessionId(sid), "quokka", "");
    assert(lg.matches.length == 0,
            "legacy turnId==0 entry must not be indexed (F10), got %s matches".format(
                lg.matches.length));
    // The genuine adjacent turn IS indexed.
    auto g = di.query(qEmb(), SessionId(sid), "alpha", "");
    assert(g.hasHistory && g.matches.length > 0, "genuine adjacent turn must be indexed");
    assert(g.matches[0].episode.turnEnd == 5, "genuine turn must carry turnEnd 5");
    assert(di.maxTurn(qEmb(), SessionId(sid)) == 5,
            "maxTurn must reflect only the genuine turn (F10)");
}

version (unittest) {
    // ------------------------------------------------------------------
    // onCheckpoint is asynchronous: it enqueues and returns immediately even
    // when the worker's embedder is slow (no inline embedding).
    // ------------------------------------------------------------------

    /// Embedder that sleeps per embed; proves onCheckpoint never embeds inline.
    private class SlowDiEmbedder : Embedder {
        override void destroy() {
        }

        override string modelName() {
            // Must match the query-path embedder (qEmb) values so the
            // read-only DB open succeeds - in production the worker and the
            // agent embedder come from the same EmbedConfig/factory.
            return "di-test";
        }

        override long dimensions() {
            return 8;
        }

        override bool supportsTokenization() {
            return false;
        }

        override EmbedResult embedQuery(string text) {
            return embed(text);
        }

        override EmbedResult embedDocument(string text) {
            return embed(text);
        }

        EmbedResult embed(string text) {
            Thread.sleep(200.dur!"msecs");
            auto v = new float[8];
            foreach (ref f; v)
                f = 1.0f;
            return EmbedResult(v);
        }

        override EmbedResult embedQuery(int[] tokens) {
            return embed(tokens);
        }

        override EmbedResult embedDocument(int[] tokens) {
            return embed(tokens);
        }

        EmbedResult embed(int[] tokens) {
            return EmbedResult(EmbedError("no tokens"));
        }

        override int[] tokenize(string text) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            return 64;
        }
    }

    /// Factory that injects the slow embedder into the worker thread (instead
    /// of the process-global factory registry).
    private Embedder slowDiFactory(EmbedConfig config) {
        return new SlowDiEmbedder();
    }

    /// Embedder whose embed(string) always fails; used to verify the graceful
    /// embed-error path and the text-only fallback (review Issue 2).
    private class FailEmbedder : Embedder {
        override void destroy() {
        }

        override string modelName() {
            return "di-test"; // must match the worker's DB values
        }

        override long dimensions() {
            return 8;
        }

        override bool supportsTokenization() {
            return false;
        }

        override EmbedResult embedQuery(string text) {
            return embed(text);
        }

        override EmbedResult embedDocument(string text) {
            return embed(text);
        }

        EmbedResult embed(string text) {
            return EmbedResult(EmbedError("test embed failure"));
        }

        override EmbedResult embedQuery(int[] tokens) {
            return embed(tokens);
        }

        override EmbedResult embedDocument(int[] tokens) {
            return embed(tokens);
        }

        EmbedResult embed(int[] tokens) {
            return EmbedResult(EmbedError("test embed failure"));
        }

        override int[] tokenize(string text) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            return 64;
        }
    }
}

unittest {
    import std.datetime.stopwatch : StopWatch;

    auto tmpDir = testArea("dialogue_slow");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "di-test", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);
    auto di = new DialogueIndex(tmpDir, cfg, ragCfg, &slowDiFactory);
    scope (exit)
        di.dispose;
    Thread.sleep(100.dur!"msecs");

    string sid = "20240503-130000-dead";
    auto u = Message(Role.user, true, "Slow embedder async question", "");
    u.turnId = 1;
    auto cp = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: sid, evictedSummarized: [
        Chat.MessageT(u)
    ], evictedPurged: null, evictedInPlace: null, turnStart: 1, turnEnd: 1,
        summaryText: "", originalLength: 1, newLength: 0, newContextSize: 0);

    StopWatch sw;
    sw.start();
    di.onCheckpoint(cp);
    sw.stop();
    auto elapsed = sw.peek.total!"msecs";
    assert(elapsed < 100,
            "onCheckpoint must enqueue and return, not embed inline (took %s ms)".format(elapsed));

    // The job is still processed asynchronously: drain and verify.
    send(di.workerTid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(10.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain");
    auto res = di.query(qEmb(), SessionId(sid), "async", "");
    assert(res.hasHistory && res.matches.length > 0,
            "async-enqueued episode must be indexed after drain");
}

@("maxTurn cold start: a second DialogueIndex instance on the same dir (simulated restart) derives maxTurn from the DB, not from memory")
unittest {
    auto s = setupTest("cold_start_max_turn");
    scope (exit)
        teardownTest(s);

    string sid = "20240504-140000-1234";

    // Instance A: index turns 1 and 2, then drain and dispose.
    {
        auto diA = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
        scope (exit)
            diA.dispose;
        Thread.sleep(100.dur!"msecs");

        auto u1 = Message(Role.user, true, "Cold question one", "");
        u1.turnId = 1;
        auto u2 = Message(Role.user, true, "Cold question two", "");
        u2.turnId = 2;
        auto cp = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: sid,
                evictedSummarized: [Chat.MessageT(u1), Chat.MessageT(u2)],
                evictedPurged: null, evictedInPlace: null, turnStart: 1, turnEnd: 2,
                summaryText: "", originalLength: 2, newLength: 0, newContextSize: 0);
        diA.onCheckpoint(cp);

        send(diA.workerTid, DiDrain(thisTid));
        bool drained = false;
        receiveTimeout(10.dur!"seconds", (DiDrained _) { drained = true; });
        assert(drained, "worker A did not drain");
    }

    // Instance B on the same dir: simulated restart.
    auto diB = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
    scope (exit)
        diB.dispose;
    Thread.sleep(100.dur!"msecs");

    assert(diB.maxTurn(qEmb(), SessionId(sid)) == 2,
            "cold-start maxTurn must be DB-derived (got %s)".format(diB.maxTurn(qEmb(),
                SessionId(sid))));
    assert(diB.hasHistory(qEmb(), SessionId(sid)), "cold-start hasHistory must be true");
    auto res = diB.query(qEmb(), SessionId(sid), "two", "");
    assert(res.hasHistory && res.matches.length > 0, "cold-start query must find indexed dialogue");
}

@("All three dispatch paths: text-only, vector-only, combined.")
unittest {
    auto s = setupTest("all_three_dispatch_paths");
    scope (exit)
        teardownTest(s);

    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
    scope (exit)
        di.dispose;
    Thread.sleep(100.dur!"msecs");

    string sid = "20240505-150000-7777";
    auto u = Message(Role.user, true, "The quick brown fox jumps over the lazy dog", "");
    u.turnId = 1;
    auto cp = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: sid, evictedSummarized: [
        Chat.MessageT(u)
    ], evictedPurged: null, evictedInPlace: null, turnStart: 1, turnEnd: 1,
        summaryText: "", originalLength: 1, newLength: 0, newContextSize: 0);
    di.onCheckpoint(cp);

    send(di.workerTid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(10.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain");

    // Vector-only dispatch (querySemantic): the component embeds the
    // vectorQuery with the caller-supplied query-path embedder.
    auto v = di.query(qEmb(), SessionId(sid), "", "lazy dog", topK: 10);
    assert(v.hasHistory && v.matches.length > 0, "vector-only query must return matches");
    assert(indexOf(v.matches[0].text, "quick") != size_t.max,
            "vector-only match must carry the verbatim text");

    // Combined dispatch (queryCombineSemanticText).
    auto c = di.query(qEmb(), SessionId(sid), "lazy", "quick brown fox", topK: 10);
    assert(c.hasHistory && c.matches.length > 0, "combined query must return matches");
    assert(c.matches[0].episode.turnEnd == 1, "combined match must carry turnEnd 1");
}

@("Robustness: topK clamp, embed-failure fallback (review Issues 1/2)")
unittest {
    auto s = setupTest("robustness_topk_clamp");
    scope (exit)
        teardownTest(s);

    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
    scope (exit)
        di.dispose;
    Thread.sleep(100.dur!"msecs");

    string sid = "20240506-160000-0001";
    auto u = Message(Role.user, true, "Clamp test unique phrase", "");
    u.turnId = 1;
    auto cp = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: sid, evictedSummarized: [
        Chat.MessageT(u)
    ], evictedPurged: null, evictedInPlace: null, turnStart: 1, turnEnd: 1,
        summaryText: "", originalLength: 1, newLength: 0, newContextSize: 0);
    di.onCheckpoint(cp);

    send(di.workerTid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(10.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain");

    // topK <= 0 must never crash the query (clamped to 1).
    auto r0 = di.query(qEmb(), SessionId(sid), "clamp", "", topK: 0);
    assert(r0.hasHistory, "topK=0 must still return a history result");
    auto rneg = di.query(qEmb(), SessionId(sid), "clamp", "", topK: -5);
    assert(rneg.hasHistory, "negative topK must still return a history result");
    assert(rneg.matches.length <= 1, "topK clamp must cap matches at 1");

    // Vector-only with a failing embedder -> graceful error, no throw.
    auto failEmb = new FailEmbedder();
    auto rf = di.query(failEmb, SessionId(sid), "", "anything", topK: 5);
    assert(!rf.hasHistory);
    assert(indexOf(rf.message, "error") != -1,
            "embed failure must be reported as an error, got: " ~ rf.message);

    // Text+vector with a failing embedder falls back to text-only.
    auto rt = di.query(failEmb, SessionId(sid), "clamp", "anything", topK: 5);
    assert(rt.hasHistory && rt.matches.length > 0,
            "text+vector with embed failure must fall back to text search");
}

@("dispose(): idempotent and safe before any checkpoint (item 2)")
unittest {
    auto s = setupTest("dispose_idempotent");
    scope (exit)
        teardownTest(s);

    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
    Thread.sleep(100.dur!"msecs"); // let the worker start

    // Dispose before any checkpoint: must not throw or hang.
    di.dispose();
    // Double dispose: guarded by the `disposed` flag, so the second call
    // is a no-op (no re-send, no re-wait).
    di.dispose();
}

@("Corrupted session DB: the query path degrades to no-history (item 3)")
unittest {
    import std.stdio : File;

    auto s = setupTest("corrupted_session_db");
    scope (exit)
        teardownTest(s);

    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
    scope (exit)
        di.dispose();
    Thread.sleep(100.dur!"msecs");

    string sid = "20240101-120000-cafe"; // valid id, never indexed
    // A file that exists but is not a valid SQLite database.
    File((s.tmpDir ~ (sid ~ ".db")).AbsolutePath.toString, "w").write(
            "this is not a valid sqlite database file");

    auto e = qEmb();
    assert(!di.hasHistory(e, SessionId(sid)), "corrupt DB must report no history");
    assert(di.maxTurn(e, SessionId(sid)) == 0, "corrupt DB must report maxTurn 0");
    auto res = di.query(e, SessionId(sid), "anything", "");
    assert(!res.hasHistory, "corrupt DB query must degrade to no-history");
    assert(res.matches.length == 0, "corrupt DB query must have no matches");
    assert(!res.message.startsWith("error:"),
            "corrupt DB must degrade to no-history, not an engine error: " ~ res.message);
}

@("boundary: no-history and engine-error messages are disjoint (item 4)")
unittest {
    auto s = setupTest("no_history_and_engine_error");
    scope (exit)
        teardownTest(s);

    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
    scope (exit)
        di.dispose();
    Thread.sleep(100.dur!"msecs");

    // (a) Never-indexed session: graceful no-history, NO "error:" prefix.
    string noHistSid = "20240101-120000-b001";
    auto noHist = di.query(qEmb(), SessionId(noHistSid), "", "vector query");
    assert(!noHist.hasHistory, "never-indexed session must have no history");
    assert(!noHist.message.startsWith("error:"),
            "no-history must not be error-prefixed: " ~ noHist.message);

    // Index a session so it has history.
    string histSid = "20240101-120000-b002";
    auto u = Message(Role.user, true, "boundary question", "");
    u.turnId = 1;
    auto cp = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: histSid, evictedSummarized: [
        Chat.MessageT(u)
    ], evictedPurged: null, evictedInPlace: null, turnStart: 1, turnEnd: 1,
        summaryText: "", originalLength: 1, newLength: 0, newContextSize: 0);
    di.onCheckpoint(cp);
    send(di.workerTid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(10.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain");
    assert(di.hasHistory(qEmb(), SessionId(histSid)), "session must have history");

    // (b) Embed failure on a session WITH history: "error:"-prefixed.
    auto embFail = di.query(new FailEmbedder(), SessionId(histSid), "", "vector query");
    assert(!embFail.hasHistory, "embed failure must have no history");
    assert(embFail.message.startsWith("error:"),
            "embed failure must be error-prefixed: " ~ embFail.message);

    // Non-overlap: the two message forms cannot be confused at the tool boundary.
    assert(noHist.message != embFail.message, "boundary messages must be distinct");
}

// feedback-loop guard: a ToolResponse (trace) in a checkpoint's evicted
// slice must never reach the session DB. The A4 classifier inside
// onCheckpoint (dialogueOf) drops it, so a distinctive sentinel string is
// provably absent from every indexed topic - even if a future change fed
// the raw (unfiltered) evicted slice through. Pinned at the INDEXING
// boundary (complementing the projection-level pin in tool_call/dialogue).
@("feedback-loop guard")
unittest {
    auto s = setupTest("feedback_loop_guard");
    scope (exit)
        teardownTest(s);

    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
    scope (exit)
        di.dispose;
    Thread.sleep(100.dur!"msecs");

    string sid = "20240601-010203-f6a1";

    // A prior queryDialogueHistory result: a ToolResponse whose content is
    // a distinctive sentinel. Under F6 it is trace, never dialogue.
    immutable SENTINEL = "SENTINEL-TOOLRESULT-7f3a9c";
    auto toolResp = ToolResponse(SENTINEL, "call-sentinel", "queryDialogueHistory", true);
    toolResp.turnId = 1;

    // One genuine user query and one genuine assistant answer (A4
    // dialogue), same turn as the ToolResponse.
    auto u1 = Message(Role.user, true, "F6 guard genuine question zebra", "");
    u1.turnId = 1;
    auto a1 = Message(Role.assistant, false, "F6 guard genuine answer quokka", "");
    a1.turnId = 1;

    // The evicted slice mixes the trace ToolResponse with the genuine
    // dialogue. onCheckpoint must index only the dialogue.
    auto cp = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: sid,
            evictedSummarized: [
                Chat.MessageT(toolResp), Chat.MessageT(u1), Chat.MessageT(a1)
    ], evictedPurged: null, evictedInPlace: null, turnStart: 1, turnEnd: 1,
            summaryText: "", originalLength: 3, newLength: 0, newContextSize: 0);
    di.onCheckpoint(cp);

    send(di.workerTid, DiDrain(thisTid));
    bool drained = false;
    receiveTimeout(10.dur!"seconds", (DiDrained _) { drained = true; });
    assert(drained, "worker did not drain");

    // Open the session DB read-only (model/dimensions from the query-path
    // embedder, which matches the values the worker wrote).
    auto qe = qEmb();
    auto dbOpt = openDatabase((s.tmpDir ~ (sid ~ ".db")).AbsolutePath,
            qe.modelName(), qe.dimensions(), readOnly: true);
    assert(hasValue(dbOpt), "session DB must exist after checkpoint indexing");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy();

    // Count invariant: exactly one episode (turn 1) is indexed. The trace
    // ToolResponse must not have produced a hidden extra topic.
    assert(db.getSources().length == 1,
            "expected exactly one indexed episode, got %s".format(db.getSources().length));

    // The genuine dialogue IS present and verbatim (both pieces, one chunk).
    auto good = db.queryTextSearch("zebra", 100);
    bool foundGood = false;
    foreach (m; good) {
        if (indexOf(m.text, "F6 guard genuine question zebra") != size_t.max
                && indexOf(m.text, "F6 guard genuine answer quokka") != size_t.max)
            foundGood = true;
    }
    assert(foundGood, "genuine dialogue must be indexed");

    // THE F6 GUARD (a): an FTS5 phrase search for the sentinel finds
    // nothing. If a future change indexed the ToolResponse content, this
    // phrase would match and the guard would trip.
    auto sent = db.queryTextSearch(SENTINEL, 100);
    assert(sent.length == 0,
            "sentinel ToolResponse content must not be text-searchable (F6), "
            ~ "got %s matches".format(sent.length));
    foreach (m; sent) {
        assert(indexOf(m.text, SENTINEL) == size_t.max,
                "no topic may contain ToolResponse content (F6)");
    }

    // THE F6 GUARD (b): a wide-k semantic scan surfaces every indexed
    // chunk (TestEmbedder returns all-ones vectors, so all chunks are
    // within k). None may contain the sentinel; exactly one chunk exists.
    float[] scanEmb;
    qe.embed("scan").match!((float[] v) { scanEmb = v; }, (EmbedError _) {
        scanEmb = null;
    });
    auto all = db.querySemantic(Search(scanEmb), 100);
    assert(all.length == 1, "exactly one indexed chunk expected, got %s".format(all.length));
    foreach (m; all) {
        assert(indexOf(m.text, SENTINEL) == size_t.max,
                "no topic may contain ToolResponse content (F6): " ~ m.text);
    }
}

// summary-marker regression: a merged compression summary
// entry (buildMergedSummary shape) in a checkpoint's evicted slice must
// never reach the session DB, and a legacy unstamped entry (turnId == 0,
// as loaded for pre-Phase-0 sessions) is excluded the same way - while
// genuine evicted dialogue in the same checkpoint is still indexed
// (positive control: the filter is not over-broad). Pinned at the DB
// level: the sentinel must be absent from every topic's content AND the
// summary's turn range absent from the decoded topic names.
//
// Scenario: an earlier compression summarized turns 1-3 into a merged
// summary (stamped turnId 3, saveData summary_turn_start/end markers);
// a later compression evicts a slice containing that summary entry, one
// legacy unstamped entry, and genuine turn 5.
@("summary-marker regression")
unittest {
    auto s = setupTest("summary_marker_regression");
    scope (exit)
        teardownTest(s);

    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &diTestFactory);
    scope (exit)
        di.dispose;
    Thread.sleep(100.dur!"msecs"); // let worker start

    string sid = "20240615-090807-f105";

    // (a) Merged summary entry: shape copied from SummaryAgent's
    //     buildMergedSummary (assistant Message, turnId = the summarized
    //     slice's turnEnd, saveData markers for the summarized range).
    immutable SENTINEL_SUMMARY = "SENTINEL-SUMMARY-4d8b21";
    long SUM_TURN_START = 1;
    long SUM_TURN_END = 3;
    auto summaryMsg = Message(Role.assistant, userQuery: false, content: SENTINEL_SUMMARY,
            thinking: null);
    summaryMsg.turnId = SUM_TURN_END;
    summaryMsg.saveData["summary_turn_start"] = SUM_TURN_START;
    summaryMsg.saveData["summary_turn_end"] = SUM_TURN_END;

    // (b) Legacy pre-Phase-0 entry: genuine user-query shape but
    //     unstamped (turnId 0, as loaded by session/store.d).
    immutable SENTINEL_LEGACY = "LEGACY-UNSTAMPED-9e2c57";
    auto legacyMsg = Message(Role.user, true, SENTINEL_LEGACY, "");
    legacyMsg.turnId = 0;

    // (c) Genuine user query + assistant answer (A4 dialogue) from the
    //     same checkpoint turn range (turn 5).
    auto u5 = Message(Role.user, true, "F10 regression genuine question", "");
    u5.turnId = 5;
    auto a5 = Message(Role.assistant, false, "F10 regression genuine answer", "");
    a5.turnId = 5;

    // Checkpoint for the evicted range: turnRangeOf counts the unstamped
    // legacy entry, so the min is 0 and the max is 5.
    auto cp = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: sid,
            evictedSummarized: [
                Chat.MessageT(summaryMsg), Chat.MessageT(legacyMsg),
                Chat.MessageT(u5), Chat.MessageT(a5)
    ], evictedPurged: null, evictedInPlace: null, turnStart: 0, turnEnd: 5,
            summaryText: "", originalLength: 4, newLength: 0, newContextSize: 0);
    di.onCheckpoint(cp);

    // Dispose drains the worker mailbox (DiDrain -> DiDrained); the
    // drain handler checkpoints + closes the WAL write connection,
    // leaving a clean DB for the read-only open below.
    di.dispose();

    // Open the session DB read-only (model/dimensions from the
    // query-path embedder, which matches the values the worker wrote).
    auto qe = qEmb();
    auto dbOpt = openDatabase((s.tmpDir ~ (sid ~ ".db")).AbsolutePath,
            qe.modelName(), qe.dimensions(), readOnly: true);
    assert(hasValue(dbOpt), "session DB must exist after checkpoint indexing");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy();

    // Count invariant: exactly one episode (the genuine turn 5) is
    // indexed. The summary and legacy entries must not have produced a
    // hidden extra topic.
    assert(db.getSources().length == 1,
            "expected exactly one indexed episode, got %s".format(db.getSources().length));

    // The single topic decodes (D2) to the genuine turn 5's range.
    auto meta = db.getSources()[0].origin.match!((Topic t) {
        auto m = decodeTopicName(t.name);
        assert(hasValue(m), "indexed topic must decode: " ~ t.name);
        return m.match!((EpisodeMeta x) => x, (None _) => EpisodeMeta.init);
    }, (_) => EpisodeMeta.init);
    assert(meta.turnStart == 5 && meta.turnEnd == 5,
            "the only indexed episode must be genuine turn 5, got t%s-t%s".format(
                meta.turnStart, meta.turnEnd));

    // THE F10 GUARD (a): no indexed topic covers the summary's turn
    // range (1-3). If the marker exclusion regressed, the summary would
    // be indexed as a t3_3 episode inside this range.
    bool rangeLeaks = false;
    foreach (src; db.getSources()) {
        src.origin.match!((Topic t) {
            auto m = decodeTopicName(t.name);
            if (hasValue(m)) {
                auto mm = m.match!((EpisodeMeta x) => x, (None _) => EpisodeMeta.init);
                if (mm.turnStart <= SUM_TURN_END && mm.turnEnd >= SUM_TURN_START)
                    rangeLeaks = true;
            }
        }, (_) {});
    }
    assert(!rangeLeaks, "no indexed topic may cover the summary turn range 1-3 (F10)");

    // THE F10 GUARD (b): an FTS5 phrase search for the full sentinel
    // finds nothing (cleanFts5 quotes the hyphenated token into a
    // phrase, so this is a sound "is this string indexed?" probe).
    auto sent = db.queryTextSearch(SENTINEL_SUMMARY, 100);
    assert(sent.length == 0,
            "merged summary content must not be text-searchable (F10), got "
            ~ "%s matches".format(sent.length));

    // The legacy unstamped entry is excluded the same way.
    auto leg = db.queryTextSearch(SENTINEL_LEGACY, 100);
    assert(leg.length == 0,
            "legacy turnId==0 content must not be text-searchable (F10), got "
            ~ "%s matches".format(leg.length));

    // Positive control: the genuine dialogue IS present and verbatim
    // (both pieces in one chunk) - the F10 filter is not over-broad, and
    // the FTS probes above are not vacuous.
    auto good = db.queryTextSearch("genuine", 100);
    bool foundGood = false;
    foreach (m; good) {
        if (indexOf(m.text, "F10 regression genuine question") != size_t.max
                && indexOf(m.text, "F10 regression genuine answer") != size_t.max)
            foundGood = true;
    }
    assert(foundGood, "genuine dialogue must be indexed (positive control)");

    // THE F10 GUARD (c): a wide-k semantic scan surfaces every indexed
    // chunk (TestEmbedder returns all-ones vectors, so all chunks are
    // within k). Neither sentinel may appear in any of them; exactly one
    // chunk exists.
    float[] scanEmb;
    qe.embed("scan").match!((float[] v) { scanEmb = v; }, (EmbedError _) {
        scanEmb = null;
    });
    auto all = db.querySemantic(Search(scanEmb), 100);
    assert(all.length == 1, "exactly one indexed chunk expected, got %s".format(all.length));
    foreach (m; all) {
        assert(indexOf(m.text, SENTINEL_SUMMARY) == size_t.max,
                "no topic may contain merged summary content (F10): " ~ m.text);
        assert(indexOf(m.text, SENTINEL_LEGACY) == size_t.max,
                "no topic may contain legacy turnId==0 content (F10): " ~ m.text);
    }
}
