/// ReasoningIndex: per-session reasoning-trace indexer.
///
/// Stateless listener: on every compression checkpoint it projects
/// the evicted reasoning (thinking + tool calls/responses) into a compact
/// per-turn trace, filters retrieval artifacts and unstamped/summary entries,
/// budgets the trace to the summary model's own effective context, and
/// sends exactly ONE RiJob to the shared dialogue worker (a WeakAddress
/// actor handle; an empty or dead handle drops the job silently). The
/// worker summarizes it off the mailbox (detached summarizer) and indexes
/// the record under an `r_` topic name; this module never writes the DB itself.
///
/// query() mirrors DialogueIndex.query with r_-only semantics.
///
/// There is no dispose(): the class is stateless and rides the shared
/// worker's drain (bounded drain deadline).
module llm.rag.reasoning_index;

import std.algorithm : canFind, map, max, min;
import std.algorithm.searching : endsWith, startsWith;
import std.array : join, split;
import std.conv : to, text;
import std.exception : collectException;
import std.format : format;
import std.json : JSONType, JSONValue;
import std.range : empty;
import std.string : cmp, indexOf;
import std.sumtype : match;

import logger = std.logger;

import my.actor;
import my.path : AbsolutePath;
import my.optional;

import llm.chat : Chat, Message, Role, ToolMessage, ToolResponse,
    VisionMessage, turnIdOf, traceOf;
import llm.common.config : ApproxTokenSize;
import llm.common.embedder : Embedder, EmbedError;
import llm.config : LlmConfig, SummaryModelConfig, ToolLimits;
import llm.endpoint : getContextSize;
import llm.rag.database : Database, openDatabase, SourceMatch, Search;
import llm.rag.dialogue_index : CompressionCheckpoint, CandidateHeadroom,
    DialogueIndex, EpisodeMeta, Kind, decodeTopicName, encodeTopicName, isSummaryMarker;
import llm.rag.dialogue_worker : RiJob;
import llm.rag.rag : Topic;
import llm.session.types : SessionId, isValidId;
import llm.tool_call : Context;
import llm.utility : summarizeToolCalls, summarizeToolResponse;

// Per-entry caps (consumer = summarizer).
immutable size_t ThinkingMaxChars = 2000;
immutable size_t ToolCallsMaxChars = 500;
immutable size_t ToolResponseMaxChars = 1000;
// Answer reserve = SummaryAgent.AnswerSize.
immutable int AnswerReserve = 8192;
immutable string OmissionMarkerFmt = "[%s older trace entries omitted for size]";
// Retrieval-artifact guard: never re-enter a trace.
immutable string[2] RetrievalToolNames = [
    "queryDialogueHistory", "queryReasoningHistory"
];

// Built-in default prompt (fallback; kept in sync with the shipped prompt file).
immutable string defaultReasoningPrompt = `
Output exactly these four sections for the reasoning trace below — nothing
else. No narrative. Do not restate user queries. Max ~150 words. Write None
for an empty section. In Justification, cover only the three most recent turns
present in the input, one sentence each.

Abandoned Hypotheses:
- <approach tried> — <why it failed>
Binding Decisions:
- <irreversible choice> — <why>
Current Uncertainties:
- <open question>
Justification:
- turn <n>: <one sentence>
- turn <n>: <one sentence>
- turn <n>: <one sentence>
`;

/// Load the reasoning summary prompt: the configured file (resolved against
/// LlmConfig.promptDir), falling back to the built-in default constant when
/// the file is missing or unreadable.
string loadReasoningPrompt(LlmConfig conf) {
    try {
        return conf.readPromptFile(conf.reasoningSummaryPrompt);
    } catch (Exception e) {
        logger.warningf("loadReasoningPrompt: using built-in default (%s)", e.msg);
        return defaultReasoningPrompt;
    }
}

/// Mirrors DialogueContext; avoids tool_call→agent cycle.
interface ReasoningContext : Context {
    ReasoningIndex getReasoningIndex();
    string currentSessionId();
    ToolLimits getToolLimits() @safe;
}

struct ReasoningMatch {
    EpisodeMeta episode;
    string text;
    double rank;
}

struct ReasoningQueryResult {
    ReasoningMatch[] matches;
    bool hasHistory;
    string message;
}

class ReasoningIndex {
    private {
        AbsolutePath dialogueDir; // session DBs shared with DialogueIndex
        SummaryModelConfig summaryCfg;
        int effectiveContext; // resolved ONCE at startup, cached
    }
    WeakAddress worker; // public: shared dialogue worker (actor handle)

    /// `worker`: the shared dialogue worker's actor handle. May be empty —
    /// an empty (or dead) handle drops the RiJob silently; jobs are
    /// dispatched by name ("riJob").
    this(AbsolutePath dir, SummaryModelConfig cfg, WeakAddress worker) {
        this.dialogueDir = dir;
        this.summaryCfg = cfg;
        this.worker = worker;
        // EXACTLY SummaryAgent's effective context;
        // resolved ONCE at startup — never per-checkpoint.
        this.effectiveContext = cast(int) min(getContextSize(cfg), cfg.contextChunkSize);
    }

    // Listener contract = DialogueIndex.onCheckpoint: fast, never
    // throws. ONE record per checkpoint.
    void onCheckpoint(const CompressionCheckpoint cp) nothrow {
        try {
            string sid = cp.sessionId;
            if (sid.empty || !isValidId(SessionId(sid))) {
                logger.warningf("ReasoningIndex: refusing checkpoint for invalid session '%s'",
                        sid);
                return;
            }
            // Degenerate context: (effectiveContext - AnswerReserve) is int
            // arithmetic, so <= AnswerReserve means a non-positive budget
            // (which would wrap to a huge size_t and never trim). No record
            // can fit one summarizer call, so skip loudly instead of
            // sending an unusable trace.
            if (effectiveContext <= AnswerReserve) {
                logger.warningf("ReasoningIndex: effective context %s <= answer reserve %s; "
                        ~ "no room for a budgeted reasoning record; skipping checkpoint",
                        effectiveContext, AnswerReserve);
                return;
            }
            // Reasoning projection + (today-empty) purged slice.
            auto trace = traceOf(cp.evictedSummarized ~ cp.evictedInPlace) ~ traceOf(
                    cp.evictedPurged);
            // Filters: drop turnIdOf(entry) == 0; drop isSummaryMarker;
            // drop ToolResponse m with m.toolName in RetrievalToolNames.
            Chat.MessageT[] filtered;
            foreach (entry; trace) {
                if (turnIdOf(entry) == 0)
                    continue;
                if (isSummaryMarker(entry))
                    continue;
                if (entry.match!((const Message _) => false, (const ToolMessage _) => false,
                        (const ToolResponse m) => canFind(RetrievalToolNames[], m.toolName),
                        (const VisionMessage _) => false))
                    continue;
                filtered ~= entry;
            }
            if (filtered.empty)
                return; // no empty records
            TraceLine[] lines = renderTrace(filtered);
            size_t budget = (effectiveContext - AnswerReserve) * ApproxTokenSize;
            size_t omitted = 0;
            while (lines.length > 1 && totalChars(lines) > budget) {
                lines = lines[1 .. $];
                omitted++;
            }
            if (lines.length == 1 && lines[0].text.length > budget)
                lines[0].text = lines[0].text[0 .. budget]; // hard-cap singleton
            if (lines.empty)
                return;
            if (omitted > 0)
                lines = [
                TraceLine(format(OmissionMarkerFmt, omitted), lines[0].turnId)
            ] ~ lines;
            // turnStart/turnEnd = min/max turnId over INCLUDED lines (empty ⇒
            // returned above; cp fallback subsumed).
            long turnStart = lines[0].turnId, turnEnd = lines[0].turnId;
            foreach (l; lines[1 .. $]) {
                if (l.turnId < turnStart)
                    turnStart = l.turnId;
                if (l.turnId > turnEnd)
                    turnEnd = l.turnId;
            }
            string traceText = map!(l => l.text)(lines).join("\n");
            // Dispatch by name to the shared dialogue worker's actor; an
            // empty or dead handle drops the RiJob silently.
            if (!worker.empty)
                dynSend(worker, "riJob", RiJob(sid.idup, traceText.idup, turnStart, turnEnd));
            // Counts and ids only — NEVER trace content.
            logger.tracef(
                    "ReasoningIndex: sent reasoning trace for session '%s' turns %s-%s: %s lines, %s chars, %s omitted",
                    sid, turnStart, turnEnd, lines.length, traceText.length, omitted);
        } catch (Exception e) {
            logger.errorf("reasoning checkpoint failure: %s", e.msg).collectException;
        }
    }

