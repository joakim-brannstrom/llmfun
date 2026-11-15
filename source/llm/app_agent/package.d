/// Handles the 'agent' subcommand: interactive chat loop with TUI integration.
module llm.app_agent;

import logger = std.logger;
import std.algorithm;
import std.array : empty, array, appender;
import std.conv : to, text;
import std.exception : collectException;
import std.datetime : Clock, SysTime, DateTime, UTC, Duration, dur;
import std.format : format;
import std.json : JSONType, JSONValue;
import std.string : strip, startsWith, join;
import std.sumtype : match;

import llm.agent;
import llm.agent_md;
import llm.app_agent.slash;
import llm.app_agent.ui;
import llm.app_config : UserConfig, userToLlmConfig, createRag;
import llm.chat;
import llm.common.config : LocalEmbedConfig, RemoteEmbedConfig;
import llm.config : RagConfig;
import llm.config;
import llm.dragdrop : DropOwner, DropReport, DropWorker, makeDropWorkerConfig;
import llm.metric.monitor : MetricMonitor;
import llm.query;
import llm.rag.auto_optimize : AutoOptimizeActor;
import llm.rag.dialogue_index : DialogueIndex;
import llm.rag.dialogue_worker : CompletionGate, DiDrained, ReasoningDrainBudget;
import llm.rag.reasoning_index : ReasoningIndex, loadReasoningPrompt;
import llm.rag.rag : RAG;
import llm.session : SessionId, SessionMeta, SessionFile, SessionStore, isValidId;
import llm.skill;
import llm.telemetry : sendSyslog;
import llm.tui;
import llm.types : ServerStat, IStreamCallback;
import llm.utility;
import llmfun_tui;

import my.actor : WeakAddress, scopedActor;
import my.optional : Optional, hasValue, orElse;
import my.path : Path, AbsolutePath;

private immutable SysTime UnixEpoch = SysTime(DateTime(1970, 1, 1), UTC());

private immutable(string[]) MonthAbbr = [
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov",
    "Dec"
];

/// Milliseconds of a message's processing time (0 when none was recorded).
private long durationMsOf(MessageTime t) {
    long ms;
    t.match!((Duration d) { ms = d.total!"msecs"; }, (NoProcessingTime _) {});
    return ms;
}

/// One replay walk's output: the turns and their already-merged blocks.
private struct TypedTurns {
    UiTurnData[] turns;
    UiBlockData[] blocks;
}

// Walk a persisted history once and build the typed turns/blocks. A turn opens at a
// user query; blocks go to their message's turn; turn order is the history's
// sequence (turnId's job); batch ids are recomputed
// deterministically ("<turnId>#<response index>"). Tool calls and their stored
// responses merge by call id in one pre-pass, so replay never depends on
// adjacency. An interrupted turn replays its in-flight calls as Cancelled
// (never a fabricated Ok).
private TypedTurns buildTypedTurns(Chat.MessageT[] history) {
    // Pass 1: pair every stored response with its call id (last write wins -
    // the store's upsert-merge semantics for a duplicate call id).
    ToolResponse[string] responseByCallId;
    foreach (ref msg; history) {
        msg.match!((ToolResponse a) { responseByCallId[a.toolCallId] = a; }, (_) {
        });
    }

    // Pass 2: per-turn informational message counts (assistant response +1;
    // each generated tool call +1; each tool result +1). The C-side stepCount
    // computed over the blocks (with displayability) is the rendered number.
    int[long] messageCount;
    foreach (ref msg; history) {
        msg.match!((Message a) {
            if (a.turnId != 0 && a.role != Role.user)
                messageCount[a.turnId]++;
        }, (ToolMessage a) {
            if (a.turnId != 0)
                messageCount[a.turnId] += a.toolCalls.type == JSONType.array
                    ? cast(int) a.toolCalls.array.length : 0;
        }, (ToolResponse a) {
            if (a.turnId != 0)
                messageCount[a.turnId]++;
        }, (VisionMessage a) {});
    }

    TypedTurns out_;
    int[long] batchIndex; // per turn
    int visionSeq; // vision block ids: unique across the rebuilt snapshot

    foreach (ref msg; history) {
        msg.match!((Message a) {
            if (a.turnId == 0)
                return; // the system prompt belongs to no turn
            if (a.role == Role.user) {
                if (!a.isUserQuery())
                    return; // hidden nudge/system-style entries emit nothing
                out_.turns ~= UiTurnData(a.turnId, TuiTurnState_Done,
                    messageCount.get(a.turnId, 0));
                out_.blocks ~= makeBlockData(a.turnId, "query", queryBatchId(a.turnId),
                    TuiBlockKind_Prose, TuiBlockStatus_None, 0, null, "user", a.content, 0);
                return;
            }
            if (a.role == Role.system)
                return; // hidden today
            // A model response: the batch's reasoning + content blocks.
            const bi = batchIndex[a.turnId]++;
            const bid = responseBatchId(a.turnId, bi);
            const durMs = durationMsOf(a.processTime());
            if (!a.thinking.empty)
                out_.blocks ~= makeBlockData(a.turnId, reasoningBlockId(bi), bid, TuiBlockKind_Reasoning,
                    TuiBlockStatus_None, TuiBlockFlag_Collapsible, null, null, a.thinking, durMs);
            out_.blocks ~= makeBlockData(a.turnId, contentBlockId(bi), bid,
                TuiBlockKind_Prose, TuiBlockStatus_None, 0, null,
                a.role.to!string, a.content, durMs);
        }, (ToolMessage a) {
            if (a.turnId == 0)
                return;
            const bi = batchIndex[a.turnId]++;
            const bid = responseBatchId(a.turnId, bi);
            const calls = summarizeToolCallBodies(a.toolCalls, 1000);
            const durMs = durationMsOf(a.processTime());
            if (!a.thinking.empty)
                out_.blocks ~= makeBlockData(a.turnId, reasoningBlockId(bi), bid, TuiBlockKind_Reasoning,
                    TuiBlockStatus_None, TuiBlockFlag_Collapsible, null, null, a.thinking, durMs);
            const arr = a.toolCalls.type == JSONType.array ? a.toolCalls.array : [
            ];
            size_t idx;
            foreach (call; arr) {
                const callId = jsonStringAt(call, "id");
                auto fn = "function" in call;
                const name = fn !is null ? jsonStringAt(*fn, "name") : null;
                idx++;
                if (callId.empty || name.empty)
                    continue; // same skip rule as the v4 call emitter's entries
                string meta;
                int status = TuiBlockStatus_Cancelled;
                long callDur;
                ToolResultPairs pairs;
                if (auto resp = callId in responseByCallId) {
                    meta = summarizeToolResponse(*resp, 1000);
                    status = resp.success() ? TuiBlockStatus_Ok : TuiBlockStatus_Failed;
                    callDur = durationMsOf(resp.processTime());
                    pairs = unpackToolResultPairs(*resp);
                }
                // idx is 1-based over arr, and the body entries align with arr.
                const body_ = idx <= calls.length ? calls[idx - 1] : null;
                out_.blocks ~= withResultPairs(makeBlockData(a.turnId, callId, bid, TuiBlockKind_Tool,
                    status, kToolBlockFlags, name, meta, body_, callDur), pairs);
            }
            if (a.isFinalAnswer())
                out_.blocks ~= makeBlockData(a.turnId, "answer", bid, TuiBlockKind_Answer,
                    TuiBlockStatus_None, 0, null, null, a.getFinalAnswer(), durMs);
        }, (ToolResponse a) {
            // Every response in this history was merged into its call's block
            // by the pre-pass; a response without a call in the same history
            // cannot exist here, so nothing is emitted from this arm.
        }, (VisionMessage a) {
            if (a.turnId == 0)
                return;
            out_.blocks ~= makeBlockData(a.turnId, "vision#" ~ to!string(++visionSeq),
                queryBatchId(a.turnId), TuiBlockKind_Prose, TuiBlockStatus_None,
                0, null, "user", a.content ~ " (with image)", 0);
        });
    }
    return out_;
}

private UiBlockData withResultPairs(UiBlockData block, ToolResultPairs pairs) @safe {
    block.resultKeys = pairs.keys;
    block.resultValues = pairs.values;
    block.resultRaw = pairs.raw;
    block.resultCount = cast(uint) pairs.keys.length;
    block.resultMore = pairs.more;
    return block;
}

