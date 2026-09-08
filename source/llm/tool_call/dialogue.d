/// queryDialogueHistory tool: searches the per-session dialogue index for
/// verbatim matches from previously compressed conversation episodes.
///
/// Parameters: textQuery (bare FTS5-style keywords), vectorQuery (natural
/// language, matched semantically), topK (default 5), maxTurnAge (default 0;
/// 0 or negative = no age filtering), sessionId (optional, defaults to the
/// active session). The tool validates the parameters, resolves the session
/// via isValidId, and dispatches the text-only, vector-only or combined
/// search to DialogueIndex.query, which reads the session DB read-only. An
/// unindexed session yields a graceful no-history result, never a crash.
module llm.tool_call.dialogue;

import std.conv : text;
import std.datetime : SysTime;
import std.format : format;
import std.range : empty;
import std.string : startsWith, strip;

import llm.rag.dialogue_index : DialogueContext;
import llm.session.types : SessionId, isValidId;
import llm.tool_call;
import llm.tool_call.rag : RAGContext;

mixin RegisterLlmFunctions!();

/// Parameters for the queryDialogueHistory tool.
struct QueryDialogueHistoryParams {
    @ParamDescription("Bare keywords for a full-text search, as written in the "
            ~ "conversation (exact terms, space separated). Leave empty to use "
            ~ "vectorQuery only.")
    string textQuery;

    @ParamDescription("Natural language description of what to find, matched "
            ~ "semantically. Use for paraphrases or concepts. Leave empty to "
            ~ "use textQuery only.")
    string vectorQuery;

    @ParamDescription("Maximum number of matches to return")
    @ParamOptional long topK = 5;

    @ParamDescription("Look back at most this many turns before the newest "
            ~ "indexed turn. 0 or negative: no age filtering. A positive N "
            ~ "keeps only matches within N turns of the newest indexed turn.")
    @ParamOptional long maxTurnAge = 0;

    @ParamDescription(
            "Session id (YYYYMMDD-HHMMSS-4hex) to search. Defaults " ~ "to the active session.")
    @ParamOptional string sessionId;
}

@Function("Retrieve verbatim historical dialogue from compressed turns. Use this "
        ~ "only when the user asks for exact quotes, specific numbers, error "
        ~ "messages, code, or command-line inputs that are missing from the "
        ~ "compressed summary. Pass the user's EXACT nouns and entities in the "
        ~ "query; do not paraphrase. Supply textQuery (exact keywords), "
        ~ "vectorQuery (natural language), or both. Returns matching episodes "
        ~ "with the session id, turn ranges, timestamp, and the matched " ~ "verbatim text.")
ExecuteFuncResult queryDialogueHistory(Context baseCtx, QueryDialogueHistoryParams params) {
    import llm.rag.database : cleanFts5;

    mixin(baseContextToSpecific!DialogueContext);

    // Validate parameters before touching any state.
    if (params.textQuery.strip.empty && params.vectorQuery.strip.empty)
        return ExecuteFuncResult("error: provide textQuery, vectorQuery or both", false);
    const maxK = ctx.getToolLimits().maxTopK;
    if (params.topK < 1 || params.topK > maxK)
        return ExecuteFuncResult(i"error: topK must be in [1, $(maxK)], got $(params.topK)".text,
                false);

    // Resolve the session: an explicit sessionId wins, else the active session.
    string sid = params.sessionId.strip;
    if (sid.empty)
        sid = ctx.currentSessionId();
    if (sid.empty)
        return ExecuteFuncResult("error: no active session and no sessionId provided", false);
    if (!isValidId(SessionId(sid)))
        return ExecuteFuncResult(format(
                "error: invalid sessionId '%s' (expected YYYYMMDD-HHMMSS-4hex)", sid), false);

    auto di = ctx.getDialogueIndex();
    if (di is null)
        return ExecuteFuncResult("error: dialogue index not available", false);

    // The agent's RAG embedder supplies the model/dimensions for the read-only
    // DB open and embeds the vector query; the worker's embedder is never
    // touched (no shared state between the agent and the indexing worker).
    auto ragCtx = cast(RAGContext) baseCtx;
    if (ragCtx is null || ragCtx.getRAG() is null)
        return ExecuteFuncResult("error: RAG not available", false);

    auto result = di.query(ragCtx.getRAG().embedder, SessionId(sid),
            params.textQuery.cleanFts5, params.vectorQuery, params.topK, params.maxTurnAge);

    if (!result.hasHistory) {
        // Distinguish engine errors (embed failure, missing embedder) from the
        // graceful no-history result (N3): errors must not look like success.
        if (result.message.startsWith("error:"))
            return ExecuteFuncResult(result.message, false);
        return ExecuteFuncResult(result.message, true);
    }

    if (result.matches.length == 0)
        return ExecuteFuncResult(format(
                "No matches found in the indexed dialogue history of session '%s'.", sid), true);

    // Render each match: session id, turn range, timestamp, rank, verbatim text.
    string rendered;
    foreach (i, m; result.matches) {
        if (i > 0)
            rendered ~= "\n\n";
        rendered ~= format("--- Match %s (session: %s, turns %s-%s, %s, rank: %.3f) ---\n", i + 1, sid,
                m.episode.turnStart, m.episode.turnEnd,
                formatTimestamp(m.episode.epochMillis), m.rank);
        rendered ~= m.text;
    }
    return ExecuteFuncResult(rendered, true);
}