    struct TraceLine {
        string text;
        long turnId;
    }

    /// Total characters across the lines including the newlines between them
    /// (Σ len+1, minus 1); 0 for an empty slice.
    private static size_t totalChars(const TraceLine[] ls) {
        size_t t = 0;
        foreach (l; ls)
            t += l.text.length + 1;
        return t == 0 ? 0 : t - 1;
    }

    // A dedicated formatter (NOT the summary agent's message-text formatter — it
    // emits whole content; trace carries thinking ONLY); reuses llm.utility.
    // Line shapes mirror the summary agent's (interp i"..."; hard cap
    // [0 .. min(len, cap)]); catch-all: skip.
    private static TraceLine[] renderTrace(const Chat.MessageT[] filtered) {
        TraceLine[] lines;
        foreach (entry; filtered) {
            long tid = turnIdOf(entry);
            lines ~= entry.match!((const Message m) {
                TraceLine[] r;
                if (!m.thinking.empty)
                    r ~= TraceLine(i"[t$(tid) $(m.role) thinking] ".text ~ m.thinking[0 .. min(m.thinking.length,
                        ThinkingMaxChars)], tid);
                return r;
            }, (const ToolMessage m) {
                TraceLine[] r;
                string calls = summarizeToolCalls(m.toolCalls, ToolCallsMaxChars).join(", ");
                r ~= TraceLine(i"[t$(tid) assistant tool_calls] ".text ~ calls[0 .. min(calls.length,
                    ToolCallsMaxChars)], tid);
                if (!m.thinking.empty)
                    r ~= TraceLine(i"[t$(tid) assistant thinking] ".text ~ m.thinking[0 .. min(m.thinking.length,
                        ThinkingMaxChars)], tid);
                return r;
            }, (const ToolResponse m) {
                return [
                    TraceLine(i"[t$(tid) tool $(m.toolName)] ".text ~ summarizeToolResponse(m,
                        ToolResponseMaxChars), tid)
                ];
            }, (const VisionMessage _) {
                // Defensive: traceOf excludes vision entries.
                return [TraceLine(i"[t$(tid) vision] [image]".text, tid)];
            });
        }
        return lines;
    }

    /// r_-only query mirroring DialogueIndex.query,
    /// with ONLY these differences:
    ///  1. Returns ReasoningQueryResult/ReasoningMatch; null embedder →
    ///     "error: reasoning query requires an embedder".
    ///  2. No-history notice (invalid sid / missing DB / count==0):
    ///     "No reasoning history indexed for this session yet."
    ///  3. No-history count = decodable r_ topics; 0 → no-history result.
    ///  4. Post-filter drops non-reasoning candidates.
    ///  5. computeMaxTurn → DialogueIndex.computeMaxTurn; everything else
    ///     (topK clamp, read-only openDatabase, embed, dbLimit, dispatch,
    ///     age window, topK cap AFTER filter) IDENTICAL.
    ///
    /// Never throws: DB open failure → no-history result; parse failures →
    /// entry dropped. An invalid session id is treated as no-history.
    ReasoningQueryResult query(Embedder qEmbedder, SessionId sessionId,
            string textQuery, string vectorQuery, long topK = 10, long maxTurnAge = 0) {
        if (qEmbedder is null) {
            logger.warning("ReasoningIndex: query without an embedder");
            return ReasoningQueryResult(null, false,
                    "error: reasoning query requires an embedder");
        }
        if (!isValidId(sessionId)) {
            logger.warningf("ReasoningIndex: refusing query for invalid session '%s'",
                    sessionId.to!string);
            // Defense in depth: the tool validates first, so this is
            // unreachable from queryReasoningHistory. Returned as a
            // no-history notice (not an "error:" message) on purpose - an
            // invalid id must never leak into a DB path.
            return ReasoningQueryResult(null, false,
                    "No reasoning history indexed for this session yet.");
        }
        if (topK < 1)
            topK = 1; // clamp: negative/zero topK must never slice with a bad bound
        auto dbOpt = openDatabase((dialogueDir ~ (sessionId.to!string ~ ".db"))
                .AbsolutePath, qEmbedder.modelName(), qEmbedder.dimensions(), readOnly: true);

        if (!hasValue(dbOpt)) {
            return ReasoningQueryResult(null, false,
                    "No reasoning history indexed for this session yet.");
        }

        auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
        scope (exit)
            db.destroy();

        // Check if the DB has any REASONING (r_) sources. No-history
        // detection is kind-specific - a session whose DB holds only d_
        // (dialogue) topics reports no reasoning history.
        size_t sourceCount;
        try {
            foreach (src; db.getSources) {
                src.origin.match!((Topic t) {
                    auto metaOpt = decodeTopicName(t.name);
                    if (hasValue(metaOpt)
                        && metaOpt.match!(m => m.kind == Kind.reasoning, (_) => false))
                        sourceCount++;
                }, (_) {});
            }
        } catch (Exception e) {
            logger.tracef("getSources failed: %s", e.msg);
            return ReasoningQueryResult(null, false,
                    "No reasoning history indexed for this session yet.");
        }
        if (sourceCount == 0) {
            return ReasoningQueryResult(null, false,
                    "No reasoning history indexed for this session yet.");
        }

        // Embed the vector query with the caller-supplied (agent) embedder.
        float[] embedded;
        bool embedFailed;
        string embedError;
        if (!vectorQuery.empty) {
            qEmbedder.embedQuery(vectorQuery).match!((float[] v) { embedded = v; }, (EmbedError e) {
                embedFailed = true;
                embedError = e.errorMsg;
                logger.tracef("ReasoningIndex.query: embed failed: %s", e.errorMsg);
            });
        }

        // Query the DB with candidate headroom: the maxTurnAge window, the
        // unparseable-topic filter, and the kind filter can drop
        // candidates, so topK is applied AFTER post-filtering,
        // not by the DB LIMIT alone.
        const long dbLimit = max(topK * 10, CandidateHeadroom);

        // Dispatch the appropriate search
        SourceMatch[] raw;
        if (!textQuery.empty && !embedFailed && !vectorQuery.empty) {
            raw = db.queryCombineSemanticText(Search(embedded), textQuery, dbLimit);
        } else if (!textQuery.empty) {
            raw = db.queryTextSearch(textQuery, dbLimit);
        } else if (!embedFailed && !vectorQuery.empty) {
            raw = db.querySemantic(Search(embedded), dbLimit);
        } else if (!vectorQuery.empty) {
            return ReasoningQueryResult(null, false,
                    "error: could not embed vectorQuery: " ~ embedError);
        } else {
            return ReasoningQueryResult(null, false, "No query provided.");
        }

        // Compute maxTurn for the age window
        long maxTurn = 0;
        if (maxTurnAge > 0) {
            maxTurn = DialogueIndex.computeMaxTurn(db);
        }

        // Post-filter: parse topic, apply age window
        ReasoningMatch[] matches;
        foreach (sm; raw) {
            auto metaOpt = sm.origin.match!((Topic t) => decodeTopicName(t.name),
                    (_) => none!EpisodeMeta());
            if (!hasValue(metaOpt))
                continue; // drop unparseable topics

            auto meta = metaOpt.match!((EpisodeMeta m) => m, (None _) => EpisodeMeta.init);

            if (meta.kind != Kind.reasoning)
                continue; // reasoning queries never surface dialogue records

            // Apply maxTurnAge window
            if (maxTurnAge > 0 && maxTurn > 0) {
                if (meta.turnEnd < maxTurn - maxTurnAge)
                    continue;
            }

            matches ~= ReasoningMatch(meta, sm.text, sm.rank);
        }

        // Cap at topK (after filtering)
        if (matches.length > topK)
            matches = matches[0 .. topK];

        return ReasoningQueryResult(matches, true, "");
    }
}