struct AgentApp {
    package {
        LlmConfig llmConf;
        RAG rag;
        DialogueIndex dialogueIndex;
        ReasoningIndex reasoningIndex;
        MetricMonitor monitor;
        Agent agent_;
        SessionStore sessionStore;
        SessionMeta activeSession;
        SessionId pendingDeleteId; // session id awaiting /delete confirmation

        // True while the in-memory chat differs from the last persisted state.
        // commitActiveSession() only writes (and bumps updatedAt) when this is
        // set, so mere navigation - switching or re-clicking a row - never
        // rewrites a file and never changes the sidebar sort order. Sorting is
        // by the latest message in a session. Invariant: every code path that
        // appends persisted content to the chat must set `chatDirty`; the
        // commit is otherwise no-op-gated to protect sidebar `updatedAt`
        // ordering.
        bool chatDirty;

        // The startup system prompt, cached exactly where startup computes it
        string systemPrompt_;

        ServerStat lastServerStat;
        bool debugMode;
        UserConfig.AgentChatConfig conf_;
        UiMessenger uiMsg;
        SkillManager skillManager_;
        SlashCommandRegistry slashCommands_;

        // The agent loop is commanded to continue.
        // Cleared after it has successfully started working.
        bool forceRunAgentLoop;

        // From LlmConfig so model switch and /refresh-agent-md honor the config
        bool noCwdConfig_;
        // Injectable AGENTS.md location for rebuildSystemPrompt: empty
        // resolves to the process CWD's AGENTS.md (production); tests point it
        // at a fixture file so they never depend on the repo's real AGENTS.md.
        Path agentsMdFile_;
        // latches a rebuild requested while a run was in flight, applied at
        // the next safe point after the run completes.
        bool agentMdRebuildPending_;

        // The app-side half of "a run is in flight", true for the whole bracket of a
        // turn entry point
        bool runInFlight;

        // nesting depth of open run brackets: the turn-start compression and
        // the /plan / /code pipelines bracket runs that can sit inside a query
        // bracket, so the ready edge only crosses at the outermost close.
        int runBracketDepth_;

        // The busy edge, bracketing one turn entry point's run. The ready flag
        // clears on the first (outermost) open and sets on the last close; the
        // crossing carries the messenger's busy/ready message so the composer
        // follows the real run span. Unwinding closes the bracket (scope-exit),
        // keeping the flag and the composer consistent on thrown runs.
        void agentBusy() {
            if (runBracketDepth_++ == 0) {
                runInFlight = true;
                uiMsg.busy();
            }
        }

        void agentReady() {
            if (runBracketDepth_ > 0 && --runBracketDepth_ == 0) {
                runInFlight = false;
                uiMsg.ready();
            }
        }

        TypedEmitContext typedCtx_;
        long[] typedEmittedTurnIds_;
        long noticeSeq_;
    }

    private bool oneShotQuery;

    @disable this(this);

    this(UserConfig.AgentChatConfig conf) {
        this.conf_ = conf;
        this.uiMsg = new UiMessenger(new TuiBlockedSink());
        registerBuiltinCommands(slashCommands_);
        foreach (cmd; startupSlashCommands()) {
            auto ignore = slashCommands_.register(cmd);
        }
    }

    /// Public plugin seam: register a slash command on this agent instance.
    public void registerSlashCommand(SlashCommand cmd) {
        auto ignore = slashCommands_.register(cmd);
    }

    /// Public plugin seam: access the live command registry (e.g. to render
    /// help text or list command names for TUI completion).
    /// Not thread-safe: register commands before the agent actor starts
    /// (or at module load via `addStartupSlashCommand`); concurrent
    /// `register` from another thread while the TUI dispatches is a data
    /// race.
    public ref SlashCommandRegistry slashCommands() {
        return slashCommands_;
    }

    /// First half of the two-phase exit: enqueue the dialogue
    /// worker's drain and return true. Returns false when there is no
    /// dialogue index - nothing was enqueued and the caller must NOT wait
    /// for a DiDrained that will never come.
    package bool beginDispose(WeakAddress replyTo, CompletionGate gate) {
        if (dialogueIndex is null)
            return false;
        dialogueIndex.beginDispose(replyTo, gate);
        return true;
    }

    /// Second half of the two-phase exit: the dispose body minus the drain.
    /// The drain is already done by the time this runs (the caller went
    /// beginDispose -> DiDrained / drainTimeout), so the CRITICAL ORDER of
    /// the dispose holds: no embedding runs against weights being
    /// destroyed. The shared worker also drained queued RiJobs and bounded
    /// the in-flight summarization jobs at drain; ReasoningIndex
    /// itself has nothing to dispose.
    package void finishDispose() {
        dialogueIndex = null;
        if (rag) {
            rag.destroy;
            rag = null;
        }
        if (agent_) {
            if (activeSession.id.length > 0) {
                // processResult already commits after every query; this is a
                // safety net for error/early-exit paths. Dirty-gated: a chat
                // that was already persisted is not rewritten and its
                // updatedAt stays untouched.
                commitActiveSession();
            }
            // Empty-session cleanup on clean exit, after the final commit.
            // The store guard is REQUIRED: dispose runs on the actor
            // failure paths (start() catch / hooks) and a failed
            // setupSession leaves the store null while agent_ is already
            // set. The active session is exempted even when empty.
            // Single-writer: the sweep runs on the agent thread only; it
            // never touches state.json (saved below, in the same order).
            if (sessionStore) {
                try {
                    auto swept = sessionStore.sweepEmptySessions(activeSession.id);
                    if (swept.length > 0) {
                        logger.tracef("Swept %s empty session(s) on exit: %s",
                                swept.length, swept.map!(s => s.get).join(", "));
                    }
                } catch (Exception e) {
                    logger.trace(e.msg).collectException;
                }
            }
            llmConf.saveState();
            agent_ = null;
        }
    }

    /// Compatibility dispose for tests: both phases inline. A scoped
    /// supervisor actor pumps the drain reply (the pattern
    /// DialogueIndex.dispose itself uses); the wait is
    /// skipped entirely when there is nothing to drain.
    package void dispose() {
        auto sup = scopedActor;
        auto gate = new CompletionGate;
        if (beginDispose(sup.address(), gate))
            sup.receiveTimeout(ReasoningDrainBudget + 10.dur!"seconds", (DiDrained _) {
            });
        // Close the gate before the scoped address is torn down at scope
        // exit: a late reply is skipped under the gate, never sent to a
        // torn-down address.
        gate.close();
        finishDispose();
    }

    package string printHelp() {
        return slashCommands_.helpText();
    }

    /// Public plugin seam: send a chat message to the UI. After the typed
    /// cutover (task 5) every such message is a Notice block (task 3) - the
    /// v4 per-message chat row is gone. The body travels verbatim; a Notice
    /// attaches to the store's newest turn, which is where a notice emitted
    /// between runs belongs.
    public void sendChatMessage(Args...)(string msg, Args args) {
        static if (args.length > 0) {
            msg = format(msg, args);
        }
        emitNoticeTyped(msg);
    }

    private void progressCallback(size_t currentChunk, size_t totalChunks, string status) {
        emitNoticeTyped(i"assistant: Compressing... $(currentChunk)/$(totalChunks) : $(status)"
                .text);
    }

    private void sendRuntimeStatus(bool readyState) {
        uiMsg.runtimeStatus(UiRuntimeStatus(phase: readyState ? TuiPhase.idle : TuiPhase.thinking, contextUsed: lastServerStat
                .context, contextMax: agent_.modelContextSize, compactAt: (agent_.modelContextSize * 9) / 10, tokensPerSecond: lastServerStat
                .predictedPerSecond, model: llmConf.activeModelDisplayName()));
    }

    package void doCompress(bool force) {
        if (!agent_.needCompression && !force)
            return;
        // Stamp the owning session into any checkpoint event this compression
        // fires. Pool callbacks set their own or leave "" - the dialogue
        // indexer refuses to index events with an empty sessionId.
        agent_.setCompressionCheckpointSessionId(activeSession.id.get);
        logger.tracef("compression checkpoint session id: %s", activeSession.id.get);
        const ctxUsed = agent_.stat.context;
        agentBusy();
        scope (exit)
            agentReady();
        auto res = agent_.compress(force: force, callback: &this.progressCallback);
        // A compression that actually rewrote history (summarized or purged
        // entries change the message list) makes the chat dirty; a no-op
        // compression leaves the persisted state untouched.
        if (res.originalLength != res.newLength || res.purgedCount > 0) {
            chatDirty = true;
            emitCompressionTyped();
        }
        emitNoticeTyped(compressionResultToString(res.compressed, res.originalLength,
                res.newLength, res.keptXCount, res.keptXTokens, ctxUsed, res.newContextSize));
    }

