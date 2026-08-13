/// Handles the 'agent' subcommand: interactive chat loop with TUI integration.
module llm.app_agent;

import logger = std.logger;
import std.algorithm;
import std.array : empty, array, appender;
import std.concurrency;
import std.conv : to, text;
import std.exception : collectException, ifThrown;
import std.datetime : Clock, SysTime, DateTime, UTC, dur;
import std.format : format;
import std.json : JSONType;
import std.stdio : writeln, writefln, readln, writef, stdout;
import std.string : strip, startsWith, join;
import std.sumtype : match;

import llm.agent;
import llm.agent_md;
import llm.app_config : UserConfig, userToLlmConfig, createRag;
import llm.chat;
import llm.coder;
import llm.config;
import llm.memory;
import llm.metric.monitor : MetricMonitor;
import llm.pipeline : prettyPrint;
import llm.plan;
import llm.query;
import llm.rag.rag : RAG;
import llm.session : SessionId, SessionMeta, SessionFile, SessionStore, resolveSessionRef;
import llm.skill;
import llm.tui;
import llm.types : ServerStat, StreamMessage, StreamToolCall;
import llm.utility;
import llmfun_tui;

import my.path : Path, AbsolutePath;
import my.optional : Optional, hasValue, orElse;

/// Unix epoch for timestamp conversion (1970-01-01T00:00:00Z).
private immutable SysTime UnixEpoch = SysTime(DateTime(1970, 1, 1), UTC());

/// Month abbreviation lookup table (1-indexed: Month.feb == 2, so index 1).
private immutable(string[]) MonthAbbr = [
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov",
    "Dec"
];

/// Result of deciding a `/delete` command against the pending confirmation state.
private enum PendingDeleteAction {
    ignore, // no pending confirmation
    confirm, // resolved id matches the pending id
    clear // pending exists but a different id was given
}

/** Build the static `/help` text (module-level so unit tests need no AgentApp). */
private string buildHelpText() {
    string[] s;
    s ~= "llmfun agent mode - type a query and press Tab to start.";
    s ~= " Use /commands for special actions:";
    s ~= "";
    s ~= "   (bare query)       Send a message to the agent";
    s ~= "   /help              Show this help message";
    s ~= "   /quit, /q, /exit   Exit the agent";
    s ~= "   /stop              Stop processing the currently active query";
    s ~= "   /compact           Force compress the chat history";
    s ~= "   /sessions          List chat sessions (index, id, title, preview)";
    s ~= "   /switch <n|id|title>  Switch to another session";
    s ~= "   /new               Start a new chat session";
    s ~= "   /rename <title>    Rename the current session";
    s ~= "   /delete <n>        Delete a session (repeat to confirm)";
    s ~= "   /clear             Clear the current chat history";
    s ~= "   /model             List available models";
    s ~= "   /model <index>     Select model by index";
    s ~= "   /model <name>      Select model by exact name (case-insensitive)";
    s ~= "   /plan <query>      Run the plan pipeline";
    s ~= "   /code <query>      Run the coder pipeline";
    s ~= "   /debug             Toggle verbose debug output";
    s ~= "   /skills            List available skills";
    s ~= "   /refresh-agent-md  Force re-summarize AGENTS.md";
    return s.join("\n");
}

struct AgentApp {
    private {
        LlmConfig llmConf;
        RAG rag;
        MetricMonitor monitor;
        Agent agent_;
        SessionStore sessionStore;
        SessionMeta activeSession;
        SessionId pendingDeleteId; // session id awaiting /delete confirmation (Task 10)
        bool oneShotQuery;
        Tid uiTid;
        ServerStat lastServerStat;
        bool debugMode;
        UserConfig.AgentChatConfig conf_;
        UiMessenger uiMsg;
        SkillManager skillManager_;

        enum AgentStatus {
            active,
            terminate
        }
    }

    @disable this(this);

    this(UserConfig.AgentChatConfig conf) {
        this.conf_ = conf;
        this.uiMsg = new UiMessenger(Tid.init, true);
    }

    void dispose() {
        if (uiTid != Tid.init) {
            try {
                uiMsg.terminate();
            } catch (Exception) {
                // UI thread may have already terminated
            }
            uiTid = Tid.init;
        }
        if (rag) {
            rag.destroy;
            rag = null;
        }
        if (agent_) {
            if (activeSession.id.length > 0) {
                // processResult already commits after every query; this second
                // save is a safety net for error/early-exit paths (harmless
                // rewrite that bumps updatedAt once more).
                commitActiveSession();
            }
            llmConf.saveState();
            agent_ = null;
        }
    }

    // TODO: If help text ever needs externalization (config file, i18n),
    //       the function signature should accept a content parameter.
    private string printHelp(UserConfig.AgentChatConfig conf) {
        import std.process : environment;

        if (environment.get("LLMFUN_NO_SPLASH") || !conf.prompt.empty)
            return null;

        return buildHelpText();
    }

