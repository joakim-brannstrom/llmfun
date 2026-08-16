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
import std.json : JSONType, JSONValue;
import std.string : strip, startsWith, join;
import std.sumtype : match;

import llm.agent;
import llm.agent_md;
import llm.app_agent.slash;
import llm.app_agent.ui;
import llm.app_config : UserConfig, userToLlmConfig, createRag;
import llm.chat;
import llm.config;
import llm.memory;
import llm.metric.monitor : MetricMonitor;
import llm.query;
import llm.rag.rag : RAG;
import llm.session : SessionId, SessionMeta, SessionFile, SessionStore, isValidId;
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

        // True while the in-memory chat differs from the last persisted
        // state. commitActiveSession() only writes (and bumps updatedAt)
        // when this is set, so mere navigation - switching or re-clicking a
        // row - never rewrites a file and never changes the sidebar sort
        // order. Sorting is by the latest message in a session.
        bool chatDirty;

        ServerStat lastServerStat;
        bool debugMode;
        UserConfig.AgentChatConfig conf_;
        UiMessenger uiMsg;
        SkillManager skillManager_;
        SlashCommandRegistry slashCommands_;

        // The agent loop is commanded to continue.
        // Cleared after it has successfully started working.
        bool forceRunAgentLoop;
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
                // processResult already commits after every query; this is a
                // safety net for error/early-exit paths. Dirty-gated: a chat
                // that was already persisted is not rewritten and its
                // updatedAt stays untouched.
                commitActiveSession();
            }
            // A13: empty-session cleanup on clean exit, after the final
            // commit. The guard is REQUIRED (M9): dispose() runs on the
            // scope(exit) path and a failed setupSession leaves the store
            // null while agent_ is already set. The active session is
            // exempted (W15) even when empty. Single-writer (C8): the
            // sweep runs on the agent thread only; state.json is not
            // touched by it (saved below, unchanged order).
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
                lastServerStat, llmConf.activeModelDisplayName()));
    }

    package void doCompress(bool force) {
        if (!agent_.needCompression && !force)
            return;
        // G1: stamp the owning session into any checkpoint event this
        // compression fires. Pool callbacks set their own or leave "" —
        // Phase 1 refuses to index events with an empty sessionId.
        agent_.setCompressionCheckpointSessionId(activeSession.id.get);
        logger.tracef("compression checkpoint session id: %s", activeSession.id.get);
        const ctxUsed = agent_.stat.context;
        uiMsg.busy;
        auto res = agent_.compress(force: force, callback: &this.progressCallback);
        // A compression that actually rewrote history (summarized or purged
        // entries change the message list) makes the chat dirty; a no-op
        // compression leaves the persisted state untouched.
        if (res.originalLength != res.newLength || res.purgedCount > 0)
            chatDirty = true;
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
    /// Strips system messages before persisting.
    ///
    /// Gated on `chatDirty`: a clean chat (nothing changed since the last
    /// save) skips the write entirely, so switching sessions, re-clicking
    /// the active row, or a clean shutdown can never bump `updatedAt` and
    /// reorder the sidebar. The flag is set by real content changes (a user
    /// query, `/clear`, a compression that rewrote history) and cleared
    /// here on success; a failed save keeps it so the next commit retries.
    package void commitActiveSession() @trusted nothrow {
        if (!chatDirty)
            return;
        try {
            auto doc = agent_.chat.toSaveJson();

            // Strip role: "system" entries from messages
            auto msgs = doc["messages"].array.filter!(entry => entry.type != JSONType.object
                    || !("role" in entry.object) || entry["role"].str != "system").array;
            doc["messages"] = msgs;

            // Persist the TurnID counter high-water mark as a session-header
            // key (A5); the session store preserves unknown header keys via
            // meta.extra (D2) and rebuilds the header on save.
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
            // Note: no sendSessionList() here - the list refresh happens
            // only on the success path to keep exactly one send per
            // sidebar action (L10). Adding a send here would double-send
            // in doDeleteSession's defensive branch (failed first
            // activation, then a successful create() activation).
        }

        auto sf = orElse(sfOpt, SessionFile());

        // Clear chat history, keeping system prompt at history[0] (I1: use chat.clear
        // directly instead of clearHistory() to avoid redundant syncContextFromChat)
        agent_.chat.clear;
        agent_.chat.load(sf.doc);
        // The loaded chat matches the persisted file: nothing to save yet.
        chatDirty = false;
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

        // R8/L10: the switch path terminates here - send the refreshed
        // snapshot (covers switch, delete-active fallback, and any
        // startup-triggered switches). Guarded for one-shot mode inside.
        sendSessionList();
    }

    /** Switch to a different session: commit current + activate target.
     *
     * Single commit of the current session (W11), then activate the target.
     * A no-op switch to the already-active session still commits, so pending
     * changes are persisted. The commit is dirty-gated: when the chat is
     * already persisted it is a no-op and `updatedAt` stays untouched, so
     * navigation never changes the sidebar sort order.
     */
    package void switchToSession(SessionId id) {
        if (id == activeSession.id) {
            commitActiveSession(); // persist pending changes on a no-op switch
            // R8/L10: activateSession is not called on the no-op path, so
            // refresh here - the commit may have changed counts/preview.
            sendSessionList();
        } else {
            commitActiveSession();
            activateSession(id); // sends the refreshed list itself (L10)
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
        if (stripped.empty) {
            // runAgent already rejects empty args; this guards direct callers.
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
        // R8/L10: mutating callee - the active session's title changed, so
        // the sidebar snapshot is refreshed here (slash /rename is exempt
        // from the single-send rule: the receive-loop refresh may repeat it).
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
            // L10 single-send note: a FAILED activation returns before
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
            this.sendChatMessage("Session deleted: %s. Switched to '%s'%s.", TuiChatMessageType_Assistant,
                    shortSessionId(id), activeSession.title, createdFresh ? " (new session)" : "");
        } else {
            this.sendChatMessage("Session deleted: %s",
                    TuiChatMessageType_Assistant, shortSessionId(id));
            // R8/L10: no activateSession on this path, so the removed session
            // must leave the sidebar here (the active-delete path refreshes
            // inside activateSession).
            sendSessionList();
        }
    }

    /** Map a session list to the sidebar snapshot items (pure).
     *
     * `isActive` marks the session whose id equals `activeId`; all other
     * fields pass through unchanged, preserving the caller's sort order
     * (the store already sorts by updatedAt descending).
     */
    package static UiSessionItem[] mapSessionItems(const SessionMeta[] sessions, SessionId activeId) @safe pure nothrow {
        auto items = new UiSessionItem[sessions.length];
        foreach (i, ref const s; sessions) {
            items[i] = UiSessionItem(s.id, s.title, s.preview, s.messageCount,
                    s.id.get == activeId.get);
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
     */
    private void sendSessionList() {
        if (!uiMsg.isActive())
            return; // one-shot mode: no UI thread to send to
        auto items = mapSessionItems(sessionStore.list(), activeSession.id);
        send(uiTid, cast(immutable) UiSessionList(items));
    }

    /** Sidebar select handler (UiSessionSelect): clear pending delete (A5),
     * D12-validate the untrusted UI id, then switch. Store failures degrade
     * to a chat message (N2/L9) - the receive loop keeps running.
     */
    package void doSidebarSelect(SessionId id) {
        pendingDeleteId = SessionId.init; // A5
        try {
            if (!isValidId(id)) {
                this.sendChatMessage("error: Invalid session id '%s'. Switch rejected.",
                        TuiChatMessageType_Assistant, id);
                return;
            }
            switchToSession(id); // sends the refreshed list (L10)
        } catch (Exception e) {
            this.sendChatMessage("error: Failed to switch session: %s.",
                    TuiChatMessageType_Assistant, e.msg);
        }
    }

    /** Sidebar new handler (UiSessionNew): clear pending delete (A5), then
     * create + switch. Store failures degrade to a chat message (N2/L9).
     */
    package void doSidebarNew() {
        pendingDeleteId = SessionId.init; // A5
        try {
            doCreateSession(); // sends the refreshed list (L10)
        } catch (Exception e) {
            this.sendChatMessage("error: Failed to create session: %s.",
                    TuiChatMessageType_Assistant, e.msg);
        }
    }

    /** Sidebar rename handler (UiSessionRename): clear pending delete (A5),
     * D12-validate the id, reject empty titles only (no length cap, mirrors
     * /rename), rename the CARRIED id (A8), refresh the active meta on
     * success, and always send the refreshed list (L10 - the rename goes
     * straight to the store, so this handler owns the send on both paths).
     */
    package void doSidebarRename(SessionId id, string title) {
        pendingDeleteId = SessionId.init; // A5
        try {
            if (!isValidId(id)) {
                this.sendChatMessage("error: Invalid session id '%s'. Rename rejected.",
                        TuiChatMessageType_Assistant, id);
                return;
            }
            // Exact mirror of doRenameSession: reject empty/stripped
            // titles only; no length cap anywhere (SessionStore.rename
            // accepts any non-empty title).
            auto stripped = title.strip;
            if (stripped.length == 0) {
                this.sendChatMessage("error: Rename rejected — empty title.",
                        TuiChatMessageType_Assistant);
                return;
            }
            auto result = sessionStore.rename(id, stripped);
            if (hasValue(result)) {
                auto newMeta = orElse(result, SessionMeta());
                if (id == activeSession.id)
                    activeSession = newMeta; // keep the active meta fresh
                this.sendChatMessage("Session renamed to '%s'.",
                        TuiChatMessageType_Assistant, newMeta.title);
            } else {
                // M3: unknown/corrupt id - error message, active meta
                // unchanged, list still refreshed below.
                this.sendChatMessage("error: Failed to rename session '%s'.",
                        TuiChatMessageType_Assistant, shortSessionId(id));
            }
            // L10: rename goes straight to the store (no sending callee),
            // so this handler sends the refreshed list on both paths.
            sendSessionList();
        } catch (Exception e) {
            this.sendChatMessage("error: Failed to rename session: %s.",
                    TuiChatMessageType_Assistant, e.msg);
        }
    }

    /** Sidebar delete handler (UiSessionDelete): clear pending delete (A5),
     * D12-validate the id, then delete. The C++ panel already ran the
     * two-step confirmation, so D delegates to the Phase 1 method (active
     * fallback incl.).
     */
    package void doSidebarDelete(SessionId id) {
        pendingDeleteId = SessionId.init; // A5
        try {
            if (!isValidId(id)) {
                this.sendChatMessage("error: Invalid session id '%s'. Delete rejected.",
                        TuiChatMessageType_Assistant, id);
                return;
            }
            doDeleteSession(id); // sends the refreshed list (L10)
        } catch (Exception e) {
            this.sendChatMessage("error: Failed to delete session: %s.",
                    TuiChatMessageType_Assistant, e.msg);
        }
    }

    // TODO: make the method nothrow to ensure it never accidentally exited
    private AgentStatus runAgent(string query) {
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
            // state . Clear before dispatch — a stale confirmation would
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
            // the result has already been processed by this.processResult
            auto ignored = agent_.runToCompletion(&this.processResult,
                    compressCallback: &this.progressCallback, interrupt: () {
                return isStopAgentTriggered;
            });
        }

        return rval;
    }

    package IStreamCallback makeStreamCallback() {
        return new StreamMessageUpdater(uiMsg, agent_.modelContextSize,
                llmConf.activeModelDisplayName);
    }

    package IStreamCallback makePipelineStreamCallback() {
        return new PipelineStreamMessageUpdater(uiMsg, agent_.modelContextSize,
                llmConf.activeModelDisplayName);
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
        sessionStore = new SessionStore(llmConf.chatDir);
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
            // Startup loads persisted state: nothing to save yet.
            chatDirty = false;
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

    package void continueAgent() {
        forceRunAgentLoop = true;
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

        // Register BEFORE setupSession(): a throw in setupSession (e.g. an
        // unusable session dir) must still run dispose(), which then finds
        // sessionStore null while agent_ is set - the M9 guard inside
        // dispose() covers exactly this production-reachable shape.
        scope (exit)
            this.dispose(); // Ensures cleanup on any exception after setup

        setupSession(agentMdState);

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

        // R8: initial sidebar snapshot right after the message replay;
        // guarded by uiMsg.isActive() so one-shot mode never sends.
        sendSessionList();

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
                    // every completed query can change counts/preview and the
                    // updatedAt sort order (commitActiveSession on save), so
                    // refresh the sidebar snapshot.
                    sendSessionList();
                }
            }, (UiSessionSelect a) { this.doSidebarSelect(a.id); }, (UiSessionNew _) {
                this.doSidebarNew();
            }, (UiSessionRename a) { this.doSidebarRename(a.id, a.title); }, (UiSessionDelete a) {
                this.doSidebarDelete(a.id);
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

// --- Test: sidebar snapshot mapping (SessionMeta[] -> UiSessionItem[]) ---

unittest {
    SessionMeta a, b;
    a.id = SessionId("20260618-153045-a1b2");
    a.title = "Alpha";
    a.preview = "prev a";
    a.messageCount = 3;
    a.userMessageCount = 2;
    b.id = SessionId("20260618-153045-b3c4");
    b.title = "Beta";
    b.preview = "prev b";
    b.messageCount = 5;
    b.userMessageCount = 1;

    // Active marker follows the activeId argument; order is preserved.
    auto items = AgentApp.mapSessionItems([a, b], b.id);
    assert(items.length == 2);
    assert(items[0].id == a.id);
    assert(items[0].title == "Alpha");
    assert(items[0].preview == "prev a");
    assert(items[0].messageCount == 3);
    assert(!items[0].isActive);
    assert(items[1].id == b.id);
    assert(items[1].title == "Beta");
    assert(items[1].messageCount == 5);
    assert(items[1].isActive);

    // Empty input -> empty snapshot; no active session -> no active row.
    assert(AgentApp.mapSessionItems([], SessionId.init).length == 0);
    auto noActive = AgentApp.mapSessionItems([a], SessionId.init);
    assert(noActive.length == 1 && !noActive[0].isActive);
}

// --- Test: sidebar invalid-id rejection for each action type (D12) ---

unittest {
    import llm.app_config : UserConfig;

    // No store is configured: reaching the store would crash the test, so
    // a green run proves rejection happens BEFORE any store access.
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    auto bad = SessionId("bad-id");

    app.doSidebarSelect(bad);
    assert(app.pendingDeleteId == SessionId.init, "Select clears pending delete (A5)");

    app.doSidebarRename(bad, "x");
    assert(app.pendingDeleteId == SessionId.init, "Rename clears pending delete (A5)");

    app.doSidebarDelete(bad);
    assert(app.pendingDeleteId == SessionId.init, "Delete clears pending delete (A5)");
}

// --- Test: sidebar rename input validation (empty title rejected, long
// non-empty title accepted - no length cap, matches /rename) ---

unittest {
    import std.file : exists, rmdirRecurse, mkdirRecurse;
    import std.path : buildPath;
    import std.array : replicate;

    auto tmpDir = buildPath("llmfun_test", "app_agent_rename_validate");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.sessionStore = store;
    app.activeSession = meta;

    // Whitespace-only title: rejected, store unchanged.
    app.doSidebarRename(meta.id, "   ");
    assert(orElse(store.load(meta.id), SessionFile()).meta.title == meta.title,
            "empty title must not change the stored title");

    // Long non-empty title: accepted (no length cap anywhere).
    auto longTitle = "T".replicate(300);
    app.doSidebarRename(meta.id, longTitle);
    assert(orElse(store.load(meta.id), SessionFile()).meta.title == longTitle,
            "long non-empty title must be accepted");
    assert(app.activeSession.title == longTitle,
            "active meta must refresh when the renamed id is active");
}

// --- Test: sidebar rename-none error handling (unknown id, corrupt file:
// error emitted, active meta unchanged, list still refreshed) ---

unittest {
    import std.file : exists, rmdirRecurse, mkdirRecurse, write;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_rename_none");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.sessionStore = store;
    app.activeSession = meta;
    // Active UiMessenger pointed at this thread: the handler's error chat
    // message and the sendSessionList() refresh both land in this thread's
    // own mailbox, so the test can observe "error emitted" and "list still
    // refreshed" without spawning a thread.
    app.uiMsg = new UiMessenger(thisTid, false);
    app.uiTid = thisTid;

    // Unknown id (valid format, no file): rename -> none -> error message,
    // active meta unchanged, list still refreshed.
    auto unknown = SessionId("20260618-153045-ffff");
    app.doSidebarRename(unknown, "New title");
    bool gotError = false;
    receiveTimeout(dur!"seconds"(1), (UiChatMessage m) {
        gotError = m.msg == "error: Failed to rename session 'ffff'.";
    });
    assert(gotError, "rename-none must emit the error chat message");
    assert(app.activeSession.id == meta.id, "rename-none must keep the active meta");
    assert(app.activeSession.title == meta.title);
    bool gotList = false;
    receiveTimeout(dur!"seconds"(1), (immutable UiSessionList l) {
        gotList = true;
        assert(l.items.length == 1, "list still refreshed on rename-none");
    });
    assert(gotList, "rename-none must still refresh the sidebar list");

    // Corrupt file (valid id, garbage JSON): same none path.
    write(buildPath(tmpDir, "20260618-153046-0bad.json"), "{ not json !!!");
    auto corrupt = SessionId("20260618-153046-0bad");
    app.doSidebarRename(corrupt, "New title");
    gotError = false;
    receiveTimeout(dur!"seconds"(1), (UiChatMessage m) {
        gotError = m.msg == "error: Failed to rename session '0bad'.";
    });
    assert(gotError, "corrupt-file rename must emit the error chat message");
    assert(app.activeSession.id == meta.id, "corrupt-file rename must keep the active meta");
    gotList = false;
    receiveTimeout(dur!"seconds"(1), (immutable UiSessionList l) {
        gotList = true;
    });
    assert(gotList, "corrupt-file rename must still refresh the sidebar list");
}

// --- Test: stale pending-delete clearing by the sidebar New handler (A5)
// and store-exception degradation in a sidebar handler (L9) ---

unittest {
    import std.file : exists, rmdirRecurse, mkdirRecurse;
    import std.path : buildPath;

    // A store whose create() throws simulates a disk-full failure (L9).
    static class ThrowingStore : SessionStore {
        this(string dir) {
            super(dir.Path);
        }

        override SessionMeta create() @trusted {
            throw new Exception("simulated disk full");
        }
    }

    auto tmpDir = buildPath("llmfun_test", "app_agent_new_throw");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.sessionStore = new ThrowingStore(tmpDir);
    auto stale = SessionId("stale-pending");
    app.pendingDeleteId = stale;

    // The handler clears pendingDeleteId on entry (A5) even though the
    // create() below throws; the exception is caught and logged as a chat
    // message (N2/L9) - the receive loop keeps running.
    app.doSidebarNew();
    assert(app.pendingDeleteId == SessionId.init,
            "New handler must clear stale pending delete on entry (A5)");
}

// --- Test: sidebar snapshot keeps store order (updatedAt descending) with
// the active marker; clicking must not reorder ---

unittest {
    import std.file : exists, mkdirRecurse, rmdirRecurse, write;
    import std.format : format;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_snapshot_order");
    // Clear any stale dir from a crashed earlier run so the deterministic
    // store-order precondition cannot be disturbed by leftover files.
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    // Three sessions with distinct updatedAt so the store order is
    // deterministic: a (100) < c (200) < b (300) -> store order [b, c, a].
    auto a = SessionId("20260618-153045-a1b2");
    auto b = SessionId("20260618-153045-b3c4");
    auto c = SessionId("20260618-153045-c5d6");
    void writeSession(SessionId id, long updatedAt) {
        write(buildPath(tmpDir, id.get ~ ".json"),
                format(`{"title": "T", "createdAt": 100, "updatedAt": %d, "messages": []}`,
                    updatedAt));
    }

    writeSession(a, 100);
    writeSession(b, 300);
    writeSession(c, 200);

    auto store = new SessionStore(tmpDir.Path);
    assert(store.list().map!(s => s.id).array == [b, c, a],
            "store order must be updatedAt descending");

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.sessionStore = store;
    app.activeSession.id = c; // the middle one is active
    // Active UiMessenger pointed at this thread: sendSessionList() lands in
    // this thread's own mailbox (same pattern as the rename-none test).
    app.uiMsg = new UiMessenger(thisTid, false);
    app.uiTid = thisTid;

    // Active session stays in store order, only marked active.
    app.sendSessionList();
    bool gotList = false;
    receiveTimeout(dur!"seconds"(1), (immutable UiSessionList l) {
        gotList = true;
        assert(l.items.length == 3);
        assert(l.items[0].id == b && !l.items[0].isActive,
            "most recent session leads regardless of the active marker");
        assert(l.items[1].id == c && l.items[1].isActive,
            "the active session must stay at its store position");
        assert(l.items[2].id == a && !l.items[2].isActive);
    });
    assert(gotList, "sendSessionList must emit the store-ordered snapshot");

    // Active at index 0 (store order): order unchanged, marked active.
    app.activeSession.id = b;
    app.sendSessionList();
    gotList = false;
    receiveTimeout(dur!"seconds"(1), (immutable UiSessionList l) {
        gotList = true;
        assert(l.items.length == 3);
        assert(l.items[0].id == b && l.items[0].isActive);
        assert(l.items[1].id == c && l.items[2].id == a, "snapshot must keep the store order");
    });
    assert(gotList, "sendSessionList must emit the store-ordered snapshot");

    // Active absent from the list: snapshot keeps the store order.
    app.activeSession.id = SessionId("20260618-153045-9999");
    app.sendSessionList();
    gotList = false;
    receiveTimeout(dur!"seconds"(1), (immutable UiSessionList l) {
        gotList = true;
        assert(l.items.length == 3);
        assert(l.items[0].id == b && l.items[1].id == c && l.items[2].id == a,
            "absent active id must leave the snapshot in store order");
        assert(!l.items[0].isActive && !l.items[1].isActive && !l.items[2].isActive);
    });
    assert(gotList, "sendSessionList must emit the store-ordered snapshot");

    // Empty store: empty snapshot, no throw.
    app.sessionStore = new SessionStore(buildPath(tmpDir, "empty_sub").Path);
    app.sendSessionList();
    gotList = false;
    receiveTimeout(dur!"seconds"(1), (immutable UiSessionList l) {
        gotList = true;
        assert(l.items.length == 0, "empty store must produce an empty snapshot");
    });
    assert(gotList, "sendSessionList must emit the empty snapshot");
}

// --- Test: navigation never rewrites a session (updatedAt unchanged) and a
// dirty chat commits with an updatedAt bump ---

unittest {
    import my.filter : ReFilter;
    import std.file : exists, mkdirRecurse, rmdirRecurse, write;
    import std.format : format;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_switch_no_bump");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);

    // Two sessions with distinct updatedAt: newer (300) and older (100).
    auto newer = SessionId("20260618-153045-b3c4");
    auto older = SessionId("20260618-153045-a1b2");
    void writeSession(SessionId id, long updatedAt) {
        write(buildPath(tmpDir, "chat", id.get ~ ".json"), format(
                `{"title": "T", "createdAt": 100, "updatedAt": %d,` ~ `"messages": [{"role": "user", "content": "hi"}]}`,
                updatedAt));
    }

    writeSession(newer, 300);
    writeSession(older, 100);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);
    auto newerFile = store.load(newer);
    assert(hasValue(newerFile), "setup: active session file must load");
    app.activeSession = orElse(newerFile, SessionFile()).meta;

    const long newerUpdatedBefore = app.activeSession.updatedAt;

    // Switching away from the clean active session must not rewrite it.
    app.switchToSession(older);
    auto afterSwitch = orElse(store.load(newer), SessionFile());
    assert(afterSwitch.meta.updatedAt == newerUpdatedBefore,
            "switching must not bump updatedAt of the session you leave");

    // Switching back: same guarantee for the other direction.
    const long olderUpdatedBefore = app.activeSession.updatedAt;
    app.switchToSession(newer);
    assert(orElse(store.load(older), SessionFile()).meta.updatedAt == olderUpdatedBefore,
            "switching back must not bump updatedAt either");
    assert(app.activeSession.id == newer, "setup: active is the newer session again");

    // A real content change (a user query) marks the chat dirty and the
    // commit bumps updatedAt.
    const long beforeQuery = app.activeSession.updatedAt;
    app.agent_.addUserQuery("hello newer");
    app.chatDirty = true;
    app.commitActiveSession();
    assert(app.chatDirty == false, "a successful commit must clear the dirty flag");
    assert(app.activeSession.updatedAt >= beforeQuery, "a query commit must bump updatedAt");

    // A clean commit is a no-op: no further bump, no rewrite.
    const long afterQuery = app.activeSession.updatedAt;
    app.commitActiveSession();
    assert(app.activeSession.updatedAt == afterQuery, "a clean commit must not rewrite the file");
}

version (unittest) {
    /// Minimal headless LlmConfig for constructing a real Agent in tests:
    /// one code model (never contacted) plus a temp summary prompt file.
    /// dataDir is intentionally NOT created so dispose()'s saveState() is
    /// a no-op - the tests never write a state.json.
    ///
    /// Coupling invariants: the Agent is built with null SkillManager/
    /// MetricMonitor/RAG (tolerated by the constructor) and a model name
    /// that is never contacted; any future Agent constructor change must
    /// keep this helper compiling and passing.
    private LlmConfig testLlmConfig(string tmpDir) {
        import std.file : mkdirRecurse, write;
        import std.path : buildPath;
        import llm.common.config : ServerConfig;

        auto promptDir = buildPath(tmpDir, "prompt");
        mkdirRecurse(promptDir);
        write(buildPath(promptDir, "SUMMARY.md"), "test summary prompt");

        LlmConfig cfg;
        cfg.codeModels = [
            CodeModelConfig(server: ServerConfig.init, modelName: "test-model")
        ];
        cfg.activeCodeModelIndex = 0;
        cfg.promptDir = [promptDir.Path];
        cfg.dataDir = buildPath(tmpDir, "no-state").Path;
        return cfg;
    }
}

// --- Test: dispose() sweeps empty non-active sessions on exit (A13) ---

unittest {
    import my.filter : ReFilter;
    import std.file : exists, mkdirRecurse, rmdirRecurse;
    import std.json : JSONValue;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_dispose_sweep");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);

    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);
    auto active = store.create();
    auto emptyNonActive = store.create();
    auto nonEmpty = store.create();

    // nonEmpty gets one user message via save().
    JSONValue doc;
    doc["messages"] = JSONValue();
    doc["messages"].array = [];
    JSONValue userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = "hello";
    doc["messages"].array ~= userMsg;
    assert(store.save(nonEmpty.id, nonEmpty, doc).userMessageCount == 1,
            "setup: non-empty session needs 1 user message");

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    app.activeSession = active;
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);
    // The in-memory chat carries the active session's user message, as in
    // production after a query: the dirty flag is set so
    // commitActiveSession() persists it BEFORE the sweep.
    app.agent_.addUserQuery("active hello");
    app.chatDirty = true;

    app.dispose(); // must not throw

    assert(!exists(buildPath(tmpDir, "chat", emptyNonActive.id.get ~ ".json")),
            "empty non-active session file must be swept on exit");
    assert(exists(buildPath(tmpDir, "chat", active.id.get ~ ".json")),
            "active session must survive dispose()");
    assert(exists(buildPath(tmpDir, "chat", nonEmpty.id.get ~ ".json")),
            "non-empty non-active session must survive");
    assert(orElse(store.load(active.id), SessionFile()).meta.userMessageCount == 1,
            "commit runs before the sweep: active session keeps its user message");
    assert(app.agent_ is null, "dispose() must clear the agent");
}

// --- Test: dispose() with a null session store (M9 guard) ---

unittest {
    import my.filter : ReFilter;
    import std.file : exists, mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_dispose_null_store");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    // M9: a failed setupSession leaves the store null while agent_ is set
    // and the active id empty; dispose() must not dereference the store.
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);
    app.dispose(); // must not throw
    assert(app.agent_ is null, "dispose() must clear the agent");
}

