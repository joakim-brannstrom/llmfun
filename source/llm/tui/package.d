module llm.tui;

import core.time : Duration;
import logger = std.logger;
import std.algorithm : filter, among, min, max;
import std.array : appender, Appender, empty, array;
import std.conv : text;
import std.logger;
import std.sumtype : match;

import my.path : Path;
import my.actor;
import my.time : prettyDuration;

import llm.session : SessionId;
import llm.chat : MessageTime, NoProcessingTime;
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

// The single display-policy site for the message time suffix.
// No suffix when no time was measured or the stored value is negative (corrupt - trace and stop).
// Otherwise: " [<prettyDuration(d)>]". Tier formatting is my.time.prettyDuration's job.
package string timeSuffix(MessageTime time) {
    return time.match!((Duration d) {
        if (d < Duration.init) {
            logger.tracef("negative processing time %s: treated as no processing time", d);
            return "";
        }
        return " [" ~ prettyDuration(d) ~ "]";
    }, (NoProcessingTime nt) => "");
}

// One captured log entry, field for field (design §6.4). D no longer formats a line: the
// C++ ring is the single bounded store, and the archive line is formatted there from these
// fields, so the record the UI sees is the record the runtime produced.
struct TuiLogRecordData {
    LogLevel level;
    long timestampMs;
    int line;
    string message;
    string moduleName;
    string funcName;
    string file;
    int seq = 0;
}

enum string kQueueDropMarker = "queue-drop";

// The severity as the UI's five-step scale (TuiLogLevel).
//
// This std.logger's `LogLevel` has no `debug` member at all (all, trace, info, warning, error,
// critical, fatal, off), so `TuiLogLevel_Debug` is never sent from D: `trace` is the developer
// level that only the Raw Log shows, and there is nothing between it and info. `critical` and
// `fatal` fold into `error` - the UI has no rendering that tells them apart, and a user-facing
// history wants "something failed", not the runtime's severity taxonomy. `all` is a filter
// value (a message is never logged at it) and `off` silences the logger; both land on the ends
// of the scale, as does anything unknown off the wire.
static assert(LogLevel.all < LogLevel.trace && LogLevel.trace < LogLevel.info
        && LogLevel.info < LogLevel.warning && LogLevel.warning < LogLevel.error
        && LogLevel.error < LogLevel.critical && LogLevel.critical < LogLevel.fatal && LogLevel.fatal < LogLevel.off,
        "std.logger's LogLevel order changed: toTuiLevel's comparisons assume it");
TuiLogLevel toTuiLevel(LogLevel level) @safe {
    if (level <= LogLevel.trace)
        return TuiLogLevel_Trace;
    if (level == LogLevel.info)
        return TuiLogLevel_Info;
    if (level == LogLevel.warning)
        return TuiLogLevel_Warn;
    if (level >= LogLevel.error && level <= LogLevel.fatal)
        return TuiLogLevel_Error;
    return TuiLogLevel_Info;
}

// The first line of a message: the user-facing one-liner the Activity row prints.
//
// Byte level on purpose: a newline is one byte in UTF-8, so slicing cannot split a codepoint,
// and a message is only ever cut where the runtime put a line break.
string firstLineOf(string msg) @safe {
    import std.string : indexOf;

    const at = msg.indexOf('\n');
    return at < 0 ? msg : msg[0 .. at];
}

// Everything after the first line, or an empty string. Only the *multi-line* tail becomes
// the record's `detail` (the Raw Log's context); a single-line message has no detail, which
// keeps the Activity projection from holding the message twice.
string restLinesOf(string msg) @safe {
    import std.string : indexOf;

    const at = msg.indexOf('\n');
    return at < 0 ? null : msg[at + 1 .. $];
}

private __gshared shared(int) s_logSeq;

/// The one stamp dispenser: hand out the next log-order number.
private int nextLogSeq() @trusted {
    import core.atomic : atomicOp;

    return atomicOp!"+="(s_logSeq, 1);
}