    private string formatSkillsList() {
        if (skillManager_ is null) {
            return "No skill manager initialized.";
        }

        auto skills = skillManager_.getManifest();
        if (skills.empty) {
            return "No skills are currently loaded.";
        }

        auto alwaysApplyCount = skills.filter!(skill => skill.alwaysApply).array.length;
        auto lines = appender!(string[])();
        lines.put("Available skills:");
        lines.put("");

        foreach (skill; skills) {
            auto tag = skill.alwaysApply ? " [always-apply]" : "";
            auto desc = skill.description.length > 80
                ? skill.description[0 .. 77] ~ "..." : skill.description;
            lines.put(format("  %-25s %s", skill.name ~ tag, desc));
        }

        lines.put("");
        lines.put(i"$(skills.length) skills available, $(alwaysApplyCount) always-apply".text);
        return lines[].join("\n");
    }

    private void handleRefreshAgentMd() {
        this.sendChatMessage("assistant: Refreshing AGENTS.md... (summarizing, please wait)",
                TuiChatMessageType_Assistant);
        try {
            auto newState = refreshAgentMd(llmConf, rag);
            if (newState.isValid()) {
                agent_.setSystemPrompt(llmConf.getPrompt(skillManager: skillManager_, promptName: llmConf.agentPrompt,
                        addSkills: true, agentMdSummary: newState.summary));
                this.sendChatMessage("assistant: AGENTS.md refreshed successfully.\nSummary (%d chars):\n%s",
                        TuiChatMessageType_Assistant, newState.summary.length, newState.summary);
            } else {
                this.sendChatMessage("assistant: No AGENTS.md found in workarea, or refresh failed.",
                        TuiChatMessageType_Assistant);
            }
        } catch (Exception e) {
            this.sendChatMessage("assistant: Error refreshing AGENTS.md: %s",
                    TuiChatMessageType_Assistant, e.msg);
        }
    }

    private void sendChatMessage(Args...)(string msg, TuiChatMessageType type, Args args) {
        static if (args.length > 0) {
            msg = format(msg, args);
        }
        uiMsg.chatMessage(msg, type);
    }

    private void sendChatThinkMessage(Args...)(string msg, string thinking,
            TuiChatMessageType type, Args args) {
        static if (args.length > 0) {
            msg = format(msg, args);
        }
        uiMsg.chatThinkMessage(msg, thinking, type);
    }

    private void progressCallback(size_t currentChunk, size_t totalChunks, string status) {
        uiMsg.chatMessage(i"assistant: Compressing... $(currentChunk)/$(totalChunks) : $(status)".text,
                TuiChatMessageType_Assistant);
    }

    private void setStatusText(bool readyState) {
        uiMsg.statusText(formatStatusText(readyState, agent_.modelContextSize,
                lastServerStat, llmConf.activeModelName()));
    }

    private void doCompress(bool force) {
        if (!agent_.needCompression && !force)
            return;
        const ctxUsed = agent_.stat.context;
        uiMsg.busy;
        auto res = agent_.compress(force: force, callback: &this.progressCallback);
        uiMsg.chatMessage(compressionResultToString(res.compressed, res.originalLength,
                res.newLength, res.keptXCount, res.keptXTokens, ctxUsed, res.newContextSize),
                TuiChatMessageType_Assistant);
        uiMsg.ready;
    }

    private void processChatMessage(Chat.MessageT m, bool printUser) {
        m.match!((Message a) {
            if (!a.role.among(Role.user, Role.system) || (printUser && a.role != Role.system)) {
                auto msgType = a.role == Role.user ? TuiChatMessageType_User
                    : TuiChatMessageType_Assistant;
                this.sendChatThinkMessage("%s: %s", a.thinking, msgType, a.role, a.content);
            } else {
                logger.tracef("[%s]: %s", a.role, a.content);
            }
        }, (ToolMessage a) {
            auto calls = summarizeToolCalls(a.toolCalls, 1000);
            this.sendChatThinkMessage("tool call: %(%-s\n%)", a.thinking,
                TuiChatMessageType_ToolCall, calls);
            if (a.isFinalAnswer()) {
                uiMsg.finalAnswer(a.getFinalAnswer());
            }
        }, (ToolResponse a) {
            if (!isHiddenToolResponse(a.toolName)) {
                this.sendChatMessage("tool result %s: %-s %s", TuiChatMessageType_ToolResponse,
                    a.success ? "✅" : "❌", a.toolName, summarizeToolResponse(a, 1000));
            }
        }, (VisionMessage a) {
            this.sendChatMessage("user: %s (with image)", TuiChatMessageType_User, a.content);
        });
    }

    private void processResult(ProcessResult result) {
        logger.trace(result.status != ProcessResult.Status.ok, result);
        lastServerStat = result.stat;
        foreach (m; result.chat) {
            this.processChatMessage(m, printUser: false);
        }
        commitActiveSession();
    }

