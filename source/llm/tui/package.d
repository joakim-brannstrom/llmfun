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
}

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
        // The store's bounds (5000 records / 2 MB) belong to the C++ ring; this queue only
        // has to survive one frame. What it cannot keep is counted, not dropped silently.
        if (entries[].length >= MaxQueued) {
            ++droppedInQueue;
            return;
        }
        const SysTime stamp = payload.timestamp;
        // Unix-epoch milliseconds, because that is what every consumer reads:
        // `tui.cpp`'s `archiveTimestamp()` and the Activity clock both do `gmtime(ms / 1000)`.
        // `SysTime.stdTime` counts hnsecs from *1 A.D.*, so `stdTime / 10_000` keeps the time
        // of day and moves the date ~1969 years out; `toUnixTime` is the epoch seconds and
        // `fracSecs` the sub-second part (`toUnixTime` alone is whole-second only).
        const long stampMs = stamp.stdTime > 0
            ? cast(long) stamp.toUnixTime * 1_000L + stamp.fracSecs.total!"msecs" : 0;
        entries.put(TuiLogRecordData(payload.logLevel, stampMs, payload.line,
                payload.msg, payload.moduleName, payload.funcName, payload.file));
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
            drained ~= TuiLogRecordData(LogLevel.warning, 0, 0,
                    "log queue dropped " ~ droppedInQueue.text ~ " records (the frame loop did not drain in time)",
                    null, null, null);
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
    static assert(__traits(identifier, TuiLogRecord.tupleof[5]) == "message"
            && __traits(identifier, TuiLogRecord.tupleof[6]) == "detail"
            && __traits(identifier, TuiLogRecord.tupleof[7]) == "module"
            && __traits(identifier, TuiLogRecord.tupleof[8]) == "function" && __traits(identifier,
                TuiLogRecord.tupleof[9]) == "file",
            "TuiLogRecord's field names changed: update makeLogRecord");

    auto message = firstLineOf(record.message);
    auto detail = restLinesOf(record.message);
    return TuiLogRecord(toTuiLevel(record.level), action, record.timestampMs,
            record.line, 0, // reserved
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

/// The Raw Log's logger rule, as data (D-RAWLOG, task 33).
///
/// Opening the developer surface raises the process' in-process logger to the developer level and
/// closing it - or quitting - restores what was configured. The rule is one struct so it can be
/// tested without a window, and so there is exactly one place that decides "raise" and "restore".
///
/// `std.logger` has no `debug` step at all (`all < trace < info < warning < error`), so the
/// design's "raise to debug" is `trace`: the one level below `info` that carries developer detail.
/// `UiLog=trace` asks for the same floor, so the design's parenthetical is satisfied by
/// construction; a level the user already made more verbose than `trace` (`all`) is never
/// lowered, because the surface may add detail but never remove it.
///
/// The archive (`tuiSetLogging`) is deliberately not part of this: it keeps the level the user
/// configured, and neither `--debug-ui` nor an open surface turns it into a trace file.
struct RawLogLevelBorrow {
    private {
        LogLevel saved;
        bool raised;
    }

    /// One call per frame with the surface's state; one write per edge. Returns true when this
    /// call changed the level (a test reads it; production only needs the effect).
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
        /// The Raw Log's borrowed logger level (D-RAWLOG): one raise per open, one restore per
        /// close, and the same restore on quit.
        RawLogLevelBorrow rawLogLevel;
    }

    package {
        string query_;
        string statusText;
        // Last header summary computed by addChatMessage (test seam: the
        // llm.tui.tests module is in package llm.tui and can read it).
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

    bool hasMoreEvents() {
        return tuiHasMoreEvents() == 1;
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

    void setStatusText(string s) {
        statusText = s;
    }

    void setMaxWidth(int w) {
        tuiSetMaxWidth(tuiState, w);
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
        auto status = msg.status.toTuiString;
        auto currentTool = msg.currentTool.toTuiString;
        String finish;
        // The layout is a wire contract with `cpp_tui/tui_api.h` (120 bytes: task 39a appended
        // the lane's fields); a reordering fails the build instead of sending a tool name where
        // the UI expects a count.
        static assert(PipelineChatMessage.sizeof == 120,
                "PipelineChatMessage layout changed: update tui_api.h and this mirror together");
        tuiPipelineAgentUpdate(tuiState, id, PipelineChatMessage(content: content, reasoning: reasoning, role: role,
                finishReason: finish, status: status, currentTool: currentTool,
                contextUsed: msg.contextUsed, contextMax: msg.contextMax));
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

    // The Raw Log's logger rule (D-RAWLOG): while the surface is up the process logs at the
    // developer level, and closing it - or quitting - restores what was configured.
    //
    // One poll per frame and one edge per change, never a write per frame: the level is a
    // process-wide setting and every write of it is visible to every producer thread.
    //
    // `std.logger` has no `debug` step at all (`all < trace < info < warning < error`), so the
    // design's "raise to debug" is `trace` - the one level below `info` that carries developer
    // detail. `UiLog=trace` asks for the same floor, so the design's parenthetical is satisfied
    // by construction; a level the user already made more verbose than `trace` (`all`) is never
    // lowered. The archive (`tuiSetLogging`) is deliberately untouched: it keeps the level the
    // user configured, and neither `--debug-ui` nor an open surface turns it into a trace file.
    void syncRawLogLevel() {
        rawLogLevel.sync(tuiIsRawLogOpen(tuiState) != 0);
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

    void render() {
        tuiBackendNewFrame(tuiState);

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

        // It runs after the frame has drawn, so a `/rawlog` or an `Esc` in
        // this frame is already reflected in the flag.
        syncRawLogLevel();

        tuiBackendRender(tuiState);
    }
}

interface TUIListener {
    void userQuery(string s);
    void sessionSelect(SessionId id);
    void sessionNew();
    void sessionRename(SessionId id, string title);
    void sessionDelete(SessionId id);
    void uiTerminated();
    void uiStartupFailed(string reason);
}

interface TUICommands {
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

        // Should the first frame start with the Raw Log open (`--debug-ui`, task 33)?
        bool debugUi;

        TypedAddress!TUIListener listenerAddress;
        Channel!TUIListener listener;

        immutable SlowUpdateInterval = 10.dur!"msecs";
        immutable FastUpdateInterval = 2.dur!"msecs";

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

        // Non-empty when the backend attach failed (it runs in onSpawn, on
        // the actor's own thread); dispatched once from onSpawn via
        // uiStartupFailed (the typed listener exists only there) instead of
        // arming the frame tick. The startup-failure test injects it before
        // onSpawn runs.
        string startupError;

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

        SysTime nextUpdate;
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
            TuiBackendMode mode, bool debugUi = false) {
        assert(maxWidth >= 0 && maxWidth <= 10_000,
                "maxWidth out of int-safe range: " ~ maxWidth.to!string);
        this.listenerAddress = listener;
        this.debugUi = debugUi;
        pendingMode = mode;
        initPending = true;
        state = tuiCreateState();
        ui = TextUserInterface(state);
        ui.setMaxWidth(cast(int) maxWidth);
        ui.setIniFile(iniFile);
    }

    // Headless injection seam (tests): raw C state pointer, no init; a
    // by-value TextUserInterface parameter would double-free the state (its
    // copy's dtor runs at ctor exit, then the field's dtor runs again in
    // onExit). Ownership transfers on successful return only; a throw
    // (assert) leaves the state with the caller.
    this(TypedAddress!TUIListener listener, Path iniFile, long maxWidth, TuiState* state) {
        assert(maxWidth >= 0 && maxWidth <= 10_000,
                "maxWidth out of int-safe range: " ~ maxWidth.to!string);
        this.listenerAddress = listener;
        this.state = state;
        ui = TextUserInterface(state);
        ui.setMaxWidth(cast(int) maxWidth);
        ui.setUiAsStdLogger;
        ui.setIniFile(iniFile);
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
                ui.setStatusText(note);
            // `--debug-ui` (task 33): open the developer surface *before* the first frame, so it
            // is up on the frame the window appears. The flag is a per-run request: the open
            // state is never persisted, and the surface's own `/rawlog`/Close still work.
            if (debugUi)
                ui.setRawLogOpen(true);
        }

        this.selfRef = selfRef;
        listener = typeof(listener)(listenerAddress, this.selfRef);
        // Production path: attach the backend HERE, on the actor's own
        // thread (see the ctor note); the test seam may have injected a
        // startup error before spawn, in which case no init is attempted.
        if (initPending && startupError.empty)
            attachBackend();
        if (!startupError.empty) {
            listener.uiStartupFailed(startupError);
            return;
        }
        selfRef.scheduleRepeating(SlowUpdateInterval, "uiTick");
    }

    void onExit(ExitMsg _) {
        ui = TextUserInterface.init; // dtor: tuiDestroyState
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
        import std.string : strip;

        // it is only worth processing the rest of the method if render() has
        // been called because the rest of the function react on "input" that
        // is updated by render via imgui.
        if (Clock.currTime > nextUpdate) {
            ui.render();
            nextUpdate = Clock.currTime + SlowUpdateInterval;
        } else {
            return;
        }

        // GUI window close: the C frame reports user-terminated
        // (tuiRender returned 0). Run the same exit handshake as the
        // app-initiated uiTerminate (final render + notify + exit).
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
            ui.setStatusText("Stopping agent");
        }

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
            nextUpdate = Clock.currTime + FastUpdateInterval;
        }
    }

    // Repeating self-tick target (armed in onSpawn). The shell dispatches
    // this at every interval and re-arms after each fire; the render cadence
    // is still gated by nextUpdate inside postProcess.
    void uiTick() {
        if (running)
            postProcess();
    }

    void uiMsg(UiChatMessage a) {
        ui.addChatMessage(a.msg, null, a.type, a.nr, a.time);
        postProcess;
    }

    void uiMsg(UiChatThinkMessage a) {
        ui.addChatMessage(a.msg, a.thinking, a.type, a.nr, a.time);
        postProcess;
    }

    void uiMsg(UiFinalAnswer a) {
        ui.addChatMessage(a.msg, null, TuiChatMessageType_FinalAnswer, a.nr, a.time);
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
    // Idempotent: the window-close path can observe user-terminated on later
    // frames before the exit message is processed; only the first call runs
    // (running flips false and later calls no-op). cancelTick keeps the
    // zombie quiescent: without it the shell would keep re-arming due ticks
    // (uiTick no-ops via `running`) until the system shuts the actor down at
    // process exit.
    void uiTerminate() {
        if (!running)
            return;
        running = false;
        selfRef.cancelTick();
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
// `updatedAtMs` and `flags` are the v5 additions (task 24) the workbench rail
// groups and marks rows with; the legacy panel ignores them, so both renderers
// can share one snapshot. `flags` bit 0 = a run is in flight. `unread` and
// `pinned` are UI-local by design and are therefore not here (FR-03, D-19).
struct UiSessionItem {
    SessionId id;
    string title;
    string preview;
    size_t messageCount;
    bool isActive;
    long updatedAtMs; // SessionMeta.updatedAt (unix seconds) * 1000; 0 = unknown
    int flags; // bit 0 = running (TuiSessionFlag_Running); no other bit is used
}

struct UiSessionList {
    const(UiSessionItem)[] items;
}
