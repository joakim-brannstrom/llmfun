module llm.tui;

import logger = std.logger;
import std.algorithm : filter, among, min, max;
import std.array : appender, Appender, empty, array;
import std.logger;

import my.path : Path;
import my.actor;

import llm.session : SessionId;

import llmfun_tui;

// Convert a D string to an inbound API `String` (no allocation)
String toTuiString(string s) {
    return String(s.ptr, s.length);
}

// Converts an outbound API `String` to a D string (allocates via `idup`, strips trailing null/newline).
string toString(String s) {
    if (s.len == 0)
        return null;
    auto r = s.data[0 .. s.len].idup;
    while (!r.empty && r[$ - 1].among('\0', '\n')) {
        r = r[0 .. $ - 1];
    }
    return r;
}

// Extracts a short summary from a message (strips `#` headers, takes first line up to 100 chars).
string shortSummary(string msg) nothrow {
    import std.algorithm : until;
    import std.ascii : isASCII, isAlphaNum, isWhite;
    import std.exception : collectException;
    import std.range : take;
    import std.string : strip;
    import std.uni : byCodePoint, byGrapheme, Grapheme;
    import std.utf : toUTF8, UTFException;

    try {
        immutable hash = Grapheme('#');
        immutable newline = Grapheme('\n');

        return msg.byGrapheme
            .filter!(a => a != hash)
            .until!(a => a == newline)
            .take(100).byCodePoint.toUTF8.strip;
    } catch (UTFException e) {
    } catch (Exception e) {
        logger.tracef("this should not happen: %s", e.msg).collectException;
    }

    return cast(string)(cast(const(ubyte[])) msg).filter!(a => a.isASCII)
        .filter!(a => (a.isAlphaNum || a.isWhite))
        .until!(a => a == '\n')
        .take(100).array;
}

// A D `Logger` implementation that captures log entries and drains them for display in the TUI log tab.
class TuiLogger : Logger {
    import core.sync.mutex;
    import std.format : format;

    private {
        Appender!(string[]) entries;
        Mutex mtx;
        immutable MaxEntries = 1000;
    }

    this(const LogLevel lvl = LogLevel.warning) @safe {
        super(lvl);
        this.mtx = new Mutex;
    }

    override void writeLogMsg(ref LogEntry payload) @trusted {
        import std.datetime : Clock;

        mtx.lock_nothrow();
        scope (exit)
            mtx.unlock_nothrow();
        if (entries[].length < MaxEntries) {
            entries.put(format("%s - %s: %s [%s:%d]", Clock.currTime,
                    payload.logLevel, payload.msg, payload.funcName, payload.line));
        }
    }

    string[] drainEntries() @safe {
        mtx.lock_nothrow();
        scope (exit)
            mtx.unlock_nothrow();
        auto tmp = entries[];
        entries.clear();
        return tmp;
    }
}

struct TuiLogSwap {
    private {
        bool isSwapped = false;
        shared(Logger) prev;
        shared(TuiLogger) tui;
    }

    ~this() {
        if (isSwapped) {
            sharedLog = prev;
        }
        isSwapped = false;
    }

    string[] drainEntries() @trusted {
        return (cast() tui).drainEntries();
    }
}

TuiLogSwap swapToTuiLogger() @trusted {
    auto prev = sharedLog;
    auto n = cast(shared) new TuiLogger(LogLevel.all);
    sharedLog = n;
    return TuiLogSwap(true, prev, n);
}

void tuiLogToTui(ref TuiLogSwap log, TuiState* tuiState) {
    if (!log.isSwapped)
        return;

    foreach (msg; log.drainEntries) {
        string summary = shortSummary(msg);
        auto s = String(summary.ptr, summary.length);
        auto q = String(msg.ptr, msg.length);
        tuiAddLogMessage(tuiState, s, q);
    }
}

struct TextUserInterface {
    private {
        TuiState* tuiState;
        TuiScreen* tuiScreen;
        TuiLogSwap logSwap;
        bool userTerminated_;
    }

    // Package-visible state: read/write seam for the llm.tui.tests driver
    // (the test module lives in package llm.tui, so package members are
    // reachable; plan task 9).
    package {
        string query_;
        string statusText;
    }

    this(TuiState* state, TuiScreen* screen) {
        this.tuiState = state;
        tuiSetLogging(tuiState, false);
        this.tuiScreen = screen;
    }

    ~this() {
        tuiDestroyState(tuiState);
        tuiShutdown(tuiScreen);
    }

    bool hasMoreEvents() {
        return tuiHasMoreEvents() == 1;
    }