version (unittest) {
    import core.time : dur;
    import std.datetime : Clock;
    import std.json : JSONValue;
    import llm.common.config : ServerConfig, EmbedConfig, RemoteEmbedConfig;
    import llm.common.embedder : Embedder, EmbedResult, EmbedError;
    import llm.config : RagConfig;
    import llm.rag.dialogue_worker : CompletionGate, DiDrained, ReasoningDrainBudget;
    import llm.rag.rag : Origin, addToDatabase, Document;
    import llm.test_util : TestArea, testArea;
    import my.path : Path;

    /// Deterministic test embedder: returns an all-ones 8-dim vector.
    private class RiTestEmbedder : Embedder {
        override void destroy() {
        }

        override string modelName() {
            return "ri-test";
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

        override int[] tokenize(string text, bool addSpecial) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            // The reasoning topic prefix ("Topic: r_<sid>__t<s>_<e>__<epoch> | ")
            // is the same length as the dialogue prefix; 150 keeps the
            // effective window >= 95 graphemes so short test records index as
            // one verbatim chunk.
            return 150;
        }
    }

    /// Factory that injects the test embedder into the worker actor (instead
    /// of the process-global factory registry, which parallel tests race on).
    private Embedder riTestFactory(EmbedConfig config) {
        return new RiTestEmbedder();
    }

    /// Fake summarizer: fixed record text; proves a ctor-supplied SummarizerFn
    /// reaches the spawned worker's reasoning path.
    private string riFakeSummarizer(string prompt, string traceText) {
        return "RI_FAKE_RECORD_T4";
    }

    /// Helper: create a temp dir and return config.
    private struct RiTestSetup {
        TestArea tmpDir;
        EmbedConfig cfg;
        RagConfig ragCfg;
    }

    private RiTestSetup setupTest(string testName, string file = __FILE__, uint line = __LINE__) {
        auto tmpDir = testArea(testName, file, line);

        auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
                modelName: "ri-test", dimensions: 8));
        auto ragCfg = RagConfig(windowOverlapPercent: 10);

        return RiTestSetup(tmpDir, cfg, ragCfg);
    }

    private void teardownTest(ref RiTestSetup s) {
        s.tmpDir.cleanup;
    }

    /// Query-path embedder: modelName/dimensions must match the seed writer's
    /// so read-only DB opens succeed.
    private RiTestEmbedder qEmb() {
        return new RiTestEmbedder();
    }

    /// Seed a session's DB directly with one encoded topic (bypassing the
    /// worker), mirroring seedTopicDb in dialogue_index.d: open the
    /// per-session DB with the query embedder's model/dimensions, index the
    /// text, rebuild the FTS index, then checkpoint + close so a read-only
    /// open sees a clean, sidecar-free DB.
    private void seedTopicDb(RiTestSetup s, string sid, string topic, string text) {
        auto qe = qEmb();
        auto dbOpt = openDatabase((s.tmpDir ~ (sid ~ ".db")).AbsolutePath,
                qe.modelName(), qe.dimensions(), readOnly: false);
        assert(hasValue(dbOpt), "seed DB must open");
        auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
        size_t nBatch;
        auto doc = Document(origin: Origin(Topic(topic)), data: text);
        auto res = addToDatabase(db, qe, doc, s.ragCfg, nBatch, topic);
        assert(res.chunks > 0, "seeded record must index at least one chunk");
        db.fts5Rebuild;
        db.run("PRAGMA wal_checkpoint(TRUNCATE);");
        db.destroy();
    }

    /// RiJob capture store. The capture actor acts as the "worker" for the
    /// ReasoningIndex under test (its address is passed to the ctor); on
    /// receiving an RiJob it acks its supervisor so the test can
    /// synchronize before reading the store.
    struct RiJobCapture {
        RiJob job;
        bool got;
    }

    /// Stand-in worker for the capture tests: stores the RiJob it receives
    /// (dispatched by name, like the real dialogue worker) and acks the
    /// supervisor. The ack orders the store write before the test reads it.
    final class RiJobCaptureActor {
        private {
            shared(RiJobCapture)* cap_;
            WeakAddress out_;
        }

        this(shared(RiJobCapture)* cap, WeakAddress outAddr) {
            this.cap_ = cap;
            this.out_ = outAddr;
        }

        void riJob(RiJob j) {
            (*cap_).job = j;
            (*cap_).got = true;
            dynSend(out_, "capCk", "cap-ack");
        }
    }

    /// Test seam: a Logger that captures formatted messages so the
    /// fallback test can assert the warning. Installed via the std.logger
    /// `sharedLog` swap (same pattern as D7LogCapture in dialogue_worker.d);
    /// thread-safe because other tests' threads may log concurrently.
    private class T6LogCapture : logger.Logger {
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

// renderTrace tests
unittest {
    // Message with thinking → single thinking line, exact expected shape.
    auto m = Message(Role.assistant, false, "", "thinking body");
    m.turnId = 5;
    auto lines = ReasoningIndex.renderTrace([Chat.MessageT(m)]);
    assert(lines.length == 1, "expected one thinking line, got %s".format(lines.length));
    assert(lines[0].text == "[t5 assistant thinking] thinking body",
            "unexpected line: " ~ lines[0].text);
    assert(lines[0].turnId == 5);

    // Message without thinking → skipped.
    auto bare = Message(Role.assistant, false, "content only", "");
    bare.turnId = 6;
    assert(ReasoningIndex.renderTrace([Chat.MessageT(bare)]).empty,
            "content-only Message must produce no line");
}

unittest {
    // ToolMessage → tool_calls line THEN thinking line (order matters).
    JSONValue fcall;
    fcall["name"] = "readFile";
    fcall["arguments"] = "{}";
    JSONValue fn;
    fn["function"] = fcall;
    JSONValue calls;
    calls.array = [fn];
    auto tm = ToolMessage("tm thinking", calls, JSONValue.init, JSONValue.init);
    tm.turnId = 4;
    auto lines = ReasoningIndex.renderTrace([Chat.MessageT(tm)]);
    assert(lines.length == 2, "expected tool_calls + thinking, got %s".format(lines.length));
    assert(lines[0].text == "[t4 assistant tool_calls] readFile()",
            "unexpected tool_calls line: " ~ lines[0].text);
    assert(lines[1].text == "[t4 assistant thinking] tm thinking",
            "unexpected thinking line: " ~ lines[1].text);
    assert(lines[0].turnId == 4 && lines[1].turnId == 4);

    // ToolMessage without thinking → tool_calls line only.
    auto tm2 = ToolMessage("", JSONValue(JSONType.array), JSONValue.init, JSONValue.init);
    tm2.turnId = 9;
    auto lines2 = ReasoningIndex.renderTrace([Chat.MessageT(tm2)]);
    assert(lines2.length == 1, "expected one line, got %s".format(lines2.length));
    assert(lines2[0].text == "[t9 assistant tool_calls] Wants to run: <unknown>",
            "unexpected line: " ~ lines2[0].text);
}

unittest {
    // ToolResponse → tool line with the summarized response.
    auto tr = ToolResponse("file contents here", "call_1", "readFile", true);
    tr.turnId = 7;
    auto lines = ReasoningIndex.renderTrace([Chat.MessageT(tr)]);
    assert(lines.length == 1, "expected one tool line, got %s".format(lines.length));
    assert(lines[0].text == "[t7 tool readFile] file contents here",
            "unexpected line: " ~ lines[0].text);
}

unittest {
    // Caps: over-long thinking is hard-cut to ThinkingMaxChars.
    string longThinking;
    foreach (i; 0 .. 2500)
        longThinking ~= "x";
    auto m = Message(Role.assistant, false, "", longThinking);
    m.turnId = 8;
    auto lines = ReasoningIndex.renderTrace([Chat.MessageT(m)]);
    assert(lines.length == 1);
    assert(lines[0].text.length == 24 + ThinkingMaxChars,
            "capped line must be prefix + exactly %s chars, got %s".format(ThinkingMaxChars,
                lines[0].text.length));
    assert(lines[0].text.startsWith("[t8 assistant thinking] "),
            "cap must keep the shape prefix: " ~ lines[0].text[0 .. 24]);

    // Caps: over-long tool response is cut by summarizeToolResponse.
    string longContent;
    foreach (i; 0 .. 1500)
        longContent ~= "y";
    auto tr = ToolResponse(longContent, "call_2", "writeFile", true);
    tr.turnId = 10;
    auto tl = ReasoningIndex.renderTrace([Chat.MessageT(tr)]);
    assert(tl.length == 1);
    // "[t10 tool writeFile] " (20) + ToolResponseMaxChars + "... (1500 chars)"
    assert(tl[0].text.endsWith("... (1500 chars)"),
            "capped tool line must carry the summary suffix: "
            ~ tl[0].text[tl[0].text.length - 30 .. $]);
}

@("Budget tests (capture actor receiving RiJob")
unittest {
    // effectiveContext 16384 → budget (16384-8192)*2 = 16384 chars. Nine
    // single-digit-tid thinking lines of 2024 chars each total 18224, so
    // exactly the OLDEST line is dropped and the marker carries N=1.
    auto s = setupTest("reasoning_budget_test");
    scope (exit)
        teardownTest(s);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    RiJobCapture capStore;
    auto cap = sys.spawn!RiJobCaptureActor(cast(shared(RiJobCapture)*)&capStore, sup.address());

    auto ri = new ReasoningIndex(s.tmpDir, SummaryModelConfig(contextSize: 16384), cap.weakRef);
    string sid = "20240101-120000-bad1";
    Chat.MessageT[] evicted;
    foreach (i; 1 .. 10) { // tids 1..9
        string th;
        foreach (j; 0 .. 2000)
            th ~= "x";
        auto m = Message(Role.assistant, false, "", th);
        m.turnId = i;
        evicted ~= Chat.MessageT(m);
    }
    auto cp = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: sid, evictedSummarized: evicted,
            evictedPurged: null, evictedInPlace: null, turnStart: 1, turnEnd: 9,
            summaryText: "", originalLength: 9, newLength: 0, newContextSize: 0);
    ri.onCheckpoint(cp);

    // Wait for the capture actor's ack (sent after receiving the RiJob)
    // before reading the store.
    assert(sup.receiveTimeout(2.dur!"seconds", (string s) {
            assert(s == "cap-ack");
        }), "capture actor must ack");
    assert(capStore.got, "RiJob must be sent for a budgeted trace");
    assert(capStore.job.sessionId == sid, "sessionId mismatch: " ~ capStore.job.sessionId);
    assert(capStore.job.turnStart == 2 && capStore.job.turnEnd == 9,
            "range must be min..max of INCLUDED lines, got %s-%s".format(
                capStore.job.turnStart, capStore.job.turnEnd));
    auto textLines = capStore.job.traceText.split("\n");
    assert(textLines.length == 9, "expected 1 marker + 8 lines, got %s".format(textLines.length));
    assert(textLines[0] == "[1 older trace entries omitted for size]",
            "marker must carry the omitted count: " ~ textLines[0]);
    assert(textLines[1].startsWith("[t2 assistant thinking] "),
            "oldest kept line must be t2: " ~ textLines[1][0 .. 24]);
}