    package void processResult(ProcessResult result) {
        logger.trace(result.status != ProcessResult.Status.ok, result);
        lastServerStat = result.stat;
        this.emitStepTyped(result);
        // Set the flag HERE, not in the emitter - the emitter only draws; the
        // persisted state changed above it in the run loop.
        if (result.chat.length > 0)
            chatDirty = true;
        commitActiveSession();
    }

    // Save the current agent chat to the active session file. Strips system
    // messages before persisting.
    //
    // Gated on `chatDirty`: a clean chat (nothing changed since the last save)
    // skips the write entirely, so switching sessions, re-clicking the active
    // row, or a clean shutdown can never bump `updatedAt` and reorder the
    // sidebar. The flag is set by real content changes (a user query,
    // `/clear`, a compression that rewrote history, or an agent turn appending
    // persisted content; the per-turn setter is in `processResult`) and
    // cleared here on success; a failed save keeps it so the next commit
    // retries.
    package void commitActiveSession() @trusted nothrow {
        if (!chatDirty)
            return;
        try {
            auto doc = agent_.chat.toSaveJson();

            auto msgs = doc["messages"].array.filter!(entry => entry.type != JSONType.object
                    || !("role" in entry.object) || entry["role"].str != "system").array;
            doc["messages"] = msgs;

            // Persist the TurnID counter high-water mark as a session-header
            // key; the session store preserves unknown header keys via
            // meta.extra and rebuilds the header on save.
            if (activeSession.extra.type == JSONType.null_) {
                activeSession.extra = JSONValue.emptyObject;
            }
            activeSession.extra["next_turn_id"] = agent_.chat.nextTurnId();

            activeSession = sessionStore.save(activeSession.id, activeSession, doc);
            chatDirty = false;
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
    }

    /** Activate a session by id: load into memory, clear UI, replay history.
     *
     * Loads the session FIRST - on failure: send error and abort, keeping the
     * current in-memory chat and active id. On success: clear history,
     * re-apply the cached startup system prompt, load doc, reset response
     * index, sync context, clear UI, replay messages, resend UiInitHistory,
     * update status and state.
     */
    private void activateSession(SessionId id) {
        // Commit current session was already done by caller (switchToSession)
        // or this is a fresh activation (startup/delete-active fallback).

        auto sfOpt = sessionStore.load(id);
        if (!hasValue(sfOpt)) {
            this.sendChatMessage("error: Cannot load session '%s' (not found or corrupt). Staying in current session.",
                    id);
            return; // keep the current session unchanged
            // Note: no sendSessionList() here - the list refresh happens
            // only on the success path to keep exactly one send per
            // sidebar action. Adding a send here would double-send in
            // doDeleteSession's defensive branch (failed first activation,
            // then a successful create() activation).
        }

        auto sf = orElse(sfOpt, SessionFile());

        // Clear chat history, keeping system prompt at history[0]. Use
        // chat.clear directly instead of clearHistory() to avoid redundant
        // syncContextFromChat.
        agent_.chat.clear;
        // Re-apply the cached startup system prompt: Chat.clear keeps history[0] and the loaded doc has no system entries, so the previous chat's prompt would otherwise survive the switch. setSystemPrompt replaces history[0], so the order versus load above is insensitive; before the startup call the empty cache makes this a harmless no-op.
        if (systemPrompt_.length > 0)
            agent_.setSystemPrompt(systemPrompt_);
        agent_.chat.load(sf.doc);
        // The loaded chat matches the persisted file: nothing to save yet.
        chatDirty = false;
        agent_.chat.resetResponseIndex();
        agent_.syncContextFromChat();
        agent_.seedBrokerFromChat();
        // Status bar must reflect the target session, not the previous one
        lastServerStat = ServerStat(startContext: agent_.chat.approxContextSize);

        uiMsg.threadClear();
        uiMsg.pipelineClear();

        // The switch discards the old session's live turn context entirely
        typedCtx_ = null;
        this.emitReplayTyped();

        if (uiMsg.isActive()) {
            uiMsg.initHistory(agent_.getUserQueries.map!(a => a.content).array);
        }

        activeSession = sf.meta;
        sendRuntimeStatus(true);

        llmConf.activeChatSessionId = id.get;
        llmConf.saveState();

        // The switch path terminates here - send the refreshed snapshot
        // (covers switch, delete-active fallback, and any startup-triggered
        // switches). Guarded for one-shot mode inside.
        sendSessionList();
    }

    /** Switch to a different session: commit current + activate target.
     *
     * Single commit of the current session, then activate the target.
     * A no-op switch to the already-active session still commits, so pending
     * changes are persisted. The commit is dirty-gated: when the chat is
     * already persisted it is a no-op and `updatedAt` stays untouched, so
     * navigation never changes the sidebar sort order.
     */
    package void switchToSession(SessionId id) {
        if (id == activeSession.id) {
            commitActiveSession(); // persist pending changes on a no-op switch
            // activateSession is not called on the no-op path, so refresh
            // here - the commit may have changed counts/preview.
            sendSessionList();
        } else {
            commitActiveSession();
            activateSession(id); // sends the refreshed list itself
        }
    }

    /** Format a unix timestamp as a human-readable relative or absolute date. */
    private string formatSessionDate(long unixSec) @trusted {
        if (unixSec == 0)
            return "never";
        // Use proper Unix epoch (1970-01-01), not DateTime.init (year 0)
        auto dt = (UnixEpoch + unixSec.dur!"seconds").toLocalTime();
        auto now = Clock.currTime();

        if (dt.year == now.year && dt.month == now.month && dt.day == now.day) {
            return format("%02d:%02d", dt.hour, dt.minute);
        }
        if (dt.year == now.year) {
            return format("%s %02d", MonthAbbr[cast(size_t)(dt.month - 1)], dt.day);
        }
        return format("%04d-%02d-%02d", dt.year, dt.month, dt.day);
    }

    /** Extract the short id (hex suffix) from a full session id. */
    package static string shortSessionId(SessionId id) @safe pure nothrow {
        // id format: YYYYMMDD-HHMMSS-NNNN
        auto idStr = id.get;
        ptrdiff_t pos = -1;
        foreach (i, c; idStr) {
            if (c == '-')
                pos = cast(ptrdiff_t) i;
        }
        if (pos >= 0 && pos < idStr.length) {
            return idStr[pos + 1 .. $];
        }
        return idStr;
    }

    /** List all sessions with index, active marker, id, title, preview, count, date. */
    package void doListSessions() {
        auto sessions = sessionStore.list();

        if (sessions.length == 0) {
            this.sendChatMessage("No sessions found.");
            return;
        }

        auto lines = appender!(string[])();
        lines.put("Sessions (most recent first):");

        foreach (i, s; sessions) {
            // Fixed-width marker keeps the short-id column aligned
            auto marker = (s.id == activeSession.id) ? " [*]" : "    ";
            auto shortId = shortSessionId(s.id);
            auto preview = s.preview.length > 0 ? s.preview : "(empty)";
            auto dateStr = formatSessionDate(s.updatedAt);

            lines.put(format("  %d.%s %-4s  %-25s — %-25s — %s msgs (updated %s)",
                    i + 1, marker, shortId, s.title, preview, s.messageCount, dateStr));
        }

        this.sendChatMessage(lines[].join("\n"));
    }

    /** Create a new session and switch to it.
     *
     * Exactly one save of the current session. The new session is its own
     * confirmation: it becomes the active rail row and the thread is empty. No
     * chat message is posted - a synthetic assistant turn here renders as an
     * "agent - 1 step" work group and buries the real transcript.
     */
    package void doCreateSession() {
        auto newMeta = sessionStore.create();
        switchToSession(newMeta.id);
    }

    /** Rename the active session.
     *
     * Strips whitespace; rejects empty title (keeps previous).
     */
    package void doRenameSession(string title) {
        auto stripped = title.strip;
        if (stripped.empty) {
            // runAgent already rejects empty args; this guards direct callers.
            this.sendChatMessage("error: Rename rejected — empty title, keeping previous title '%s'.",
                    activeSession.title);
            return;
        }

        auto result = sessionStore.rename(activeSession.id, stripped);
        if (hasValue(result)) {
            activeSession = orElse(result, SessionMeta());
            this.sendChatMessage("Session renamed to '%s'.", activeSession.title);
        } else {
            this.sendChatMessage("error: Failed to rename session '%s'.",
                    shortSessionId(activeSession.id));
        }
        // Mutating callee - the active session's title changed, so the
        // sidebar snapshot is refreshed here (slash /rename is exempt from
        // the single-send rule: the receive-loop refresh may repeat it).
        sendSessionList();
    }
    /** Pick the fallback session after deleting the active one (pure).
     *
     * Params:
     *   remaining = sessions that still exist (after the delete)
     *
     * Returns: id of the most recently updated remaining session, or
     *          `SessionId.init` when the list is empty (the caller then
     *          creates a fresh one).
     */
    package static SessionId pickFallbackAfterDelete(const SessionMeta[] remaining) @safe pure nothrow {
        if (remaining.length == 0)
            return SessionId.init;
        size_t best = 0;
        foreach (i, s; remaining) {
            if (s.updatedAt > remaining[best].updatedAt)
                best = i;
        }
        return remaining[best].id;
    }

    /** Delete a session by id.
     *
     * If the deleted session is the active one, activates a fallback WITHOUT
     * committing first - the deleted file must not be recreated by the
     * fallback switch. Fallback = most recently updated remaining session,
     * else a fresh session.
     */
    package void doDeleteSession(SessionId id) {
        auto wasActive = (id == activeSession.id);
        sessionStore.remove(id);

        bool createdFresh = false;
        if (wasActive) {
            auto remaining = sessionStore.list();
            auto fallbackId = pickFallbackAfterDelete(remaining);
            if (fallbackId.length == 0) {
                fallbackId = sessionStore.create().id;
                createdFresh = true;
            }
            activateSession(fallbackId); // never commits
            // Defensive: if the fallback failed to load, do not keep pointing
            // at the deleted id - a later commit would resurrect the file.
            // Single-send note: a FAILED activation returns before
            // sendSessionList() (see activateSession's load-failure path), so
            // this defensive second activation is the only successful one on
            // this path and sends the list exactly once. The branch never
            // double-sends; it relies on the failure path sending nothing.
            if (activeSession.id == id) {
                activateSession(sessionStore.create().id);
                createdFresh = true;
            }
        }

        if (wasActive) {
            this.sendChatMessage("Session deleted: %s. Switched to '%s'%s.",
                    shortSessionId(id), activeSession.title, createdFresh ? " (new session)" : "");
        } else {
            this.sendChatMessage("Session deleted: %s", shortSessionId(id));
            // No activateSession on this path, so the removed session must
            // leave the sidebar here (the active-delete path refreshes inside
            // activateSession).
            sendSessionList();
        }
    }

    /** Map a session list to the sidebar snapshot items (pure).
     *
     * `isActive` marks the session whose id equals `activeId`; all other
     * fields pass through unchanged, preserving the caller's sort order
     * (the store already sorts by updatedAt descending).
     *
     * `updatedAtMs` is `SessionMeta.updatedAt` (unix seconds) converted to
     * milliseconds - the one conversion, no formatting (D-SESSION-TS). A
     * timestamp of 0 stays 0, which the rail reads as "unknown" and answers
     * with a flat list rather than a fabricated date.
     *
     * `activeRunning` sets the `running` flag (bit 0) for the *active* row only.
     * That is the documented hole in the snapshot: the store has no per-session
     * run state, and the run the actor knows about is always the active
     * session's, so any other row is left at 0 instead of guessing.
     *
     * `pinnedIds` sets the `pinned` flag (bit 1) on every row whose id is in
     * the persisted pin set (`LlmConfig.pinnedChatSessions`): pinned is agent
     * state, not `ui.conf` preference, so it crosses the ABI with the snapshot.
     */
    package static UiSessionItem[] mapSessionItems(const SessionMeta[] sessions,
            SessionId activeId, bool activeRunning = false, string[] pinnedIds = null) @safe pure nothrow {
        auto items = new UiSessionItem[sessions.length];
        foreach (i, ref const s; sessions) {
            const isActive = s.id.get == activeId.get;
            const pinned = pinnedIds.canFind(s.id.get);
            items[i] = UiSessionItem(s.id, s.title, s.preview, s.messageCount,
                    isActive, s.updatedAt * 1000, (isActive && activeRunning
                        ? 1 : 0) | (pinned ? 2 : 0));
        }
        return items;
    }

    /** Send the current session snapshot to the UI thread.
     *
     * Maps `sessionStore.list()` (already sorted by updatedAt descending,
     * i.e. by the latest message in each session) to UiSessionItem[] with
     * the active marker. The sidebar shows this order verbatim - clicking a
     * row never reorders the list; only a new message in a session moves
     * that session to the top. Guarded by `uiMsg.isActive()` so one-shot
     * mode (-p) never sends.
     *
     * The `running` flag is read from `runInFlight`, not from a parameter: the
     * actor owns that state for exactly as long as `runAgent` runs, and a
     * parameter would let any of the eight call sites report "no run" in the
     * middle of one (a rename served from a slash command during a run would
     * silently stop the rail's spinner). The store has no per-session run state,
     * so every row except the active one keeps flag 0 - task 24's documented
     * hole, which the multi-session run model (task 38) closes.
     */
    package void sendSessionList() {
        if (!uiMsg.isActive())
            return;
        auto items = mapSessionItems(sessionStore.list(), activeSession.id,
                runInFlight, llmConf.pinnedChatSessions);
        uiMsg.sessionList(items);
    }

    /** Relay one drop-chip report to the UI thread.
     *
     * The DropWorker sends these from its own actor thread; the relay goes
     * through the same UiMessenger channel as every other agent -> UI
     * message. Guarded by `uiMsg.isActive()` so one-shot mode (-p) never
     * sends (no worker exists there either way).
     */
    package void onDropReport(DropReport r) {
        if (!uiMsg.isActive())
            return;
        uiMsg.dropChipReport(UiDropReport(r.chipId, r.state, r.finalPath, r.display));
    }

    /** Sidebar select handler (TUIListener.sessionSelect): clear pending
     * delete, validate the untrusted UI id, then switch. Store failures
     * degrade to a chat message - the actor keeps processing.
     */
    package void doSidebarSelect(SessionId id) {
        pendingDeleteId = SessionId.init;
        try {
            if (!isValidId(id)) {
                this.sendChatMessage("error: Invalid session id '%s'. Switch rejected.", id);
                return;
            }
            switchToSession(id); // sends the refreshed list
        } catch (Exception e) {
            this.sendChatMessage("error: Failed to switch session: %s.", e.msg);
        }
    }

    /** Sidebar new handler (TUIListener.sessionNew): clear pending delete,
     * then create + switch. Store failures degrade to a chat message.
     */
    package void doSidebarNew() {
        pendingDeleteId = SessionId.init;
        try {
            doCreateSession(); // sends the refreshed list
        } catch (Exception e) {
            this.sendChatMessage("error: Failed to create session: %s.", e.msg);
        }
    }

    /** Sidebar rename handler (TUIListener.sessionRename): clear pending delete,
     * validate the id, reject empty titles only (no length cap, mirrors
     * /rename), rename the CARRIED id, refresh the active meta on
     * success, and always send the refreshed list - the rename goes
     * straight to the store, so this handler owns the send on both paths.
     */
    package void doSidebarRename(SessionId id, string title) {
        pendingDeleteId = SessionId.init;
        try {
            if (!isValidId(id)) {
                this.sendChatMessage("error: Invalid session id '%s'. Rename rejected.", id);
                return;
            }
            // Exact mirror of doRenameSession: reject empty/stripped
            // titles only; no length cap anywhere (SessionStore.rename
            // accepts any non-empty title).
            auto stripped = title.strip;
            if (stripped.length == 0) {
                this.sendChatMessage("error: Rename rejected — empty title.");
                return;
            }
            auto result = sessionStore.rename(id, stripped);
            if (hasValue(result)) {
                auto newMeta = orElse(result, SessionMeta());
                if (id == activeSession.id)
                    activeSession = newMeta; // keep the active meta fresh
                this.sendChatMessage("Session renamed to '%s'.", newMeta.title);
            } else {
                // Unknown/corrupt id
                this.sendChatMessage("error: Failed to rename session '%s'.", shortSessionId(id));
            }
            // The rename goes straight to the store (no sending callee),
            // so this handler sends the refreshed list on both paths.
            sendSessionList();
        } catch (Exception e) {
            this.sendChatMessage("error: Failed to rename session: %s.", e.msg);
        }
    }

    /** Sidebar delete handler (TUIListener.sessionDelete).
     * Clear pending delete, validate the id, then delete. The UI panel queues
     * the delete single-press (no confirmation); this handler validates the id
     * and delegates to doDeleteSession (active fallback incl.).
     */
    package void doSidebarDelete(SessionId id) {
        pendingDeleteId = SessionId.init;
        try {
            if (!isValidId(id)) {
                this.sendChatMessage("error: Invalid session id '%s'. Delete rejected.", id);
                return;
            }
            doDeleteSession(id); // sends the refreshed list
            if (llmConf.pinnedChatSessions.canFind(id.get)) {
                llmConf.pinnedChatSessions = llmConf.pinnedChatSessions.filter!(
                        pinned => pinned != id.get).array;
                savePinsEagerly();
            }
        } catch (Exception e) {
            this.sendChatMessage("error: Failed to delete session: %s.", e.msg);
        }
    }

    package void doSidebarPinToggle(SessionId id) {
        pendingDeleteId = SessionId.init;
        try {
            if (!isValidId(id) || !sessionStore.has(id))
                return;
            if (llmConf.pinnedChatSessions.canFind(id.get)) {
                llmConf.pinnedChatSessions = llmConf.pinnedChatSessions.filter!(
                        pinned => pinned != id.get).array;
            } else {
                llmConf.pinnedChatSessions ~= id.get;
            }
            savePinsEagerly();
            sendSessionList();
        } catch (Exception e) {
            logger.warningf("pin toggle refresh failed: %s", e.msg);
        }
    }

    package void doSidebarClone(SessionId id) {
        pendingDeleteId = SessionId.init;
        try {
            if (!isValidId(id)) {
                this.sendChatMessage("error: Invalid session id '%s'. Clone rejected.", id);
                return;
            }
            auto result = sessionStore.clone(id);
            if (hasValue(result)) {
                auto cloneMeta = orElse(result, SessionMeta());
                switchToSession(cloneMeta.id); // sends the refreshed list
                this.sendChatMessage("Session cloned to '%s'.", shortSessionId(cloneMeta.id));
            } else {
                // Unknown/corrupt id - error message, active session
                // unchanged, list still refreshed below.
                this.sendChatMessage("error: Failed to clone session '%s'.", shortSessionId(id));
                sendSessionList();
            }
        } catch (Exception e) {
            this.sendChatMessage("error: Failed to clone session: %s.", e.msg);
        }
    }

    package void savePinsEagerly() @safe {
        if (auto err = llmConf.trySaveState())
            logger.warningf("pin persistence: state.json save failed: %s", err);
    }

    // TODO: make the method nothrow to ensure it never accidentally exited
    package AgentStatus runAgent(string query) {
        // Any input other than /delete clears the pending delete confirmation.
        // It applies to bare queries and unknown commands too, so it lives here,
        // not in the registry.
        if (!query.startsWith("/delete"))
            pendingDeleteId = SessionId.init;

        bool runAgentLoop;
        AgentStatus rval;

        if (slashCommands_.isSlashCommand(query)) {
            // /delete-prefixed non-commands like `/deletefoo` skip the top
            // rule, and the registry's unknown path does not clear pending
            // state. Clear before dispatch - a stale confirmation would
            // otherwise make the next `/delete <n>` confirm-delete without
            // re-prompting.
            if (query.startsWith("/delete") && !slashCommands_.isRegistered(query))
                pendingDeleteId = SessionId.init;
            rval = slashCommands_.execute(this, query);
            query = null; // consume the query so it isn't added to the chat
            runAgentLoop = forceRunAgentLoop;
            logger.trace(forceRunAgentLoop, "agent loop forced to start");
            forceRunAgentLoop = false;
        }

        if (!query.empty) {
            agent_.addUserQuery(query);
            // The chat now carries a message that is not persisted yet.
            chatDirty = true;

            runAgentLoop = true;
        }

        if (runAgentLoop) {
            this.doCompress(false);
            // User-query echo, sent AFTER the turn-start doCompress: the
            // query (if any) is the last entry in the possibly-compressed
            // history, so it is the newest turn in the typed store.
            // isActive() keeps one-shot CLI output byte-stable: the
            // startSetup one-shot branch runs with the default TuiBlockedSink
            // and has no chat line for the prompt today.
            if (!query.empty && uiMsg.isActive())
                this.emitTurnStartTyped(query);
            bool stopped;
            auto result = agent_.runToCompletion(&this.processResult,
                    compressCallback: &this.progressCallback, interrupt: () {
                stopped = isStopAgentTriggered;
                return stopped;
            });
            if (stopped)
                this.emitStopTyped();
            if (result.status == ProcessResult.Status.agentStuckInLoop) {
                this.sendChatMessage("harness: Agent forcefully terminated because it got stuck in a loop.\nYou can restart it with /c.\nIf it gets stuck again then the model is stuck in an internal prediction loop. Clear the context (/clear) and run your query again.");
            }
        }

        // A rebuild requested anywhere inside the userQuery/one-shot bracket
        // is applied here, at the post-run safe point: the run bracket spans
        // slash dispatch (userQuery opens it before runAgent), so a /model or
        // /skills rebuild genuinely defers. The deferral is consumed later in
        // this same runAgent call - before any next model call could start -
        // so a command that only rebuilds is safe; a future command that both
        // rebuilds at dispatch and forces the loop would run the loop on the
        // stale prompt, which is the hazard to keep an eye on. Tests take the
        // guard by setting the flags directly.
        if (agentMdRebuildPending_) {
            agentMdRebuildPending_ = false;
            rebuildSystemPrompt();
        }

        return rval;
    }

    package IStreamCallback makePipelineStreamCallback() {
        return new PipelineStreamMessageUpdater(uiMsg, agent_.modelContextSize,
                llmConf.activeModelDisplayName);
    }

    /// Walk the in-memory history once and send ONE setTurns
    /// snapshot with already-merged blocks. A live turn still in flight is
    /// not persisted yet: the switch replay drops its context, so neither its
    /// turn nor its blocks enter the snapshot.
    package void emitReplayTyped() {
        auto built = buildTypedTurns(agent_.chat.getMessages());
        if (typedCtx_ !is null) {
            built.turns = built.turns.filter!(t => t.id != typedCtx_.turnId).array;
            built.blocks = built.blocks.filter!(b => b.turnId != typedCtx_.turnId).array;
        }
        uiMsg.setTurns(built.turns, built.blocks);
        typedEmittedTurnIds_ = built.turns.map!(t => t.id).array;
        noticeSeq_ = 0;
        typedCtx_ = null;
    }

    /// Live turn start: turn upsert (Running) + the query prose block.
    package void emitTurnStartTyped(string query) {
        const turnId = agent_.chat.currentTurnId();
        typedCtx_ = new TypedEmitContext;
        typedCtx_.turnId = turnId;
        typedCtx_.messageCount = 0;
        typedEmittedTurnIds_ ~= turnId;
        uiMsg.turnUpsert(UiTurnData(turnId, TuiTurnState_Running, 0));
        uiMsg.blockUpsert(makeBlockData(turnId, "query", queryBatchId(turnId),
                TuiBlockKind_Prose, TuiBlockStatus_None, 0, null, "user", query, 0));
    }

    /// Live step completion: the step-completion walk emits the batch's final
    /// blocks - merged tool blocks (Ok/Failed + result meta + duration), the
    /// next batch's reasoning/content blocks, and the Answer block when the
    /// final task_done result arrives, followed by the turn's Done upsert.
    /// Interrupted turns go through emitStopTyped instead.
    package void emitStepTyped(ProcessResult result) {
        if (typedCtx_ is null) {
            typedCtx_ = new TypedEmitContext;
            typedCtx_.turnId = agent_.chat.currentTurnId();
        }
        auto ctx = typedCtx_;

        // Pair this step's responses with their calls first. Only
        // responses whose call is also in this step pair up: a response whose
        // call streamed in an earlier step is an orphan and emits standalone.
        bool[string] stepCallIds;
        foreach (ref msg; result.chat) {
            msg.match!((ToolMessage a) {
                foreach (call; a.toolCalls.type == JSONType.array ? a.toolCalls.array : [
                    ])
                    stepCallIds[jsonStringAt(call, "id")] = true;
            }, (_) {});
        }
        ToolResponse[string] respById;
        foreach (ref msg; result.chat) {
            msg.match!((ToolResponse a) {
                if (a.toolCallId in stepCallIds)
                    respById[a.toolCallId] = a;
            }, (_) {});
        }

        // The final answer's block and the turn's Done close are deferred to
        // after the walk: later messages (an orphan result) still emit their
        // blocks first, and the Done upsert carries the walk's complete
        // messageCount.
        bool finalAnswerSeen;
        string finalAnswerBatchId;
        string finalAnswerBody;
        long finalAnswerDurMs;

        foreach (ref msg; result.chat) {
            msg.match!((Message a) {
                if (a.role != Role.assistant)
                    return; // the query echo + hidden nudges emit nothing
                const bi = ctx.batchIndex++;
                const bid = responseBatchId(ctx.turnId, bi);
                const durMs = durationMsOf(a.processTime());
                if (!a.thinking.empty)
                    uiMsg.blockUpsert(makeBlockData(ctx.turnId, reasoningBlockId(bi),
                        bid, TuiBlockKind_Reasoning, TuiBlockStatus_None,
                        TuiBlockFlag_Collapsible, null, null, a.thinking, durMs));
                uiMsg.blockUpsert(makeBlockData(ctx.turnId, contentBlockId(bi), bid,
                    TuiBlockKind_Prose, TuiBlockStatus_None, 0, null,
                    a.role.to!string, a.content, durMs));
                ctx.messageCount++;
            }, (ToolMessage a) {
                const bi = ctx.batchIndex++;
                const bid = responseBatchId(ctx.turnId, bi);
                const durMs = durationMsOf(a.processTime());
                if (!a.thinking.empty)
                    uiMsg.blockUpsert(makeBlockData(ctx.turnId, reasoningBlockId(bi),
                        bid, TuiBlockKind_Reasoning, TuiBlockStatus_None,
                        TuiBlockFlag_Collapsible, null, null, a.thinking, durMs));
                const calls = summarizeToolCallBodies(a.toolCalls, 1000);
                const arr = a.toolCalls.type == JSONType.array ? a.toolCalls.array : [
                ];
                size_t idx;
                foreach (call; arr) {
                    const callId = jsonStringAt(call, "id");
                    auto fn = "function" in call;
                    const name = fn !is null ? jsonStringAt(*fn, "name") : null;
                    idx++;
                    if (callId.empty || name.empty)
                        continue;
                    string meta;
                    int status = TuiBlockStatus_Cancelled;
                    long callDur;
                    ToolResultPairs pairs;
                    if (auto resp = callId in respById) {
                        meta = summarizeToolResponse(*resp, 1000);
                        status = resp.success() ? TuiBlockStatus_Ok : TuiBlockStatus_Failed;
                        callDur = durationMsOf(resp.processTime());
                        pairs = unpackToolResultPairs(*resp);
                    }
                    // idx is 1-based over arr, and the body entries align with arr.
                    const body_ = idx <= calls.length ? calls[idx - 1] : null;
                    uiMsg.blockUpsert(withResultPairs(makeBlockData(a.turnId != 0 ? a.turnId : ctx.turnId,
                        callId, bid, TuiBlockKind_Tool, status,
                        kToolBlockFlags, name, meta, body_, callDur), pairs));
                    ctx.clearLiveCall(callId); // merged: the stream-done flip list shrinks
                }
                ctx.messageCount += cast(int) arr.length;
                if (a.isFinalAnswer()) {
                    finalAnswerSeen = true;
                    finalAnswerBatchId = bid;
                    finalAnswerBody = a.getFinalAnswer();
                    finalAnswerDurMs = durationMsOf(a.processTime());
                }
            }, (ToolResponse a) {
                // Already merged into this walk's tool blocks: nothing to emit. A
                // response whose call streamed in an earlier step (a true orphan)
                // stays a result-only block below.
                ctx.messageCount++;
                if (a.toolCallId in respById)
                    return;
                // A true orphan (its call streamed in an earlier step): its own
                // response-only batch, so the next response batch stays separate.
                const bi = ctx.batchIndex++;
                uiMsg.blockUpsert(withResultPairs(makeBlockData(a.turnId != 0
                    ? a.turnId : ctx.turnId, a.toolCallId, responseBatchId(ctx.turnId, bi),
                    TuiBlockKind_Tool, a.success() ? TuiBlockStatus_Ok
                    : TuiBlockStatus_Failed, kToolBlockFlags, a.toolName,
                    summarizeToolResponse(a, 1000), null, durationMsOf(a.processTime())),
                    unpackToolResultPairs(a)));
            }, (VisionMessage a) {
                uiMsg.blockUpsert(makeBlockData(ctx.turnId, "vision#" ~ to!string(++ctx.visionSeq),
                    queryBatchId(ctx.turnId), TuiBlockKind_Prose, TuiBlockStatus_None,
                    0, null, "user", a.content ~ " (with image)", 0));
            });
        }

        if (finalAnswerSeen) {
            uiMsg.blockUpsert(makeBlockData(ctx.turnId, "answer", finalAnswerBatchId, TuiBlockKind_Answer,
                    TuiBlockStatus_None, 0, null, null, finalAnswerBody, finalAnswerDurMs));
            uiMsg.turnUpsert(UiTurnData(ctx.turnId, TuiTurnState_Done, ctx.messageCount));
        }
    }

    /// Stop (interrupt): the interrupted turn's state goes Interrupted and the
    /// in-flight calls flip to Cancelled (never a fabricated Ok). A partial
    /// answer stays - its Answer block is already upserted.
    package void emitStopTyped() {
        auto ctx = typedCtx_;
        if (ctx is null)
            return;
        foreach (call; ctx.liveCalls)
            uiMsg.blockUpsert(makeBlockData(call.turnId, call.id, call.batchId, TuiBlockKind_Tool,
                    TuiBlockStatus_Cancelled, kToolBlockFlags, call.title, null, call.body, 0));
        ctx.liveCalls = null;
        uiMsg.turnUpsert(UiTurnData(ctx.turnId, TuiTurnState_Interrupted, ctx.messageCount));
    }

    /// A compression progress notice as a Notice block. The body is the exact
    /// text today's chatMessage line carries (the formatter prints it
    /// verbatim), so console parity holds.
    package void emitNoticeTyped(string notice) {
        // A synthetic bare AgentApp (registry tests, some slash error paths)
        // has no chat to attach the notice to: the notice still emits, into
        // the no-turn bucket (turn 0). Every real path (live run, one-shot
        // with a built agent) has a chat, so the notice lands in the
        // store's newest turn.
        const turnId = agent_ !is null ? agent_.chat.currentTurnId() : 0;
        noticeSeq_++;
        uiMsg.blockUpsert(makeBlockData(turnId, "notice#" ~ text(noticeSeq_), queryBatchId(turnId),
                TuiBlockKind_Notice, TuiBlockStatus_None, 0, null, null, notice, 0));
    }
    /// Compression re-emission: the typed store is rebuilt from the rewritten
    /// history with ONE setTurns snapshot - removed turns cascade, no stale
    /// group survives. Safe at the turn-start compression: the live turn's
    /// upsert has not happened yet (the query echo is sent after doCompress),
    /// so a snapshot cannot kill live state.
    package void emitCompressionTyped() {
        auto built = buildTypedTurns(agent_.chat.getMessages());
        uiMsg.setTurns(built.turns, built.blocks);
        typedEmittedTurnIds_ = built.turns.map!(t => t.id).array;
        noticeSeq_ = 0;
        typedCtx_ = null;
    }

    /// The typed stream updater for the live turn (typed sibling of
    /// makeStreamCallback).
    package IStreamCallback makeTypedStreamCallback() {
        if (typedCtx_ is null) {
            typedCtx_ = new TypedEmitContext;
            typedCtx_.turnId = agent_.chat.currentTurnId();
        }
        return new TypedStreamMessageUpdater(uiMsg, agent_.modelContextSize,
                llmConf.activeModelDisplayName, typedCtx_);
    }

    /// Load the active session into the chat. The system prompt is NOT built
    /// here: startup builds it through the shared rebuildSystemPrompt() before
    /// this runs (setSystemPrompt before chat.load is order-insensitive;
    /// loaded docs carry no system entries, so exactly one system message
    /// results).
    private void setupSession() {
        sessionStore = new SessionStore(llmConf.chatDir);
        auto sessions = sessionStore.list();

        if (llmConf.activeChatSessionId.length > 0) {
            auto found = sessions.filter!(s => s.id.get == llmConf.activeChatSessionId).array;
            if (found.length > 0) {
                activeSession = found[0];
            }
        }
        if (activeSession.id.length == 0 && sessions.length > 0) {
            activeSession = sessions[0]; // most recent (sorted by updatedAt desc)
        }
        if (activeSession.id.length == 0) {
            activeSession = sessionStore.create(); // guarantee at least one session
        }

        auto sfOpt = sessionStore.load(activeSession.id);
        if (hasValue(sfOpt)) {
            auto sf = orElse(sfOpt, SessionFile());
            agent_.chat.load(sf.doc);
            // Startup loads persisted state: nothing to save yet.
            chatDirty = false;
        } else {
            logger.warningf("Failed to load active session '%s'. Starting with empty chat.",
                    activeSession.id);
        }
        agent_.chat.resetResponseIndex; // prevent replay of old history
        agent_.syncContextFromChat(); // set prevStat.context from the loaded chat
        agent_.seedBrokerFromChat(); // re-activate tools the loaded history proves were used

        // Persist active session id (covers "fresh session created" path)
        llmConf.activeChatSessionId = activeSession.id.get;
        llmConf.saveState();

        lastServerStat = ServerStat(startContext: agent_.chat.approxContextSize);
    }

    /// Rebuild the system prompt for the active model: resolve its effective
    /// AgentsMd config, reprocess AGENTS.md, recompose, re-apply. Safe while an
    /// agent run is in flight: the rebuild is DEFERRED (pending flag), applied
    /// at the next safe point after the run completes.
    ///
    /// Reachability note: the guard below really fires for a rebuild requested
    /// inside the run bracket - userQuery opens the bracket before runAgent and
    /// slash dispatch happens inside it, so /model and /skills rebuilds defer to
    /// runAgent's post-run safe point (consumed in the same runAgent call,
    /// before any next model call). A rebuild requested outside any bracket
    /// applies immediately. Tests exercise the guard by setting runInFlight
    /// directly.
    package void rebuildSystemPrompt(bool forceRefresh = false) {
        if (runInFlight) {
            agentMdRebuildPending_ = true;
            return;
        }
        auto agentsMd = effectiveAgentsMd(llmConf, llmConf.codeModels[llmConf.activeCodeModelIndex]);
        auto state = processAgentMd(llmConf, noCwdConfig_, rag, forceRefresh,
                agentsMd, llmConf.codeModels[llmConf.activeCodeModelIndex], agentsMdFile_);
        agent_.setSystemPrompt(systemPrompt_ = llmConf.getPrompt(skillManager: skillManager_, promptName: llmConf
                .agentPrompt, addSkills: true, agentsMd: AgentsMdPrompt(state.summary,
                state.ragTopic)));
        if (agentMdRebuildPending_)
            agentMdRebuildPending_ = false;
    }

    package void continueAgent() {
        forceRunAgentLoop = true;
    }
}

/// Parse the `LLMFUN_TUI_BACKEND` value; invalid values warn and yield Auto.
TuiBackendMode parseTuiBackendMode(string value) {
    import std.string : toLower;

    switch (value.toLower) {
    case "auto":
        return TuiBackendMode_Auto;
    case "gui":
        return TuiBackendMode_Gui;
    case "tui":
        return TuiBackendMode_Tui;
    case "":
        break;
    default:
        logger.warningf("LLMFUN_TUI_BACKEND: unknown value '%s' (expected auto|gui|tui); using auto",
                value);
        break;
    }
    return TuiBackendMode_Auto;
}

/// Resolve the requested backend mode: CLI flag > env > Auto. `envValue`
/// is injectable for tests (null = read the process environment).
TuiBackendMode resolveTuiBackendMode(ref const UserConfig.AgentChatConfig conf,
        string envValue = null) {
    if (conf.gui)
        return TuiBackendMode_Gui;
    if (conf.tui)
        return TuiBackendMode_Tui;
    auto v = envValue;
    if (v is null) {
        import std.process : environment;

        v = environment.get("LLMFUN_TUI_BACKEND", null);
    }
    return parseTuiBackendMode(v);
}

struct AgentDone {
    int code;
}

class AppAgentActor : DropOwner {
    import my.actor;
    import llm.tui;