// A D `Logger` implementation that captures log entries and drains them for display in the
// TUI (Activity and the Raw Log), one record per entry.
class TuiLogger : Logger {
    import core.sync.mutex;

    /// The cap on the *queue between two frames* (the C++ ring is the bounded store).
    ///
    /// Generous on purpose: one frame's worth of an extremely chatty run is a few thousand
    /// records, so 20 000 is slack, not a limit anybody should reach. It exists because the
    /// queue is not the store: without a cap, a stalled frame loop (a blocking tool call, a
    /// paused GUI) would grow this appender for as long as the stall lasts. What is dropped is
    /// *counted* and reported on the next drain, so nothing is lost silently (NFR-17).
    private immutable MaxQueued = 20_000;

    private {
        Appender!(TuiLogRecordData[]) entries;
        Mutex mtx;
        /// Records this queue had to drop because nobody drained it in time. Flushed as one
        /// synthetic record on the next drain, then reset.
        size_t droppedInQueue;
    }

    this(const LogLevel lvl = LogLevel.warning) @safe {
        super(lvl);
        this.mtx = new Mutex;
    }

    override void writeLogMsg(ref LogEntry payload) @trusted {
        import std.datetime : SysTime;

        mtx.lock_nothrow();
        scope (exit)
            mtx.unlock_nothrow();
        const int seq = nextLogSeq();
        if (entries[].length >= MaxQueued) {
            ++droppedInQueue;
            return;
        }
        const SysTime stamp = payload.timestamp;
        // Unix-epoch milliseconds, because that is what every consumer reads
        const long stampMs = stamp.stdTime > 0
            ? cast(long) stamp.toUnixTime * 1_000L + stamp.fracSecs.total!"msecs" : 0;
        entries.put(TuiLogRecordData(payload.logLevel, stampMs, payload.line,
                payload.msg, payload.moduleName, payload.funcName, payload.file, seq));
    }

    /// The records captured since the last drain, as a *copy*: `entries[]` is a slice of the
    /// appender's own buffer, which the next `put()` overwrites, so returning it would hand the
    /// frame thread a view of live memory a producer thread is writing (task-31 review I1).
    ///
    /// A queue overflow is reported here as one synthetic warning - the count the review asked
    /// for, in the same record stream the Raw Log and Activity already read.
    TuiLogRecordData[] drainEntries() @safe {
        import std.conv : text;

        mtx.lock_nothrow();
        scope (exit)
            mtx.unlock_nothrow();
        TuiLogRecordData[] drained = entries[].dup;
        entries.clear();
        if (droppedInQueue > 0) {
            const int seq = nextLogSeq();
            drained ~= TuiLogRecordData(LogLevel.warning, 0, 0,
                    "log queue dropped " ~ droppedInQueue.text ~ " records (the frame loop did not drain in time)",
                    null, kQueueDropMarker, null, seq);
            droppedInQueue = 0;
        }
        return drained;
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

    TuiLogRecordData[] drainEntries() @trusted {
        return (cast() tui).drainEntries();
    }
}

TuiLogSwap swapToTuiLogger() @trusted {
    auto prev = sharedLog;
    auto n = cast(shared) new TuiLogger(LogLevel.all);
    sharedLog = n;
    return TuiLogSwap(true, prev, n);
}

// Build the C record for one captured entry.
//
// Positional on purpose: two of the struct's fields (`module`, `function`) are D keywords,
// so D source cannot name them. The layout is pinned by the static asserts below, and a
// reordering of `TuiLogRecord` fails the *build* instead of silently sending a module name
// where the UI expects a message.

TuiLogRecord makeLogRecord(TuiLogRecordData record, TuiLogAction action, string actionPayload) @trusted {
    static assert(TuiLogRecord.tupleof.length == 11,
            "TuiLogRecord grew: update makeLogRecord's positional list");
    static assert(is(typeof(TuiLogRecord.tupleof[0]) == int)
            && is(typeof(TuiLogRecord.tupleof[2]) == long) && is(typeof(TuiLogRecord.tupleof[5]) == String)
            && is(typeof(TuiLogRecord.tupleof[10]) == String),
            "TuiLogRecord's field order changed: update makeLogRecord's positional list");
    // The names, not only the types: two same-typed neighbours (`message`/`detail`,
    // `module`/`function`) could otherwise be swapped without a word of warning.
    static assert(__traits(identifier, TuiLogRecord.tupleof[4]) == "seq"
            && __traits(identifier, TuiLogRecord.tupleof[5]) == "message"
            && __traits(identifier, TuiLogRecord.tupleof[6]) == "detail"
            && __traits(identifier, TuiLogRecord.tupleof[7]) == "module"
            && __traits(identifier, TuiLogRecord.tupleof[8]) == "function"
            && __traits(identifier, TuiLogRecord.tupleof[9]) == "file",
            "TuiLogRecord's field names changed: update makeLogRecord");

    auto message = firstLineOf(record.message);
    auto detail = restLinesOf(record.message);
    return TuiLogRecord(toTuiLevel(record.level), action, record.timestampMs,
            record.line, record.seq, // seq: the log-order stamp from the capture site
            String(message.ptr, message.length),
            String(detail.ptr, detail.length), String(record.moduleName.ptr,
                record.moduleName.length), String(record.funcName.ptr, record.funcName.length),
            String(record.file.ptr, record.file.length),
            String(actionPayload.ptr, actionPayload.length));
}

void tuiLogToTui(ref TuiLogSwap log, TuiState* tuiState) {
    if (!log.isSwapped)
        return;

    foreach (record; log.drainEntries) {
        auto apiRecord = makeLogRecord(record, TuiLogAction_None, null);
        tuiAddLogRecord(tuiState, &apiRecord);
    }
}

struct RawLogLevelBorrow {
    private {
        LogLevel saved;
        bool raised;
    }