unittest {
    // Degenerate budget (ctx 9000 → 1616 chars < one line): the singleton
    // line is hard-capped to exactly the budget.
    auto s = setupTest("reasoning_singleton_cap_test");
    scope (exit)
        teardownTest(s);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    RiJobCapture capStore;
    auto cap = sys.spawn!RiJobCaptureActor(cast(shared(RiJobCapture)*)&capStore, sup.address());

    auto ri = new ReasoningIndex(s.tmpDir, SummaryModelConfig(contextSize: 9000), cap.weakRef);
    string longThinking;
    foreach (i; 0 .. 2000)
        longThinking ~= "x";
    auto m = Message(Role.assistant, false, "", longThinking);
    m.turnId = 3;
    auto cp = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: "20240101-120000-bad2", evictedSummarized: [
        Chat.MessageT(m)
    ], evictedPurged: null, evictedInPlace: null, turnStart: 3, turnEnd: 3,
        summaryText: "", originalLength: 1, newLength: 0, newContextSize: 0);
    ri.onCheckpoint(cp);

    assert(sup.receiveTimeout(2.dur!"seconds", (string s) {
            assert(s == "cap-ack");
        }), "capture actor must ack");
    assert(capStore.got, "RiJob must be sent even for a hard-capped singleton");
    assert(capStore.job.turnStart == 3 && capStore.job.turnEnd == 3);
    size_t budget = (9000 - AnswerReserve) * ApproxTokenSize;
    assert(capStore.job.traceText.length == budget,
            "singleton must be hard-capped to the budget, got %s (budget %s)".format(
                capStore.job.traceText.length, budget));
    assert(capStore.job.traceText.startsWith("[t3 assistant thinking] "),
            "cap must keep the shape prefix");
}