    void addChatMessage(string msg, string thinking, TuiChatMessageType type) {
        string summary = shortSummary(msg);
        auto s = String(summary.ptr, summary.length);
        auto q = String(msg.ptr, msg.length);
        auto t = String(thinking.ptr, thinking.length);
        tuiAddChatMessage(tuiState, ChatMessageParam(s, q, t, type));
    }

    void clearChat() {
        tuiClearChatMessages(tuiState);
    }

    void setIniFile(Path path) {
        import std.file : exists;

        auto s = () {
            if (path.dirName.exists) {
                return toTuiString(path);
            }
            return toTuiString(null);
        }();
        tuiSetIniFilename(tuiState, s);
    }

    void setStatusText(string s) {
        statusText = s;
    }

    void setMaxWidth(int w) {
        tuiSetMaxWidth(tuiState, w);
    }

    void setReadyStatus(bool x) {
        tuiReadyStatus(tuiState, x ? 1 : 0);
    }

    void streamChat(string msg, string thinking) {
        auto s = String(null, 0);
        auto q = String(msg.ptr, msg.length);
        auto t = String(thinking.ptr, thinking.length);
        tuiUpdateStreamChatMessage(tuiState, ChatMessageParam(s, q, t,
                TuiChatMessageType_Assistant));
    }

    void streamChatDone() {
        tuiStreamChatMessageClear(tuiState);
    }

    void pipelineMessage(UiPipelineStreamChatMessage msg) {
        auto id = msg.agentId.toTuiString;
        auto content = msg.content.toTuiString;
        auto reasoning = msg.thinking.toTuiString;
        auto role = msg.role.toTuiString;
        auto status = msg.status.toTuiString;
        String finish;
        tuiPipelineAgentUpdate(tuiState, id, PipelineChatMessage(content: content,
                reasoning: reasoning, role: role, finishReason: finish, status: status));
    }

    void pipelineMessage(UiPipelineStreamDone msg) {
        auto id = msg.agentId.toTuiString;
        tuiPipelineAgentDone(tuiState, id);
    }

    void pipelineClear() {
        tuiPipelineClear(tuiState);
    }

    void useUiLogFile(bool useFile) {
        tuiSetLogging(tuiState, useFile);
    }

    void setUiAsStdLogger() {
        logSwap = swapToTuiLogger();
    }

    string userQuery() @safe {
        auto tmp = query_;
        query_ = null;
        return tmp;
    }

    void setHistory(immutable(string)[] history) {
        if (history.empty)
            return;
        auto app = appender!(String[])();
        foreach (a; history) {
            app.put(String(a.ptr, a.length));
        }
        tuiInitQueryHistory(tuiState, app[].ptr, app[].length);
    }

    // Replaces the sidebar session snapshot.
    void setSessionList(const(UiSessionItem)[] items) {
        if (items.length == 0) {
            tuiSetSessionList(tuiState, null, 0);
            return;
        }
        auto app = appender!(SessionItem[])();
        foreach (ref const item; items) {
            app.put(SessionItem(toTuiString(item.id.get), toTuiString(item.title),
                    toTuiString(item.preview), item.messageCount, item.isActive ? 1 : 0));
        }
        tuiSetSessionList(tuiState, app[].ptr, app[].length);
    }

    // Pops at most one sidebar action from the C++ queue.
    void pollSessionAction(out TuiSessionActionType type, out string id, out string title) {
        if (tuiIsSessionActionReady(tuiState) == 0)
            return;
        auto action = tuiGetSessionAction(tuiState);
        type = action.type;
        id = toString(action.sessionId);
        title = toString(action.title);
        String_Free(action.sessionId);
        String_Free(action.title);
    }

    bool hasUserTerminated() @safe {
        return userTerminated_;
    }

    void render() {
        import std.string : strip;

        // Headless (null screen, e.g. the test seam): no ImGui backend was
        // initialized, so the C++ core render would dereference a null
        // context. Backend-level calls are null-safe; the core render is not.
        if (tuiScreen is null) {
            return;
        }

        tuiBackendNewFrame();

        if (tuiRender(tuiState) == 0) {
            userTerminated_ = true;
        }

        auto status = String(statusText.ptr, statusText.length);
        tuiSetStatusText(tuiState, status);

        if (tuiIsSubmitReady(tuiState) != 0) {
            String userQuery = tuiGetSubmitQuery(tuiState);
            query_ = toString(userQuery);
            String_Free(userQuery);
            tuiResetSubmit(tuiState);
        }

        tuiLogToTui(logSwap, tuiState);

        tuiBackendRender(tuiScreen);
    }
}

