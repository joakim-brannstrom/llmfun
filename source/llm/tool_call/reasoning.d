/// queryReasoningHistory tool: searches the per-session reasoning index for
/// structured records of the agent's past strategic thinking (abandoned
/// approaches, binding decisions, open uncertainties) produced at compression
/// checkpoints. Same validation/result conventions as the dialogue tool:
/// "error:"-prefixed strings are failures; no-history/no-matches are
/// successful graceful results. Each rendered match carries the static
/// annotation: past thought, not ground truth.
module llm.tool_call.reasoning;

import std.conv : text;
import std.datetime : SysTime;
import std.format : format;
import std.range : empty;
import std.string : startsWith, strip;

import llm.rag.reasoning_index : ReasoningContext;
import llm.session.types : SessionId, isValidId;
import llm.tool_call;
import llm.tool_call.rag : RAGContext;

mixin RegisterLlmFunctions!();

/// Parameters for the queryReasoningHistory tool.
struct QueryReasoningHistoryParams {
    @ParamDescription("Exact terms (FTS5-style) that appear in a reasoning "
            ~ "record - a decision, an abandoned approach, or a tool name. "
            ~ "Leave empty to use vectorQuery only.")
    string textQuery;

    @ParamDescription("Natural language description of the past decision, "
            ~ "alternative, or roadblock you want explained. Leave empty to "
            ~ "use textQuery only.")
    string vectorQuery;

    @ParamDescription("Maximum number of matches to return")
    @ParamOptional long topK = 5;

    @ParamDescription("Look back at most this many turns before the newest "
            ~ "indexed turn. 0 or negative: no age filtering. A positive N "
            ~ "keeps only matches within N turns of the newest indexed turn.")
    @ParamOptional long maxTurnAge = 0;

    @ParamDescription(
            "Session id (YYYYMMDD-HHMMSS-4hex) to search. Defaults to the active session.")
    @ParamOptional string sessionId;
}

@Function("Retrieve past strategic thinking, failed attempts, and binding "
        ~ "decisions from compressed context. Use ONLY when the user asks "
        ~ "why a past decision was made, or when you have tried multiple "
        ~ "approaches and suspect you are stuck repeating one. Not for "
        ~ "verbatim quotes - use queryDialogueHistory", tags:
        ["reasoning"]) ExecuteFuncResult queryReasoningHistory(Context baseCtx,
        QueryReasoningHistoryParams params) {
    import llm.rag.database : cleanFts5;

    mixin(baseContextToSpecific!ReasoningContext);

    if (params.textQuery.strip.empty && params.vectorQuery.strip.empty)
        return ExecuteFuncResult("error: provide textQuery, vectorQuery or both", false);
    const maxK = ctx.getToolLimits().maxTopK;
    if (params.topK < 1 || params.topK > maxK)
        return ExecuteFuncResult(i"error: topK must be in [1, $(maxK)], got $(params.topK)".text,
                false);

    string sid = params.sessionId.strip;
    if (sid.empty)
        sid = ctx.currentSessionId();
    if (sid.empty)
        return ExecuteFuncResult("error: no active session and no sessionId provided", false);
    if (!isValidId(SessionId(sid)))
        return ExecuteFuncResult(i"error: invalid sessionId '$(sid)' (expected YYYYMMDD-HHMMSS-4hex)".text,
                false);

    auto ri = ctx.getReasoningIndex();
    if (ri is null)
        return ExecuteFuncResult("error: reasoning index not available", false);

    // The agent's RAG embedder supplies the model/dimensions for the read-only
    // DB open and embeds the vector query (same as the dialogue tool).
    auto ragCtx = cast(RAGContext) baseCtx;
    if (ragCtx is null || ragCtx.getRAG() is null)
        return ExecuteFuncResult("error: RAG not available", false);

    auto result = ri.query(ragCtx.getRAG().embedder, SessionId(sid),
            params.textQuery.cleanFts5, params.vectorQuery, params.topK, params.maxTurnAge);

    if (!result.hasHistory) {
        // Engine errors must not look like success.
        // TODO: bad design that the message string is checked for a magic
        // prefix. It should be either a unique type such that SumType is used,
        // value or error. Or the status should be a boolean flag or enum.
        if (result.message.startsWith("error:"))
            return ExecuteFuncResult(result.message, false);
        return ExecuteFuncResult(result.message, true);
    }

    if (result.matches.length == 0)
        return ExecuteFuncResult(format(
                "No matches found in the indexed reasoning history of session '%s'.", sid), true);

    // Render each match: session id, turn range, timestamp, rank, record
    // text, then the static annotation with the match's turn range.
    string rendered;
    foreach (i, m; result.matches) {
        if (i > 0)
            rendered ~= "\n\n";
        rendered ~= format("--- Match %s (session: %s, turns %s-%s, %s, rank: %.3f) ---\n", i + 1, sid,
                m.episode.turnStart, m.episode.turnEnd,
                formatTimestamp(m.episode.epochMillis), m.rank);
        rendered ~= m.text;
        rendered ~= i"\n(Past thought, not ground truth — formed during turns $(
                m.episode.turnStart)-$(m.episode.turnEnd). ".text
            ~ "If it contradicts the user's latest instruction or verbatim "
            ~ "facts, ignore it. Do not repeat an abandoned approach without new evidence.)";
    }
    return ExecuteFuncResult(rendered, true);
}