/// Format an epoch-millis timestamp as YYYY-MM-DD HH:MM:SS in the local time
/// zone (fromUnixTime yields UTC; toLocalTime shifts it for display).
private string formatTimestamp(long epochMillis) {
    auto t = SysTime.fromUnixTime(epochMillis / 1000).toLocalTime;
    return format("%04d-%02d-%02d %02d:%02d:%02d", t.year, cast(int) t.month,
            t.day, t.hour, t.minute, t.second);
}

version (unittest) {
    import std.algorithm : canFind, map;
    import std.array : array, replicate;
    import std.conv : to;
    import std.file : exists, FileException, mkdirRecurse, rmdirRecurse;
    import std.json : JSONValue, parseJSON;
    import std.math : sqrt;
    import std.path : baseName;
    import std.string : count, indexOf, split, startsWith, strip, toLower;
    import std.sumtype : match;

    import my.optional;
    import my.path : AbsolutePath, Path;

    import llm.agent.context : AgentContext;
    import llm.chat : Chat, Message, Role, ToolMessage, ToolResponse, turnIdOf;
    import llm.common.config : EmbedConfig, LocalEmbedConfig, RemoteEmbedConfig, ServerConfig;
    import llm.common.embedder : Embedder, EmbedError, EmbedResult;
    import llm.config : LlmConfig, RagConfig, RagDatabaseConfig, SummaryModelConfig;
    import llm.rag.database : Database, openDatabase;
    import llm.rag.dialogue_index : DialogueIndex, encodeTopicName;
    import llm.rag.rag : Document, Origin, RAG, Topic, addToDatabase;
    import llm.summary_agent : SummaryAgent;
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
    /// the semantic query paths to rank episodes meaningfully.
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

    /// Factory that injects TestEmbedder into the worker thread (instead of
    /// the process-global factory registry, which parallel tests race on).
    private Embedder testEmbFactory(EmbedConfig config) {
        return new TestEmbedder();
    }

    /// TestEmbedder whose embed() always fails - exercises the vector-query
    /// error path (model name/dims inherited so read-only DB opens succeed).
    private class FailEmbedder : TestEmbedder {
        override EmbedResult embed(string text) {
            return EmbedResult(EmbedError("boom"));
        }
    }

    /// An AgentContext with a TestEmbedder-backed RAG and (optionally) a
    /// DialogueIndex, working under <testDir>/ctx. The RAG is registered in
    /// `testDir` so cleanup() destroys it before the dir is removed. `emb`
    /// overrides the RAG embedder (e.g. a failing one for error-path tests).
    private AgentContext makeContext(ref TestArea testDir, string activeSessionId,
            DialogueIndex di = null, bool withRag = true, Embedder emb = null) {
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
        if (di !is null)
            ctx.setDialogueIndex(di);
        return ctx;
    }

    /// A DialogueIndex over the test's area (same embedder config as before:
    /// unreachable 127.0.0.1:0 remote, 8 dims). The area is where this test's
    /// session DBs live, so worker writes and seedSessionDb land in one dir.
    private DialogueIndex makeDi(TestArea testDir) {
        return new DialogueIndex(testDir.workArea, EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
                modelName: "test", dimensions: 8)),
                RagConfig(windowOverlapPercent: 10), &testEmbFactory);
    }

    /// Seed a session's dialogue DB directly (bypassing the worker: fast and
    /// deterministic). One episode per (turn, text) under an encoded Topic
    /// name, into the test's own area: all sessions a test seeds share that
    /// area's side-by-side <sid>.db files. Closes the DB before returning so
    /// read-only opens see a committed file.
    private void seedSessionDb(string sid, long[] turns, string[] texts, TestArea testDir) {
        auto dbPath = (testDir.workArea ~ (sid ~ ".db")).AbsolutePath;
        auto dbOpt = openDatabase(dbPath, "test", 8);
        assert(hasValue(dbOpt), "seed DB must open");
        auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
        scope (exit)
            db.destroy();
        size_t nBatch;
        long epoch = 1700000000000L;
        foreach (i, t; turns) {
            auto topic = encodeTopicName(sid, t, t, epoch + i * 1000L);
            auto doc = Document(origin: Origin(Topic(topic)), data: texts[i]);
            auto res = addToDatabase(db, new TestEmbedder(), doc,
                    RagConfig(windowOverlapPercent: 10), nBatch, topic);
            assert(res.chunks > 0, "episode must index at least one chunk");
        }
        // addToDatabase fills TextChunkTbl only; the external-content FTS index
        // needs an explicit rebuild before text queries can find the episodes
        // (the worker does the same per job / drain).
        db.fts5Rebuild;
    }

    /// Number of "--- Match" blocks in a rendered tool result.
    private size_t countMatches(string msg) {
        return count(msg, "--- Match");
    }
}

