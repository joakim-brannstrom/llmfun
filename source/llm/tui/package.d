module llm.tui;

import logger = std.logger;
import std.algorithm : filter, among;
import std.array : appender, Appender, empty, array;
import std.logger;
import std.concurrency;

import my.path : Path;

import llmfun_tui;

String toTuiString(string s) {
    return String(s.ptr, s.length);
}

string toString(String s) {
    if (s.len == 0)
        return null;
    auto r = s.data[0 .. s.len].idup;
    while (!r.empty && r[$ - 1].among('\0', '\n')) {
        r = r[0 .. $ - 1];
    }
    return r;
}

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

    void setUiAsStdLogger() {
        logSwap = swapToTuiLogger();
    }

    void useUiLogFile(bool useFile) {
        tuiSetLogging(tuiState, useFile);
    }

    string userQuery() @safe {
        auto tmp = query_;
        query_ = null;
        return tmp;
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

    void setReadyStatus(bool x) {
        tuiReadyStatus(tuiState, x ? 1 : 0);
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

struct UiChatMessage {
    string msg;
    TuiChatMessageType type = TuiChatMessageType_Assistant;
}

struct UiChatThinkMessage {
    string msg;
    string thinking;
    TuiChatMessageType type = TuiChatMessageType_Assistant;
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

void spawnUserInterface(Tid ownerTid) {
    import std.string : strip;
    import std.datetime : dur;
    import llm.utility : stopAgent, playNotification;

    auto ui = makeTui();
    ui.setUiAsStdLogger;
    bool running = true;
    do {
        try {
            receiveTimeout(10.dur!"msecs", (UiShutdown _) { running = false; }, (UiSetIniFile a) {
                ui.setIniFile(a.path);
            }, (UiChatMessage a) { ui.addChatMessage(a.msg, null, a.type); }, (UiChatThinkMessage a) {
                ui.addChatMessage(a.msg, a.thinking, a.type);
            }, (UiClearChat _) { ui.clearChat; }, (UiStatusText a) {
                ui.setStatusText(a.status);
            }, (UiLogFile a) { ui.useUiLogFile(a.useFile); }, (UiTerminate _) {
                running = false;
            }, (UiAgentBusy _) { ui.setReadyStatus(false); }, (UiAgentReady _) {
                ui.setReadyStatus(true);
                playNotification();
            });

            ui.render();
            auto query = ui.userQuery;
            if (!query.strip.empty) {
                if (query == "/stop") {
                    stopAgent();
                    ui.setStatusText("Stopping agent");
                } else {
                    send(ownerTid, UiUserQuery(query));
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