    package AgentApp app;

    private {
        ActorRef self_;
        UserConfig uconf;
        System* sys;
        WeakAddress supervisor;
        TypedAddress!TextUserInterfaceActor tui_;

        // two-phase exit state (startExit -> diDrained/drainTimeout ->
        // finishExit); exiting_ latches forever: one exit per actor.
        bool exiting_;
        bool drainDone_;
        int exitCode_;
        ExitReason exitReason_;
        CompletionGate drainGate_;
    }

    // runs on the MAIN thread inside sys.spawn (ctor-on-calling-thread).
    // supervisor is a WeakAddress: agent sends are safe even if the
    // supervisor thread already finished (drop, no crash).
    this(UserConfig uconf, UserConfig.AgentChatConfig conf, System* sys, WeakAddress supervisor) {
        this.uconf = uconf;
        this.sys = sys;
        this.supervisor = supervisor;
        this.app = AgentApp(conf); // slash registration happens here
    }

    void onSpawn(ActorRef self) {
        self_ = self;
    }

    // DropWorker -> owner edge: the chip's outcome is relayed to the UI
    // thread through the messenger (the worker never touches the sink).
    void onDropReport(DropReport r) {
        app.onDropReport(r);
    }

    void start() {
        try {
            startSetup();
        } catch (Exception e) {
            logger.errorf("AppAgentActor start failed (%s): %s",
                    cast(Exception) e !is null ? "Exception" : "Error", e.msg);
            startExit(1, ExitReason.unhandledException);
        }
    }