/// Format an epoch-millis timestamp as YYYY-MM-DD HH:MM:SS in the local time
/// zone (same helper as dialogue.d).
private string formatTimestamp(long epochMillis) {
    auto t = SysTime.fromUnixTime(epochMillis / 1000).toLocalTime;
    return format("%04d-%02d-%02d %02d:%02d:%02d", t.year, cast(int) t.month,
            t.day, t.hour, t.minute, t.second);
}

version (unittest) {
    import std.algorithm : canFind;
    import std.concurrency : thisTid;
    import std.file : mkdirRecurse;
    import std.math : sqrt;
    import std.string : count, split, toLower;
    import std.sumtype : match;

    import my.optional;
    import my.path : AbsolutePath, Path;

    import llm.agent.context : AgentContext;
    import llm.common.embedder : Embedder, EmbedError, EmbedResult;
    import llm.config : LlmConfig, RagConfig, RagDatabaseConfig, SummaryModelConfig;
    import llm.rag.database : Database, openDatabase;
    import llm.rag.dialogue_index : Kind, encodeTopicName;
    import llm.rag.rag : Document, Origin, RAG, Topic, addToDatabase;
    import llm.rag.reasoning_index : ReasoningIndex;
    import llm.test_util : TestArea, testArea;

    /// A valid session id (YYYYMMDD-HHMMSS-4hex) shared by the tests.
    private immutable TestSessionId = "20240101-120000-abcd";

    /// Deterministic djb2 hash (this Phobos build has no std.hash): stable
    /// across runs so the same word always lands in the same bucket.
    private uint wordHash(string s) {
        uint h = 5381u;
        foreach (c; s)
            h = ((h << 5) + h) + cast(uint) c;
        return h;
    }

    /// Deterministic hash-based embedder: bag-of-words vectors hashed into 8
    /// buckets and L2-normalized. Identical text yields identical vectors and
    /// more shared words yield smaller L2 distance - enough structure for
    /// the semantic query paths to rank records meaningfully.
    private class TestEmbedder : Embedder {
        override void destroy() {
        }

        override string modelName() {
            return "test";
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
            auto v = new float[dimensions];
            v[] = 0;
            foreach (word; text.toLower.split)
                v[wordHash(word) % dimensions] += 1;
            double norm = 0;
            foreach (f; v)
                norm += cast(double) f * f;
            if (norm > 0) {
                double n = sqrt(norm);
                foreach (ref f; v)
                    f = cast(float)(f / n);
            }
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

        override int[] tokenize(string text, bool addSpecial) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            // The topic prefix ("Topic: d_|r_<sid>__t<s>_<e>__<epoch> | ")
            // is 53-55 chars and is reserved out of the batch budget before
            // chunking. 150 keeps the effective window >= 95 graphemes, above
            // the longest test record, so single-piece records index as one
            // verbatim chunk.
            return 150;
        }
    }

    /// TestEmbedder whose embed() always fails - exercises the vector-query
    /// error path (the "error:" prefix -> success:false seam) and the
    /// text-fallback path (mirrors the dialogue tool's FailEmbedder).
    private class FailEmbedder : TestEmbedder {
        override EmbedResult embed(string text) {
            return EmbedResult(EmbedError("boom"));
        }
    }

    /// An AgentContext with a TestEmbedder-backed RAG and (optionally) a
    /// An AgentContext with a TestEmbedder-backed RAG and (optionally) a
    /// ReasoningIndex, working under <testDir>/ctx. The RAG is registered in
    /// `testDir` so cleanup() destroys it before the dir is removed. `emb`
    /// overrides the RAG embedder.
    private AgentContext makeContext(ref TestArea testDir, string activeSessionId,
            ReasoningIndex ri = null, bool withRag = true, Embedder emb = null) {
        auto workDir = testDir ~ "ctx";
        mkdirRecurse(workDir.toString);
        auto conf = LlmConfig();
        conf.workArea = workDir;
        conf.activeChatSessionId = activeSessionId;
        RAG rag = null;
        if (withRag) {
            rag = new RAG(emb !is null ? emb : new TestEmbedder(),
                    RagDatabaseConfig(workDir ~ "rag.sqlite3", "test rag"), null);
            testDir.addRag(rag);
        }
        auto ctx = new AgentContext(conf, rag, null);
        if (ri !is null)
            ctx.setReasoningIndex(ri);
        return ctx;
    }

    /// A ReasoningIndex over the test's area: the area is where this test's
    /// session DBs live, so seedSessionDb and the query's read-only opens
    /// target one dir. workerTid = thisTid() is safe: the tool query path
    /// never sends an RiJob (only onCheckpoint does, and it is not called
    /// from the tool).
    private ReasoningIndex makeRi(TestArea testDir) {
        return new ReasoningIndex(testDir.workArea,
                SummaryModelConfig(contextSize: 16384), thisTid());
    }

    /// Seed a session's DB directly with records of the given kind
    /// (bypassing the worker: fast and deterministic). One record per
    /// (turn, text) under an encoded topic name, into the test's own area:
    /// all records a test seeds share that area's side-by-side <sid>.db
    /// files. Closes the DB before returning so read-only opens see a
    /// committed file.
    private void seedSessionDb(string sid, long[] turns, string[] texts,
            TestArea testDir, Kind kind = Kind.dialogue) {
        auto dbPath = (testDir.workArea ~ (sid ~ ".db")).AbsolutePath;
        auto dbOpt = openDatabase(dbPath, "test", 8);
        assert(hasValue(dbOpt), "seed DB must open");
        auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
        scope (exit)
            db.destroy();
        size_t nBatch;
        long epoch = 1700000000000L;
        foreach (i, t; turns) {
            auto topic = encodeTopicName(sid, t, t, epoch + i * 1000L, kind);
            auto doc = Document(origin: Origin(Topic(topic)), data: texts[i]);
            auto res = addToDatabase(db, new TestEmbedder(), doc,
                    RagConfig(windowOverlapPercent: 10), nBatch, topic);
            assert(res.chunks > 0, "record must index at least one chunk");
        }
        // addToDatabase fills TextChunkTbl only; the external-content FTS
        // index needs an explicit rebuild before text queries can find the
        // records (the worker does the same per job / drain).
        db.fts5Rebuild;
    }

    /// Number of "--- Match" blocks in a rendered tool result.
    private size_t countMatches(string msg) {
        return count(msg, "--- Match");
    }
}