// onCheckpoint filter + idup tests
unittest {
    // Valid sid: artifacts (retrieval ToolResponse), unstamped (turnId==0)
    // and summary-marker entries are DROPPED; the payload is idup'd and the
    // range spans the included lines only.
    auto s = setupTest("reasoning_on_checkpoint_filter_test");
    scope (exit)
        teardownTest(s);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    RiJobCapture capStore;
    auto cap = sys.spawn!RiJobCaptureActor(cast(shared(RiJobCapture)*)&capStore, sup.address());

    auto ri = new ReasoningIndex(s.tmpDir, SummaryModelConfig(contextSize: 16384), cap.weakRef);
    string sid = "20240101-120000-ab01";

    auto good1 = Message(Role.assistant, false, "", "first thinking");
    good1.turnId = 2;
    auto good2 = Message(Role.assistant, false, "", "seventh thinking");
    good2.turnId = 7;

    // Retrieval artifact: must never re-enter a trace.
    auto artifact = ToolResponse("old retrieval payload", "call_x", "queryDialogueHistory", true);
    artifact.turnId = 5;

    // Unstamped entry (turnId == 0).
    auto unstamped = Message(Role.assistant, false, "", "unstamped thinking");

    // Summary marker: thinking + summary_turn_start in saveData.
    JSONValue sd;
    sd["summary_turn_start"] = 2;
    auto marker = Message(Role.assistant, false, "", "marker thinking", JSONValue.init, sd);
    marker.turnId = 3;

    auto cp = CompressionCheckpoint(timestamp: Clock.currTime, sessionId: sid,
            evictedSummarized: [
                Chat.MessageT(good1), Chat.MessageT(artifact),
                Chat.MessageT(unstamped), Chat.MessageT(marker),
                Chat.MessageT(good2)
    ], evictedPurged: null, evictedInPlace: null, turnStart: 2, turnEnd: 7,
            summaryText: "", originalLength: 5, newLength: 0, newContextSize: 0);
    ri.onCheckpoint(cp);

    assert(sup.receiveTimeout(2.dur!"seconds", (string s) {
            assert(s == "cap-ack");
        }), "capture actor must ack");
    assert(capStore.got, "RiJob must be sent for the surviving entries");
    assert(capStore.job.sessionId == sid);
    assert(capStore.job.turnStart == 2 && capStore.job.turnEnd == 7,
            "range must cover included lines only, got %s-%s".format(capStore.job.turnStart,
                capStore.job.turnEnd));
    // idup'd payload: the captured strings must not alias this test's locals
    // (onCheckpoint's stack strings are gone by the time the actor reads).
    assert(capStore.job.sessionId.cmp(sid) == 0);
    string expected = "[t2 assistant thinking] first thinking\n[t7 assistant thinking] seventh thinking";
    assert(capStore.job.traceText.ptr != expected.ptr,
            "traceText must be an idup'd copy, not an alias");
    assert(capStore.job.traceText == expected,
            "traceText must carry exactly the included lines, got: " ~ capStore.job.traceText);
}

unittest {
    // Invalid sid and all-filtered checkpoints → NO RiJob (silent for the
    // all-filtered case; the invalid-sid case logs a warning).
    auto s = setupTest("reasoning_on_checkpoint_nosend_test");
    scope (exit)
        teardownTest(s);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    RiJobCapture capStore;
    auto cap = sys.spawn!RiJobCaptureActor(cast(shared(RiJobCapture)*)&capStore, sup.address());

    auto ri = new ReasoningIndex(s.tmpDir, SummaryModelConfig(contextSize: 16384), cap.weakRef);

    // Invalid sid.
    auto bad = Message(Role.assistant, false, "", "thinking");
    bad.turnId = 1;
    ri.onCheckpoint(CompressionCheckpoint(timestamp: Clock.currTime, sessionId: "invalid",
            evictedSummarized: [Chat.MessageT(bad)], evictedPurged: null,
            evictedInPlace: null, turnStart: 1, turnEnd: 1,
            summaryText: "", originalLength: 1, newLength: 0, newContextSize: 0));

    // All entries filtered (retrieval artifact only).
    auto artifact = ToolResponse("payload", "call_y", "queryReasoningHistory", true);
    artifact.turnId = 2;
    ri.onCheckpoint(CompressionCheckpoint(timestamp: Clock.currTime, sessionId: "20240101-120000-ab02",
            evictedSummarized: [Chat.MessageT(artifact)], evictedPurged: null,
            evictedInPlace: null, turnStart: 2, turnEnd: 2, summaryText: "",
            originalLength: 1, newLength: 0, newContextSize: 0));

    // Neither checkpoint sent an RiJob: the capture actor acks only on
    // receiving one, so no ack may arrive within the window.
    bool acked = false;
    sup.receiveTimeout(2.dur!"seconds", (string _) { acked = true; });
    assert(!acked, "no RiJob was sent; a cap-ack implies one arrived");
    assert(!capStore.got, "invalid session and all-filtered checkpoints must not send an RiJob");
}

unittest {
    // Degenerate context (effectiveContext <= AnswerReserve): the int
    // budget would be non-positive (wrapping to a huge size_t that never
    // trims), so onCheckpoint skips with a warning — no RiJob for ctx
    // 4096 or exactly 8192 (valid sids, so this exercises the guard, not
    // the invalid-sid path).
    auto s = setupTest("reasoning_degenerate_context_test");
    scope (exit)
        teardownTest(s);

    Chat.MessageT[] evicted;
    foreach (i; 1 .. 3) {
        string th;
        foreach (j; 0 .. 2000)
            th ~= "x";
        auto m = Message(Role.assistant, false, "", th);
        m.turnId = i;
        evicted ~= Chat.MessageT(m);
    }

    long[] smallContexts = [4096, 8192];
    foreach (idx; 0 .. smallContexts.length) {
        long ctx = smallContexts[idx];
        auto sys = makeSystem;
        scope (exit)
            sys.shutdown();
        auto sup = scopedActor;
        RiJobCapture capStore;
        auto cap = sys.spawn!RiJobCaptureActor(cast(shared(RiJobCapture)*)&capStore,
                sup.address());
        auto ri = new ReasoningIndex(s.tmpDir, SummaryModelConfig(contextSize: ctx), cap.weakRef);
        ri.onCheckpoint(CompressionCheckpoint(timestamp: Clock.currTime,
                sessionId: "20240101-120000-dea" ~ idx.to!string,
                evictedSummarized: evicted, evictedPurged: null, evictedInPlace: null, turnStart: 1, turnEnd: 2,
                summaryText: "", originalLength: 2, newLength: 0, newContextSize: 0));
        // No RiJob was sent: the capture actor acks only on receiving one,
        // so no ack may arrive within the window.
        bool acked = false;
        sup.receiveTimeout(2.dur!"seconds", (string _) { acked = true; });
        assert(!acked, "no RiJob was sent (ctx %s); a cap-ack implies one arrived".format(ctx));
        assert(!capStore.got,
                "ctx %s <= AnswerReserve must not send an RiJob (budget would wrap)".format(ctx));
    }
}

// query tests (TestArea + fake embedder; seeded DBs)
unittest {
    // Mixed DB (d_ + r_ topics): only r_ matches surface; a d_-only query
    // term yields hasHistory true with zero matches (kind filter).
    auto s = setupTest("reasoning_query_mixed_db_test");
    scope (exit)
        teardownTest(s);

    auto ri = new ReasoningIndex(s.tmpDir, SummaryModelConfig(contextSize: 16384), WeakAddress.init);
    string sid = "20240101-120000-cafe";
    seedTopicDb(s, sid, encodeTopicName(sid, 5, 5, 1700000000001, Kind.reasoning),
            "REASONING_TOKEN abandoned hypothesis binding decision record");
    seedTopicDb(s, sid, encodeTopicName(sid, 9, 9, 1700000000002),
            "DIALOGUE_TOKEN user asked and the agent answered");

    auto result = ri.query(qEmb(), SessionId(sid), "REASONING_TOKEN", "");
    assert(result.hasHistory, "mixed DB must report reasoning history");
    assert(result.matches.length == 1, "exactly one r_ match, got %s".format(result.matches.length));
    assert(result.matches[0].episode.kind == Kind.reasoning);
    assert(result.matches[0].episode.sessionId == sid);
    assert(result.matches[0].episode.turnStart == 5 && result.matches[0].episode.turnEnd == 5);
    assert(result.matches[0].text.indexOf("REASONING_TOKEN") >= 0);

    // d_-only term: the d_ candidate is post-filtered out.
    auto dResult = ri.query(qEmb(), SessionId(sid), "DIALOGUE_TOKEN", "");
    assert(dResult.hasHistory, "history exists (r_ topics are present)");
    assert(dResult.matches.empty, "d_ candidates must be filtered by the kind filter");
    assert(dResult.message == "");
}