// --- Test: parameter validation rejects bad input without crashing ---
unittest {
    auto testDir = testArea("param_validation");
    scope (exit)
        testDir.cleanup();
    auto di = makeDi(testDir);
    scope (exit)
        di.dispose();
    auto ctx = makeContext(testDir, TestSessionId, di);

    // Both queries empty.
    auto r = queryDialogueHistory(ctx, QueryDialogueHistoryParams());
    assert(!r.success, r.msg);
    assert(r.msg.startsWith("error:"), r.msg);

    // topK out of range: zero, negative, above the context's limit (default 20).
    const maxK = ctx.getToolLimits().maxTopK;
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "x", topK: 0));
    assert(!r.success && r.msg.startsWith("error:"), r.msg);
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "x", topK: -1));
    assert(!r.success && r.msg.startsWith("error:"), r.msg);
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "x", topK: maxK + 1));
    assert(!r.success && r.msg.startsWith("error:"), r.msg);
    // The limit itself is accepted (validated against the context, not a
    // module constant, so a config raising maxTopK is honored).
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "x", topK: maxK));
    assert(r.success, r.msg);

    // Invalid session ids are rejected explicitly (never silently ignored).
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "x",
            sessionId: "../../etc/passwd"));
    assert(!r.success && r.msg.startsWith("error:"), r.msg);
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "x",
            sessionId: "not-an-id"));
    assert(!r.success && r.msg.startsWith("error:"), r.msg);
}

// --- Test: graceful no-history and missing-dependency responses ---
unittest {
    auto testDir = testArea("graceful_no_history");
    scope (exit)
        testDir.cleanup();
    auto di = makeDi(testDir);
    scope (exit)
        di.dispose();

    // Fresh session: indexer present but nothing indexed yet.
    auto ctx = makeContext(testDir, TestSessionId, di);
    auto r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "anything"));
    assert(r.success, r.msg);
    assert(r.msg == "No dialogue history indexed for this session yet.", r.msg);

    // No active session and no explicit sessionId: a caller error, not a
    // graceful no-history - the tool cannot resolve the target session.
    auto ctx2 = makeContext(testDir, "", di);
    r = queryDialogueHistory(ctx2, QueryDialogueHistoryParams(vectorQuery: "anything"));
    assert(!r.success, r.msg);
    assert(r.msg == "error: no active session and no sessionId provided", r.msg);

    // Missing dialogue index: explicit error, no crash.
    auto ctx3 = makeContext(testDir, TestSessionId, null);
    r = queryDialogueHistory(ctx3, QueryDialogueHistoryParams(textQuery: "x"));
    assert(!r.success && r.msg.canFind("dialogue index not available"), r.msg);

    // Missing RAG: explicit error, no crash.
    auto ctx4 = makeContext(testDir, TestSessionId, di, withRag: false);
    r = queryDialogueHistory(ctx4, QueryDialogueHistoryParams(textQuery: "x"));
    assert(!r.success && r.msg.canFind("RAG not available"), r.msg);
}

// --- Test: embedder failure on the vector path is an error, not a no-history ---
unittest {
    auto testDir = testArea("embedder_failure");
    scope (exit)
        testDir.cleanup();
    seedSessionDb(TestSessionId, [1], ["PostgreSQL default port is 5432."], testDir);
    auto di = makeDi(testDir);
    scope (exit)
        di.dispose();
    auto ctx = makeContext(testDir, TestSessionId, di, true, new FailEmbedder());

    // vector-only: the embed failure must surface as an explicit error
    // (success: false), never as a graceful "no history" success.
    auto r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(vectorQuery: "anything"));
    assert(!r.success, r.msg);
    assert(r.msg.canFind("could not embed"), r.msg);

    // text+vector with a failing embedder falls back to the text path.
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "PostgreSQL",
            vectorQuery: "anything"));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 1, r.msg);
    assert(r.msg.canFind("5432"), r.msg);
}

// --- Test: indexed session with no hits reports no matches (not an error) ---
unittest {
    auto testDir = testArea("indexed_no_matches");
    scope (exit)
        testDir.cleanup();
    seedSessionDb(TestSessionId, [1], ["PostgreSQL default port is 5432."], testDir);
    auto di = makeDi(testDir);
    scope (exit)
        di.dispose();
    auto ctx = makeContext(testDir, TestSessionId, di);

    // The DB exists and has history, but the term matches nothing: a
    // successful search with an empty result set, not an error.
    auto r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "zebra"));
    assert(r.success, r.msg);
    assert(r.msg.canFind("No matches found"), r.msg);
    assert(r.msg.canFind(TestSessionId), r.msg);
}