// parameter validation rejects bad input without crashing (table-driven) ---
unittest {
    auto testDir = testArea("param_validation");
    scope (exit)
        testDir.cleanup();
    auto ri = makeRi(testDir);
    auto ctx = makeContext(testDir, TestSessionId, ri);
    const maxK = ctx.getToolLimits().maxTopK;

    struct Case {
        QueryReasoningHistoryParams params;
        string expectMsg;
    }

    Case[] cases = [
        Case(QueryReasoningHistoryParams(), "error: provide textQuery, vectorQuery or both"),
        Case(QueryReasoningHistoryParams(textQuery: "x", topK: 0), format("topK must be in [1, %s]",
                maxK)),
        Case(QueryReasoningHistoryParams(textQuery: "x", topK: -1), format("topK must be in [1, %s]",
                maxK)),
        Case(QueryReasoningHistoryParams(textQuery: "x", topK: maxK + 1),
                format("topK must be in [1, %s]", maxK)),
        Case(QueryReasoningHistoryParams(textQuery: "x", sessionId: "../../etc/passwd"), "error: invalid sessionId"),
        Case(QueryReasoningHistoryParams(textQuery: "x", sessionId: "not-an-id"),
                "error: invalid sessionId")
    ];
    foreach (c; cases) {
        auto r = queryReasoningHistory(ctx, c.params);
        assert(!r.success, r.msg);
        assert(r.msg.canFind(c.expectMsg), "expected '" ~ c.expectMsg ~ "' in: " ~ r.msg);
    }

    // The limit itself is accepted (validated against the context, not a
    // module constant, so a config raising maxTopK is honored).
    auto r = queryReasoningHistory(ctx, QueryReasoningHistoryParams(textQuery: "x", topK: maxK));
    assert(r.success, r.msg);
}