unittest {
    // d_-only DB → no reasoning history (kind-specific detection).
    auto s = setupTest("reasoning_query_donly_db_test");
    scope (exit)
        teardownTest(s);

    auto ri = new ReasoningIndex(s.tmpDir, SummaryModelConfig(contextSize: 16384), WeakAddress.init);
    string sid = "20240101-120000-feed";
    seedTopicDb(s, sid, encodeTopicName(sid, 5, 5, 1700000000003),
            "DIALOGUE_TOKEN only dialogue here");

    auto result = ri.query(qEmb(), SessionId(sid), "DIALOGUE_TOKEN", "");
    assert(!result.hasHistory, "d_-only DB must report no reasoning history");
    assert(result.message == "No reasoning history indexed for this session yet.");
    assert(result.matches.empty);
}

unittest {
    // maxTurnAge window: with maxTurn 10 and maxTurnAge 2, the turnEnd-5
    // r_ record is dropped, the turnEnd-10 one survives.
    auto s = setupTest("reasoning_query_max_turn_age_test");
    scope (exit)
        teardownTest(s);

    auto ri = new ReasoningIndex(s.tmpDir, SummaryModelConfig(contextSize: 16384), WeakAddress.init);
    string sid = "20240101-120000-f00d";
    seedTopicDb(s, sid, encodeTopicName(sid, 5, 5, 1700000000004,
            Kind.reasoning), "AGE_TOKEN old record");
    seedTopicDb(s, sid, encodeTopicName(sid, 10, 10, 1700000000005,
            Kind.reasoning), "AGE_TOKEN new record");

    auto noWindow = ri.query(qEmb(), SessionId(sid), "AGE_TOKEN", "");
    assert(noWindow.matches.length == 2,
            "no window: both r_ records, got %s".format(noWindow.matches.length));

    auto windowed = ri.query(qEmb(), SessionId(sid), "AGE_TOKEN", "", 10, 2);
    assert(windowed.matches.length == 1,
            "window must keep only the recent record, got %s".format(windowed.matches.length));
    assert(windowed.matches[0].episode.turnEnd == 10,
            "survivor must be the turnEnd-10 record, got %s".format(
                windowed.matches[0].episode.turnEnd));
}

unittest {
    // Error / no-history paths: null embedder, invalid sid, missing DB.
    auto s = setupTest("reasoning_query_error_paths_test");
    scope (exit)
        teardownTest(s);

    auto ri = new ReasoningIndex(s.tmpDir, SummaryModelConfig(contextSize: 16384), WeakAddress.init);

    Embedder nullEmb;
    auto noEmb = ri.query(nullEmb, SessionId("20240101-120000-cafe2"), "anything", "");
    assert(!noEmb.hasHistory);
    assert(noEmb.message == "error: reasoning query requires an embedder",
            "unexpected message: " ~ noEmb.message);

    auto badSid = ri.query(qEmb(), SessionId("invalid"), "anything", "");
    assert(!badSid.hasHistory);
    assert(badSid.message == "No reasoning history indexed for this session yet.");

    auto missingDb = ri.query(qEmb(), SessionId("20240101-120000-cafe3"), "anything", "");
    assert(!missingDb.hasHistory);
    assert(missingDb.message == "No reasoning history indexed for this session yet.");
}

// DialogueIndex ctor forwarding (worker contract, dialogue_index.d)
unittest {
    // Actor ctor with defaults (sys + factory only): every optional param
    // defaults to its neutral value — the worker starts and drains cleanly.
    auto s = setupTest("dialogue_ctor_defaults_test");
    scope (exit)
        teardownTest(s);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &sys, &riTestFactory);
    scope (exit)
        di.dispose();
    auto sup = scopedActor;
    auto gate = new CompletionGate;
    di.beginDispose(sup.address(), gate);
    assert(sup.receiveTimeout(ReasoningDrainBudget + 10.dur!"seconds", (DiDrained _) {
        }), "worker did not drain");
    gate.close();
}

unittest {
    // Actor ctor: a caller-supplied SummarizerFn must reach the worker
    // (RiJob → fake record indexed under an r_ topic verbatim).
    auto s = setupTest("dialogue_ctor_forwarding_test");
    scope (exit)
        teardownTest(s);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto di = new DialogueIndex(s.tmpDir, s.cfg, s.ragCfg, &sys, &riTestFactory,
            SummaryModelConfig(contextSize: 16384), "fake reasoning prompt", &riFakeSummarizer);
    scope (exit)
        di.dispose();

    string sid = "20240101-120000-ab99";
    dynSend(di.worker, "riJob", RiJob(sid, "trace text for the t4 ctor forwarding test", 4, 8));
    auto sup = scopedActor;
    auto gate = new CompletionGate;
    di.beginDispose(sup.address(), gate);
    assert(sup.receiveTimeout(ReasoningDrainBudget + 10.dur!"seconds", (DiDrained _) {
        }), "worker did not drain after RiJob");
    gate.close();

    auto dbOpt = openDatabase((s.tmpDir ~ (sid ~ ".db")).AbsolutePath, "ri-test", 8, readOnly: true);
    assert(dbOpt.hasValue, "session DB missing after drain");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy();

    string chunkTexts;
    int chunkCount = 0;
    {
        auto stmt = db.prepare("SELECT text FROM TextChunkTbl;");
        foreach (ref r; stmt.get.execute) {
            chunkCount++;
            chunkTexts ~= r.peek!string(0) ~ "\n";
        }
    }
    assert(chunkCount == 1, "expected exactly 1 chunk for the record, got %s".format(chunkCount));
    assert(chunkTexts == "RI_FAKE_RECORD_T4\n",
            "ctor-supplied SummarizerFn must reach the worker; got: " ~ chunkTexts);

    auto hits = db.queryTextSearch("RI_FAKE_RECORD_T4", 10);
    assert(hits.length >= 1, "record must be FTS-searchable");
    string topicName = hits[0].origin.match!((Topic t) => t.name, (_) => "");
    auto metaOpt = decodeTopicName(topicName);
    assert(metaOpt.hasValue, "r_ topic must decode, got: " ~ topicName);
    auto meta = metaOpt.match!((EpisodeMeta m) => m, (None _) => EpisodeMeta());
    assert(meta.kind == Kind.reasoning, "kind must be reasoning");
    assert(meta.sessionId == sid, "sessionId mismatch: " ~ meta.sessionId);
    assert(meta.turnStart == 4 && meta.turnEnd == 8,
            "turn range mismatch: %s-%s".format(meta.turnStart, meta.turnEnd));
}

// defaultReasoningPrompt sanity
unittest {
    // The built-in prompt must carry the four section headings in order and
    // the hard constraints; the shipped prompt file is IDENTICAL to it and
    // the agent loads that file in preference (fallback = this constant).
    assert(defaultReasoningPrompt.startsWith("\nOutput exactly these four sections"),
            "prompt must open with the constraint paragraph");
    size_t posAbandoned = defaultReasoningPrompt.indexOf("Abandoned Hypotheses:");
    size_t posBinding = defaultReasoningPrompt.indexOf("Binding Decisions:");
    size_t posUncertain = defaultReasoningPrompt.indexOf("Current Uncertainties:");
    size_t posJust = defaultReasoningPrompt.indexOf("Justification:");
    assert(posAbandoned < posBinding && posBinding < posUncertain
            && posUncertain < posJust, "section order must be preserved");
    assert(defaultReasoningPrompt.indexOf("Max ~150 words") >= 0, "word budget must be present");
    assert(defaultReasoningPrompt.indexOf("one sentence each") >= 0,
            "per-turn justification constraint must be present");
}