// --- Test: unified dispatch over a seeded DB (text-only, vector-only, combined) ---
unittest {
    auto testDir = testArea("unified_dispatch");
    scope (exit)
        testDir.cleanup();
    seedSessionDb(TestSessionId, [1, 10], [
        "PostgreSQL default port is 5432.",
        "Restart nginx with systemctl restart nginx."
    ], testDir);
    auto di = makeDi(testDir);
    scope (exit)
        di.dispose();
    auto ctx = makeContext(testDir, TestSessionId, di);

    // Text-only: the FTS path matches the PostgreSQL episode exactly.
    auto r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "PostgreSQL"));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 1, r.msg);
    assert(r.msg.canFind("5432") && !r.msg.canFind("nginx"), r.msg);
    assert(r.msg.canFind("turns 1-1"), r.msg);
    assert(r.msg.canFind("session: " ~ TestSessionId), r.msg);

    // Vector-only: the semantic path returns both episodes, the PostgreSQL
    // one (sharing all query words) ranked first.
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(vectorQuery: "PostgreSQL default port is 5432"));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 2, r.msg);
    assert(r.msg.canFind("5432") && r.msg.canFind("systemctl"), r.msg);
    assert(r.msg.indexOf("turns 1-1") < r.msg.indexOf("turns 10-10"), r.msg);

    // Combined: the nginx episode hits both FTS and the vector query and
    // ranks first via the fusion score.
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "nginx",
            vectorQuery: "how to restart the nginx web server"));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 2, r.msg);
    assert(r.msg.indexOf("turns 10-10") < r.msg.indexOf("turns 1-1"), r.msg);
}

// --- Test: maxTurnAge windowing (0 and negative mean no filter) ---
unittest {
    auto testDir = testArea("max_turn_age");
    scope (exit)
        testDir.cleanup();
    seedSessionDb(TestSessionId, [1, 5, 10], [
        "record one alpha value", "record two beta value",
        "record ten gamma value"
    ], testDir);
    auto di = makeDi(testDir);
    scope (exit)
        di.dispose();
    auto ctx = makeContext(testDir, TestSessionId, di);

    // maxTurnAge 0 (the default): no age filtering, all episodes returned.
    auto r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "record"));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 3, r.msg);
    assert(r.msg.canFind("alpha") && r.msg.canFind("beta") && r.msg.canFind("gamma"), r.msg);

    // Same via the explicit default, and via a negative value.
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "record", maxTurnAge: 0));
    assert(countMatches(r.msg) == 3, r.msg);
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "record", maxTurnAge: -3));
    assert(countMatches(r.msg) == 3, r.msg);
    // An arbitrary other negative behaves like 0 (no age filtering).
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "record", maxTurnAge: -2));
    assert(countMatches(r.msg) == 3, r.msg);

    // maxTurnAge 3: only turns within 3 of the newest indexed turn (10)
    // survive the window.
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "record", maxTurnAge: 3));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 1, r.msg);
    assert(r.msg.canFind("gamma") && !r.msg.canFind("alpha") && !r.msg.canFind("beta"), r.msg);
    assert(r.msg.canFind("turns 10-10"), r.msg);
}

// --- Test: multi-noun FTS recall - every episode containing all exact nouns
// is returned verbatim (F7's promise to the prompt rule) ---
unittest {
    auto testDir = testArea("multi_noun_fts_recall");
    scope (exit)
        testDir.cleanup();
    // Each episode contains all three exact nouns in a different phrasing.
    // The textQuery with all three nouns is space separated, which cleanFts5
    // passes to FTS5 as implicit AND ("a b c" = a AND b AND c): every episode
    // that contains all the nouns is recalled. (An episode containing only a
    // SUBSET of the query nouns is NOT recalled - the boundary is asserted
    // below; it is FTS5 semantics, not a defect, and F7's recall contract
    // depends on it.)
    seedSessionDb(TestSessionId, [1, 2, 3, 20],
            [
                "The PostgreSQL log said nginx was unreachable, so I ran systemctl restart nginx.",
                "systemctl status shows both nginx and PostgreSQL are active after the reboot.",
                "We moved the PostgreSQL data and the nginx site config, then systemctl daemon-reload.",
                "PostgreSQL default port is 5432."
    ], testDir);
    auto di = makeDi(testDir);
    scope (exit)
        di.dispose();
    auto ctx = makeContext(testDir, TestSessionId, di);

    // One textQuery with all three nouns.
    auto r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "PostgreSQL nginx systemctl"));
    assert(r.success, r.msg);
    // All three full-noun episodes are recalled with their verbatim text.
    assert(countMatches(r.msg) == 3, r.msg);
    assert(r.msg.canFind("The PostgreSQL log said nginx was unreachable"), r.msg);
    assert(r.msg.canFind("We moved the PostgreSQL data and the nginx site config"), r.msg);
    assert(r.msg.canFind("turns 1-1") && r.msg.canFind("turns 2-2")
            && r.msg.canFind("turns 3-3"), r.msg);
    // Boundary: the turn-20 episode contains only a subset of the query nouns
    // (PostgreSQL, no nginx/systemctl); implicit AND does not match it. Pinning
    // this documents the recall contract: more query nouns than an episode
    // contains is not a fuzzy match.
    assert(!r.msg.canFind("5432"), r.msg);
    assert(!r.msg.canFind("turns 20-20"), r.msg);
}