interface TUIListener {
    void userQuery(string s);
    void sessionSelect(SessionId id);
    void sessionNew();
    void sessionRename(SessionId id, string title);
    void sessionDelete(SessionId id);
    void uiTerminated();
}

interface TUICommands {
    void uiMsg(UiSetIniFile m);
    void uiMsg(UiChatMessage m);
    void uiMsg(UiChatThinkMessage m);
    void uiMsg(UiFinalAnswer m);
    void uiMsg(UiStatusText m);
    void uiMsg(UiInitHistory m);
    void uiMsg(UiSessionList m);
    void uiMsg(UiStreamChatMessage m);
    void uiMsg(UiLogFile m);
    void uiMsg(UiPipelineStreamChatMessage m);
    void uiMsg(UiPipelineStreamDone m);
    void uiStreamChatDone();
    void uiPipelineClear();
    void uiClearChat();
    void uiAgentBusy();
    void uiAgentReady();
    void uiTerminate();
}

class TextUserInterfaceActor : TUICommands {
    import std.string : strip;
    import std.datetime : dur, Clock, Duration, SysTime;
    import llm.utility : stopAgent, playNotification;
    import std.conv : to;

    private {
        ActorRef selfRef;

        TypedAddress!TUIListener listenerAddress;
        Channel!TUIListener listener;

        immutable UpdateInterval = 10.dur!"msecs";

        // Guards uiTick(): a due tick entry may still be dispatched after
        // uiTerminate (cancelTick drops the pending one; an in-flight one
        // runs once) - skip the poll/render work rather than touch the
        // torn-down UI.
        bool running = true;
    }

    // Package-visible state: read/write seam for the llm.tui.tests driver
    // (see the TextUserInterface block above).
    package {
        // Incremented each render cycle which make it possible to observe if
        // and how often the UI render has executed.
        ulong updateCycle;

        TextUserInterface ui;

        TuiSessionActionType pendingAction = TuiSessionAction_None;

        string pendingActionId;
        string pendingActionTitle;

        SysTime nextUpdate;
    }

    this(TypedAddress!TUIListener listener, long maxWidth) {
        this(listener, maxWidth, tuiCreateState(), tuiInit());
    }

    // Headless injection seam (tests): raw C pointers, because a by-value
    // TextUserInterface parameter would double-free the state (its copy's
    // dtor runs at ctor exit, then the field's dtor runs again in onExit).
    // Ownership of state/screen transfers on successful return only; a
    // throw (assert) leaves them with the caller.
    this(TypedAddress!TUIListener listener, long maxWidth, TuiState* state, TuiScreen* screen) {
        assert(maxWidth >= 0 && maxWidth <= 10_000,
                "maxWidth out of int-safe range: " ~ maxWidth.to!string);
        this.listenerAddress = listener;
        ui = TextUserInterface(state, screen);
        ui.setMaxWidth(cast(int) maxWidth);
        ui.setUiAsStdLogger;
    }

    void onSpawn(ActorRef selfRef) {
        this.selfRef = selfRef;
        listener = typeof(listener)(listenerAddress, this.selfRef);
        selfRef.scheduleRepeating(UpdateInterval, "uiTick");
    }

    void onExit(ExitMsg _) {
        ui = TextUserInterface.init; // dtor: tuiDestroyState + tuiShutdown
    }

    void onException(Exception e) {
        logger.warning("TUI actor exception: ", e.msg);
    }

    void onError(ErrorMsg e) {
        logger.warning("TUI actor error: ", e.reason.to!string);
    }

    // Must be called by every message handler except uiTerminate. Polls the
    // pending user query and at most one session action, then renders if due.
    private void postProcess() {
        // it is only worth processing the rest of the method if render() has
        // been called because the rest of the function react on "input" that
        // is updated by render via imgui.
        if (Clock.currTime > nextUpdate) {
            ui.render();
            nextUpdate = Clock.currTime + UpdateInterval;
        } else {
            return;
        }

        ++updateCycle;
        auto query = ui.userQuery();

        if (!query.strip.empty) {
            if (query == "/stop") {
                stopAgent();
                ui.setStatusText("Stopping agent");
            } else {
                listener.userQuery(query);
            }
        }

        if (pendingAction != TuiSessionAction_None) {
            switch (pendingAction) {
            case TuiSessionAction_Select:
                listener.sessionSelect(SessionId(pendingActionId));
                break;
            case TuiSessionAction_New:
                listener.sessionNew();
                break;
            case TuiSessionAction_Rename:
                listener.sessionRename(SessionId(pendingActionId), pendingActionTitle);
                break;
            case TuiSessionAction_Delete:
                listener.sessionDelete(SessionId(pendingActionId));
                break;
            default:
                logger.warningf("Unknown session action type %s", pendingAction);
                break;
            }
            pendingAction = TuiSessionAction_None;
            pendingActionId = null;
            pendingActionTitle = null;
        }

        // Poll one session action per frame; never overwrite an
        // action still awaiting forward.
        if (pendingAction == TuiSessionAction_None) {
            ui.pollSessionAction(pendingAction, pendingActionId, pendingActionTitle);
        }

        if (ui.hasMoreEvents()) {
            dynSend(selfRef.address, "uiTick");
            nextUpdate = Clock.currTime; // force an update
        }
    }