// Shipped prompt file + loadReasoningPrompt
unittest {
    // The shipped llmfun/config/prompt/REASONING_SUMMARY.md must be
    // byte-identical to the built-in constant, so the file-present and the
    // fallback experiences are the same prompt. (The constant's WYSIWYG
    // literal carries a leading newline; the shipped file carries it too.)
    LlmConfig conf;
    conf.promptDir = [Path("llmfun/config/prompt")];

    string shipped = conf.readPromptFile("REASONING_SUMMARY.md");
    assert(shipped == defaultReasoningPrompt,
            "shipped REASONING_SUMMARY.md must be byte-identical to defaultReasoningPrompt");
    assert(loadReasoningPrompt(conf) == defaultReasoningPrompt,
            "loadReasoningPrompt must resolve the shipped file for the default config");

    // The reasoning prompt is strictly different from the dialogue summary
    // prompt and carries the four section headers.
    string dialogueSummary = conf.readPromptFile("SUMMARY.md");
    assert(shipped != dialogueSummary, "reasoning and dialogue summary prompts must differ");
    assert(dialogueSummary.canFind("Create a concise but comprehensive summary"),
            "sanity: SUMMARY.md must be the dialogue summary prompt");
    foreach (header; [
        "Abandoned Hypotheses:", "Binding Decisions:", "Current Uncertainties:",
        "Justification:"
    ])
        assert(shipped.canFind(header), "section header missing from shipped file: " ~ header);
}

unittest {
    // Missing file: readPromptFile throws, loadReasoningPrompt falls back to
    // the built-in default with a warning — startup never hard-fails on this
    // file.
    import llm.test_util : sharedLogSwapMutex;

    auto s = setupTest("loadReasoningPrompt_fallback");
    scope (exit)
        teardownTest(s);

    LlmConfig conf;
    conf.promptDir = [s.tmpDir.workArea]; // empty TestArea: no prompt files

    bool threw = false;
    try {
        conf.readPromptFile("REASONING_SUMMARY.md");
    } catch (Exception) {
        threw = true;
    }
    assert(threw, "readPromptFile must throw when the prompt file is missing");

    // Capture the fallback warning via the std.logger sharedLog seam
    // (restored on exit). The swap+drain window is serialized against the
    // other capture tests (llm.config, llm.rag.dialogue_worker) through
    // test_util's sharedLogSwapMutex: silly runs unittests in parallel, so
    // overlapping windows would send log lines into the WRONG capture.
    synchronized (sharedLogSwapMutex) {
        auto prevLog = logger.sharedLog;
        auto prevLevel = logger.globalLogLevel;
        auto cap = cast(shared) new T6LogCapture();
        logger.sharedLog = cap;
        logger.globalLogLevel = logger.LogLevel.trace;
        scope (exit) {
            logger.globalLogLevel = prevLevel;
            logger.sharedLog = prevLog;
        }

        string loaded = loadReasoningPrompt(conf); // must not throw
        assert(loaded == defaultReasoningPrompt,
                "missing file must fall back to the built-in default prompt");
        bool warned = false;
        foreach (l; (cast() cap).takeLines())
            if (l.canFind("loadReasoningPrompt: using built-in default"))
                warned = true;
        assert(warned, "fallback must log a warning");
    }
}

unittest {
    // Present custom file: loadReasoningPrompt returns the file verbatim.
    import std.file : write;

    auto s = setupTest("loadReasoningPrompt_custom");
    scope (exit)
        teardownTest(s);

    string custom = "CUSTOM REASONING SUMMARY PROMPT v1\nAbandoned Hypotheses:\n";
    write((s.tmpDir.workArea ~ "REASONING_SUMMARY.md").toString, custom);

    LlmConfig conf;
    conf.promptDir = [s.tmpDir.workArea];
    string loaded = loadReasoningPrompt(conf);
    assert(loaded == custom, "present custom prompt must be returned verbatim");
}

// End-to-end integration — checkpoint → worker → r_ record →
// retrieval
version (unittest) {
    // E2E helpers.
    //
    // Forced deviations, each independently verified:
    //  1. Nested `e2eFactory`/`fakeSummarizer` are not viable: nested
    //     functions produce delegates even without captures, and
    //     `EmbedderFactory`/`SummarizerFn` are plain function pointers
    //     (spawn-legal; the DI seam note in dialogue_worker.d), so both
    //     helpers are module-scope like the other worker fakes.
    //  2. `.contains` does not exist for ranges in the project's Phobos
    //     (LDC 1.42 / 2.112.1; std.string and std.algorithm searched) —
    //     `.canFind` (already imported at module level) is used instead.
    //  3. Module-scope variables are thread-local by default, so the capture
    //     destination is `__gshared`: the test publishes it ONCE before the
    //     worker is spawned (thread creation publishes the write) and only
    //     reads happen afterwards. The fake keeps NO state; cross-thread
    //     observation is the mailbox capture alone.
    //  4. `JSONValue(JSONType.array)` does not work in this Phobos: it
    //     builds a uinteger, not an empty array (a `.array` read throws
    //     at runtime), and the mixed-type AA literal does not compile;
    //     the tool-calls JSON is built with the established setter
    //     pattern, same resulting shape.

    /// All-ones 8-dim test embedder, aliased to the shared test
    /// embedder. The worker-side factory and the query-side instances share
    /// modelName()/dimensions() ("ri-test",
    /// 8), as the read-only DB open requires.
    alias E2ETestEmbedder = RiTestEmbedder;

    private Embedder e2eFactory(EmbedConfig config) {
        return new E2ETestEmbedder();
    }

    /// Capture destination for the fake (`__gshared` by necessity:
    /// module-scope vars are thread-local by default; published once
    /// before the worker spawns).
    private __gshared WeakAddress e2eCaptureAddr;

    /// Canned 4-section records with distinctive tokens.
    private immutable string e2eCanned1 = "Abandoned Hypotheses:\n- brute-force zebra — O(n!) timed out\n"
        ~ "Binding Decisions:\n- memoized zebra — O(1)/query\n"
        ~ "Current Uncertainties:\n- None\nJustification:\n- turn 1: ZEBRADECISION timing failed";
    private immutable string e2eCanned2 = "Abandoned Hypotheses:\n- None\nBinding Decisions:\n- kept memoized zebra — SECONDRECORD\nCurrent Uncertainties:\n- None\nJustification:\n- turn 2: stands";

    /// Fake summarizer (DI seam): capture each trace to the test
    /// supervisor (NO shared state); throw on MAKEFAIL; else branch on content.
    private string e2eFakeSummarizer(string prompt, string traceText) {
        dynSend(e2eCaptureAddr, "e2eCap", traceText);
        if (traceText.canFind("MAKEFAIL"))
            throw new Exception("fake summarizer failure");
        if (traceText.canFind("second thinking zebra"))
            return e2eCanned2;
        return e2eCanned1;
    }
}