// --- Test: topK caps the rendered matches AFTER the headroom over-fetch ---
unittest {
    auto testDir = testArea("topk_cap");
    scope (exit)
        testDir.cleanup();
    // Six episodes all matching one keyword: DialogueIndex over-fetches up to
    // max(topK * 10, CandidateHeadroom) rows from the DB, then applies topK
    // after the post-filter, so exactly topK matches are rendered.
    seedSessionDb(TestSessionId, [1, 2, 3, 4, 5, 6], [
        "zebra checkpoint alpha", "zebra checkpoint beta",
        "zebra checkpoint gamma", "zebra checkpoint delta",
        "zebra checkpoint epsilon", "zebra checkpoint zeta"
    ], testDir);
    auto di = makeDi(testDir);
    scope (exit)
        di.dispose();
    auto ctx = makeContext(testDir, TestSessionId, di);

    // topK 3: exactly three matches, no fourth rendered.
    auto r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "zebra", topK: 3));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 3, r.msg);
    assert(r.msg.canFind("--- Match 3") && !r.msg.canFind("--- Match 4"), r.msg);

    // The default topK (5) caps the six candidates at five.
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "zebra"));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 5, r.msg);
    assert(r.msg.canFind("--- Match 5") && !r.msg.canFind("--- Match 6"), r.msg);
}

// --- Test: windowed query with older candidates (headroom sanity) ---
unittest {
    auto testDir = testArea("windowed_headroom");
    scope (exit)
        testDir.cleanup();
    // Twelve episodes all matching "window": with maxTurnAge 3 the age filter
    // drops turns 1-8 (maxTurn 12 keeps turnEnd >= 9). The headroom over-fetch
    // must be large enough that windowing does not starve the result: the
    // newest matching episodes come back whole. (A true starvation case needs
    // more than CandidateHeadroom pre-filter candidates - impractical for a
    // unit test - so this pins the window/cap/headroom interaction with a
    // realistic candidate set.)
    seedSessionDb(TestSessionId, [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12],
            [
                "window checkpoint one", "window checkpoint two",
                "window checkpoint three", "window checkpoint four",
                "window checkpoint five", "window checkpoint six",
                "window checkpoint seven", "window checkpoint eight",
                "window checkpoint nine", "window checkpoint ten",
                "window checkpoint eleven", "window checkpoint twelve"
    ], testDir);
    auto di = makeDi(testDir);
    scope (exit)
        di.dispose();
    auto ctx = makeContext(testDir, TestSessionId, di);

    // The four in-window episodes (turns 9-12) are all returned verbatim;
    // the default topK (5) is not binding.
    auto r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "window",
            maxTurnAge: 3));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 4, r.msg);
    assert(r.msg.canFind("window checkpoint nine") && r.msg.canFind("window checkpoint ten"), r.msg);
    assert(r.msg.canFind("window checkpoint eleven")
            && r.msg.canFind("window checkpoint twelve"), r.msg);
    assert(!r.msg.canFind("window checkpoint one")
            && !r.msg.canFind("window checkpoint eight"), r.msg);

    // Cap under windowing: topK 3 renders exactly three of the four, and every
    // rendered match is in-window (no turns 1-8).
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "window",
            maxTurnAge: 3, topK: 3));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 3, r.msg);
    assert(r.msg.canFind("--- Match 3") && !r.msg.canFind("--- Match 4"), r.msg);
    assert(!r.msg.canFind("turns 1-1") && !r.msg.canFind("turns 5-5")
            && !r.msg.canFind("turns 8-8"), r.msg);
}

// --- Test: rendering carries turn range, timestamp, session id, verbatim text ---
unittest {
    import std.regex : match, regex;

    auto testDir = testArea("rendering_fields");
    scope (exit)
        testDir.cleanup();
    seedSessionDb(TestSessionId, [1], ["PostgreSQL default port is 5432."], testDir);
    auto di = makeDi(testDir);
    scope (exit)
        di.dispose();
    auto ctx = makeContext(testDir, TestSessionId, di);

    auto r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "PostgreSQL"));
    assert(r.success, r.msg);
    // Header shape: --- Match 1 (session: <sid>, turns 1-1, YYYY-MM-DD HH:MM:SS, rank: R) ---
    // FTS5 bm25 ranks are negative (more negative = better), so allow a sign.
    auto headerPattern = format(
            r"--- Match 1 \(session: %s, turns 1-1, \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}, rank: -?\d+\.\d{3}\) ---\n",
            TestSessionId);
    assert(!r.msg.match(regex(headerPattern)).empty, r.msg);
    // The verbatim episode text follows the header.
    assert(r.msg.canFind("PostgreSQL default port is 5432."), r.msg);
}

