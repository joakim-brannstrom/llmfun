/// Handles the 'agent' subcommand: interactive chat loop with TUI integration.
module llm.app_agent;

import logger = std.logger;
import std.algorithm;
import std.array : empty, array, appender;
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
import llm.rag.dialogue_worker : CompletionGate, DiDrained, ReasoningDrainBudget;
import llm.rag.reasoning_index : ReasoningIndex, loadReasoningPrompt;
import llm.rag.rag : RAG;
import llm.session : SessionId, SessionMeta, SessionFile, SessionStore, isValidId;
import llm.skill;
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

    /// Phase 1 of the two-phase exit: enqueue the dialogue
    /// worker's drain and return true. Returns false when there is no
    /// dialogue index — nothing was enqueued and the caller must NOT wait
    /// for a DiDrained that will never come.
    package bool beginDispose(WeakAddress replyTo, CompletionGate gate) {
        if (dialogueIndex is null)
            return false;
        dialogueIndex.beginDispose(replyTo, gate);
        return true;
    }

    /// Phase 2 of the two-phase exit: the dispose body minus the drain.
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
            // never touches state.json (saved below, unchanged order).
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
            uiMsg.initHistory(agent_.getUserQueries.map!(a => a.content).array.idup);
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
            return;
        auto items = mapSessionItems(sessionStore.list(), activeSession.id);
        uiMsg.sessionList(items);
    }

    /** Sidebar select handler (TUIListener.sessionSelect): clear pending
     * delete, validate the untrusted UI id, then switch. Store failures
     * degrade to a chat message - the actor keeps processing.
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

    /** Sidebar new handler (TUIListener.sessionNew): clear pending delete,
     * then create + switch. Store failures degrade to a chat message.
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

    /** Sidebar delete handler (TUIListener.sessionDelete): clear pending delete,
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

class AppAgentActor {
    import my.actor;
    import llm.tui;

    private {
        ActorRef self_;
        AgentApp app;
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

    void start() {
        try {
            startSetup();
        } catch (Exception e) {
            // onException (mylib hook) only fires for Exception. A
            // core.exception.Error escaping a handler violates
            // ActorShell.process()'s nothrow contract: the scheduler
            // worker dies inside the pool (TaskPool.doJob swallows the
            // Throwable into task.exception, thread returns to idle) and
            // this actor is orphaned — no AgentDone, supervisor hangs
            // forever. Catching every Throwable here turns any start
            // failure into a logged, clean exit.
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
        auto reasoningPrompt = loadReasoningPrompt(app.llmConf);
        app.rag = createRag(app.llmConf);
        if (app.rag is null) {
            startExit(1, ExitReason.userShutdown);
            return;
        }
        app.skillManager_ = makeSkillManager(app.llmConf);
        auto agentMdState = processAgentMd(app.llmConf, uconf.noCwdConfig, app.rag);
        if (agentMdState.isValid())
            logger.tracef("AGENTS.md processed, summary length: %s", agentMdState.summary.length);
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

        app.setupSession(agentMdState);
        app.oneShotQuery = !app.conf_.prompt.empty;
        if (app.oneShotQuery) {
            app.runAgent(app.conf_.prompt);
            // explicit, before AgentDone (startExit -> finishExit)
            startExit(0, ExitReason.userShutdown);
            return;
        }

        app.updateRagMemory();

        // Bounded-mailbox canary: the legacy
        // std.concurrent TUI mailbox was bounded at 100; the actor path
        // uses 1000 — deliberately higher, because 100 proved too
        // restrictive. When the TUI mailbox is full, sends from actor
        // workers are dropped — never blocked — and counted in
        // `tui_.addr.get.dropped` (blocking a pool worker is the deadlock
        // the bound prevents); only non-actor threads, like this
        // supervisor, may block on a full mailbox. Residual, accepted:
        // stream messages may be dropped during a real stall — the TUI
        // renders frames anyway, so a dropped frame message is safe;
        // control messages are low-rate and the mailbox drains at frame
        // rate, so their drop probability is negligible.
        // Config.detached: the TUI actor must own one FIXED thread. Its
        // GLFW window/GL context are thread-affine - created in onSpawn and
        // driven by every frame. On the shared pool the frames hop across
        // worker threads where the context is not current; the GL calls
        // become no-ops and ImGui device-object creation fails at runtime
        // ("failed to compile vertex shader"). The dedicated thread keeps
        // onSpawn and all frames on the same thread (design: single UI
        // thread drives the C API).
        tui_ = sys.spawnBounded!(Config.detached, TextUserInterfaceActor)(1000,
                TypedAddress!TUIListener(self_.address().lock()),
                app.llmConf.tui.maxWidth, resolveTuiBackendMode(app.conf_));
        app.uiMsg = new UiMessenger(new TuiChannelSink(tui_));
        monitor(self_.address(), tui_);
        app.uiMsg.setIniFile(app.llmConf.dataDir ~ "imgui.ini"); // ONCE
        app.uiMsg.initHistory(app.agent_.getUserQueries.map!(a => a.content).array.idup);
        app.agent_.setStreamUpdate(app.makeStreamCallback);

        foreach (m; app.agent_.chat.getMessages())
            app.processChatMessage(m, printUser: true);

        app.sendSessionList();
        auto helpText = app.printHelp(app.conf_);
        if (helpText !is null)
            app.sendChatMessage(helpText, TuiChatMessageType_User);
        if (app.llmConf.beginConsolidation) {
            logger.infof("Memory consolidation pending at session #%s",
                    app.llmConf.sessionCount + 1);
            runMemoryConsolidation(app.llmConf, app.rag, app.monitor,
                    (string msg, TuiChatMessageType t) => app.sendChatMessage(msg, t));
        }
        app.setStatusText(true);
    }

    void userQuery(string s) {
        auto query = s.strip;
        if (!query.empty) {
            app.sendChatMessage(query, TuiChatMessageType_User);
            app.uiMsg.busy();
            clearStopAgent();
            app.setStatusText(false);
            final switch (app.runAgent(query)) {
            case AgentStatus.active:
                break;
            case AgentStatus.terminate:
                app.uiMsg.terminate();
                break;
            }
            app.uiMsg.ready();
            app.sendSessionList();
            // uiAgentReady only clears the busy flag (Thinking indicator,
            // session-panel gating); without this rewrite the status bar
            // keeps the last "Busy" text (legacy loop-top setStatusText(true)
            // parity).
            app.setStatusText(true);
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

    // The dialogue worker sends this directly to its owner (this actor —
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

    // Single exit funnel. Phase 1 (here): enqueue the dialogue worker's
    // drain and arm the bounded deadline. Phase 2 (diDrained /
    // drainTimeout): finishExit. Idempotent: every exit path funnels into
    // this, and a second call is a no-op. An exit without a dialogue
    // index finishes immediately — there is no drain to wait for.
    // Unannotated (inferred @system): the no-drain path calls finishExit,
    // which must catch Throwable below.
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

    // Phase-2 trigger: the bounded deadline fires before the drain reply.
    // Not exercised in the unittest suite: arming it needs a drain that
    // never answers, and the worker only stops answering past its own 330 s
    // drain deadline (this actor safety timer: 340 s) -- both too long to
    // wait in-suite. The one-line body converges on the same finishExit as
    // the DiDrained reply, which the exit integration test in
    // llm.app_agent.tests pins on the real worker; this deadline stays a
    // documented, code-reviewed hole.
    void drainTimeout() {
        diDrained(DiDrained());
    }

    // Phase 2: cleanup, then die. finishDispose does file I/O
    // (commitActiveSession/saveState) and CAN throw; a second throw here
    // would escape the hook as a nothrow violation and silently kill the
    // scheduler worker (TaskPool.doJob swallows the Throwable) — the
    // exact hang class the start() guard protects against. sendExit runs
    // ONLY here, after the cleanup.
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
    // destroys it LAST — after sys.shutdown + pool.finish — which keeps
    // its mailbox alive for the agent's final AgentDone.
    auto sup = scopedActor;

    // Explicit 3-worker pool — the sys actors (AppAgentActor,
    // TextUserInterfaceActor, and the dialogue worker spawned by the
    // DialogueIndex actor ctor) each occupy one pool worker for their
    // full duration; the agents and workers ARE actors, the pool serves
    // those workloads. Never default pool.
    // Pool-size policy (a convention — the library does not check it —
    // documented in the mylib actor README, "Bounded mailbox"):
    // pool >= actor count + 1 — the three actors take the three pool
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