// graceful no-history and missing-dependency responses ---
unittest {
    auto testDir = testArea("graceful_no_history");
    scope (exit)
        testDir.cleanup();
    auto ri = makeRi(testDir);

    // Fresh session: indexer present but nothing indexed yet.
    auto ctx = makeContext(testDir, TestSessionId, ri);
    auto r = queryReasoningHistory(ctx, QueryReasoningHistoryParams(textQuery: "anything"));
    assert(r.success, r.msg);
    assert(r.msg == "No reasoning history indexed for this session yet.", r.msg);

    // No active session and no explicit sessionId: a caller error, not a
    // graceful no-history - the tool cannot resolve the target session.
    auto ctx2 = makeContext(testDir, "", ri);
    r = queryReasoningHistory(ctx2, QueryReasoningHistoryParams(vectorQuery: "anything"));
    assert(!r.success, r.msg);
    assert(r.msg == "error: no active session and no sessionId provided", r.msg);

    // Missing reasoning index: explicit error, no crash.
    auto ctx3 = makeContext(testDir, TestSessionId, null);
    r = queryReasoningHistory(ctx3, QueryReasoningHistoryParams(textQuery: "x"));
    assert(!r.success && r.msg.canFind("reasoning index not available"), r.msg);

    // Missing RAG: explicit error, no crash.
    auto ctx4 = makeContext(testDir, TestSessionId, ri, withRag: false);
    r = queryReasoningHistory(ctx4, QueryReasoningHistoryParams(textQuery: "x"));
    assert(!r.success && r.msg.canFind("RAG not available"), r.msg);
}

// embedder failure on the vector path is an error, not a no-history ---
unittest {
    auto testDir = testArea("embedder_failure");
    scope (exit)
        testDir.cleanup();
    seedSessionDb(TestSessionId, [7],
            [
                "EMBED_FAIL_TOKEN the streaming parser was chosen over the regex approach"
    ], testDir, Kind.reasoning);
    auto ri = makeRi(testDir);
    auto ctx = makeContext(testDir, TestSessionId, ri, true, new FailEmbedder());

    // vector-only: the embed failure must surface as an explicit error
    // (success: false), never as a graceful "no history" success.
    auto r = queryReasoningHistory(ctx, QueryReasoningHistoryParams(vectorQuery: "anything"));
    assert(!r.success, r.msg);
    assert(r.msg.canFind("could not embed"), r.msg);

    // text+vector with a failing embedder falls back to the text path.
    r = queryReasoningHistory(ctx, QueryReasoningHistoryParams(textQuery: "EMBED_FAIL_TOKEN",
            vectorQuery: "anything"));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 1, r.msg);
    assert(r.msg.canFind("EMBED_FAIL_TOKEN"), r.msg);
}