// --- Test: dispose() keeps the active session even when empty (W15) ---

unittest {
    import my.filter : ReFilter;
    import std.file : exists, mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_dispose_all_empty");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);

    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);
    auto active = store.create(); // stays empty on purpose (W15)
    auto emptyOther = store.create();

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    app.activeSession = active;
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);

    app.dispose(); // must not throw

    assert(exists(buildPath(tmpDir, "chat", active.id.get ~ ".json")),
            "active session must survive dispose() even when empty (W15)");
    assert(!exists(buildPath(tmpDir, "chat", emptyOther.id.get ~ ".json")),
            "empty non-active session must be swept on exit");
}

// --- Test: switch-after-compression resets stat().startContext to the
// target chat's approxContextSize (R16) ---
//
// Regression lock for the activate pipeline: activateSession must call
// syncContextFromChat() AFTER chat.load so prevStat no longer carries the
// previous session's context. The "compressed" state is modeled headlessly
// (no server call): loading the small session and syncing leaves exactly
// the invariant a real compression leaves behind (prevStat.startContext ==
// current chat context). A stale value would fail the asserts below.

unittest {
    import my.filter : ReFilter;
    import std.array : replicate;
    import std.file : exists, mkdirRecurse, rmdirRecurse;
    import std.json : JSONValue;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_switch_after_compression");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);

    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);
    auto small = store.create(); // active session: small context
    auto large = store.create(); // switch target: clearly larger context

    // Small doc: one short user message (~2 tokens).
    JSONValue smallDoc;
    smallDoc["messages"] = JSONValue();
    smallDoc["messages"].array = [];
    JSONValue smallUser;
    smallUser["role"] = "user";
    smallUser["content"] = "short";
    smallDoc["messages"].array ~= smallUser;
    small = store.save(small.id, small, smallDoc);

    // Large doc: one long user message (400 chars -> ~200 tokens).
    JSONValue largeDoc;
    largeDoc["messages"] = JSONValue();
    largeDoc["messages"].array = [];
    JSONValue largeUser;
    largeUser["role"] = "user";
    largeUser["content"] = "x".replicate(400);
    largeDoc["messages"].array ~= largeUser;
    large = store.save(large.id, large, largeDoc);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    app.activeSession = small;
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);

    // Model the post-compression state of the small session: the agent
    // chat holds the (small) session history and prevStat is synced from
    // it. staleStart is the value that would survive the switch if
    // activateSession ever stopped calling syncContextFromChat.
    app.agent_.chat.load(smallDoc);
    app.agent_.syncContextFromChat();
    const long staleStart = app.agent_.stat().startContext;
    // Modeling precondition only: syncContextFromChat() sets prevStat from
    // the chat by construction, so this cannot fail for a production bug -
    // it just confirms the model matches what a real compression leaves.
    assert(staleStart == app.agent_.chat.approxContextSize,
            "setup: prevStat must hold the small session's context");

    app.switchToSession(large.id);

    assert(app.activeSession.id == large.id, "switch must land on the target session");
    // The regression lock: startContext is reset to the TARGET chat's size.
    // A stale compressed value (staleStart) fails this assert.
    assert(app.agent_.stat().startContext == app.agent_.chat.approxContextSize,
            "stat().startContext must be reset to the target chat's context after switching");
    // Exact expectation derived from the payload defined above, immune to
    // payload-size edits and to any plausible ApproxTokenSize change (both
    // sides scale together), and still fails on a stale prevStat. The
    // large payload is 400 'x' chars, the small one "short" (5 chars).
    import llm.common.config : ApproxTokenSize; // public, stable
    const long expectedLarge = largeUser["content"].str.length / ApproxTokenSize; // ~200
    assert(app.agent_.stat().startContext == expectedLarge,
            "stat().startContext must equal the target chat's exact approx context");
    assert(app.agent_.stat().startContext > staleStart,
            "target context must be clearly larger than the stale compressed value");
}