    /// One call per frame with the borrow's state
    bool sync(bool open) {
        if (open == raised)
            return false;
        if (open) {
            saved = globalLogLevel;
            if (saved > LogLevel.trace)
                globalLogLevel = LogLevel.trace;
            raised = true;
        } else {
            globalLogLevel = saved;
            raised = false;
        }
        return true;
    }

    /// Put back what was borrowed. Idempotent, so the quit path can call it whether or not the
    /// surface is open (a quit with the surface up is the case the design names explicitly).
    void restore() {
        if (!raised)
            return;
        globalLogLevel = saved;
        raised = false;
    }

    /// Is the level currently borrowed? Read by the tests and the actor's log line.
    bool isRaised() const {
        return raised;
    }
}

struct TextUserInterface {
    private {
        TuiState* tuiState;
        TuiLogSwap logSwap;
        RawLogLevelBorrow rawLogLevel;
    }

    package {
        string query_;
        // what D last sent, replayed to the C++ frame on every render tick.
        UiRuntimeStatus runtimeStatus_;
        // Last header summary computed by addChatMessage
        string lastChatSummary;
        // Set when the C frame reports user-terminated (GUI window close);
        // consumed by the actor's postProcess exit handshake.
        bool userTerminated_;
    }

    this(TuiState* state) {
        this.tuiState = state;
        tuiSetLogging(tuiState, false);
    }

    ~this() {
        // The Raw Log's borrowed level is restored on the way out as well as on close: a quit
        // with the surface open must not leave the process logging at the developer level for
        // whatever runs after the UI (D-RAWLOG's "and in the shutdown path").
        restoreRawLogLevel();
        tuiDestroyState(tuiState);
    }

