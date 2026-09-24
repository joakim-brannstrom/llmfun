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
import llm.config : RagConfig;
import llm.config;
import llm.memory;
import llm.metric.monitor : MetricMonitor;
import llm.query;
import llm.rag.dialogue_index : DialogueIndex;
import llm.rag.dialogue_worker : DiDegraded;
import llm.rag.reasoning_index : ReasoningIndex, loadReasoningPrompt;
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
        DialogueIndex dialogueIndex;
        ReasoningIndex reasoningIndex;
        MetricMonitor monitor;
        Agent agent_;
        SessionStore sessionStore;
        SessionMeta activeSession;
        SessionId pendingDeleteId; // session id awaiting /delete confirmation

        // True while the in-memory chat differs from the last persisted state. commitActiveSession() only writes (and bumps updatedAt) when this is set, so mere navigation - switching or re-clicking a row - never rewrites a file and never changes the sidebar sort order. Sorting is by the latest message in a session. Invariant: every code path that appends persisted content to the chat must set `chatDirty`; the commit is otherwise no-op-gated to protect sidebar `updatedAt` ordering.
        bool chatDirty;

        // The startup system prompt, cached exactly where startup computes it (setupSession's setSystemPrompt call site). activateSession re-applies it on every session switch: Chat.clear keeps history[0] and loaded docs carry no system entries, so without the re-apply the previous chat's prompt would survive the switch. Written only at the startup call site; empty until it runs - an early activation then re-applies nothing (a harmless no-op; the startup call still sets the real prompt).
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
    }

    private bool oneShotQuery;
    package Tid uiTid;

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

    package void dispose() {
        // CRITICAL ORDER: drain the dialogue worker BEFORE rag.destroy
        // so no embedding runs against weights being destroyed. The shared
        // worker also drains queued RiJobs and bounds-joins in-flight
        // summarization threads; ReasoningIndex itself has nothing to
        // dispose.
        if (dialogueIndex) {
            dialogueIndex.dispose();
            dialogueIndex = null;
        }
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
            // Empty-session cleanup on clean exit, after the final commit.
            // The store guard is REQUIRED: dispose() runs on the scope(exit)
            // path and a failed setupSession leaves the store null while
            // agent_ is already set. The active session is exempted even
            // when empty. Single-writer: the sweep runs on the agent thread
            // only; it never touches state.json (saved below, unchanged order).
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
        // Stamp the owning session into any checkpoint event this compression
        // fires. Pool callbacks set their own or leave "" — the dialogue
        // indexer refuses to index events with an empty sessionId.
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
            const bool show = !a.role.among(Role.user, Role.system)
                || (a.role == Role.user && printUser && a.isUserQuery);
            if (show) {
                auto msgType = a.role == Role.user ? TuiChatMessageType_User
                    : TuiChatMessageType_Assistant;
                this.sendChatThinkMessage("%s: %s", a.thinking, msgType, a.role, a.content);
            } else {
                logger.tracef("%s: %s", a.role, a.content);
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
            // the user never directly use an API function which produce a
            // vision message. It is the LLM that make the call.
            this.sendChatMessage("user: %s (with image)", TuiChatMessageType_Assistant, a.content);
        });
    }

    package void processResult(ProcessResult result) {
        logger.trace(result.status != ProcessResult.Status.ok, result);
        lastServerStat = result.stat;
        foreach (m; result.chat) {
            this.processChatMessage(m, printUser: false);
        }
        // Append-site audit: every Agent-layer append is reported through result.chat - process() sets rval.chat = chat.lastResponses (agent) - so a non-empty turn means persisted content was appended: assistant text (parseResponse), tool traffic + feedback warnings + the taskDone ToolMessage (handleToolCalls), vision, and compression continuation (Agent.compress, appended after the previous turn's resetResponseIndex, before the next lastResponses). Not persisted content, no flag: addUserQuery (submit sets the flag itself), Chat.setSystemPrompt (history[0], stripped at commit; on the first turn of a fresh chat it can appear in the slice, benign - prevIndex defaults to 0, a nothing-appending turn returns null, and any real append sets the flag anyway), Chat.load (replay; activateSession resets the flag first), beginNewTurn (allocates a turn id, appends nothing).
        //
        // Set the flag HERE, not in processChatMessage - that is shared with the replay loop in activateSession and must keep the freshly loaded chat clean (a dirty replay would rewrite the just-loaded file).
        if (result.chat.length > 0)
            chatDirty = true;
        commitActiveSession();
    }

    /// Save the current agent chat to the active session file.
    /// Strips system messages before persisting.
    ///
    /// Gated on `chatDirty`: a clean chat (nothing changed since the last save) skips the write entirely, so switching sessions, re-clicking the active row, or a clean shutdown can never bump `updatedAt` and reorder the sidebar. The flag is set by real content changes (a user query, `/clear`, a compression that rewrote history, or an agent turn appending persisted content; the per-turn setter is in `processResult`) and cleared here on success; a failed save keeps it so the next commit retries.
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
     * Loads the session FIRST — on failure: send error and abort, keeping the current in-memory chat and active id. On success: clear history, re-apply the cached startup system prompt, load doc, reset response index, sync context, clear UI, replay messages, resend UiInitHistory, update status and state.
     */
    private void activateSession(SessionId id) {
        // Commit current session was already done by caller (switchToSession)
        // or this is a fresh activation (startup/delete-active fallback).

        auto sfOpt = sessionStore.load(id);
        if (!hasValue(sfOpt)) {
            this.sendChatMessage("error: Cannot load session '%s' (not found or corrupt). Staying in current session.",
                    TuiChatMessageType_Assistant, id);
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

        // Same day: show time only
        if (dt.year == now.year && dt.month == now.month && dt.day == now.day) {
            return format("%02d:%02d", dt.hour, dt.minute);
        }
        // Same year: show month abbreviation and day
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
     * Exactly one save of the current session.
     * The confirmation message appears in the new session's chat view.
     */
    package void doCreateSession() {
        auto newMeta = sessionStore.create();
        switchToSession(newMeta.id);
        // Confirmation message sent in the new session's context
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
            // at the deleted id — a later commit would resurrect the file.
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
            this.sendChatMessage("Session deleted: %s. Switched to '%s'%s.", TuiChatMessageType_Assistant,
                    shortSessionId(id), activeSession.title, createdFresh ? " (new session)" : "");
        } else {
            this.sendChatMessage("Session deleted: %s",
                    TuiChatMessageType_Assistant, shortSessionId(id));
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
    package void sendSessionList() {
        if (!uiMsg.isActive())
            return; // one-shot mode: no UI thread to send to
        auto items = mapSessionItems(sessionStore.list(), activeSession.id);
        send(uiTid, cast(immutable) UiSessionList(items));
    }

    /** Sidebar select handler (UiSessionSelect): clear pending delete,
     * validate the untrusted UI id, then switch. Store failures degrade
     * to a chat message - the receive loop keeps running.
     */
    package void doSidebarSelect(SessionId id) {
        pendingDeleteId = SessionId.init;
        try {
            if (!isValidId(id)) {
                this.sendChatMessage("error: Invalid session id '%s'. Switch rejected.",
                        TuiChatMessageType_Assistant, id);
                return;
            }
            switchToSession(id); // sends the refreshed list
        } catch (Exception e) {
            this.sendChatMessage("error: Failed to switch session: %s.",
                    TuiChatMessageType_Assistant, e.msg);
        }
    }

    /** Sidebar new handler (UiSessionNew): clear pending delete, then
     * create + switch. Store failures degrade to a chat message.
     */
    package void doSidebarNew() {
        pendingDeleteId = SessionId.init;
        try {
            doCreateSession(); // sends the refreshed list
        } catch (Exception e) {
            this.sendChatMessage("error: Failed to create session: %s.",
                    TuiChatMessageType_Assistant, e.msg);
        }
    }

    /** Sidebar rename handler (UiSessionRename): clear pending delete,
     * validate the id, reject empty titles only (no length cap, mirrors
     * /rename), rename the CARRIED id, refresh the active meta on
     * success, and always send the refreshed list - the rename goes
     * straight to the store, so this handler owns the send on both paths.
     */
    package void doSidebarRename(SessionId id, string title) {
        pendingDeleteId = SessionId.init;
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
                // Unknown/corrupt id - error message, active meta
                // unchanged, list still refreshed below.
                this.sendChatMessage("error: Failed to rename session '%s'.",
                        TuiChatMessageType_Assistant, shortSessionId(id));
            }
            // The rename goes straight to the store (no sending callee),
            // so this handler sends the refreshed list on both paths.
            sendSessionList();
        } catch (Exception e) {
            this.sendChatMessage("error: Failed to rename session: %s.",
                    TuiChatMessageType_Assistant, e.msg);
        }
    }

    /** Sidebar delete handler (UiSessionDelete): clear pending delete,
     * validate the id, then delete. The C++ panel already ran the
     * two-step confirmation, so D delegates to doDeleteSession (active
     * fallback incl.).
     */
    package void doSidebarDelete(SessionId id) {
        pendingDeleteId = SessionId.init;
        try {
            if (!isValidId(id)) {
                this.sendChatMessage("error: Invalid session id '%s'. Delete rejected.",
                        TuiChatMessageType_Assistant, id);
                return;
            }
            doDeleteSession(id); // sends the refreshed list
        } catch (Exception e) {
            this.sendChatMessage("error: Failed to delete session: %s.",
                    TuiChatMessageType_Assistant, e.msg);
        }
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
            // state. Clear before dispatch — a stale confirmation would
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
            auto result = agent_.runToCompletion(&this.processResult,
                    compressCallback: &this.progressCallback, interrupt: () {
                return isStopAgentTriggered;
            });
            if (result.status == ProcessResult.Status.agentStuckInLoop) {
                this.sendChatMessage("harness: Agent forcefully terminated because it got stuck in a loop.\nYou can restart it with /c.\nIf it gets stuck again then the model is stuck in an internal prediction loop. Clear the context (/clear) and run your query again.",
                        TuiChatMessageType_System);
            }
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

        // do not slow down startup if the user only has an in-memory RAG:
        // then the memory files are indexed every time the app starts
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
        agent_.chat.resetResponseIndex; // prevent replay of old history
        agent_.syncContextFromChat(); // set prevStat.context from the loaded chat

        agent_.setSystemPrompt(systemPrompt_ = llmConf.getPrompt(skillManager: skillManager_, promptName: llmConf
                .agentPrompt, addSkills: true, agentMdSummary: agentMdState.summary));

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

        // Load BEFORE the DialogueIndex ctor: the ctor spawns the worker,
        // which needs the prompt as a spawn argument. Missing file falls
        // back to the built-in default.
        auto reasoningPrompt = loadReasoningPrompt(llmConf);

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

        // Create the DialogueIndex (spawns the worker actor on its own thread).
        auto dialogueRagCfg = RagConfig(windowOverlapPercent: 10, nBatch: 1,
                maxChunksPerTopic: 512);
        dialogueIndex = new DialogueIndex(llmConf.dialogueDir.AbsolutePath, llmConf.embedConfig,
                dialogueRagCfg, null, llmConf.summaryModel, reasoningPrompt, null);
        agent_.addCompressionCheckpointListener(&dialogueIndex.onCheckpoint);
        agent_.toolContext().setDialogueIndex(dialogueIndex);

        // Second checkpoint listener on the shared worker (dialogue
        // listener stays first, so DiJob keeps mailbox priority).
        reasoningIndex = new ReasoningIndex(llmConf.dialogueDir.AbsolutePath,
                llmConf.summaryModel, dialogueIndex.workerTid);
        agent_.addCompressionCheckpointListener(&reasoningIndex.onCheckpoint);
        agent_.toolContext().setReasoningIndex(reasoningIndex);

        // Register BEFORE setupSession(): a throw in setupSession (e.g. an
        // unusable session dir) must still run dispose(), which then finds
        // sessionStore null while agent_ is set - the store guard inside
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

        // only update memory for non-oneshot: oneshot mode is assumed to need max speed/low latency
        updateRagMemory();

        uiTid = spawn(&spawnUserInterface, thisTid, llmConf.tui.maxWidth);
        uiMsg = new UiMessenger(uiTid, false);
        uiMsg.setIniFile(llmConf.dataDir ~ "imgui.ini");
        send(uiTid, UiInitHistory(agent_.getUserQueries.map!(a => a.content).array.idup));
        send(uiTid, UiSetIniFile(llmConf.dataDir ~ "imgui.ini"));
        agent_.setStreamUpdate(makeStreamCallback);

        foreach (m; agent_.chat.getMessages()) {
            this.processChatMessage(m, printUser: true);
        }

        // Initial sidebar snapshot right after the message replay;
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
            }, (UiTerminated _) { running = false; }, (DiDegraded d) {
                // The dialogue worker sends this exactly once (its embedder
                // is unavailable for the process lifetime). The worker already
                // logged the cause; this owner-side line records the
                // degradation state for the agent.
                logger.warningf("dialogue index worker degraded: %s (dialogue indexing disabled for process lifetime)",
                    d.reason);
            });
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