// --- Test: delete-active falls back to the most recently updated
// remaining session and the snapshot drops the deleted id (R16) ---

unittest {
    import my.filter : ReFilter;
    import std.file : exists, mkdirRecurse, rmdirRecurse, write;
    import std.format : format;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_delete_active_fallback");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);

    // Three sessions with distinct updatedAt: a (400, the active one to
    // delete) > b (300, the expected fallback) > c (200).
    auto a = SessionId("20260618-153045-a1b2");
    auto b = SessionId("20260618-153045-b3c4");
    auto c = SessionId("20260618-153045-c5d6");
    // Test-only helper: userContent must be JSON-safe ASCII (no escaping).
    // createdAt is arbitrary (100): only updatedAt matters for ordering.
    void writeSession(SessionId id, long updatedAt, string userContent) {
        write(buildPath(tmpDir, "chat", id.get ~ ".json"), format(
                `{"title": "T", "createdAt": 100, "updatedAt": %d,` ~ `"messages": [{"role": "user", "content": "%s"}]}`,
                updatedAt, userContent));
    }

    writeSession(a, 400, "hello-a");
    writeSession(b, 300, "hello-b");
    writeSession(c, 200, "hello-c");

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);
    auto aFile = store.load(a);
    assert(hasValue(aFile), "setup: active session file must load");
    app.activeSession = orElse(aFile, SessionFile()).meta;
    // Active UiMessenger pointed at this thread: the fallback activation's
    // sendSessionList() and the confirmation chat message land in this
    // thread's own mailbox (same pattern as the rename-none test).
    app.uiMsg = new UiMessenger(thisTid, false);
    app.uiTid = thisTid;

    app.doDeleteSession(a);

    assert(app.activeSession.id == b,
            "delete-active must switch to the most recently updated remaining session");
    assert(!exists(buildPath(tmpDir, "chat", a.get ~ ".json")),
            "the deleted session file must be gone");
    assert(exists(buildPath(tmpDir, "chat", b.get ~ ".json")),
            "the fallback session file must survive");
    assert(exists(buildPath(tmpDir, "chat", c.get ~ ".json")),
            "the other remaining session file must survive");

    // Snapshot: deleted id absent; the fallback is the most recently
    // updated remaining session, so it leads the store order and is
    // marked active.
    bool gotList = false;
    receiveTimeout(dur!"seconds"(1), (immutable UiSessionList l) {
        gotList = true;
        assert(l.items.length == 2, "snapshot must exclude the deleted id");
        assert(l.items[0].id == b && l.items[0].isActive,
            "fallback session must lead the snapshot and be active");
        assert(l.items[1].id == c && !l.items[1].isActive);
    });
    assert(gotList, "delete-active must refresh the sidebar snapshot");

    // Confirmation chat message on the fallback path (exact string locks
    // the user-facing wording per R16).
    bool gotDeleted = false;
    string lastChatMsg;
    receiveTimeout(dur!"seconds"(1), (UiChatMessage m) {
        gotDeleted = m.msg == "Session deleted: a1b2. Switched to 'T'.";
        lastChatMsg = m.msg; // keep for the diagnostic below
    });
    assert(gotDeleted, "delete-active must emit the switch confirmation, got: " ~ lastChatMsg);

    // Drain the remaining UI messages so this thread's mailbox stays
    // clean for subsequent unittests (receiveTimeout scans past unmatched
    // types, but other tests in this binary may match on them). The
    // snapshot handler uses the immutable variant: UiSessionList is sent
    // as cast(immutable), so a mutable handler would never match.
    // INVARIANT: keep this handler list in sync with everything
    // activateSession/sendSessionList/setStatusText can emit (a new UI
    // message type added there would silently stay in the mailbox here).
    foreach (_; 0 .. 20) {
        bool drained = receiveTimeout(dur!"msecs"(50), (UiClearChat _) {}, (UiPipelineClear _) {
        }, (UiChatThinkMessage _) {}, (UiInitHistory _) {}, (UiStatusText _) {}, (UiChatMessage _) {
        }, (immutable UiSessionList _) {});
        if (!drained)
            break;
    }
}