    /// Save the current agent chat to the active session file.
    /// Strips system messages (D13) before persisting.
    private void commitActiveSession() @trusted nothrow {
        try {
            auto doc = agent_.chat.toSaveJson();

            // D13: strip role: "system" entries from messages
            auto msgs = doc["messages"].array.filter!(entry => entry.type != JSONType.object
                    || !("role" in entry.object) || entry["role"].str != "system").array;
            doc["messages"] = msgs;

            activeSession = sessionStore.save(activeSession.id, activeSession, doc);
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
    }

    // =========================================================================
    // Session orchestration methods (Task 8) — D5/D6
    // =========================================================================

    /** Activate a session by id: load into memory, clear UI, replay history.
     *
     * Loads the session FIRST — on failure: send error and abort, keeping
     * the current in-memory chat and active id (W4). On success: clear
     * history, load doc, reset response index, sync context, clear UI,
     * replay messages, resend UiInitHistory, update status and state.
     */
    private void activateSession(SessionId id) {
        // Commit current session was already done by caller (switchToSession)
        // or this is a fresh activation (startup/delete-active fallback).

        auto sfOpt = sessionStore.load(id);
        if (!hasValue(sfOpt)) {
            this.sendChatMessage("error: Cannot load session '%s' (not found or corrupt). Staying in current session.",
                    TuiChatMessageType_Assistant, id);
            return; // W4: keep current session unchanged
        }

        auto sf = orElse(sfOpt, SessionFile());

        // Clear chat history, keeping system prompt at history[0] (I1: use chat.clear
        // directly instead of clearHistory() to avoid redundant syncContextFromChat)
        agent_.chat.clear;
        // Load the session doc into chat
        agent_.chat.load(sf.doc);
        // W1: prevent replay of old history on next query
        agent_.chat.resetResponseIndex();
        // W5: sync context size from loaded chat
        agent_.syncContextFromChat();
        // Status bar must reflect the target session, not the previous one
        lastServerStat = ServerStat(startContext: agent_.chat.approxContextSize);

        // Clear UI chat and pipeline
        uiMsg.clearChat();
        uiMsg.pipelineClear();

        // Replay messages through the UI
        foreach (m; agent_.chat.getMessages()) {
            this.processChatMessage(m, printUser: true);
        }

        // Resend UiInitHistory (UI mode only)
        if (uiMsg.isActive()) {
            send(uiTid, UiInitHistory(agent_.getUserQueries.map!(a => a.content).array.idup));
        }

        // Update active session and status
        activeSession = sf.meta;
        setStatusText(true);

        // Persist active session id
        llmConf.activeChatSessionId = id.get;
        llmConf.saveState();
    }

    /** Switch to a different session: commit current + activate target.
     *
     * Single commit of the current session (W11), then activate the target.
     * A no-op switch to the already-active session still commits so pending
     * changes are persisted.
     */
    private void switchToSession(SessionId id) {
        if (id == activeSession.id) {
            commitActiveSession(); // persist pending changes on a no-op switch
        } else {
            commitActiveSession();
            activateSession(id);
        }
    }

    /** Format a unix timestamp as a human-readable relative or absolute date. */
    private string formatSessionDate(long unixSec) @trusted {
        if (unixSec == 0)
            return "never";
        // C1: use proper Unix epoch (1970-01-01), not DateTime.init (year 0)
        auto dt = (UnixEpoch + unixSec.dur!"seconds").toLocalTime();
        auto now = Clock.currTime();

        // Same day: show time only
        if (dt.year == now.year && dt.month == now.month && dt.day == now.day) {
            return format("%02d:%02d", dt.hour, dt.minute);
        }
        // Same year: show month abbreviation and day (M1: use lookup table)
        if (dt.year == now.year) {
            return format("%s %02d", MonthAbbr[cast(size_t)(dt.month - 1)], dt.day);
        }
        // Different year: full date
        return format("%04d-%02d-%02d", dt.year, dt.month, dt.day);
    }

    /** Extract the short id (hex suffix) from a full session id. */
    private static string shortSessionId(SessionId id) @safe pure nothrow {
        // Format: YYYYMMDD-HHMMSS-NNNN — return the last 4 hex chars
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
    private void doListSessions() {
        auto sessions = sessionStore.list();

        if (sessions.length == 0) {
            this.sendChatMessage("No sessions found.", TuiChatMessageType_Assistant);
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

        this.sendChatMessage(lines[].join("\n"), TuiChatMessageType_Assistant);
    }

    /** Create a new session and switch to it.
     *
     * Exactly one save of the current session (W11).
     * The confirmation message appears in the new session's chat view (I2).
     */
    private void doCreateSession() {
        auto newMeta = sessionStore.create();
        switchToSession(newMeta.id);
        // Confirmation message sent in the new session's context (I2)
        this.sendChatMessage("Created new session: '%s' (%s)",
                TuiChatMessageType_Assistant, newMeta.title, shortSessionId(newMeta.id));
    }

    /** Rename the active session.
     *
     * Strips whitespace; rejects empty title (keeps previous).
     */
    private void doRenameSession(string title) {
        auto stripped = title.strip;
        if (stripped.length == 0) {
            // runAgent already rejects empty args; this guards direct callers
            // (e.g. Phase 2 sidebar reuse, D5).
            this.sendChatMessage("error: Rename rejected — empty title, keeping previous title '%s'.",
                    TuiChatMessageType_Assistant, activeSession.title);
            return;
        }

        auto result = sessionStore.rename(activeSession.id, stripped);
        if (hasValue(result)) {
            activeSession = orElse(result, SessionMeta());
            this.sendChatMessage("Session renamed to '%s'.",
                    TuiChatMessageType_Assistant, activeSession.title);
        } else {
            this.sendChatMessage("error: Failed to rename session '%s'.",
                    TuiChatMessageType_Assistant, shortSessionId(activeSession.id));
        }
    }
    /** Decide how a `/delete` command proceeds given the pending state (pure).
     *
     * Params:
     *   pendingId = session id awaiting confirmation (SessionId.init = none)
     *   resolvedId = id resolved from this command's argument
     *
     * Returns: confirm when the ids match, clear when a different id is
     *          given, ignore when there is no pending confirmation.
     */
    private static PendingDeleteAction decideDeleteCommand(SessionId pendingId, SessionId resolvedId) @safe pure nothrow {
        if (pendingId.length == 0)
            return PendingDeleteAction.ignore;
        if (pendingId == resolvedId)
            return PendingDeleteAction.confirm;
        return PendingDeleteAction.clear;
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
    private static SessionId pickFallbackAfterDelete(const SessionMeta[] remaining) @safe pure nothrow {
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
     * committing first (W2 skip-save: the deleted file must not be recreated
     * by the fallback switch). Fallback = most recently updated remaining
     * session, else a fresh session.
     */
    private void doDeleteSession(SessionId id) {
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
            activateSession(fallbackId); // never commits (W2)
            // Defensive: if the fallback failed to load, do not keep pointing
            // at the deleted id — a later commit would resurrect the file (W2).
            if (activeSession.id == id) {
                activateSession(sessionStore.create().id);
                createdFresh = true;
            }
        }

        if (wasActive) {
            this.sendChatMessage("Session deleted: %s. Switched to '%s'%s.", TuiChatMessageType_Assistant,
                    shortSessionId(id), activeSession.title, createdFresh ? " (new session)" : "");
        } else {
            this.sendChatMessage("Session deleted: %s",
                    TuiChatMessageType_Assistant, shortSessionId(id));
        }
    }

    private AgentStatus runAgent(string query) {
        // Any command other than /delete clears the pending delete confirmation (Task 10)
        if (!query.startsWith("/delete"))
            pendingDeleteId = SessionId.init;

        if (query.among("/quit", "/q", "/exit")) {
            return AgentStatus.terminate;
        } else if (query == "/compact") {
            this.doCompress(true);
            return AgentStatus.active;
        } else if (query == "/new") {
            doCreateSession();
            return AgentStatus.active;
        } else if (query == "/clear") {
            // Old /new behavior: explicit in-session wipe (F11). Order matters:
            // clearHistory -> UI clear -> context reset -> pipelineClear -> save.
            agent_.clearHistory(); // keeps system prompt at history[0]
            uiMsg.clearChat();
            lastServerStat = ServerStat(startContext: 0); // context resets to 0
            uiMsg.pipelineClear();
            commitActiveSession();
            this.sendChatMessage("Cleared chat history in session '%s'.",
                    TuiChatMessageType_Assistant, shortSessionId(activeSession.id));
            return AgentStatus.active;
        } else if (query == "/help") {
            auto helpText = this.printHelp(conf_);
            if (helpText !is null) {
                this.sendChatMessage(helpText, TuiChatMessageType_User);
            }
            return AgentStatus.active;
        } else if (query == "/debug") {
            debugMode = !debugMode;
            uiMsg.logFile(debugMode);
            logger.globalLogLevel = debugMode ? logger.LogLevel.trace : logger.LogLevel.info;
            this.sendChatMessage("Debug output: %s",
                    TuiChatMessageType_Assistant, debugMode ? "ON" : "OFF");
            return AgentStatus.active;
        } else if (query == "/model" || query.startsWith("/model ")) {
            auto arg = query == "/model" ? "" : query["/model ".length .. $].strip();
            if (arg.empty) {
                auto m = "Available models:";
                foreach (i, model; llmConf.codeModels) {
                    auto activeMarker = (i == cast(size_t) llmConf.activeCodeModelIndex) ? " [active]"
                        : "";
                    m ~= format("  %s  %s%s\n", i, model.name, activeMarker);
                }
                m ~= "Use /model <index> or /model <name> to switch.";
                this.sendChatMessage(m, TuiChatMessageType_Assistant);
            } else {
                const oldModel = llmConf.activeCodeModel.name;
                bool switched;
                size_t idx = ifThrown(arg.to!long, -1);
                if (idx >= 0) {
                    switched = llmConf.selectModelByIndex(idx);
                    if (!switched) {
                        this.sendChatMessage("error: Invalid model index '%s'. Valid indices: 0-%s.",
                                TuiChatMessageType_Assistant, arg, llmConf.codeModels.length - 1);
                    }
                } else {
                    auto result = llmConf.selectModelByName(arg);
                    switched = result.empty;
                    if (!switched)
                        this.sendChatMessage("failed to switch model: %s",
                                TuiChatMessageType_Assistant, result);
                }
                if (switched) {
                    agent_.resetModel(llmConf.activeCodeModel());
                    agent_.setStreamUpdate(makeStreamCallback);
                    this.sendChatMessage("switched to model: %s\nAgent model reset: %s -> %s, context: %s",
                            TuiChatMessageType_Assistant,
                            llmConf.activeModelName(), oldModel,
                            agent_.modelName, agent_.modelContextSize);
                }
            }
            return AgentStatus.active;
        } else if (query.startsWith("/plan ")) {
            uiMsg.pipelineClear;
            auto q = query["/plan ".length .. $];
            sendChatMessage("assistant: Running plan pipeline: %s", TuiChatMessageType_Assistant, q);
            auto result = runPlanPipeline(q, llmConf, rag, monitor, () {
                return isStopAgentTriggered;
            }, llmConf.toolFilter.to(), makePipelineStreamCallback);
            sendChatMessage(prettyPrint(result), TuiChatMessageType_Assistant);
            return AgentStatus.active;
        } else if (query.startsWith("/code ")) {
            uiMsg.pipelineClear;
            auto q = query["/code ".length .. $];
            sendChatMessage("assistant: Running coder pipeline: %s",
                    TuiChatMessageType_Assistant, q);
            auto result = runCoderPipeline(q, llmConf, rag, monitor, () {
                return isStopAgentTriggered;
            }, llmConf.toolFilter.to(), makePipelineStreamCallback);
            if (result.wasInterrupted) {
                this.sendChatMessage("assistant: Pipeline interrupted by user.",
                        TuiChatMessageType_Assistant);
                return AgentStatus.active;
            }
            this.sendChatMessage(i"assistant: $(prettyPrint(result))".text,
                    TuiChatMessageType_Assistant);
            return AgentStatus.active;
        } else if (query == "/skills") {
            this.sendChatMessage(this.formatSkillsList(), TuiChatMessageType_Assistant);
            return AgentStatus.active;
        } else if (query == "/refresh-agent-md") {
            this.handleRefreshAgentMd();
            return AgentStatus.active;
        } else if (query == "/sessions") {
            doListSessions();
            return AgentStatus.active;
        } else if (query.startsWith("/switch ")) {
            auto arg = query["/switch ".length .. $].strip();
            if (arg.empty) {
                this.sendChatMessage(
                        "error: /switch requires an argument. Usage: /switch <index|id|title>. Use /sessions to list.",
                        TuiChatMessageType_Assistant);
            } else {
                auto sessions = sessionStore.list();
                if (sessions.empty) {
                    this.sendChatMessage("No sessions available. Use /new to create one.",
                            TuiChatMessageType_Assistant);
                } else {
                    auto resolved = resolveSessionRef(sessions, arg);
                    if (hasValue(resolved)) {
                        auto id = orElse(resolved, SessionId.init);
                        switchToSession(id);
                    } else {
                        this.sendChatMessage("error: Unknown session '%s'. Use /sessions to list available sessions.",
                                TuiChatMessageType_Assistant, arg);
                    }
                }
            }
            return AgentStatus.active;
        } else if (query.startsWith("/rename ")) {
            auto arg = query["/rename ".length .. $].strip();
            if (arg.empty) {
                this.sendChatMessage("error: /rename requires a title argument. Usage: /rename <title>.",
                        TuiChatMessageType_Assistant);
            } else {
                doRenameSession(arg);
            }
            return AgentStatus.active;
        } else if (query == "/delete" || query.startsWith("/delete ")) {
            auto arg = query == "/delete" ? "" : query["/delete ".length .. $].strip();
            if (arg.empty) {
                this.sendChatMessage("error: /delete requires an index. Usage: /delete <n>.",
                        TuiChatMessageType_Assistant);
                pendingDeleteId = SessionId.init;
                return AgentStatus.active;
            }
            auto idx = ifThrown(arg.to!long, -1L);
            auto sessions = sessionStore.list();
            if (idx < 1 || idx > cast(long) sessions.length) {
                this.sendChatMessage("error: Unknown session index '%s'. Use /sessions to list available sessions.",
                        TuiChatMessageType_Assistant, arg);
                pendingDeleteId = SessionId.init;
                return AgentStatus.active;
            }
            auto resolvedId = sessions[cast(size_t)(idx - 1)].id;
            final switch (decideDeleteCommand(pendingDeleteId, resolvedId)) {
            case PendingDeleteAction.ignore:
                pendingDeleteId = resolvedId;
                this.sendChatMessage("Confirm deletion of session '%s' (%s, %s msgs) by repeating /delete %s.",
                        TuiChatMessageType_Assistant,
                        shortSessionId(resolvedId), sessions[cast(size_t)(idx - 1)].title,
                        sessions[cast(size_t)(idx - 1)].messageCount, arg);
                break;
            case PendingDeleteAction.confirm:
                pendingDeleteId = SessionId.init;
                doDeleteSession(resolvedId);
                break;
            case PendingDeleteAction.clear:
                pendingDeleteId = SessionId.init;
                this.sendChatMessage("Deletion cancelled (different session). Repeat /delete <n> to start over.",
                        TuiChatMessageType_Assistant);
                break;
            }
            return AgentStatus.active;
        } else if (query.empty) {
            return AgentStatus.active;
        } else if (query.startsWith("/")) {
            pendingDeleteId = SessionId.init; // any other command clears pending delete state
            this.sendChatMessage("system: Unknown command: '%s'. Type /help for available commands.",
                    TuiChatMessageType_Assistant, query);
            return AgentStatus.active;
        }

        agent_.addUserQuery(query);
        this.doCompress(false);
        auto result = agent_.runToCompletion(&this.processResult,
                compressCallback: &this.progressCallback, interrupt: () {
            return isStopAgentTriggered;
        });
        return AgentStatus.active;
    }

    private IStreamCallback makeStreamCallback() {
        return new StreamMessageUpdater(uiMsg, agent_.modelContextSize, llmConf.activeModelName);
    }

    private IStreamCallback makePipelineStreamCallback() {
        return new PipelineStreamMessageUpdater(uiMsg, agent_.modelContextSize,
                llmConf.activeModelName);
    }

    private void updateRagMemory() {
        import llm.vfs : FlatVfs;
        import std.path : baseName, stripExtension, dirName;
        import llm.rag.rag : add, Document, Origin, Offset, Topic;
        import my.set;
        import my.path : AbsolutePath;

        // do not slowdown startup if the user only have an in-memory because
        // then they are indexed every time the user start
        if (rag is null || rag.isPrimaryInMemory || llmConf.noMemory)
            return;

        auto vfs = FlatVfs(llmConf.memoryArea);
        Set!string topics;
        foreach (name; vfs.getAllFiles) {
            try {
                vfs.read(name.baseName).match!((string content) {
                    auto res = rag.add(Document(Origin(cast(Path) name),
                        content, Offset.init), llmConf.ragConfig);
                    logger.infof(res.length != 0, "Add memory '%s' to RAG", name);
                    topics.add(name);
                }, (_) {});
            } catch (Exception e) {
                logger.trace(e.msg);
            }
        }
        logger.trace(topics);

        foreach (src; rag.db.getSources) {
            src.origin.match!((Path a) {
                if (a !in topics && vfs.isRoot(a.dirName.AbsolutePath)) {
                    logger.tracef("Removing memory '%s'", a);
                    rag.db.removeSource(src.origin);
                }
            }, (_) {});
        }
    }

    // Session startup: wire SessionStore into startup (design 6.3)
    private void setupSession(AgentMdState agentMdState) {
        sessionStore = new SessionStore(llmConf.scratchArea ~ "chat");
        auto sessions = sessionStore.list();

        // Resolve active session: saved id -> most recent -> create fresh
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

        // Load the active session into agent's chat
        auto sfOpt = sessionStore.load(activeSession.id);
        if (hasValue(sfOpt)) {
            auto sf = orElse(sfOpt, SessionFile());
            agent_.chat.load(sf.doc);
        } else {
            logger.warningf("Failed to load active session '%s'. Starting with empty chat.",
                    activeSession.id);
        }
        agent_.chat.resetResponseIndex; // W1: prevent replay of old history
        agent_.syncContextFromChat(); // W5: set prevStat.context from loaded chat

        agent_.setSystemPrompt(llmConf.getPrompt(skillManager: skillManager_, promptName: llmConf.agentPrompt,
                addSkills: true, agentMdSummary: agentMdState.summary));

        // Persist active session id (covers "fresh session created" path)
        llmConf.activeChatSessionId = activeSession.id.get;
        llmConf.saveState();

        lastServerStat = ServerStat(startContext: agent_.chat.approxContextSize);
    }

    int run(UserConfig uconf) {
        makeDefaultFileStructure();
        if (conf_.setupDirs)
            makeLocalSetupFileStructure(LlmConfig.init);

        llmConf = readConfig(uconf.config, !conf_.prompt.empty,
                uconf.noCwdConfig, uconf.trustedConfig, conf_.workArea).userToLlmConfig(conf_);

        rag = createRag(llmConf);
        if (rag is null)
            return 1;

        skillManager_ = makeSkillManager(llmConf);

        // Process AGENTS.md (hybrid: summary in prompt, full content in RAG)
        auto agentMdState = processAgentMd(llmConf, uconf.noCwdConfig, rag);
        if (agentMdState.isValid())
            logger.tracef("AGENTS.md processed, summary length: %s", agentMdState.summary.length);

        monitor = new MetricMonitor(llmConf.scratchArea ~ "monitor.jsonl");
        agent_ = new Agent("main", llmConf, skillManager_, monitor, rag, llmConf.toolFilter.to());

        setupSession(agentMdState);

        scope (exit)
            this.dispose(); // Ensures cleanup on any exception after setup

        // oneShotQuery: true  = CLI prompt mode (no UI thread, UiMessenger blocked)
        //                 false = full UI mode (UI thread spawned, UiMessenger active)
        oneShotQuery = !conf_.prompt.empty;

        if (oneShotQuery) {
            uiMsg = new UiMessenger(Tid.init, true);
            this.runAgent(conf_.prompt);
            return 0;
        }

        // only update memory for non-oneshot because it is assumed that oneshot need max speed/low latency
        updateRagMemory();

        uiTid = spawn(&spawnUserInterface, thisTid);
        uiMsg = new UiMessenger(uiTid, false);
        uiMsg.setIniFile(llmConf.scratchArea ~ "imgui.ini");
        send(uiTid, UiInitHistory(agent_.getUserQueries.map!(a => a.content).array.idup));
        send(uiTid, UiSetIniFile(llmConf.scratchArea ~ "imgui.ini"));
        agent_.setStreamUpdate(makeStreamCallback);

        foreach (m; agent_.chat.getMessages()) {
            this.processChatMessage(m, printUser: true);
        }
        auto helpText = this.printHelp(conf_);
        if (helpText !is null) {
            this.sendChatMessage(helpText, TuiChatMessageType_User);
        }

        if (llmConf.beginConsolidation) {
            logger.infof("Memory consolidation pending at session #%s", llmConf.sessionCount + 1);
            runMemoryConsolidation(llmConf, rag, monitor, (string msg,
                    TuiChatMessageType t) => this.sendChatMessage(msg, t));
        }

        bool running = true;
        do {
            this.setStatusText(true);
            receive((UiUserQuery a) {
                auto query = a.query.strip;
                if (!query.empty) {
                    this.sendChatMessage(query, TuiChatMessageType_User);
                    uiMsg.busy();
                    clearStopAgent();
                    this.setStatusText(false);
                    final switch (this.runAgent(query)) {
                    case AgentStatus.active:
                        break;
                    case AgentStatus.terminate:
                        uiMsg.terminate();
                        break;
                    }
                    uiMsg.ready();
                }
            }, (UiTerminated _) { running = false; });
        }
        while (running);

        return 0;
    }
}

class UiMessenger {
    Tid uiTid;
    bool blocked;

    this(Tid t, bool b = false) {
        uiTid = t;
        blocked = b;
    }

    bool isActive() {
        return !blocked;
    }

    void setActive(bool onOff) {
        blocked = !onOff;
    }

    void ready() {
        if (blocked)
            return;
        send(uiTid, UiAgentReady.init);
    }

    void busy() {
        if (blocked)
            return;
        send(uiTid, UiAgentBusy.init);
    }

    void terminate() {
        if (blocked)
            return;
        if (uiTid == Tid.init)
            return; // safety: no UI thread to terminate
        send(uiTid, UiTerminate.init);
    }

    void chatMessage(string msg, TuiChatMessageType type) {
        if (blocked) {
            writeln(msg);
        } else {
            send(uiTid, UiChatMessage(msg, type));
        }
    }

    void chatThinkMessage(string msg, string thinking, TuiChatMessageType type) {
        if (blocked) {
            if (!thinking.empty) {
                writeln("Thinking: ", thinking);
            }
            writeln(msg);
        } else {
            send(uiTid, UiChatThinkMessage(msg, thinking, type));
        }
    }

    void statusText(string status) {
        if (blocked)
            return;
        send(uiTid, UiStatusText(status));
    }

    void finalAnswer(string msg) {
        if (blocked) {
            writeln(msg);
        } else {
            send(uiTid, UiFinalAnswer(msg));
        }
    }

    void clearChat() {
        if (blocked)
            return;
        send(uiTid, UiClearChat.init);
    }

    void logFile(bool useFile) {
        if (blocked)
            return;
        send(uiTid, UiLogFile(useFile));
    }

    void setIniFile(string path) {
        if (blocked)
            return;
        send(uiTid, UiSetIniFile(Path(path)));
    }

    // Streaming methods — silently skipped in blocked (one-shot) mode.
    // One-shot mode produces final output via writeln in chatMessage/finalAnswer;
    // incremental streaming feedback is not needed.
    void streamStatusText(string status) {
        statusText(status); // delegate to canonical method
    }

    void streamChatMessage(string msg, string thinking) {
        if (blocked)
            return;
        send(uiTid, UiStreamChatMessage(msg: msg, thinking: thinking));
    }

    void streamChatDone() {
        if (blocked)
            return;
        send(uiTid, UiStreamChatDone.init);
    }

    void pipelineStreamChatMessage(string agentId, string content,
            string thinking, string role, string status) {
        if (blocked)
            return;
        send(uiTid, UiPipelineStreamChatMessage(agentId: agentId, content: content,
                thinking: thinking, role: role, status: status));
    }

    void pipelineStreamDone(string agentId) {
        if (blocked)
            return;
        send(uiTid, UiPipelineStreamDone(agentId));
    }

    void pipelineClear() {
        if (blocked)
            return;
        send(uiTid, UiPipelineClear.init);
    }
}

string formatStatusText(bool readyState, long contextSize, ServerStat stat, string model) {
    return i"Context $(stat.context)/$(contextSize) tokens | $(
            format!"%.1f"(stat.predictedPerSecond)) tok/s | Model: '$(model)' | $(
            readyState ? "Ready" : "Busy")".text;
}

class StreamMessageUpdater : IStreamCallback {
    UiMessenger uiMsg;
    long contextSize;
    string modelName;

    this(UiMessenger messenger, long contextSize, string modelName)
    in (messenger !is null, "UiMessenger must not be null") {
        this.uiMsg = messenger;
        this.contextSize = contextSize;
        this.modelName = modelName;
    }

    override void messageUpdate(StreamMessage msg, StreamToolCall[] tools, ServerStat stat) {
        string content = msg.content;
        if (!tools.empty) {
            foreach (tool; tools) {
                content ~= "\n--- Tool ---\n";
                content ~= tool.toPrettyString(1000);
                content ~= "\n\n";
            }
        }

        uiMsg.streamStatusText(formatStatusText(false, contextSize, stat, modelName));
        uiMsg.streamChatMessage(content, msg.reasoning);
    }

    override void streamMessageDone() {
        uiMsg.streamChatDone();
    }

    override void setId(string id) {
    }

    override IStreamCallback clone() {
        return new StreamMessageUpdater(uiMsg, contextSize, modelName);
    }
}

class PipelineStreamMessageUpdater : IStreamCallback {
    UiMessenger uiMsg;
    long contextSize;
    string modelName;
    string agentId;

    this(UiMessenger messenger, long contextSize, string modelName)
    in (messenger !is null, "UiMessenger must not be null") {
        this.uiMsg = messenger;
        this.contextSize = contextSize;
        this.modelName = modelName;
    }

    override void messageUpdate(StreamMessage msg, StreamToolCall[] tools, ServerStat stat) {
        string content = msg.content;
        if (!tools.empty) {
            foreach (tool; tools) {
                content ~= "\n--- Tool ---\n";
                content ~= tool.toPrettyString(1000);
                content ~= "\n";
            }
        }

        string status = i"Context $(stat.context)/$(contextSize) tokens | $(
                format!"%.1f"(stat.predictedPerSecond)) tok/s".text;

        uiMsg.pipelineStreamChatMessage(agentId: agentId, content: content,
                thinking: msg.reasoning, role: msg.role, status: status);
    }

    override void streamMessageDone() {
        uiMsg.pipelineStreamDone(agentId);
    }

    override void setId(string id) {
        agentId = id;
    }

    override IStreamCallback clone() {
        return new PipelineStreamMessageUpdater(uiMsg, contextSize, modelName);
    }
}

int appMain(UserConfig uconf, UserConfig.AgentChatConfig conf) {
    import llm.subsystem : initLlmfunLocalModel, deinitLlmfunLocalModel;

    initLlmfunLocalModel();
    scope (exit)
        deinitLlmfunLocalModel();

    try {
        auto app = AgentApp(conf);
        return app.run(uconf);
    } catch (Exception e) {
        logger.warning(e.msg);
    }
    return 1;
}

// =============================================================================
// Unit tests for the command-layer pure logic (Task 14)
// =============================================================================

// --- Test: decideDeleteCommand state machine ---

unittest {
    // No pending confirmation -> ignore
    assert(AgentApp.decideDeleteCommand(SessionId.init,
            SessionId("idA")) == PendingDeleteAction.ignore);
    assert(AgentApp.decideDeleteCommand(SessionId.init,
            SessionId.init) == PendingDeleteAction.ignore);

    // Same id -> confirm
    assert(AgentApp.decideDeleteCommand(SessionId("idA"),
            SessionId("idA")) == PendingDeleteAction.confirm);

    // Different id -> clear
    assert(AgentApp.decideDeleteCommand(SessionId("idA"),
            SessionId("idB")) == PendingDeleteAction.clear);
    assert(AgentApp.decideDeleteCommand(SessionId("idA"),
            SessionId.init) == PendingDeleteAction.clear);
}

// --- Test: pickFallbackAfterDelete selects the most recently updated session ---

unittest {
    SessionMeta a, b, c;
    a.id = SessionId("20260618-153045-a1b2");
    a.updatedAt = 100;
    b.id = SessionId("20260618-153045-b3c4");
    b.updatedAt = 300;
    c.id = SessionId("20260618-153045-c5d6");
    c.updatedAt = 200;

    // Input not sorted: the helper must scan for the maximum updatedAt
    auto remaining = [a, b, c];
    assert(AgentApp.pickFallbackAfterDelete(remaining) == b.id,
            "most recently updated session should be picked");

    // Single remaining session wins
    auto single = [a];
    assert(AgentApp.pickFallbackAfterDelete(single) == a.id);

    // Empty list -> caller creates a fresh session
    SessionMeta[] none;
    assert(AgentApp.pickFallbackAfterDelete(none) == SessionId.init);

    // Ties keep the first occurrence (deterministic)
    SessionMeta d;
    d.id = SessionId("20260618-153045-d7e8");
    d.updatedAt = 300;
    auto tie = [b, d];
    assert(AgentApp.pickFallbackAfterDelete(tie) == b.id,
            "ties should resolve deterministically to the first occurrence");
}