    void addChatMessage(string msg, string thinking, TuiChatMessageType type,
            size_t nr, MessageTime time) {
        string summary;
        if (nr == 0) {
            summary = shortSummary(msg);
        } else {
            auto head = "[" ~ nr.text ~ "]";
            auto body = shortSummary(msg);
            auto tail = timeSuffix(time);
            if (body.empty) {
                summary = tail.empty ? head : head ~ " " ~ tail;
            } else {
                summary = head ~ " " ~ body ~ tail;
            }
        }
        lastChatSummary = summary;
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

    void setRuntimeStatus(UiRuntimeStatus s) @safe {
        runtimeStatus_ = s;
    }

    void setMaxWidth(int w) {
        tuiSetMaxWidth(tuiState, w);
    }

    void setIdlePark(int ms) {
        tuiSetIdlePark(tuiState, ms);
    }

    void setReadyStatus(bool x) {
        tuiReadyStatus(tuiState, x ? 1 : 0);
    }

    // Composer Stop (task 3): the C++ frame code records a one-slot cancel
    // request; consuming it here keeps the typed "/stop" path as the only stop
    // implementation (D-CANCEL), so partial output is preserved exactly as it
    // is for the slash command.
    bool takeCancel() @trusted {
        return tuiTakeCancel(tuiState) != 0;
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
        auto currentTool = msg.currentTool.toTuiString;
        String finish;
        // The layout is a wire contract with `cpp_tui/tui_api.h` (104 bytes); a reordering
        // fails the build instead of sending a tool name where the UI expects a count.
        static assert(PipelineChatMessage.sizeof == 104,
                "PipelineChatMessage layout changed: update tui_api.h and this mirror together");
        tuiPipelineAgentUpdate(tuiState, id, PipelineChatMessage(content: content, reasoning: reasoning, role: role,
                finishReason: finish, currentTool: currentTool, contextUsed: msg.contextUsed,
                contextMax: msg.contextMax, tokensPerSecond: msg.tokensPerSecond));
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

    // Raw Log access (`--debug-ui`, task 33). One call before the first frame is all the flag
    // needs: the C++ frame reads it when it lays out, so the overlay is up on the frame the
    // window appears - which is what the design means by "starts with the Raw Log open".
    void setRawLogOpen(bool open) {
        tuiSetRawLogOpen(tuiState, open ? 1 : 0);
    }

    void syncRawLogLevel() {
        rawLogLevel.sync(tuiIsRawLogOpen(tuiState) != 0
                && tuiRawLogFloor(tuiState) == TuiLogLevel_Trace);
    }

    void restoreRawLogLevel() {
        rawLogLevel.restore();
    }

    string userQuery() @safe {
        auto tmp = query_;
        query_ = null;
        return tmp;
    }

    void setHistory(immutable(string)[] history) {
        if (history.empty) // on purpose do NOT clear the history. By not clearing it is easy
            // to "repeat" previous questions with a cleared chat.
            return;
        auto app = appender!(String[])();
        foreach (a; history) {
            app.put(String(a.ptr, a.length));
        }
        tuiInitQueryHistory(tuiState, app[].ptr, app[].length);
    }

    void setSessionList(const(UiSessionItem)[] items) {
        if (items.length == 0) {
            tuiSetSessionList(tuiState, null, 0);
            return;
        }
        auto app = appender!(SessionItem[])();
        foreach (ref const item; items) {
            app.put(SessionItem(toTuiString(item.id.get), toTuiString(item.title),
                    toTuiString(item.preview), item.messageCount,
                    item.isActive ? 1 : 0, item.updatedAtMs, item.flags, 0));
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

    void render(bool blocking = false) {
        TuiRuntimeStatus rs = TuiRuntimeStatus.init;
        rs.phase = runtimeStatus_.phase;
        rs.detail = toTuiString(runtimeStatus_.detail);
        rs.model = toTuiString(runtimeStatus_.model);
        rs.contextUsed = runtimeStatus_.contextUsed;
        rs.contextMax = runtimeStatus_.contextMax;
        rs.compactAt = runtimeStatus_.compactAt;
        rs.tokensPerSecond = runtimeStatus_.tokensPerSecond;
        static assert(TuiRuntimeStatus.sizeof == 88,
                "the D mirror of TuiRuntimeStatus must pin the ABI layout");
        tuiSetRuntimeStatus(tuiState, &rs);

        if (tuiIsSubmitReady(tuiState) != 0) {
            String userQuery = tuiGetSubmitQuery(tuiState);
            query_ = toString(userQuery);
            String_Free(userQuery);
            tuiResetSubmit(tuiState);
        }

        tuiLogToTui(logSwap, tuiState);

        syncRawLogLevel();

        tuiRenderBlocking(tuiState, blocking ? 1 : 0);

        if (tuiQuitRequested(tuiState) != 0)
            userTerminated_ = true;
    }
}

interface TUIListener {
    void userQuery(string s);
    void sessionSelect(SessionId id);
    void sessionNew();
    void sessionRename(SessionId id, string title);
    void sessionDelete(SessionId id);
    void sessionPinToggle(SessionId id);
    void sessionClone(SessionId id);
    void uiTerminated();
    void uiStartupFailed(string reason);
}

interface TUICommands {
    void uiMsg(UiChatMessage m);
    void uiMsg(UiChatThinkMessage m);
    void uiMsg(UiFinalAnswer m);
    void uiMsg(UiRuntimeStatus m);
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

// Human-readable TuiBackendKind name for the startup log line.
private string backendKindName(int kind) {
    switch (kind) {
    case TuiBackendKind_Tui:
        return "Tui";
    case TuiBackendKind_Gui:
        return "Gui";
    default:
        return "None";
    }
}

void playNotification() {
    import llm.config : ProgramName;
    import my.optional;
    import my.resource;
    import std.file : exists;
    import std.process : spawnProcess, Config;
    import std.stdio : File;
    import std.sumtype : match;

    static bool hasPlayer = true;

    if (!hasPlayer)
        return;

    auto path = dataSearch(ProgramName).resolve(Path("notification.wav"));
    path.match!((Some!ResourceFile p) {
        try {
            if (p.get.exists) {
                auto f = File("/dev/null");
                spawnProcess(["aplay", p.get.toString], f, f, f, null, Config.detached);
            }
        } catch (Exception e) {
            logger.trace(e.msg);
            hasPlayer = false;
        }
    }, (None _) {});

}

class TextUserInterfaceActor : TUICommands {
    import std.datetime : dur, Clock, Duration, SysTime;
    import llm.utility : stopAgent;
    import std.conv : to;

    private {
        ActorRef selfRef;

        TypedAddress!TUIListener listenerAddress;
        Channel!TUIListener listener;

        // Guards uiTick(): the chain stopper. An in-flight tick may still be
        // dispatched after uiTerminate - skip the poll/render work rather than
        // touch the torn-down UI. No repeating entry exists anymore, so
        // nothing needs canceling.
        bool running = true;
    }

    // Package-visible state: read/write seam for the llm.tui.tests driver
    // (see the TextUserInterface block above).
    package {
        // Incremented each render cycle which make it possible to observe if
        // and how often the UI render has executed.
        ulong updateCycle;

        // Non-empty when the backend attach failed (it runs in onSpawn, on
        // the actor's own thread); dispatched once from onSpawn via
        // uiStartupFailed (the typed listener exists only there) instead of
        // arming the frame tick. The startup-failure test injects it before
        // onSpawn runs.
        string startupError;

        bool debugUi;

        TextUserInterface ui;

        // Production path: the backend attach is deferred from the ctor to
        // onSpawn so the GLFW/GL context (thread-affine) is created on the
        // same thread that later drives the frames (the actor is spawned
        // with Config.detached). state is non-owning: ui owns it and its
        // dtor destroys it once.
        bool initPending;
        TuiBackendMode pendingMode;
        TuiState* state;

        TuiSessionActionType pendingAction = TuiSessionAction_None;

        string pendingActionId;
        string pendingActionTitle;
    }

    // Production path: create the C state now (no backend); the backend
    // attach selected by `mode` is deferred to onSpawn so the window and the
    // GL context are created on the actor's own thread - GLFW/GL state is
    // thread-affine and the frame loop below drives it (the actor is spawned
    // with Config.detached, keeping onSpawn and every frame on one thread).
    // Auto's GUI->TUI failover is resolved in C++ (a TTY check cannot be
    // faked from here); a failure records the precise reason, dispatched
    // from onSpawn. On success the active backend and any fallback note are
    // logged; the note also seeds the status line. The ui field takes the
    // state on every path so its dtor tears it down once.
    this(TypedAddress!TUIListener listener, Path iniFile, long maxWidth,
            TuiBackendMode mode, bool debugUi = false, long idleParkMs = 0) {
        assert(maxWidth >= 0 && maxWidth <= 10_000,
                "maxWidth out of int-safe range: " ~ maxWidth.to!string);
        this.listenerAddress = listener;
        this.debugUi = debugUi;
        pendingMode = mode;
        initPending = true;
        state = tuiCreateState();
        ui = TextUserInterface(state);
        ui.setMaxWidth(cast(int) maxWidth);
        ui.setIdlePark(cast(int) idleParkMs);
        ui.setIniFile(iniFile);
    }

    // Headless injection seam (tests): raw C state pointer, no init; a
    // by-value TextUserInterface parameter would double-free the state (its
    // copy's dtor runs at ctor exit, then the field's dtor runs again in
    // onExit). Ownership transfers on successful return only; a throw
    // (assert) leaves the state with the caller.
    this(TypedAddress!TUIListener listener, Path iniFile, long maxWidth,
            TuiState* state, long idleParkMs = 0) {
        assert(maxWidth >= 0 && maxWidth <= 10_000,
                "maxWidth out of int-safe range: " ~ maxWidth.to!string);
        this.listenerAddress = listener;
        this.state = state;
        ui = TextUserInterface(state);
        ui.setMaxWidth(cast(int) maxWidth);
        ui.setIdlePark(cast(int) idleParkMs);
        ui.setUiAsStdLogger;
        ui.setIniFile(iniFile);
    }

    // with the flag set, open the Raw Log before the first frame and run the logger sync off the
    // same edge the per-frame poll uses, so records emitted between process start and the first
    // frame (backend attach, first agent traffic) are captured too - but only when the floor asks
    // for trace: the poll's predicate decides, so a floor of info leaves the logger configured.
    // Without the flag, nothing changes.
    package void debugUiSpawnEdge() {
        if (!debugUi)
            return;
        ui.setRawLogOpen(true);
        ui.syncRawLogLevel();
    }

    void onSpawn(ActorRef selfRef) {
        void attachBackend() {
            // the window and the GL context are created (and the context made
            // current) HERE, the thread that also drives the frames. On success
            // the active backend and any fallback note are logged; the note also
            // seeds the status line. The TuiLogger swap only makes sense with a UI
            // that drains it: on a failed init nothing renders, so the normal
            // console logger stays installed and the app's shutdown diagnostics
            // are not swallowed.
            initPending = false;
            string note;
            if (tuiInit(state, pendingMode) != 0) {
                auto err = tuiLastError();
                startupError = .toString(err);
                String_Free(err);
                return;
            }
            logger.infof("UI backend active: %s", backendKindName(tuiBackendActive(state)));
            auto n = tuiBackendNote();
            note = .toString(n);
            String_Free(n);
            if (!note.empty)
                logger.info("UI backend note: ", note);
            ui.setUiAsStdLogger;
            if (!note.empty)
                ui.setRuntimeStatus(UiRuntimeStatus(TuiPhase.idle, note));
            debugUiSpawnEdge();
        }

        this.selfRef = selfRef;
        listener = typeof(listener)(listenerAddress, this.selfRef);
        // attach the backend HERE, on the actor's own thread.
        if (initPending && startupError.empty)
            attachBackend();
        if (!startupError.empty) {
            listener.uiStartupFailed(startupError);
            return;
        }
        dynSend(selfRef.address, "uiTick");
    }

    void onExit(ExitMsg _) {
        ui = TextUserInterface.init;
    }

    void onException(Exception e) {
        logger.warning("TUI actor exception: ", e.msg);
    }

    void onError(ErrorMsg e) {
        logger.warning("TUI actor error: ", e.reason.to!string);
    }

    /** Non-destructive peek over the actor's mailbox before the render -
     * pending work means the render must not park.
     */
    package bool hasPending() @safe {
        return selfRef.hasPending();
    }

    /** How many incoming messages are pending: the count twin of the
     * `hasPending()` peek above (same incoming-only semantics - see there
     * for the stash/delayed/repeating reasoning). The mailbox must hold
     * exactly the one re-armed tick, never an accumulated pile. */
    package size_t pendingCount() @safe {
        return selfRef.pendingCount();
    }

    // Must be called by every message handler except uiTerminate. Renders -
    // parking inside the C call when the mailbox is clean (no pending work) -
    // then polls the pending user query and at most one session action (the
    // polls harvest what the C++ side collected during the park).
    private void postProcess() {
        import std.string : strip;

        const pending = hasPending();
        ui.render(!pending);

        // GUI window close / quit chord: the parked entry answers the
        // user-terminated condition (tuiQuitRequested). Run the same exit
        // handshake as the app-initiated uiTerminate (final render + notify +
        // exit).
        if (ui.hasUserTerminated()) {
            uiTerminate();
            return;
        }

        ++updateCycle;

        auto query = ui.userQuery();

        // Composer Stop / Esc (task 3): one poll per rendered frame, before the
        // typed-query dispatch below. Same call the "/stop" intercept makes, so
        // a UI cancel and a typed stop are indistinguishable to the agent.
        if (ui.takeCancel()) {
            stopAgent();
            ui.setRuntimeStatus(UiRuntimeStatus(TuiPhase.waiting, "Stopping agent"));
        }

        if (!query.strip.empty) {
            if (query == "/stop") {
                stopAgent();
                ui.setRuntimeStatus(UiRuntimeStatus(TuiPhase.waiting, "Stopping agent"));
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
            case TuiSessionAction_PinToggle:
                listener.sessionPinToggle(SessionId(pendingActionId));
                break;
            case TuiSessionAction_Clone:
                listener.sessionClone(SessionId(pendingActionId));
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
    }

    void uiTick() {
        if (!running)
            return; // in-flight tick after uiTerminate: skip the work
        scope (exit) {
            if (running)
                dynSend(selfRef.address, "uiTick");
        }
        postProcess();
    }

    void uiMsg(UiChatMessage a) {
        ui.addChatMessage(a.msg, null, a.type, a.nr, a.time);
    }

    void uiMsg(UiChatThinkMessage a) {
        ui.addChatMessage(a.msg, a.thinking, a.type, a.nr, a.time);
    }

    void uiMsg(UiFinalAnswer a) {
        ui.addChatMessage(a.msg, null, TuiChatMessageType_FinalAnswer, a.nr, a.time);
    }

    void uiMsg(UiRuntimeStatus a) {
        ui.setRuntimeStatus(a);
    }

    void uiMsg(UiInitHistory m) {
        ui.setHistory(m.queries);
    }

    void uiMsg(UiSessionList m) {
        ui.setSessionList(m.items);
    }

    void uiMsg(UiStreamChatMessage m) {
        ui.streamChat(m.msg, m.thinking);
    }

    void uiMsg(UiLogFile a) {
        // do not call postProcess because this is only an initialization
        ui.useUiLogFile(a.useFile);
    }

    void uiMsg(UiPipelineStreamChatMessage m) {
        ui.pipelineMessage(m);
    }

    void uiMsg(UiPipelineStreamDone m) {
        ui.pipelineMessage(m);
    }

    void uiStreamChatDone() {
        ui.streamChatDone();
    }

    void uiPipelineClear() {
        ui.pipelineClear();
    }

    void uiClearChat() {
        ui.clearChat;
    }

    void uiAgentBusy() {
        ui.setReadyStatus(false);
    }

    void uiAgentReady() {
        ui.setReadyStatus(true);
        playNotification();
    }

    // Termination handshake: final render, async-notify the agent, exit.
    // Idempotent: the window-close path can observe user-terminated on later
    // frames before the exit message is processed; only the first call runs
    // (running flips false and later calls no-op).
    //
    // No repeating entry exists anymore, so nothing needs canceling - the
    // `running` guard at the top of uiTick is what stops the chain.
    void uiTerminate() {
        if (!running)
            return;
        running = false;
        ui.render();
        listener.uiTerminated();
        sendExit(selfRef.address, ExitReason.userShutdown);
    }
}

struct UiInitHistory {
    immutable(string)[] queries;
}

struct UiChatMessage {
    string msg;
    TuiChatMessageType type = TuiChatMessageType_Assistant;
    size_t nr = 0; // 0 = display-only line (undecorated)
    MessageTime time = NoProcessingTime();
}

struct UiChatThinkMessage {
    string msg;
    string thinking;
    TuiChatMessageType type = TuiChatMessageType_Assistant;
    size_t nr = 0;
    MessageTime time = NoProcessingTime();
}

struct UiStreamChatMessage {
    string msg;
    string thinking;
}

struct UiStreamChatDone {
}

struct UiFinalAnswer {
    string msg;
    size_t nr = 0;
    MessageTime time = NoProcessingTime();
}

struct UiClearChat {
}

/// The UI's phase vocabulary, mirrored from the C side's `model::Phase`
/// (`cpp_tui/model/status_chips.h`)
enum TuiPhase {
    idle = 0,
    thinking = 1,
    tooling = 2,
    writing = 3,
    waiting = 4,
    error = 5,
    offline = 6,
}

/// The run's status as data (D-STATUS v5): the four facts the now bar's chips
/// show, plus the phase and the current step. The C++ side formats the numbers
/// into the chips and never parses them back, so the sentence form is gone -
/// a value here is what the row says, not text for it to split.
///
/// Sender contract (the ABI's, `tui_api.h`): send on every render tick at or
/// below 20 Hz, one update when a turn starts doing work, one flush at a request
/// boundary when the window swallowed the newest values, and always the final one
/// when the turn ends. `phase == offline` is the one value with a consequence
/// beyond the row: it also drives the thread's offline card and the disabled
/// composer. A capacity or rate of `0` is *absence* - the row draws the missing
/// value, never `0` - so an unknown context or a stalled rate is sent as zero,
/// not as a guess.
struct UiRuntimeStatus {
    int phase = TuiPhase.idle;
    /// The current step: a tool name while tooling, one line.
    string detail;
    /// The model context's used/total tokens and the threshold the agent's
    /// `needCompression` compares the used count with.
    long contextUsed;
    long contextMax;
    long compactAt;
    /// Generated tokens per second; `0` while none is known.
    double tokensPerSecond;
    /// The model display name, `"none"` when no model is configured.
    string model = "none";
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
    double tokensPerSecond;
    string currentTool;
    long contextUsed;
    long contextMax;
}

struct UiPipelineStreamDone {
    string agentId;
}

struct UiPipelineClear {
}

// Session sidebar snapshot (D -> UI): full ordered list of sessions.
//
// `flags` bit 0 = a run is in flight, bit 1 = the user
// pinned the session.
struct UiSessionItem {
    SessionId id;
    string title;
    string preview;
    size_t messageCount;
    bool isActive;
    long updatedAtMs; // SessionMeta.updatedAt (unix seconds) * 1000; 0 = unknown
    int flags;
}

struct UiSessionList {
    const(UiSessionItem)[] items;
}