// --- Test: corrupt session files are skipped by listing and the sweep,
// and switchToSession keeps the current session on load failure (R16) ---

unittest {
    import my.filter : ReFilter;
    import std.file : exists, mkdirRecurse, rmdirRecurse, write;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_corrupt_files");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);
    auto good = store.create();
    auto emptyOther = store.create();

    // Corrupt file with a D12-valid id: present on disk, must never be
    // listed, loaded, or swept (C7/W16).
    auto corrupt = SessionId("20260618-153046-0bad");
    auto corruptPath = buildPath(tmpDir, "chat", corrupt.get ~ ".json");
    write(corruptPath, "{ not json !!!");

    // Listing skips the corrupt file.
    auto listed = store.list();
    assert(listed.map!(s => s.id).canFind(good.id), "setup: good session listed");
    assert(!listed.map!(s => s.id).canFind(corrupt),
            "corrupt file must be absent from the store listing");

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);
    auto goodFile = store.load(good.id);
    assert(hasValue(goodFile), "setup: good session file must load");
    app.activeSession = orElse(goodFile, SessionFile()).meta;
    app.uiMsg = new UiMessenger(thisTid, false);
    app.uiTid = thisTid;

    // Snapshot excludes the corrupt file. Both remaining sessions were
    // created in the same second (updatedAt tie, id-desc tiebreak), so the
    // exact order is not asserted here - only membership and the active
    // marker (order semantics are covered by the snapshot-order test).
    app.sendSessionList();
    bool gotList = false;
    receiveTimeout(dur!"seconds"(1), (immutable UiSessionList l) {
        gotList = true;
        assert(l.items.length == 2, "snapshot must exclude the corrupt file");
        auto ids = l.items.map!(i => i.id).array;
        assert(ids.canFind(good.id) && ids.canFind(emptyOther.id),
            "snapshot must list both remaining sessions");
        foreach (item; l.items) {
            if (item.id == good.id) {
                assert(item.isActive, "the active session must be marked active");
            } else {
                assert(!item.isActive, "only the active session is marked");
            }
        }
    });
    assert(gotList, "sendSessionList must emit the snapshot");

    // switchToSession on the corrupt id: error message, current session kept
    // (exact string locks the user-facing wording per R16).
    app.switchToSession(corrupt);
    bool gotError = false;
    receiveTimeout(dur!"seconds"(1), (UiChatMessage m) {
        gotError = m.msg
            == "error: Cannot load session '20260618-153046-0bad' (not found or corrupt). Staying in current session.";
    });
    assert(gotError, "switchToSession must emit the cannot-load error");
    assert(app.activeSession.id == good.id, "a failed load must keep the current session unchanged");

    // The sweep removes only list() candidates: the corrupt file is never
    // a candidate and survives untouched (W16), the empty non-active
    // session is removed, and the kept (active) id is exempted (W15).
    // Note: good is empty from creation (store.create() writes messages:
    // []); the failed switch commits nothing (the chat is clean), so the
    // keep-exemption is what protects it here; non-empty survival is
    // covered by the Task 1 store-level tests.
    auto swept = store.sweepEmptySessions(good.id);
    assert(swept.length == 1 && swept[0] == emptyOther.id,
            "sweep must remove only the empty non-active session");
    assert(!exists(buildPath(tmpDir, "chat", emptyOther.id.get ~ ".json")),
            "the empty non-active session file must be swept");
    assert(exists(corruptPath), "the sweep must leave the corrupt file untouched");
    assert(exists(buildPath(tmpDir, "chat", good.id.get ~ ".json")),
            "the kept session file must survive the sweep");
}