// Test: happy path - r_ + d_ in one session DB, only the r_ match is
// rendered, with the match header and the static annotation verbatim ---
unittest {
    auto testDir = testArea("happy_path");
    scope (exit)
        testDir.cleanup();
    seedSessionDb(TestSessionId, [12], [
        "REASONING_RECORD_TOKEN Abandoned the regex approach - it failed on nested brackets. Binding decision: switch to the streaming parser."
    ], testDir, Kind.reasoning);
    seedSessionDb(TestSessionId, [30],
            ["DIALOGUE_RECORD_TOKEN the user asked for the build status"], testDir);
    auto ri = makeRi(testDir);
    auto ctx = makeContext(testDir, TestSessionId, ri);

    auto r = queryReasoningHistory(ctx, QueryReasoningHistoryParams(textQuery: "REASONING_RECORD_TOKEN"));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 1, r.msg);
    // Header carries session id and the match's turn range; the record
    // text follows the header.
    assert(r.msg.canFind("--- Match 1 (session: " ~ TestSessionId), r.msg);
    assert(r.msg.canFind("turns 12-12"), r.msg);
    assert(r.msg.canFind("REASONING_RECORD_TOKEN"), r.msg);
    // The static annotation, verbatim, with the match's turn range.
    assert(r.msg.canFind("(Past thought, not ground truth — formed during turns 12-12. "
            ~ "If it contradicts the user's latest instruction or verbatim facts, "
            ~ "ignore it. Do not repeat an abandoned approach without new evidence.)"), r.msg);
    // A reasoning query never surfaces the dialogue record (kind filter).
    assert(!r.msg.canFind("DIALOGUE_RECORD_TOKEN"), r.msg);
}

// d_-only DB reports no reasoning history (kind-specific) ---
unittest {
    auto testDir = testArea("donly_no_history");
    scope (exit)
        testDir.cleanup();
    seedSessionDb(TestSessionId, [5], [
        "DIALOGUE_RECORD_TOKEN only dialogue here"
    ], testDir);
    auto ctx = makeContext(testDir, TestSessionId, makeRi(testDir));

    auto r = queryReasoningHistory(ctx, QueryReasoningHistoryParams(textQuery: "DIALOGUE_RECORD_TOKEN"));
    assert(r.success, r.msg);
    assert(r.msg == "No reasoning history indexed for this session yet.", r.msg);
}

// r_ present but zero hits reports no matches (not an error) ---
unittest {
    auto testDir = testArea("rno_matches");
    scope (exit)
        testDir.cleanup();
    seedSessionDb(TestSessionId, [5], ["REASONING_RECORD_TOKEN record body"],
            testDir, Kind.reasoning);
    auto ctx = makeContext(testDir, TestSessionId, makeRi(testDir));

    // The DB exists and has reasoning history, but the term matches nothing:
    // a successful search with an empty result set, not an error.
    auto r = queryReasoningHistory(ctx, QueryReasoningHistoryParams(textQuery: "zebra"));
    assert(r.success, r.msg);
    assert(r.msg.canFind("No matches found in the indexed reasoning history"), r.msg);
    assert(r.msg.canFind(TestSessionId), r.msg);
}