    private void startSetup() {
        makeDefaultFileStructure();
        if (app.conf_.setupDirs)
            makeLocalSetupFileStructure(LlmConfig.init);
        app.llmConf = readConfig(uconf.config, !app.conf_.prompt.empty,
                uconf.noCwdConfig, uconf.trustedConfig, app.conf_.workArea).userToLlmConfig(
                app.conf_);
        if (app.conf_.setupDirs)
            inheritCentralUiProfile(app.llmConf);

        if (app.llmConf.telemetry.syslog.enabled)
            sendSyslog(sys, app.llmConf.telemetry.syslog);

        app.rag = createRag(app.llmConf);
        if (app.rag is null) {
            startExit(1, ExitReason.userShutdown);
            return;
        }

        app.skillManager_ = makeSkillManager(app.llmConf);
        // The /model and /refresh-agent-md rebuilds reuse the startup input.
        app.noCwdConfig_ = uconf.noCwdConfig;
        app.monitor = new MetricMonitor(app.llmConf.dataDir ~ "monitor.jsonl");
        app.agent_ = new Agent("main", app.llmConf, app.skillManager_,
                app.monitor, app.rag, app.llmConf.toolFilter.to());
        auto dialogueRagCfg = RagConfig(windowOverlapPercent: 10, nBatch: 1,
                maxChunksPerTopic: 512);
        // owner = this actor: the worker's one-shot DiDegraded lands
        // directly on the agent's diDegraded handler (no supervisor
        // forward). The actor ctor spawns the worker on sys and leaves
        // the live handle in `worker`, so the RiJob dispatch below is
        // wired from here on.
        auto reasoningPrompt = loadReasoningPrompt(app.llmConf);
        app.dialogueIndex = new DialogueIndex(app.llmConf.dialogueDir.AbsolutePath,
                app.llmConf.embedConfig, dialogueRagCfg, sys, null,
                app.llmConf.summaryModel, reasoningPrompt, null, self_.address);
        app.agent_.addCompressionCheckpointListener(&app.dialogueIndex.onCheckpoint);
        app.agent_.toolContext().setDialogueIndex(app.dialogueIndex);
        // Live handle to the shared dialogue worker (spawned by the
        // actor ctor above); the RI dispatches RiJobs to it.
        app.reasoningIndex = new ReasoningIndex(app.llmConf.dialogueDir.AbsolutePath,
                app.llmConf.summaryModel, app.dialogueIndex.worker.weakRef);
        app.agent_.addCompressionCheckpointListener(&app.reasoningIndex.onCheckpoint);
        app.agent_.toolContext().setReasoningIndex(app.reasoningIndex);

        app.rebuildSystemPrompt();
        app.setupSession();
        app.oneShotQuery = !app.conf_.prompt.empty;
        if (app.oneShotQuery) {
            app.agentBusy();
            scope (exit)
                app.agentReady();
            app.runAgent(app.conf_.prompt);
            // explicit, before AgentDone (startExit -> finishExit)
            startExit(0, ExitReason.userShutdown);
            return;
        }

        import llm.memory_tier : MemoryTierActor;

        sys.spawn!MemoryTierActor(app.llmConf, app.rag);

        {
            // One-shot auto-optimize check
            auto embedModel = app.llmConf.embedConfig.match!((RemoteEmbedConfig a) => a.modelName,
                    (LocalEmbedConfig a) => a.modelName);
            sys.spawn!AutoOptimizeActor(app.llmConf.ragPrimary.path.AbsolutePath,
                    app.llmConf.ragConfig.autoOptimizeThreshold,
                    app.llmConf.ragConfig.autoOptimizeCheckIntervalMin, embedModel,
                    app.llmConf.embedDimensions, app.llmConf.ragConfig.busyTimeout);
        }

        auto dropWorker = sys.spawn!DropWorker(makeDropWorkerConfig(app.llmConf.workArea.AbsolutePath,
                app.llmConf.dragDrop), TypedAddress!DropOwner(self_.address().lock()));

        // threads, like this supervisor, may block on a full mailbox.
        // Config.detached: the TUI actor must own one FIXED thread. Its GLFW
        // window/GL context are thread-affine. The backend mode (where to draw) is the only
        // choice left: the workbench is the one renderer (task 41).
        tui_ = sys.spawnBounded!(Config.detached, TextUserInterfaceActor)(1000,
                TypedAddress!TUIListener(self_.address().lock()), app.llmConf.uiInitFile, app.llmConf.tui.maxWidth,
                resolveTuiBackendMode(app.conf_), app.conf_.debugUi,
                app.llmConf.tui.idleParkMs, dropWorker);
        app.uiMsg = new UiMessenger(new TuiChannelSink(tui_));
        monitor(self_.address(), tui_);
        app.uiMsg.initHistory(app.agent_.getUserQueries.map!(a => a.content).array);
        // One typed replay snapshot at startup (task 5); the per-message walk
        // is gone (see activateSession). It must run before the stream callback
        // exists: makeTypedStreamCallback seeds the live-turn context from the
        // loaded chat's last turn, which the replay would then filter out.
        app.emitReplayTyped();
        app.agent_.setStreamUpdate(app.makeTypedStreamCallback);

        app.sendSessionList();
        app.sendRuntimeStatus(true);
    }