    // Repeating self-tick target (armed in onSpawn). The shell dispatches
    // this at every interval and re-arms after each fire; the render cadence
    // is still gated by nextUpdate inside postProcess.
    void uiTick() {
        if (running)
            postProcess();
    }

    void uiMsg(UiSetIniFile a) {
        // do not call postProcess because this is only an initialization
        ui.setIniFile(a.path);
    }

    void uiMsg(UiChatMessage a) {
        ui.addChatMessage(a.msg, null, a.type);
        postProcess;
    }

    void uiMsg(UiChatThinkMessage a) {
        ui.addChatMessage(a.msg, a.thinking, a.type);
        postProcess;
    }

    void uiMsg(UiFinalAnswer a) {
        ui.addChatMessage(a.msg, null, TuiChatMessageType_FinalAnswer);
        postProcess;
    }

    void uiMsg(UiStatusText a) {
        ui.setStatusText(a.status);
        postProcess;
    }

    void uiMsg(UiInitHistory m) {
        ui.setHistory(m.queries);
        postProcess();
    }

    void uiMsg(UiSessionList m) {
        ui.setSessionList(m.items);
        postProcess();
    }

    void uiMsg(UiStreamChatMessage m) {
        ui.streamChat(m.msg, m.thinking);
        postProcess();
    }

    void uiMsg(UiLogFile a) {
        // do not call postProcess because this is only an initialization
        ui.useUiLogFile(a.useFile);
    }

    void uiMsg(UiPipelineStreamChatMessage m) {
        ui.pipelineMessage(m);
        postProcess();
    }

    void uiMsg(UiPipelineStreamDone m) {
        ui.pipelineMessage(m);
        postProcess();
    }

    void uiStreamChatDone() {
        ui.streamChatDone();
        postProcess();
    }

    void uiPipelineClear() {
        ui.pipelineClear();
        postProcess();
    }

    void uiClearChat() {
        ui.clearChat;
        postProcess;
    }

    void uiAgentBusy() {
        ui.setReadyStatus(false);
        postProcess;
    }

    void uiAgentReady() {
        ui.setReadyStatus(true);
        playNotification();
        postProcess;
    }

    // Termination handshake: final render, async-notify the agent, exit.
    // cancelTick keeps the zombie quiescent: without it the shell would
    // keep re-arming due ticks (uiTick no-ops via `running`) until the
    // system shuts the actor down at process exit.
    void uiTerminate() {
        running = false;
        selfRef.cancelTick();
        ui.render();
        listener.uiTerminated();
        sendExit(selfRef.address, ExitReason.userShutdown);
    }
}

struct UiSetIniFile {
    Path path;
}

struct UiInitHistory {
    immutable(string)[] queries;
}

struct UiChatMessage {
    string msg;
    TuiChatMessageType type = TuiChatMessageType_Assistant;
}

struct UiChatThinkMessage {
    string msg;
    string thinking;
    TuiChatMessageType type = TuiChatMessageType_Assistant;
}

struct UiStreamChatMessage {
    string msg;
    string thinking;
}

struct UiStreamChatDone {
}

struct UiFinalAnswer {
    string msg;
}

struct UiClearChat {
}

struct UiStatusText {
    string status;
}

struct UiLogFile {
    bool useFile;
}

struct UiTerminate {
}

struct UiAgentBusy {
}

struct UiAgentReady {
}

struct UiPipelineStreamChatMessage {
    string agentId;
    string content;
    string thinking;
    string role;
    string status;
}

struct UiPipelineStreamDone {
    string agentId;
}

struct UiPipelineClear {
}

// Session sidebar snapshot (D -> UI): full ordered list of sessions.
struct UiSessionItem {
    SessionId id;
    string title;
    string preview;
    size_t messageCount;
    bool isActive;
}

struct UiSessionList {
    const(UiSessionItem)[] items;
}
