/// Handles the 'agent' subcommand: interactive chat loop with TUI integration.
module llm.app_agent;

import logger = std.logger;
import std.algorithm;
import std.array : empty, array, appender;
import std.concurrency;
import std.conv : to, text;
import std.exception : collectException;
import std.datetime : Clock, SysTime, DateTime, UTC, dur;
import std.format : format;
import std.json : JSONType;
import std.string : strip, startsWith, join;
import std.sumtype : match;

import llm.agent;
import llm.agent_md;
import llm.app_agent.slash;
import llm.app_agent.ui; // UiMessenger, formatStatusText, stream updaters
import llm.app_config : UserConfig, userToLlmConfig, createRag;
import llm.chat;
import llm.config;
import llm.memory;
import llm.metric.monitor : MetricMonitor;
import llm.query;
import llm.rag.rag : RAG;
import llm.session : SessionId, SessionMeta, SessionFile, SessionStore;
import llm.skill;
import llm.tui;
import llm.types : ServerStat, IStreamCallback;
import llm.utility;
import llmfun_tui;

import my.path : Path, AbsolutePath;
import my.optional : Optional, hasValue, orElse;

private immutable SysTime UnixEpoch = SysTime(DateTime(1970, 1, 1), UTC());

private immutable(string[]) MonthAbbr = [
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov",
    "Dec"
];

struct AgentApp {
    package {
        LlmConfig llmConf;
        RAG rag;
        MetricMonitor monitor;
        Agent agent_;
        SessionStore sessionStore;
        SessionMeta activeSession;
        SessionId pendingDeleteId; // session id awaiting /delete confirmation
        ServerStat lastServerStat;
        bool debugMode;
        UserConfig.AgentChatConfig conf_;
        UiMessenger uiMsg;
        SkillManager skillManager_;
        SlashCommandRegistry slashCommands_;
    }

    private {
        bool oneShotQuery;
        Tid uiTid;
    }

    @disable this(this);