// maxTurnAge windowing (0 and negative mean no filter) ---
unittest {
    auto testDir = testArea("max_turn_age");
    scope (exit)
        testDir.cleanup();
    seedSessionDb(TestSessionId, [5], ["AGE_TOKEN old reasoning record"], testDir, Kind.reasoning);
    seedSessionDb(TestSessionId, [10], ["AGE_TOKEN new reasoning record"],
            testDir, Kind.reasoning);
    auto ctx = makeContext(testDir, TestSessionId, makeRi(testDir));

    // maxTurnAge 0 (the default): no age filtering, both records returned.
    auto r = queryReasoningHistory(ctx, QueryReasoningHistoryParams(textQuery: "AGE_TOKEN"));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 2, r.msg);

    // A negative value behaves like 0 (no age filtering) - parity with
    // the dialogue tool's negative cases.
    r = queryReasoningHistory(ctx, QueryReasoningHistoryParams(textQuery: "AGE_TOKEN",
            maxTurnAge: -2));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 2, r.msg);

    // maxTurnAge 2: only turns within 2 of the newest indexed turn (10)
    // survive the window; the annotation carries the survivor's range.
    r = queryReasoningHistory(ctx, QueryReasoningHistoryParams(textQuery: "AGE_TOKEN",
            maxTurnAge: 2));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 1, r.msg);
    assert(r.msg.canFind("turns 10-10"), r.msg);
    assert(!r.msg.canFind("turns 5-5"), r.msg);
    assert(r.msg.canFind("formed during turns 10-10"), r.msg);
}

// AgentContext implements ReasoningContext (pair mirrors the dialogue one) ---
unittest {
    auto testDir = testArea("agent_context_pair");
    scope (exit)
        testDir.cleanup();
    auto ri = makeRi(testDir);
    auto ctx = makeContext(testDir, TestSessionId, ri);

    // The getter returns exactly the instance the setter stored.
    assert(ctx.getReasoningIndex() is ri, "getReasoningIndex must return the set instance");
    // The tool dispatches through the same context (every test above calls
    // queryReasoningHistory with an AgentContext, so the
    // baseContextToSpecific!ReasoningContext cast is proven).
}

// "Decision" recall suite
//
// Seeded-record recall against a temp session DB. Records are written
// directly through the codec (encodeTopicName + addToDatabase +
// fts5Rebuild); the checkpoint -> worker -> record pipeline is proven by
// the e2e integration test. This suite asserts the recall surface:
// semantic + FTS retrieval, cited turn range, the static annotation
// verbatim, session scoping.
//
// Documented deviations from the original task (each forced by the
// implemented API/runtime):
//
//  1. Session ids must be valid `YYYYMMDD-HHMMSS-4hex` (IdPattern regex,
//      llm/session/types.d); the original's `...-reca/-recl/-reco/
//      -reco2` suffixes contain non-hex letters, so isValidId rejects
//      them and EVERY original query would return "error: invalid
//      sessionId". Hex-valid siblings are used instead:
//      4eca / 4ec1 / 4ec0 / 4ec2.
//  2. `encodeTopicName` is (sessionId, turnStart, turnEnd, epochMillis,
//      kind): kind LAST (the design-time kind-first form would break the
//      dialogue indexer's call sites). The original called the kind-first
//      form; adapted to the implemented signature.
//  3. The original's `makeRi(TestArea)` helper is a redefinition of the
//      module's existing makeRi (same name and signature); the existing
//      helper is reused. Equivalent for the query path — a seeded-record
//      test never sends an RiJob, so the worker Tid is never touched.
//      (The reuse also removes the original helper's only ServerConfig /
//      SummaryModelConfig-server use.)
//  4. Session scoping: the queried sibling session is seeded as well
//      (mirroring the dialogue session-scoping test in dialogue.d): a
//      session with no DB yields the graceful no-history notice (pinned
//      by the no-history tests above) — "No matches found" requires an
//      existing r_ history. Seeding the sibling keeps the original's
//      exact assertion while the raw !canFind(REASONTOKEN) check makes
//      the no-leak assertion non-vacuous, and an active-session positive
//      control closes the loop (the record IS retrievable in its own
//      session).
//  5. The original's import block is omitted: no import line is needed —
//      every name the adapted code uses resolves via existing imports
//      (the block's `Tid` / `ServerConfig` names are not resolvable here
//      at all, but they are unused once the helper is reused). A SECOND
//      selective import of `std.algorithm : canFind` makes the full
//      unittest build fail with "canFind matches conflicting symbols" in
//      agent/package.d's guard unittests (agent/package.d imports this
//      module and std.algorithm; isolated both ways: reverting only this
//      file → green control, re-adding only this one import → the
//      conflict returns).