// --- Test: integration - compression checkpoint -> worker indexing -> tool query ---
unittest {
    auto testDir = testArea("integration_checkpoint_index_query");
    scope (exit)
        testDir.cleanup();
    // The worker gets the test embedder injected by makeDi (no global
    // factory registry involved).

    auto di = makeDi(testDir);
    scope (exit)
        di.dispose();

    // Wire the compression checkpoints into the indexer.
    auto agent = SummaryAgent(SummaryModelConfig(modelName: "test",
            contextSize: 8192, contextChunkSize: 8192));
    agent.setCheckpointSessionId(TestSessionId);
    agent.addCheckpointListener(&di.onCheckpoint);

    // Chat geometry (same as the checkpoint spike test): turns 1-8, the turn-5
    // reply oversized (9000 chars / 2 = 4500 tokens) -> compress evicts exactly
    // turns 1-5.
    const Answer = "The answer to the integration question is 424242. ";
    const OversizedReply = Answer ~ "x".replicate(8950);
    auto chat = Chat();
    chat.setSystemPrompt("sys");
    foreach (turn; 1 .. 9) {
        chat.addUserQuery("q" ~ turn.to!string);
        if (turn < 8) {
            const reply = (turn == 5) ? OversizedReply : "ok";
            chat.add(Message(Role.assistant, userQuery: false, content: reply, thinking: null));
        }
    }
    assert(chat.getMessages.length == 16);

    // Compress: fires the checkpoint, the indexer sends the DiJob.
    auto res = agent.compress(chat);
    assert(res.compressed);
    assert(res.newLength == 6); // 1 (system) + KeepLast (5)

    // Drain the worker: the DiJob is fully indexed (FTS flushed, DB closed
    // clean) before DiDrained arrives, so the query below is deterministic.
    di.dispose();

    // Query through the tool against the active session.
    auto ctx = makeContext(testDir, TestSessionId, di);
    auto r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "integration question"));
    assert(r.success, r.msg);
    // The exact evicted string comes back verbatim, with the turn-5 metadata.
    assert(r.msg.canFind("The answer to the integration question is 424242."), r.msg);
    assert(r.msg.canFind("turns 5-5"), r.msg);
    assert(r.msg.canFind("session: " ~ TestSessionId), r.msg);
    // The tool only reads: the chat is not re-compressed by the query.
    assert(chat.getMessages.length == 6);
}

// --- Test: item 1 tool-boundary surface ---
// A single self-contained test that drives the REAL compression path (no fake
// ChatData, C2) and asserts every documented behaviour of the queryDialogueHistory
// API surface through the tool:
//   * the real compress -> checkpoint -> worker-index -> query round trip
//   * empty params -> a well-formed !success (item 6)
//   * never-indexed session -> graceful no-history, NO "error:" prefix
//   * corrupt session DB written via File (N2) -> degrades to no-history, NOT an
//     engine error (item 3), disjoint from the "error:" form (item 4)
//   * embed failure on a session WITH history -> "error:" prefix (item 7)
//   * dispose() is idempotent and safe before/after (item 2)
unittest {
    import std.stdio : File;

    auto testDir = testArea("item1_tool_boundary");
    scope (exit)
        testDir.cleanup();
    // The worker gets the test embedder injected by makeDi (no global
    // factory registry involved).

    auto di = makeDi(testDir);

    // Drive the REAL compression path.
    auto agent = SummaryAgent(SummaryModelConfig(modelName: "test",
            contextSize: 8192, contextChunkSize: 8192));
    agent.setCheckpointSessionId(TestSessionId);
    agent.addCheckpointListener(&di.onCheckpoint);
    const Answer = "The item-1 integration answer is 987654. ";
    const Oversized = Answer ~ "x".replicate(8950);
    // Chat geometry: 8 user queries (turns 1-8) + 7 assistant replies (turns
    // 1-7; oversized at turn 5, "ok" otherwise) + 1 system prompt = 16
    // messages. Compress evicts the oversized early turns; newLength == 6.
    auto chat = Chat();
    chat.setSystemPrompt("sys");
    foreach (turn; 1 .. 9) {
        chat.addUserQuery("q" ~ turn.to!string);
        if (turn < 8) {
            const reply = (turn == 5) ? Oversized : "ok";
            chat.add(Message(Role.assistant, userQuery: false, content: reply, thinking: null));
        }
    }
    auto res = agent.compress(chat);
    assert(res.compressed);
    assert(res.newLength == 6); // 1 (system) + KeepLast (5)
    di.dispose(); // drain: DiJob fully indexed before the query below

    // (a) Item 1 core: the evicted turn comes back verbatim through the tool.
    auto ra = queryDialogueHistory(makeContext(testDir, TestSessionId, di),
            QueryDialogueHistoryParams(textQuery: "item-1 integration answer"));
    assert(ra.success, ra.msg);
    assert(ra.msg.canFind("The item-1 integration answer is 987654."), ra.msg);
    assert(ra.msg.canFind("session: " ~ TestSessionId), ra.msg);

    // (b) Item 6: empty params (both empty) is a well-formed !success.
    auto rb = queryDialogueHistory(makeContext(testDir, TestSessionId, di),
            QueryDialogueHistoryParams());
    assert(!rb.success, rb.msg);

    // (c) Never-indexed session: graceful no-history, NO "error:" prefix.
    string noHistSid = "20240101-120000-beef";
    auto rc = queryDialogueHistory(makeContext(testDir, noHistSid, di),
            QueryDialogueHistoryParams(textQuery: "anything"));
    assert(rc.success, rc.msg);
    assert(rc.msg == "No dialogue history indexed for this session yet.", rc.msg);
    assert(!rc.msg.startsWith("error:"), "no-history must not be error-prefixed: " ~ rc.msg);

    // (d) Item 3 (N2): corrupt session DB written via File -> degrades to
    //     no-history, NOT an engine error.
    string corruptSid = "20240101-120000-cafe";
    File((testDir ~ (corruptSid ~ ".db")).toString, "w").write("this is not sqlite");
    auto rd = queryDialogueHistory(makeContext(testDir, corruptSid, di),
            QueryDialogueHistoryParams(textQuery: "anything"));
    assert(rd.success, rd.msg);
    assert(rd.msg == "No dialogue history indexed for this session yet.", rd.msg);
    assert(!rd.msg.startsWith("error:"),
            "corrupt DB must degrade to no-history, not an engine error: " ~ rd.msg);

    // (e) Item 7: embed failure on a session WITH history -> "error:" prefix.
    auto rf = queryDialogueHistory(makeContext(testDir, TestSessionId, di, true,
            new FailEmbedder()), QueryDialogueHistoryParams(textQuery: "",
            vectorQuery: "anything"));
    assert(!rf.success, rf.msg);
    assert(rf.msg.startsWith("error:"), "embed failure must be error-prefixed: " ~ rf.msg);

    // (f) Item 4 (N3 boundary): the no-history form and the engine-error form are
    //     disjoint at the tool boundary.
    assert(rc.msg != rf.msg, "no-history and engine-error forms must be distinct");
    assert(!rc.msg.startsWith("error:") && rf.msg.startsWith("error:"),
            "N3 boundary: no-history has no error prefix, engine error does");

    // Item 2 (dispose idempotent at the tool level): final double dispose.
    di.dispose();
    di.dispose();
}