    this(UserConfig.AgentChatConfig conf) {
        this.conf_ = conf;
        this.uiMsg = new UiMessenger(Tid.init, true);
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
    /// Not thread-safe: register commands before `run()` (or at module load
    /// via `addStartupSlashCommand`); concurrent `register` from another
    /// thread while the TUI dispatches is a data race.
    public ref SlashCommandRegistry slashCommands() {
        return slashCommands_;
    }

    private void dispose() {
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
    package string printHelp(UserConfig.AgentChatConfig conf) {
        import std.process : environment;

        if (environment.get("LLMFUN_NO_SPLASH") || !conf.prompt.empty)
            return null;

        return slashCommands_.helpText();
    }

    /// Public plugin seam: send a chat message to the UI.
    public void sendChatMessage(Args...)(string msg, TuiChatMessageType type, Args args) {
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

    package void doCompress(bool force) {
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
    package void commitActiveSession() @trusted nothrow {
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
        agent_.chat.load(sf.doc);
        agent_.chat.resetResponseIndex();
        agent_.syncContextFromChat();
        // Status bar must reflect the target session, not the previous one
        lastServerStat = ServerStat(startContext: agent_.chat.approxContextSize);

        uiMsg.clearChat();
        uiMsg.pipelineClear();

        foreach (m; agent_.chat.getMessages()) {
            this.processChatMessage(m, printUser: true);
        }

        if (uiMsg.isActive()) {
            send(uiTid, UiInitHistory(agent_.getUserQueries.map!(a => a.content).array.idup));
        }

        activeSession = sf.meta;
        setStatusText(true);

        llmConf.activeChatSessionId = id.get;
        llmConf.saveState();
    }

    /** Switch to a different session: commit current + activate target.
     *
     * Single commit of the current session (W11), then activate the target.
     * A no-op switch to the already-active session still commits so pending
     * changes are persisted.
     */
    package void switchToSession(SessionId id) {
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
    package static string shortSessionId(SessionId id) @safe pure nothrow {
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
    package void doListSessions() {
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
    package void doCreateSession() {
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
    package void doRenameSession(string title) {
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
     * committing first (W2 skip-save: the deleted file must not be recreated
     * by the fallback switch). Fallback = most recently updated remaining
     * session, else a fresh session.
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
        // Any input other than /delete clears the pending delete confirmation.
        // It applies to bare queries and unknown commands too, so it lives here,
        // not in the registry.
        if (!query.startsWith("/delete"))
            pendingDeleteId = SessionId.init;

        if (query.empty)
            return AgentStatus.active;

        if (slashCommands_.isSlashCommand(query)) {
            // /delete-prefixed non-commands like `/deletefoo` skip the top
            // rule, and the registry's unknown path does not clear pending
            // state . Clear before dispatch — a stale confirmation would
            // otherwise make the next `/delete <n>` confirm-delete without
            // re-prompting.
            if (query.startsWith("/delete") && !slashCommands_.isRegistered(query))
                pendingDeleteId = SessionId.init;
            return slashCommands_.execute(this, query);
        }

        agent_.addUserQuery(query);
        this.doCompress(false);
        auto result = agent_.runToCompletion(&this.processResult,
                compressCallback: &this.progressCallback, interrupt: () {
            return isStopAgentTriggered;
        });
        return AgentStatus.active;
    }

    package IStreamCallback makeStreamCallback() {
        return new StreamMessageUpdater(uiMsg, agent_.modelContextSize, llmConf.activeModelName);
    }

    package IStreamCallback makePipelineStreamCallback() {
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

    private void setupSession(AgentMdState agentMdState) {
        sessionStore = new SessionStore(llmConf.dataDir ~ "chat");
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

    private int run(UserConfig uconf) {
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

        monitor = new MetricMonitor(llmConf.dataDir ~ "monitor.jsonl");
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
        uiMsg.setIniFile(llmConf.dataDir ~ "imgui.ini");
        send(uiTid, UiInitHistory(agent_.getUserQueries.map!(a => a.content).array.idup));
        send(uiTid, UiSetIniFile(llmConf.dataDir ~ "imgui.ini"));
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

// --- Test: dispatcher-level pending-delete parity ---

unittest {
    // The most safety-critical behavior of the dispatcher: a stale
    // `pendingDeleteId` must never make the next `/delete <n>` confirm-delete
    // without re-prompting. runAgent's parity guard
    // (`query.startsWith("/delete") && !slashCommands_.isRegistered(query)`)
    // clears /delete-prefixed NON-commands; the top rule clears everything
    // else; registered /delete-prefixed commands are left to their handlers.
    // The registry's unknown path does NOT clear pending state.
    import llm.app_config : UserConfig;

    // Blocked UiMessenger (W5) — the unknown path writes via writeln, never
    // a null uiMsg dereference.
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    auto stale = SessionId("stale-id");

    // Registered /delete-prefixed command: the dispatcher must NOT clear —
    // the handler owns the delete state machine. deleteprobe's handler does
    // not touch pendingDeleteId, so any dispatcher clear would be visible.
    app.registerSlashCommand(SlashCommand("deleteprobe", [], [],
            SlashArgMode.none, 900, (ref AgentApp a, string arg) => AgentStatus.active));
    app.pendingDeleteId = stale;
    assert(app.runAgent("/deleteprobe") == AgentStatus.active);
    assert(app.pendingDeleteId == stale,
            "dispatcher must not clear pending for registered /delete-prefixed commands");

    // `/deletefoo` (unregistered /delete-prefixed typo): the registry's
    // unknown path does not clear pending, so runAgent's guard is the ONLY
    // clearing path. Without it the next `/delete 3` would confirm-delete
    // immediately instead of re-prompting.
    app.pendingDeleteId = stale;
    assert(app.runAgent("/deletefoo") == AgentStatus.active);
    assert(app.pendingDeleteId == SessionId.init,
            "/delete-prefixed non-commands must clear a stale confirmation");

    // Non-/delete unknown: cleared by the top rule, not the registry.
    app.pendingDeleteId = stale;
    assert(app.runAgent("/nope") == AgentStatus.active);
    assert(app.pendingDeleteId == SessionId.init, "non-/delete inputs clear via the top rule");

    // `/delete` (registered, handler-owned): dispatch still reaches the
    // handler; the empty-arg error path clears pending.
    app.pendingDeleteId = stale;
    assert(app.runAgent("/delete") == AgentStatus.active);
    assert(app.pendingDeleteId == SessionId.init);
}