version (unittest) {
    // (No imports here — every name the adapted code uses resolves
    // through existing imports; see note 5 above.)

    // All-ones 8-dim embedder; modelName/dimensions MUST match the seeded DB
    // header ("test", 8) for read-only opens. Uniform ranks — recall is
    // asserted via topK coverage, not ranking.
    private class RiTestEmbedder : Embedder {
        override void destroy() {
        }

        override string modelName() {
            return "test";
        }

        override long dimensions() {
            return 8;
        }

        override bool supportsTokenization() {
            return false;
        }

        override EmbedResult embedQuery(string text) {
            return embedAll();
        }

        override EmbedResult embedDocument(string text) {
            return embedAll();
        }

        EmbedResult embedAll() {
            auto v = new float[8];
            foreach (ref f; v)
                f = 1.0f;
            return EmbedResult(v);
        }

        override EmbedResult embedQuery(int[] tokens) {
            return EmbedResult(EmbedError("no tokens"));
        }

        override EmbedResult embedDocument(int[] tokens) {
            return EmbedResult(EmbedError("no tokens"));
        }

        override int[] tokenize(string text, bool addSpecial) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            // Original value, probe-verified end-to-end: each record below
            // indexes as 8-9 chunks at this window and the tool's default
            // topK=5 still returns the record's stated-reason token
            // (probe: chunks=8/9, tokenInTop5=true).
            return 64;
        }
    }

    // Seed one r_ record directly (mirrors seedSessionDb in dialogue.d,
    // with the reasoning kind; note 2: kind-last signature).
    private void seedReasoningDb(string sid, long ts, long te, string recordText, TestArea testDir) {
        auto dbPath = (testDir.workArea ~ (sid ~ ".db")).AbsolutePath;
        auto dbOpt = openDatabase(dbPath, "test", 8);
        assert(hasValue(dbOpt), "seed DB must open");
        auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
        scope (exit)
            db.destroy();
        size_t nBatch;
        auto topic = encodeTopicName(sid, ts, te, 1700000000000L, Kind.reasoning);
        auto doc = Document(origin: Origin(Topic(topic)), data: recordText);
        auto res = addToDatabase(db, new RiTestEmbedder(), doc,
                RagConfig(windowOverlapPercent: 10), nBatch, topic);
        assert(res.chunks > 0, "record must index at least one chunk");
        db.fts5Rebuild;
    }

    // AgentContext with a RAG (RiTestEmbedder) + wired ReasoningIndex under
    // <testDir>/ctx (mirrors makeContext, dialogue.d:257-274). Worker unused
    // (records are seeded; the query path never sends an RiJob): the shared
    // makeRi above supplies thisTid() as the worker Tid and is safe here.
    private AgentContext makeRiCtx(ref TestArea testDir, string sid, ReasoningIndex ri) {
        auto workDir = testDir ~ "ctx";
        mkdirRecurse(workDir.toString);
        auto conf = LlmConfig();
        conf.workArea = workDir;
        conf.activeChatSessionId = sid;
        auto rag = new RAG(new RiTestEmbedder(),
                RagDatabaseConfig(workDir ~ "rag.sqlite3", "test rag"), null);
        testDir.addRag(rag);
        auto ctx = new AgentContext(conf, rag, null);
        ctx.setReasoningIndex(ri);
        return ctx;
    }
}