// --- Test: A4 partitioning keeps ToolResponse out of the dialogue projection ---
unittest {
    auto chat = Chat();
    chat.setSystemPrompt("sys");
    chat.addUserQuery("what is 2+2?");
    chat.add(ToolMessage("computing", JSONValue([JSONValue("call-math")])));
    chat.add(ToolResponse("4", "call-math", "math", true));
    JSONValue sd;
    sd["taskDoneAnswer"] = JSONValue("The answer is 4.");
    chat.add(ToolMessage("final reasoning",
            JSONValue([JSONValue("call-done")]), JSONValue.init, sd));
    chat.add(ToolResponse("done", "call-done", "taskDone", true));

    auto dialogue = chat.getDialogueHistory;
    // Dialogue projection: the user query and the final-answer ToolMessage only
    // - both ToolResponses and the non-final ToolMessage are excluded (A4).
    assert(dialogue.length == 2, "unexpected dialogue size " ~ dialogue.length.to!string);
    assert(dialogue[0].match!((Message m) => m.isUserQuery
            && m.content == "what is 2+2?", (_) => false));
    assert(dialogue[1].match!((ToolMessage m) => m.isFinalAnswer(), (_) => false));
    assert(turnIdOf(dialogue[0]) == 1);
    foreach (entry; dialogue) {
        assert(!entry.match!((ToolResponse tr) => true, (_) => false),
                "ToolResponse must never enter the dialogue projection");
    }
}

// --- Test: tool registration and JSON parameter parsing ---
unittest {
    // Registered exactly once, with a distinct name (no queryBestMatch clash).
    int n = 0;
    foreach (f; getFunctions())
        if (f.name == "queryDialogueHistory")
            ++n;
    assert(n == 1, "queryDialogueHistory must be registered exactly once");
    auto names = getFunctions().map!(f => f.name).array;
    assert(names.canFind("queryBestMatch"), "queryBestMatch must still be registered");

    // JSON args parse into the params struct; optional fields keep defaults.
    auto args = parseJSON(`{"textQuery": "nginx", "vectorQuery": "restart web server",
            "topK": 3, "maxTurnAge": -1, "sessionId": "20240101-120000-abcd"}`);
    auto parsed = initParams!QueryDialogueHistoryParams(args, toParams!QueryDialogueHistoryParams);
    assert(parsed.errorMsg.empty, parsed.errorMsg);
    assert(parsed.value.textQuery == "nginx");
    assert(parsed.value.vectorQuery == "restart web server");
    assert(parsed.value.topK == 3);
    assert(parsed.value.maxTurnAge == -1);
    assert(parsed.value.sessionId == TestSessionId);

    auto minimal = parseJSON(`{"textQuery": "a", "vectorQuery": ""}`);
    auto minimalParams = initParams!QueryDialogueHistoryParams(minimal,
            toParams!QueryDialogueHistoryParams);
    assert(minimalParams.errorMsg.empty, minimalParams.errorMsg);
    assert(minimalParams.value.topK == 5);
    assert(minimalParams.value.maxTurnAge == 0);
    assert(minimalParams.value.sessionId.empty);

    // Unknown keys are rejected by the parser.
    auto bad = parseJSON(`{"textQuery": "a", "vectorQuery": "", "bogus": 1}`);
    auto badParams = initParams!QueryDialogueHistoryParams(bad,
            toParams!QueryDialogueHistoryParams);
    assert(!badParams.errorMsg.empty);
}