    void userQuery(string s) {
        auto query = s.strip;
        if (!query.empty) {
            {
                app.agentBusy();
                scope (exit)
                    app.agentReady();
                app.sendSessionList();
                clearStopAgent();
                app.sendRuntimeStatus(false);
                final switch (app.runAgent(query)) {
                case AgentStatus.active:
                    break;
                case AgentStatus.terminate:
                    app.uiMsg.terminate();
                    break;
                }
            }
            app.sendSessionList();
            app.sendRuntimeStatus(true);
        }
    }

    void sessionSelect(SessionId id) {
        app.doSidebarSelect(id);
    }

    void sessionNew() {
        app.doSidebarNew();
    }

    void sessionRename(SessionId id, string title) {
        app.doSidebarRename(id, title);
    }

    void sessionDelete(SessionId id) {
        app.doSidebarDelete(id);
    }

    void sessionPinToggle(SessionId id) {
        app.doSidebarPinToggle(id);
    }

    void sessionClone(SessionId id) {
        app.doSidebarClone(id);
    }

    void uiTerminated() {
        startExit(0, ExitReason.userShutdown);
    }

    // The TUI actor failed to bring up its backend (e.g. --gui without a
    // display, or Auto in a non-interactive console). Auto's fallback is
    // C++-side and already resolved when this fires, so there is nothing to
    // retry: report the reason and exit non-zero (deterministic for scripts).
    void uiStartupFailed(string reason) {
        logger.warning("TUI startup failed: ", reason);
        startExit(1, ExitReason.userShutdown);
    }