// --- Test (a): "why did you …" → record with stated reason retrieved,
//     turn range cited, annotation present.
unittest {
    auto testDir = testArea("recall_abandon_reason");
    scope (exit)
        testDir.cleanup();
    string sid = "20240101-120000-4eca";
    seedReasoningDb(sid, 3, 7,
            "Abandoned Hypotheses:\n" ~ "- brute-force zebra — timed out on 3 retries\n"
            ~ "Binding Decisions:\n- memoized zebra\nCurrent Uncertainties:\n- None\n"
            ~ "Justification:\nREASONTOKEN switched to memoized zebra because brute force timed out",
            testDir);
    auto ctx = makeRiCtx(testDir, sid, makeRi(testDir));

    auto r = queryReasoningHistory(ctx, QueryReasoningHistoryParams(vectorQuery: "why did you switch to the memoized approach"));
    assert(r.success, r.msg);
    assert(r.msg.canFind("REASONTOKEN"), "record not retrieved");
    assert(r.msg.canFind("turns 3-7"), "turn range not cited");
    assert(r.msg.canFind("(Past thought, not ground truth — formed during turns 3-7. "
            ~ "If it contradicts the user's latest instruction or verbatim facts, ignore it. "
            ~ "Do not repeat an abandoned approach without new evidence.)"),
            "static annotation missing");

    r = queryReasoningHistory(ctx, QueryReasoningHistoryParams(textQuery: "REASONTOKEN"));
    assert(r.success && r.msg.canFind("REASONTOKEN"), r.msg);
}

// --- Test (b): stuck loop — retrying an abandoned approach; prior failure
//     surfaces with the anti-retry annotation.
unittest {
    auto testDir = testArea("recall_stuck_loop");
    scope (exit)
        testDir.cleanup();
    string sid = "20240101-120000-4ec1";
    seedReasoningDb(sid, 10, 12, "Abandoned Hypotheses:\n- None\nBinding Decisions:\n" ~ "- do not retry brute-force zebra — ABANDONEDTOKEN failed at turn 10 (timeout, retried twice)\n" ~ "Current Uncertainties:\n- None\nJustification:\n- turn 10: first failure\n- turn 11: retry failed\n- turn 12: abandoned",
            testDir);
    auto ctx = makeRiCtx(testDir, sid, makeRi(testDir));

    auto r = queryReasoningHistory(ctx, QueryReasoningHistoryParams(vectorQuery: "should I retry the brute-force zebra approach"));
    assert(r.success, r.msg);
    assert(r.msg.canFind("ABANDONEDTOKEN"), "prior failure not surfaced");
    assert(r.msg.canFind("Do not repeat an abandoned approach without new evidence."),
            "anti-retry annotation missing");
    assert(r.msg.canFind("turns 10-12"), "turn range not cited");
}

// session scoping — records never leak across sessions.
unittest {
    auto testDir = testArea("recall_session_scope");
    scope (exit)
        testDir.cleanup();
    string sid = "20240101-120000-4ec0";
    seedReasoningDb(sid, 1, 2, "REASONTOKEN isolated record", testDir);
    // Note 4: the sibling session is seeded too, so the cross-session query
    // exercises the "history present, no matches" path (a missing DB would
    // yield the pinned no-history notice instead — not "No matches
    // found") and the no-leak check below is non-vacuous: the record IS
    // searchable in its own session.
    string sid2 = "20240101-120000-4ec2";
    seedReasoningDb(sid2, 5, 6, "OTHER_TOKEN isolated record", testDir);
    auto ctx = makeRiCtx(testDir, sid, makeRi(testDir));

    auto r = queryReasoningHistory(ctx, QueryReasoningHistoryParams(textQuery: "REASONTOKEN",
            sessionId: sid2));
    assert(r.success && r.msg.canFind("No matches found"), r.msg);
    assert(r.msg.canFind(sid2), r.msg);
    assert(!r.msg.canFind("REASONTOKEN"), "record leaked across sessions");

    // Positive control in the same fixture: the same query reaches the
    // record in its own (active) session — the cross-session miss above is
    // the scoping, not a broken query path.
    r = queryReasoningHistory(ctx, QueryReasoningHistoryParams(textQuery: "REASONTOKEN"));
    assert(r.success && r.msg.canFind("REASONTOKEN"), r.msg);
}