unittest {
    // E2E: crafted checkpoints (thinking + tool traffic) → onCheckpoint →
    // shared worker → spawned fake → RiRecord → r_ record; retrieval via
    // semantic + FTS; kind separation for the dialogue query; a
    // retrieval-artifact ToolResponse never re-enters a trace; a throwing
    // job never affects a DiJob in the same mailbox (failure isolation).
    auto tmpDir = testArea("reasoning_e2e_worker", __FILE__, __LINE__);
    scope (exit)
        tmpDir.cleanup;

    string sid = "20240101-120000-e2ea";
    // The fake runs on the worker's spawned summarizer thread: publish the
    // capture address before the worker exists (thread creation publishes
    // the write). The supervisor is a scoped actor (no System needed).
    auto sup = scopedActor;
    e2eCaptureAddr = sup.address();
    scope (exit)
        e2eCaptureAddr = WeakAddress.init; // no stale capture target left behind

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "e2e-embed", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);
    auto summaryCfg = SummaryModelConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "e2e-summary", contextSize: 16384); // 16384 → positive budget (at 8192 budget is 0, no RiJob)

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto di = new DialogueIndex(tmpDir, cfg, ragCfg, &sys, &e2eFactory,
            summaryCfg, "e2e-reasoning-prompt", &e2eFakeSummarizer);
    scope (exit)
        di.dispose;
    auto ri = new ReasoningIndex(tmpDir, summaryCfg, di.worker.weakRef);

    CompressionCheckpoint mkcp(long ts, long te, Chat.MessageT[] msgs) {
        return CompressionCheckpoint(timestamp: Clock.currTime, sessionId: sid, evictedSummarized: msgs,
                evictedPurged: null, evictedInPlace: null, turnStart: ts, turnEnd: te, summaryText: "s",
                originalLength: msgs.length, newLength: 0, newContextSize: 0);
    }

    // -- Checkpoint 1: thinking + tool traffic (the reasoning projection).
    auto m1 = Message(Role.assistant, false, "Let me try brute force first.",
            "first thinking zebra: brute force over permutations, O(n!)");
    m1.turnId = 1;
    JSONValue zcall;
    zcall["name"] = "zebraSolver";
    zcall["arguments"] = "{\"n\": 8}";
    JSONValue zfn;
    zfn["function"] = zcall;
    JSONValue calls;
    calls.array = [zfn];
    auto t1 = ToolMessage("call the solver", calls);
    t1.turnId = 1;
    auto r1 = ToolResponse("file: zebras.txt (42 lines)", "call_1", "readFile", true);
    r1.turnId = 1;
    ri.onCheckpoint(mkcp(1, 1, [
        Chat.MessageT(m1), Chat.MessageT(t1), Chat.MessageT(r1)
    ]));

    // -- Checkpoint 2: the trace carries a retrieval-artifact ToolResponse.
    auto m2 = Message(Role.assistant, false, "Continuing with memoization.",
            "second thinking zebra: verify the memo table bounds");
    m2.turnId = 2;
    auto artifact = ToolResponse(e2eCanned1, "call_2", "queryReasoningHistory", true);
    artifact.turnId = 2;
    ri.onCheckpoint(mkcp(2, 2, [Chat.MessageT(m2), Chat.MessageT(artifact)]));

    // -- Checkpoint 3: the fake throws (RiDone) — plus a dialogue DiJob in
    //    the SAME mailbox (failure isolation).
    auto m3 = Message(Role.assistant, false, "Failing pass.",
            "MAKEFAIL thinking text for the throwing fake");
    m3.turnId = 3;
    ri.onCheckpoint(mkcp(3, 3, [Chat.MessageT(m3)]));

    auto du = Message(Role.user, true, "What is the capital of France?", "");
    du.turnId = 1;
    auto da = Message(Role.assistant, false, "The capital of France is Paris.", "");
    da.turnId = 1;
    di.onCheckpoint(mkcp(1, 1, [Chat.MessageT(du), Chat.MessageT(da)]));

    // -- Drain: the bounded drain deadline consumes ALL in-flight completions before
    //    DB destroy; each fake sends its capture before its completion.
    //    Traces (string) and the DiDrained reply both land on the supervisor.
    auto gate = new CompletionGate;
    di.beginDispose(sup.address(), gate);
    bool drained = false;
    string[] traces;
    while (!drained) {
        bool got = sup.receiveTimeout(ReasoningDrainBudget + 10.dur!"seconds", (DiDrained _) {
            drained = true;
        }, (string t) { traces ~= t; });
        assert(got, "drain/capture receive timed out");
    }
    gate.close();
    assert(traces.length == 3, "expected 3 captured traces, got " ~ traces.length.to!string);

    // -- Feedback loop: match by content (fakes run on concurrent threads).
    string t1t, t2t;
    foreach (t; traces) {
        if (t.canFind("first thinking zebra"))
            t1t = t;
        else if (t.canFind("second thinking zebra"))
            t2t = t;
    }
    assert(!t1t.empty, "missing checkpoint-1 trace");
    assert(t1t.canFind("zebraSolver"), "tool_calls line missing from trace");
    assert(!t2t.empty, "missing checkpoint-2 trace");
    assert(!t2t.canFind(e2eCanned1), "artifact ToolResponse re-entered the trace");

    // -- Reasoning retrieval: both canned records; semantic + FTS.
    //    Why 4 matches instead of 2: each record (195/148
    //    chars) exceeds the embedder's ~97-grapheme window, so it indexes as
    //    TWO verbatim chunks — the semantic path returns all 4 r_ chunks and
    //    nothing else (the dialogue chunk is kind-filtered).
    auto rq1 = ri.query(new E2ETestEmbedder(), SessionId(sid), "",
            "why did you choose the zebra approach");
    assert(rq1.hasHistory, "reasoning query should see history");
    assert(!rq1.message.startsWith("error:"), "no error: path by design: " ~ rq1.message);
    assert(rq1.matches.length == 4,
            "expected 4 r_ chunks (2 records x 2 chunks), got " ~ rq1.matches.length.to!string);
    bool found1 = false, found2 = false;
    foreach (m; rq1.matches) {
        assert(m.episode.kind == Kind.reasoning, "r_-only results");
        if (m.text.canFind("ZEBRADECISION")) {
            found1 = true;
            assert(m.episode.turnStart == 1 && m.episode.turnEnd == 1, "record-1 turns");
        }
        if (m.text.canFind("SECONDRECORD"))
            found2 = true;
    }
    assert(found1 && found2, "semantic path missed a record");

    auto rq2 = ri.query(new E2ETestEmbedder(), SessionId(sid), "ZEBRADECISION", "");
    assert(rq2.hasHistory, "FTS query should see history");
    assert(rq2.matches.length == 1 && rq2.matches[0].text.canFind("ZEBRADECISION"),
            "FTS path must return exactly the canned record");

    // -- The FTS path covers the second canned record too.
    auto rq2b = ri.query(new E2ETestEmbedder(), SessionId(sid), "SECONDRECORD", "");
    assert(rq2b.hasHistory && rq2b.matches.length == 1 && rq2b.matches[0].text.canFind(
            "SECONDRECORD"), "FTS path must return exactly the canned-2 chunk");

    // -- Failure isolation: the throwing fake indexed nothing for turn 3.
    auto rq3 = ri.query(new E2ETestEmbedder(), SessionId(sid), "MAKEFAIL", "");
    assert(rq3.matches.length == 0, "throwing fake must not index a record");
    // Non-vacuous — the zero-match result must come from the
    // history-present path, not from a missing DB / no r_ history.
    assert(rq3.hasHistory && rq3.message.empty,
            "isolation check must run against a live session: " ~ rq3.message);

    // -- Dialogue query on the same session excludes every r_ record.
    auto dq = di.query(new E2ETestEmbedder(), SessionId(sid), "capital France", "");
    assert(dq.hasHistory, "dialogue query should see the d_ episode");
    assert(dq.matches.length == 1 && dq.matches[0].episode.kind == Kind.dialogue
            && dq.matches[0].text.canFind("Paris"), "d_-only episode, no r_ leak");

    // -- The same kind filter on the SEMANTIC path. The
    //    all-ones embedder makes every chunk an equally-ranked candidate, so
    //    the dialogue-side filter must drop all
    //    four r_ chunks (5 matches instead of 1 if it is removed).
    auto dqSem = di.query(new E2ETestEmbedder(), SessionId(sid), "", "zebra");
    assert(dqSem.hasHistory && dqSem.matches.length == 1
            && dqSem.matches[0].episode.kind == Kind.dialogue && dqSem.matches[0].text.canFind("Paris"),
            "semantic dialogue query must drop r_ candidates");
}