    // The dialogue worker sends this directly to its owner (this actor -
    // wired as `self_` in the DI ctor in startSetup): no supervisor
    // forward.
    void diDegraded(string reason) {
        // The dialogue worker sends this exactly once (its embedder is
        // unavailable for the process lifetime). The worker already logged
        // the cause; this owner-side line records the degradation state.
        logger.warningf("dialogue index worker degraded: %s (dialogue indexing disabled for process lifetime)",
                reason);
    }

    void onDownMessage(DownMsg d) {
        logger.warningf("TUI actor terminated unexpectedly: %s", d.reason.to!string);
        startExit(1, ExitReason.kill);
    }

    void onException(Exception e) {
        logger.warning("AppAgentActor exception: ", e.msg);
        startExit(1, ExitReason.kill);
    }

    void onError(ErrorMsg e) {
        logger.warning("AppAgentActor error: ", e.reason.to!string);
        startExit(1, ExitReason.kill);
    }

    void startExit(int code, ExitReason reason) {
        if (exiting_)
            return;
        exiting_ = true;
        exitCode_ = code;
        exitReason_ = reason;
        drainGate_ = new CompletionGate;
        if (app.beginDispose(self_.address, drainGate_)) {
            // Arm the deadline AFTER the drain is enqueued (send order):
            // the worker answers well before this fires; the deadline
            // only covers a worker that never answers.
            dynDelayedSend(self_.address,
                    Clock.currTime + ReasoningDrainBudget + 10.dur!"seconds", "drainTimeout");
        } else {
            drainGate_.close();
            drainDone_ = true;
            finishExit();
        }
    }

