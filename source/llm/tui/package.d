module llm.tui;

import logger = std.logger;
import std.algorithm : filter, among, min, max;
import std.array : appender, Appender, empty, array;
import std.logger;
import std.concurrency;

import my.path : Path;

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
        string query_;
        bool userTerminated_;
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

auto makeTui() {
    return TextUserInterface(tuiCreateState(), tuiInit());
}

struct UiShutdown {
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

struct UiUserQuery {
    string query;
}

struct UiLogFile {
    bool useFile;
}

struct UiTerminate {
}

struct UiTerminated {
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
    UiSessionItem[] items;
}

// Session sidebar actions (UI -> D), consumed by the agent receive loop.
struct UiSessionSelect {
    SessionId id;
}

struct UiSessionNew {
}

struct UiSessionRename {
    SessionId id;
    string title;
}

struct UiSessionDelete {
    SessionId id;
}

void spawnUserInterface(Tid ownerTid, long maxWidth) {
    import std.string : strip;
    import std.datetime : dur, Clock, Duration;
    import llm.utility : stopAgent, playNotification;
    import std.conv : to;

    setMaxMailboxSize(thisTid, 100, OnCrowding.block);
    register("llmfun_tui", thisTid);

    auto ui = makeTui();
    assert(maxWidth >= 0 && maxWidth <= 10_000,
            "maxWidth out of int-safe range: " ~ maxWidth.to!string);
    ui.setMaxWidth(cast(int) maxWidth);
    ui.setUiAsStdLogger;
    bool running = true;
    TuiSessionActionType pendingAction = TuiSessionAction_None;
    string pendingActionId;
    string pendingActionTitle;

    immutable UpdateInterval = 10.dur!"msecs";
    auto nextUpdate = Clock.currTime;
    do {
        try {
            // dfmt off
            receiveTimeout(UpdateInterval,
                (UiShutdown _) { running = false; },
                (UiSetIniFile a) { ui.setIniFile(a.path); },
                (UiChatMessage a) { ui.addChatMessage(a.msg, null, a.type); },
                (UiChatThinkMessage a) { ui.addChatMessage(a.msg, a.thinking, a.type); },
                (UiFinalAnswer a) { ui.addChatMessage(a.msg, null, TuiChatMessageType_FinalAnswer); },
                (UiClearChat _) { ui.clearChat; },
                (UiStatusText a) { ui.setStatusText(a.status); },
                (UiLogFile a) { ui.useUiLogFile(a.useFile); },
                (UiTerminate _) { running = false; },
                (UiAgentBusy _) { ui.setReadyStatus(false); },
                (UiAgentReady _) { ui.setReadyStatus(true); playNotification(); },
                (UiStreamChatMessage a) { ui.streamChat(a.msg, a.thinking); },
                (UiStreamChatDone _) { ui.streamChatDone; },
                (UiInitHistory a) { ui.setHistory(a.queries); },
                (UiPipelineStreamChatMessage a) { ui.pipelineMessage(a); },
                (UiPipelineStreamDone a) { ui.pipelineMessage(a); },
                (UiPipelineClear _) { ui.pipelineClear; },
                (immutable UiSessionList a) { ui.setSessionList(a.items); }
            );
            // dfmt on

            auto query = ui.userQuery;

            if (!query.strip.empty) {
                if (query == "/stop") {
                    stopAgent();
                    ui.setStatusText("Stopping agent");
                } else {
                    send(ownerTid, UiUserQuery(query));
                }
            }

            if (pendingAction != TuiSessionAction_None) {
                switch (pendingAction) {
                case TuiSessionAction_Select:
                    send(ownerTid, UiSessionSelect(SessionId(pendingActionId)));
                    break;
                case TuiSessionAction_New:
                    send(ownerTid, UiSessionNew.init);
                    break;
                case TuiSessionAction_Rename:
                    send(ownerTid, UiSessionRename(SessionId(pendingActionId),
                            pendingActionTitle));
                    break;
                case TuiSessionAction_Delete:
                    send(ownerTid, UiSessionDelete(SessionId(pendingActionId)));
                    break;
                default:
                    logger.warningf("Unknown session action type %d", pendingAction);
                    break;
                }
                pendingAction = TuiSessionAction_None;
                pendingActionId = null;
                pendingActionTitle = null;
            }

            if (Clock.currTime > nextUpdate) {
                ui.render();
                nextUpdate = Clock.currTime + UpdateInterval;
                // Poll one session action per frame into the stash; never
                // overwrite an action that is still awaiting forward.
                if (pendingAction == TuiSessionAction_None) {
                    ui.pollSessionAction(pendingAction, pendingActionId, pendingActionTitle);
                }
            }
        } catch (Exception e) {
            logger.trace(e);
            logger.warning(e.msg);
        }
    }
    while (running);
    send(ownerTid, UiTerminated.init);
}