// --- Test: cross-session E2E - per-session isolation, explicit targeting,
// per-DB maxTurn independence (F5) ---
unittest {
    auto testDir = testArea("cross_session_e2e");
    scope (exit)
        testDir.cleanup();
    // A second valid session id (D12), distinct from TestSessionId.
    const string SessionB = "20240101-120100-beef";
    // One exact string present in BOTH sessions' DBs (once each - the same
    // text twice in ONE session DB would hit addToDatabase's content-hash
    // dedup), plus a string unique to each session - the isolation evidence.
    const string Shared = "The deploy token is deploytoken-9090.";
    const string OnlyA = "Alphasession secret is alphaonly-1234.";
    const string OnlyB = "Betasession secret is betaonly-5678.";
    // An OLDER A episode mentioning the same "deploytoken" keyword in a
    // different phrasing (dedup-safe) so A's age window has something to drop.
    const string OlderMention = "After the outage we rotated the deploytoken credential.";

    // Both session DBs must live under ONE test area (one .db per session),
    // so seed them into the test area dir.
    // A: deep turn range 1-10; the shared string at turn 10 (the newest), an
    // older keyword-mention at turn 3, the A-unique at turn 1, filler elsewhere.
    long[] aTurns = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
    string[] aTexts = [
        OnlyA, "Alpha filler episode two.", OlderMention,
        "Alpha filler episode four.", "Alpha filler episode five.",
        "Alpha filler episode six.", "Alpha filler episode seven.",
        "Alpha filler episode eight.", "Alpha filler episode nine.", Shared
    ];
    seedSessionDb(TestSessionId, aTurns, aTexts, testDir);
    // B: shallow turn range 1-2; the B-unique at turn 1, the shared string
    // at turn 2.
    seedSessionDb(SessionB, [1, 2], [OnlyB, Shared], testDir);
    // Both session DBs exist side by side under the test area.
    assert(exists(testDir ~ (TestSessionId ~ ".db")), "session A db missing under test area");
    assert(exists(testDir ~ (SessionB ~ ".db")), "session B db missing under test area");

    auto di = makeDi(testDir);
    scope (exit)
        di.dispose();
    // Active session is A; the tool defaults to it when sessionId is empty.
    auto ctx = makeContext(testDir, TestSessionId, di);

    // (1) Explicit targeting: sessionId = B queries ONLY B's DB.
    auto r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "betaonly",
            sessionId: SessionB));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 1, r.msg);
    assert(r.msg.canFind(OnlyB), r.msg);
    assert(r.msg.canFind("session: " ~ SessionB), r.msg);
    assert(r.msg.canFind("turns 1-1"), r.msg);

    // A's unique string is absent from B's results (per-DB isolation).
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "alphaonly",
            sessionId: SessionB));
    assert(r.success, r.msg);
    assert(r.msg.canFind("No matches found"), r.msg);

    // The overlapping string, targeted at B, comes back with B's turn range
    // (turn 2), never A's (turn 3 or 10).
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "deploytoken",
            sessionId: SessionB));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 1, r.msg);
    assert(r.msg.canFind(Shared), r.msg);
    assert(r.msg.canFind("session: " ~ SessionB), r.msg);
    assert(r.msg.canFind("turns 2-2"), r.msg);
    // A's episodes (turns 3 and 10) are absent from B's results.
    assert(!r.msg.canFind("turns 3-3") && !r.msg.canFind("turns 10-10"), r.msg);
    assert(!r.msg.canFind("rotated"), r.msg);

    // (2) Active-session default: no sessionId -> A's DB only, even though B
    // contains the same overlapping string.
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "deploytoken"));
    assert(r.success, r.msg);
    // A's keyword matches: the older mention at turn 3 and the shared string
    // at turn 10 (both rendered verbatim); B's turn-2 occurrence of the same
    // shared string must never appear.
    assert(countMatches(r.msg) == 2, r.msg);
    assert(r.msg.canFind("turns 10-10") && r.msg.canFind("turns 3-3"), r.msg);
    assert(r.msg.canFind(OlderMention), r.msg);
    assert(!r.msg.canFind("turns 2-2"), r.msg);
    assert(r.msg.canFind("session: " ~ TestSessionId), r.msg);

    // B's unique string is absent from A's default query.
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "betaonly"));
    assert(r.success, r.msg);
    assert(r.msg.canFind("No matches found"), r.msg);

    // (3) Per-DB maxTurn (F5): the SAME maxTurnAge = 1 window must be
    // governed by EACH DB's own newest indexed turnEnd, never a cross-session
    // one.
    // A: maxTurn = 10 -> the window keeps turnEnd >= 9, so only the turn-10
    // shared episode survives and the turn-3 occurrence is dropped.
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "deploytoken",
            sessionId: TestSessionId, maxTurnAge: 1));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 1, r.msg);
    assert(r.msg.canFind("turns 10-10"), r.msg);
    assert(!r.msg.canFind("turns 3-3"), r.msg);
    // B: maxTurn = 2 -> the window keeps turnEnd >= 1, so the turn-2 shared
    // episode survives. (If A's maxTurn = 10 governed, B's turn 2 would be
    // filtered out and this query would report "No matches found".)
    r = queryDialogueHistory(ctx, QueryDialogueHistoryParams(textQuery: "deploytoken",
            sessionId: SessionB, maxTurnAge: 1));
    assert(r.success, r.msg);
    assert(countMatches(r.msg) == 1, r.msg);
    assert(r.msg.canFind("turns 2-2"), r.msg);
}