    // Phase-2 trigger: the worker's drain reply, delivered to self_
    // (the replyTo passed to beginDispose).
    void diDrained(DiDrained d) {
        if (!exiting_ || drainDone_)
            return;
        drainDone_ = true;
        if (drainGate_ !is null)
            drainGate_.close();
        finishExit();
    }

    void drainTimeout() {
        diDrained(DiDrained());
    }

    private void finishExit() {
        try {
            app.finishDispose();
        } catch (Throwable t) {
            logger.errorf("finishDispose during exit failed: %s", t.msg);
        }
        agentDone(exitCode_);
        sendExit(self_.address, exitReason_);
    }

    private void agentDone(int code) {
        dynSend(supervisor, "agentDone", AgentDone(code));
    }
}

int appMain(UserConfig uconf, UserConfig.AgentChatConfig conf) {
    import llm.subsystem : initLlmfunLocalModel, deinitLlmfunLocalModel;
    import my.actor;
    import std.parallelism : TaskPool;

    initLlmfunLocalModel();
    scope (exit)
        deinitLlmfunLocalModel();

    // The supervisor is a scoped actor pumped by this main thread (not a
    // pool worker). Declared before the scope guards so LIFO teardown
    // destroys it LAST - after sys.shutdown + pool.finish - which keeps
    // its mailbox alive for the agent's final AgentDone.
    auto sup = scopedActor;

    // Explicit 3-worker pool - the sys actors (AppAgentActor,
    // TextUserInterfaceActor, and the dialogue worker spawned by the
    // DialogueIndex actor ctor) each occupy one pool worker for their
    // full duration; the agents and workers ARE actors, the pool serves
    // those workloads. Never default pool.
    // Pool-size policy (a convention - the library does not check it -
    // documented in the mylib actor README, "Bounded mailbox"):
    // pool >= actor count + 1 - the three actors take the three pool
    // workers and this main thread (the supervisor wait below) is the
    // +1: it holds no worker, so it is the only thread allowed to block
    // on a bounded mailbox; pool workers never block (they drop instead).
    // makeSystem(pool) does not own the pool, so sys.shutdown() never
    // finishes it. Daemon the pool so its idle worker threads do not block
    // the runtime's exit join by themselves, and finish it explicitly in
    // the shutdown scope below -- the daemon flag only exempts the threads
    // from that join, it does not terminate them.
    auto pool = new TaskPool(3);
    pool.isDaemon = true;
    auto sys = makeSystem(pool);
    scope (exit) {
        // Two-phase exit + join-all: the agent actor drains the dialogue
        // worker (bounded deadline) and finishes its cleanup before its
        // sendExit, so by the time the wait below returns no actor owns
        // state a later phase needs. Stop the scheduler, then stop and
        // join the external pool: without finish(true) the pool worker
        // threads outlive shutdown and process exit hangs in the
        // runtime's thread_joinAll (the daemon flag only exempts them
        // from that join, it does not terminate them). finish(true)
        // makes the worker threads exit.
        sys.shutdown();
        pool.finish(true);
    }
    try {
        auto agent = sys.spawn!AppAgentActor(uconf, conf, &sys, sup.address());
        dynSend(agent, "start");
        int code = 0;
        bool done_ = false;
        while (!done_) {
            // The 30s re-wait only handles spurious wakeups; the agent's
            // AgentDone (delivered to sup by the agent itself) ends the
            // loop.
            sup.receiveTimeout(30.dur!"seconds", (AgentDone ad) {
                code = ad.code;
                done_ = true;
            });
        }
        return code;
    } catch (Exception e) {
        logger.warning(e.msg);
    }
    return 1;
}
