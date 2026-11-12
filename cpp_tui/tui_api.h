#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* tui_api.h — Pure C API header for the TUI library
 *
 * This header defines a C-compatible interface using only raw pointers
 * and a plain String struct. No C++ strings, no references, no templates,
 * no C++ namespaces, no function overloading.
 *
 * D interop: D callers import this header directly via extern(C):
 *
 *   extern(C) {
 *       struct String { const(char)* data; size_t len; }
 *       struct ChatMessageParam {
 *           String summary;
 *           String text;
 *           String thinking;
 *           TuiChatMessageType type;
 *       }
 *       struct TuiState {}
 *       void tuiAddChatMessage(TuiState* state, ChatMessageParam param);
 *       // ... etc
 *   }
 *
 * Memory allocator: All strings returned by this API are allocated via
 * malloc (C standard library). Callers MUST free them via String_Free()
 * which uses free(). Do NOT mix allocators — using D's GC or a different
 * allocator to free these pointers is undefined behavior.
 *
 *   Thread-local (per-thread):
 *     tuiLastError
 *     tuiBackendNote
 */

/* TUI_API_VERSION - API generation marker. Documentation only: no runtime
 * behavior depends on it.
 */
#define TUI_API_VERSION 11

/* TuiChatMessageType — Pure C enum for chat message types.
 * Used to color-code chat message headers in the TUI.
 * Values mirror the C++ ChatMessageType enum in tui.h.
 */
typedef enum TuiChatMessageType {
    TuiChatMessageType_User = 0,
    TuiChatMessageType_Assistant = 1,
    TuiChatMessageType_ToolCall = 2,
    TuiChatMessageType_ToolResponse = 3,
    TuiChatMessageType_Vision = 4,
    TuiChatMessageType_System = 5,
    TuiChatMessageType_FinalAnswer = 6,
    TuiChatMessageType_Count = 7 /* Sentinel — not a valid type */
} TuiChatMessageType;

/* String — Plain old data struct representing a string slice.
 * No constructors, no destructors, no hidden state. Trivially copyable.
 *
 * Ownership rules:
 *   - String values passed TO mutating API functions (tuiAddOutputLine,
 *     tuiAddChatMessage, etc.) are NOT copied; the caller must ensure the
 *     underlying buffer lives long enough for the call to complete.
 *   - String_New() is an exception: it copies the input buffer via
 *     malloc, so the caller's original buffer can be freed immediately.
 *   - Strings returned FROM this API are heap-allocated (via malloc)
 *     and MUST be freed with String_Free() when no longer needed.
 */
typedef struct String {
    const char* data;
    size_t len;
} String;

/* Create a String from a null-terminated C string.
 * The returned String points to a newly malloc'd copy of the data.
 * Caller must free the result with String_Free().
 * Null-safe: returns {NULL, 0} if cstr is NULL.
 */
String String_New(const char* cstr);

/* Create a String from a raw buffer (non-null-terminated).
 * The returned String points to a newly malloc'd copy of `len` bytes from `data`.
 * Caller must free the result with String_Free().
 * Null-safe: returns {NULL, 0} if data is NULL (len is ignored).
 */
String String_NewBuf(const char* data, size_t len);

/* Free a String that was returned by an API function or created via String_New / String_NewBuf.
 * No-op if s.data is NULL.
 * Undefined behavior if s.data was not allocated by String_New, String_NewBuf, or an API function.
 * Caller must ensure the pointer matches the allocator that produced it.
 * Note: Since s is passed by value, s.data is NOT zeroed after freeing.
 * The caller should zero the String struct if needed: s.data = NULL; s.len = 0;
 */
void String_Free(String s);

/* ChatMessageParam - Bundles all parameters for a chat message.
 *
 * Pass by value (trivially copyable). The thinking field is optional
 * and may be {NULL, 0} if no thinking content exists.
 *
 * Example initialization:
 *   ChatMessageParam msg = {
 *       .summary  = String{"User", 4},
 *       .text     = String{"Hello", 5},
 *       .thinking = {NULL, 0},  // no thinking content
 *       .type     = TuiChatMessageType_User,
 *   };
 */
typedef struct ChatMessageParam {
    String summary;
    String text;
    String thinking;
    TuiChatMessageType type;
} ChatMessageParam;

/* PipelineChatMessage - one streaming update for a pipeline agent lane (the Agents tab's
 * per-agent row).
 *
 * Ownership: every String is inbound (non-owning); `tuiPipelineAgentUpdate` copies their data
 * during the call, so the caller's buffers may be reused or freed as soon as it returns.
 *
 * Field notes:
 *   * `currentTool` - the newest tool this agent has called in the response ("" = none yet);
 *   * `contextUsed` / `contextMax` - tokens in the context, and the model's context size. A
 *     `contextUsed` above 0 is drawn whatever the ceiling is; the ceiling is what makes a *zero*
 *     unambiguous for the sender: `0/8192` is a real zero, `0/0` is no reading at all.
 *     `contextUsed` alone cannot tell those apart, which is why the pair is sent instead of the
 *     count on its own.
 *   * `tokensPerSecond` - D's rate, sent raw: the UI formats it and never recomputes it.
 *     `0.0` means "no reading this tick" (a queued lane draws no rate), the same convention
 *     as a `contextUsed` of 0.
 */
typedef struct PipelineChatMessage {
    String content;
    String reasoning;
    String role;
    String finishReason;    /* sent empty; only copied */
    String currentTool;     /* newest tool of this response; "" = none yet */
    int64_t contextUsed;    /* tokens in context; 0 is a value */
    int64_t contextMax;     /* the model's ceiling; 0 = unknown. It is only
                             * needed to make a ZERO unambiguous (0/8192 = a real zero, 0/0 = no
                             * reading at all); a contextUsed above 0 is drawn either way */
    double tokensPerSecond; /* D's per-response or rolling rate; 0.0 = no reading */
} PipelineChatMessage;

/* TuiToolCallParam - One tool call a pipeline agent made.
 *
 * Ownership: all Strings are inbound (non-owning). The receiver copies the
 * data during tuiPipelineAgentToolCall, so the caller's buffers may be reused
 * or freed immediately after the call returns. `arguments` is truncated
 * D-side to 1000 chars, so the wire never carries an unbounded tool payload.
 */
typedef struct TuiToolCallParam {
    String name;      /* tool name, copied by the receiver */
    String arguments; /* truncated D-side to <=1000 chars */
    String id;        /* the call id, as sent by the model */
} TuiToolCallParam;

/* TuiToolResultParam - The result of one tool call a pipeline agent made.
 *
 * Ownership: all Strings are inbound (non-owning), copied by the receiver.
 * `text` is truncated D-side to 1000 chars. `ok` is `int` rather than a C++
 * `bool` so the struct's padding is deterministic on every ABI.
 */
typedef struct TuiToolResultParam {
    String name; /* tool name (repeated from the call, keeps rows self-contained) */
    int ok;      /* 1 = success, 0 = failure (saveData["success"], default 0) */
    String text; /* result text, truncated D-side to <=1000 chars */
} TuiToolResultParam;

/* SessionItem - One row of the session sidebar snapshot.
 *
 * Ownership: all Strings are inbound (non-owning). The panel copies their
 * data during tuiSetSessionList, so the caller's buffers may be reused or
 * freed immediately after the call returns.
 *
 * The three fields after `isActive` are *appended*, so every earlier offset
 * is unchanged and a caller that is recompiled against this header and
 * leaves them zero (a `{0}`-style initializer) still produces a valid
 * snapshot:
 * `updatedAtMs == 0` means "unknown", which the rail renders as a flat list
 * with no fabricated dates, and `flags == 0` means no row is marked running.
 *
 * That compatibility is **source-level**. D and C++ ship together, so an
 * object file compiled against an older, smaller layout of this struct and
 * linked against this library would stride the wrong number of bytes through
 * the array. "Append only" means no *recompiled* caller changes behaviour; it
 * is not a promise that a stale binary keeps working.
 *
 * Example initialization:
 *   SessionItem item = {
 *       .id           = String{"abc123", 6},
 *       .title        = String{"My Chat", 7},
 *       .preview      = String{"Hello", 5},
 *       .messageCount = 3,
 *       .isActive     = 1,
 *       .updatedAtMs  = 1766083200000,
 *       .flags        = TuiSessionFlag_Running,
 *   };
 */
typedef struct SessionItem {
    String id; /* immutable session id */
    String title;
    String preview; /* first user message, truncated by D */
    size_t messageCount;
    int isActive;        /* 1 = active, 0 = not */
    int64_t updatedAtMs; /* unix ms; 0 = unknown */
    int32_t flags;       /* TuiSessionFlag_* bits, 0 = none */
    int32_t reserved;    /* must be 0 */
} SessionItem;

/* SessionItem.flags - what the session snapshot may say about a row.
 *
 * Running (bit 0) and Pinned (bit 1) cross the ABI. `unread` stays UI-local by
 * design: unread is "updatedAt advanced while this session was
 * not active during this process lifetime", so it is not a fact the store has.
 * Bits above 1 are reserved for later additive flags and must be zero, exactly
 * like `reserved`.
 */
#define TuiSessionFlag_Running 1 /* bit 0: a run is in flight for this session */
#define TuiSessionFlag_Pinned 2  /* bit 1: the user pinned this session */

/* TuiSessionActionType - Kind of a session sidebar action.
 * Append-only ordering: existing values must never be renumbered or
 * reused, so future actions (Fork, Export, Archive, Search, ...) can be
 * added without breaking the D-side mapping.
 */
typedef enum TuiSessionActionType {
    TuiSessionAction_None = 0,      /* sentinel - no action (empty queue) */
    TuiSessionAction_Select = 1,    /* switch to the session */
    TuiSessionAction_New = 2,       /* create a new session */
    TuiSessionAction_Rename = 3,    /* rename the session (title payload) */
    TuiSessionAction_Delete = 4,    /* delete the session (the UI never re-asks) */
    TuiSessionAction_PinToggle = 5, /* toggle the session's pinned bit */
    TuiSessionAction_Clone = 6      /* duplicate the session (dispatch lives D-side) */
} TuiSessionActionType;

/* SessionAction - One queued sidebar action, consumed by tuiGetSessionAction.
 *
 * Ownership: the Strings returned by tuiGetSessionAction are owned
 * (malloc'd) and MUST be freed with String_Free() when no longer needed.
 * An empty field is returned as {NULL, 0}.
 *
 * Fields:
 *   sessionId - target session id; empty ({NULL, 0}) for New.
 *   title     - new title for Rename; empty for all other types.
 */
typedef struct SessionAction {
    TuiSessionActionType type;
    String sessionId;
    String title;
} SessionAction;

/* TuiState — Opaque handle to the internal TUI state; the single lifecycle
 * object of the C API.
 * Created via tuiCreateState(), a backend is attached via tuiInit(), and the
 * whole state is torn down via tuiDestroyState().
 */
typedef struct TuiState TuiState;

/* TuiChipStatus - Life of one dropped attachment (ordinals are ABI: the
 * D-side worker switches on them). Append-only: existing values must never be
 * renumbered or reused.
 */
typedef enum TuiChipStatus {
    TuiChipStatus_Pending = 0, /* dropped, D has not reported back yet */
    TuiChipStatus_Done = 1, /* the workarea copy exists; the label is its workarea-relative path */
    TuiChipStatus_FailedRaw = 2, /* conversion failed, the raw copy was attached; the label is its
                                    workarea-relative path */
    TuiChipStatus_Failed = 3     /* nothing was attached; the label is the short reason */
} TuiChipStatus;

/* TuiDroppedPath - One queued drop, consumed by tuiGetDroppedPath.
 *
 * Ownership: `path` is owned (malloc'd) and MUST be freed with String_Free()
 * when no longer needed. An empty path is returned as {NULL, 0}.
 *
 * Fields:
 *   chipId - the id of the chip the drop created (0 = none, never a real id);
 *            report the chip back through tuiSetChipStatus with this id.
 *   path   - the raw OS path, verbatim. Never echoed into a message as-is.
 */
typedef struct TuiDroppedPath {
    int64_t chipId;
    String path;
} TuiDroppedPath;

/* TuiChipParam - One chip of the row snapshot, returned by tuiGetChipAt.
 *
 * Ownership: `path` is owned (malloc'd) and MUST be freed with String_Free()
 * when no longer needed. An empty path is returned as {NULL, 0}.
 *
 * Fields:
 *   chipId - the chip's id (report or cancel by it; 0 = none).
 *   status - a TuiChipStatus value.
 *   path   - the workarea-relative path on Done/FailedRaw; empty otherwise.
 */
typedef struct TuiChipParam {
    int64_t chipId;
    int32_t status; /* TuiChipStatus value */
    String path;
} TuiChipParam;

/* Retrieve the last error message as an owned String.
 * The error is consumed (cleared) on the first call — this is intentional
 * so that stale errors from previous operations don't persist indefinitely.
 * If you need to inspect the error without consuming it, call this function
 * immediately after the operation that may have failed.
 * Returns {NULL, 0} if no error was set.
 * Caller must free the result with String_Free().
 * Thread-safe: thread-local storage.
 */
String tuiLastError(void);

/* TuiBackendMode — requested backend for tuiInit (append-only). */
typedef enum TuiBackendMode {
    TuiBackendMode_Auto = 0, /* try the GUI, fall back to the terminal UI */
    TuiBackendMode_Tui = 1,  /* terminal UI; no GUI attempt */
    TuiBackendMode_Gui = 2   /* require the GUI; fail if it cannot start */
} TuiBackendMode;

/* TuiBackendKind — backend actually attached to a state (append-only). */
typedef enum TuiBackendKind {
    TuiBackendKind_None = 0, /* no backend (headless/tests) */
    TuiBackendKind_Tui = 1,
    TuiBackendKind_Gui = 2
} TuiBackendKind;

/* Initialize and attach a backend to the state.
 *
 * mode selects what to bring up (any other value is treated as Auto):
 *   - TuiBackendMode_Tui: the terminal (ncurses) backend; no GUI attempt.
 *   - TuiBackendMode_Gui: the GUI (GLFW/OpenGL3) backend; fails if it cannot
 *     start — no fallback.
 *   - TuiBackendMode_Auto: try the GUI backend first; on failure, if stdin
 *     AND stdout are TTYs, print one line "GUI unavailable: <reason>;
 *     falling back to terminal UI" to stderr, record it as the backend note
 *     (see tuiBackendNote) and attach the terminal backend instead;
 *     otherwise fail with the GUI reason. (If the terminal fallback itself
 *     fails, the reported error combines both reasons and no note is
 *     recorded.)
 *
 * Creates the ImGui context (destroyed by tuiDestroyState) and applies the
 * dark theme after the backend initialized. Returns 0 on success.
 *
 * On failure the return value is non-zero; the precise stage reason is
 * available via tuiLastError(). The state stays backend-less and the call
 * may be retried (possibly with a different mode). Re-initializing an
 * already-initialized state fails cleanly; at most one initialized state
 * may exist per process.
 */
int tuiInit(TuiState* state, TuiBackendMode mode);

/* Create a new TUI state object.
 *
 * Allocates and initializes a TuiState instance containing empty output,
 * empty input buffer, disabled submission flag, and default auto-scroll.
 * No backend is attached and no ImGui context is created — call tuiInit()
 * to attach a backend.
 * The state is independent — you can create multiple states and pass them
 * to API functions to manage separate TUI sessions.
 *
 * Returns an opaque TuiState* on success, NULL on failure (e.g. out of memory).
 */
TuiState* tuiCreateState(void);

/* Destroy a TUI state object and free all associated memory.
 *
 * Tears down the attached backend (if any) and destroys the ImGui context
 * that tuiInit created for this state; an ImGui context created by the
 * caller itself (headless test harnesses) is left alone.
 *
 * After this call the TuiState* handle is invalid and must not be used again.
 *
 * Null-safe: passing NULL is a no-op. Safe before tuiInit.
 */
void tuiDestroyState(TuiState* state);

/* Process backend input and start a new ImGui frame.
 *
 * This function encapsulates the backend calls required to begin a frame:
 * reads terminal input (ncurses) or pumps window events (GLFW), then creates
 * a new ImGui frame. Call this at the start of each iteration of your main
 * loop, before calling tuiRender().
 *
 * None-safe: no-op if state is NULL or has no backend attached.
 */
void tuiBackendNewFrame(TuiState* state);

/* Render the current ImGui frame through the active backend.
 *
 * This function encapsulates the backend calls required to end a frame:
 * renders the ImGui draw list and presents it — draws the text renderer to
 * the terminal, or presents to the GLFW window. Call this at the end of each
 * iteration of your main loop, after calling tuiRender().
 *
 * None-safe: no-op if state is NULL or has no backend attached.
 *
 * Typical frame loop:
 *
 *   tuiBackendNewFrame(state);
 *   if (tuiRender(state) == 0) break;  // user requested exit
 *   // ... process input, update state ...
 *   tuiBackendRender(state);
 */
void tuiBackendRender(TuiState* state);

/* Render one UI frame using the given state.
 *
 * Draws the three UI regions: the scrollable output area, the multiline
 * input field, and the status line. Handles keyboard shortcuts internally
 * (Ctrl+C to exit, End to scroll to bottom, Escape to clear input).
 *
 * Returns 0 if the user requested exit (pressed Ctrl+C, or closed the GUI
 * window), 1 otherwise. The caller should break the main loop when this
 * returns 0.
 *
 * None-safety (deliberate asymmetry): tuiRender(NULL) returns 0, while a valid
 * state with no backend attached is inert and returns 1 (continue). No ImGui
 * access happens in either case.
 */
int tuiRender(TuiState* state);

/* The parked render entry.
 *
 * One call = one pump + render + present cycle: pumps backend input and
 * begins a new ImGui frame (tuiBackendNewFrame), renders, presents, and -
 * when `blocking` is non-zero - waits one idle park (16 ms default, ~30 fps
 * design cap) inside the call. Input wakes the wait early. The user-terminated
 * condition (the quit chord or the GUI window close) returns before any
 * wait, so the caller's loop ends through its normal handshake.
 *
 * blocking = 0 never waits: one pump + render + present, then return.
 *
 * None-safety: a NULL state, an uninitialized state, or a state with no
 * backend attached is inert - returns without touching anything.
 *
 * Distinct C symbol: the C API is extern "C" - no overloading - so the
 * parked entry cannot share the tuiRender name.
 *
 * Frame-thread only, like tuiRender.
 */
void tuiRenderBlocking(TuiState* state, int blocking);

/* Peek the user-terminated condition the frame loop just observed: the quit
 * chord (the palette's Quit row, the keymap's quit chord) or the GUI window
 * close.
 *
 * Non-consuming, like tuiIsSubmitReady: the condition stays
 * set - the actor is exiting and nothing resets it.
 *
 * None-safety: a NULL state, an uninitialized state, or a state with no
 * backend attached answers 0.
 *
 * Frame-thread only, like tuiRender.
 */
int tuiQuitRequested(const TuiState* state);

/* Get the backend currently attached to the state as a TuiBackendKind.
 *
 * Returns TuiBackendKind_None for a NULL state or an uninitialized
 * (backend-less) state.
 */
int tuiBackendActive(const TuiState* state);

/* Get the fallback note recorded by the most recent tuiInit call on the
 * calling thread, as an owned String (free with String_Free()).
 *
 * Non-empty only when an Auto init fell back from the GUI to the terminal
 * backend; the note is exactly the one-line message printed to stderr:
 * "GUI unavailable: <reason>; falling back to terminal UI".
 * Every tuiInit call resets the note, so it is empty ({NULL, 0}) when the
 * most recent call had no fallback (including a call rejected by the init
 * guards). Thread-local, like tuiLastError; reading does not consume it.
 */
String tuiBackendNote(void);

/* Set the logging to on/off; may be called at any time, including between frames (the
 * `/debug` command toggles it at runtime).
 *
 * On: `llmfun_ui_log.txt` (in the process's working directory) is opened lazily at the first
 * archived line, and a latch set by a previous failed open or write is cleared and the failed
 * stream dropped - a new request is a new attempt. Off: the handle is closed, so nothing lands
 * in a file the user switched off. A failed open or write is never retried per record.
 */
void tuiSetLogging(TuiState* state, int onOff);

/* Cap the TUI's rendered width in terminal columns. 0 = unlimited (default,
 * current behavior). Positive values are the cap in columns and should be in
 * [40, 10000]; a positive value below 40 is raised to 40 (a smaller cap would
 * leave the TUI stuck on its "Terminal too small!" screen). Negative values
 * are treated as 0 (unlimited). Call after tuiCreateState and before the
 * first tuiBackendNewFrame/tuiRender; a late call applies from the next
 * frame. Effective width = min(terminal width, maxWidth). Text backend only:
 * the GUI pixel profile ignores the cap; see tuiSetMaxWidth in
 * tui.h). Null-safe.
 */
void tuiSetMaxWidth(TuiState* state, int maxWidth);

/* Set the idle park timeout in milliseconds: what a blocking render pass
 * waits when nothing happened in it. Only the idle cadence depends on it -
 * with input the wait wakes early on events, so latency is unaffected. The
 * effective idle cycle is the newFrame wait (~17 ms) + this park + the
 * render time; the default 16 ms gives the ~30 fps design cap (24 fps
 * under software GL). Non-positive keeps the default. Valid values are
 * clamped to [4, 1000] (~40 fps to
 * ~1 fps idle); e.g. 75 ms lands a software-GL box near 10 fps idle.
 * Null-safe.
 */
void tuiSetIdlePark(TuiState* state, int parkMs);

/* Set the file imgui persists its settings to (io.IniFilename)
    Call this BEFORE `tuiInit`: the name is captured when tuiInit creates the
    ImGui context, and a later call has no effect. NULL or an empty filename
    disables persistence (imgui's "$cwd/imgui.ini" default is not used).
 */
void tuiSetIniFilename(TuiState* state, String filename);

/* Initialize the history in the input field */
void tuiInitQueryHistory(TuiState* state, const String* history, size_t count);

/* Append a line to the scrollable output display area.
 *
 * The output area has a maximum capacity (1000 lines). When exceeded, the
 * oldest lines are evicted first (FIFO). The `line` parameter is an inbound
 * String — its data is copied internally, so the caller's buffer can be
 * freed or reused immediately after this call returns.
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiAddLogMessage(TuiState* state, String summary, String text);

/* Structured log records
 *
 * The log crossed this ABI as two formatted strings, which cannot carry the fields the
 * Raw Log needs (module, function, file, line) and forced D to format a line the UI then
 * had to un-format. A record carries the fields instead: D forwards what its
 * `std.logger.LogEntry` already holds, C++ owns the one bounded ring, and the archive line
 * is formatted in C++ from the record (the historical format is preserved by
 * `logArchiveLine()`).
 *
 * Ownership: every String here is inbound. The implementation copies all of them during
 * the call, so D may free or reuse its buffers as soon as the call returns (C-02). No
 * exception crosses the boundary (C-03), and no std::string sits in the struct.
 *
 * Thread: frame thread only, like every other mutating entry point (§3.8).
 */

/* TuiLogLevel — the severity of one record.
 * It mirrors D's LogLevel as the five steps the UI can tell apart: `trace` and `debug` are
 * developer detail (the Raw Log shows them, Activity does not), `info`/`warn`/`error` are
 * the user-facing three. D folds `critical` and `fatal` into `error`, and an unknown value
 * into `info`, so the UI never receives a severity it has no rendering for.
 */
typedef enum TuiLogLevel {
    TuiLogLevel_Trace = 0,
    TuiLogLevel_Debug = 1,
    TuiLogLevel_Info = 2,
    TuiLogLevel_Warn = 3,
    TuiLogLevel_Error = 4
} TuiLogLevel;

/* TuiLogAction — what a record's action row can offer.
 * An id, never a label: the UI owns the word ("Retry", "Details"), so the same record
 * renders in the user's language and a translation cannot desynchronise a callback. An
 * unknown id off the wire renders as `None` rather than as a guessed action.
 */
typedef enum TuiLogAction {
    TuiLogAction_None = 0,
    TuiLogAction_Retry = 1,
    TuiLogAction_Details = 2
} TuiLogAction;

/* TuiLogRecord — one structured log record.
 *
 * `message` is unformatted, user-facing and one line; `detail` is optional multi-line
 * context for the Raw Log. `module`/`function`/`file`/`line` are the developer fields: they
 * never reach the Activity tab (`model::projectForActivity()` has no field for them).
 *
 * `timestampMs` is **Unix-epoch milliseconds in UTC** - the unit both consumers assume, since
 * the archive formats it with `gmtime(ms / 1000)` and the Activity clock with
 * `localtime(ms / 1000)`. A sender must not pass a raw `SysTime.stdTime`/10_000 (that counts
 * from 1 A.D.). `timestampMs == 0` means "the sender had no clock" and the UI stamps the
 * arrival time.
 *
 * An out-of-range `level`/`action` is folded once, at the boundary (unknown level -> info,
 * unknown action -> none), before either the ring or the Activity projection sees it.
 */
typedef struct TuiLogRecord {
    int32_t level;        /* TuiLogLevel */
    int32_t action;       /* TuiLogAction; the UI renders the label */
    int64_t timestampMs;  /* Unix-epoch ms (UTC); 0 = UI stamps the arrival */
    int32_t line;         /* LogEntry.line, 0 = unknown */
    int32_t seq;          /* 0 = unstamped; else a per-process monotonic log-order
                             (capture-order) stamp from 1. A record the D log queue drops
                             consumes a stamp it never delivers, which turns a drop into a
                             gap the accept path can size. No wrap handling: past INT32_MAX
                             a wrap degrades into spurious gap markers, never corruption. */
    String message;       /* unformatted, user-facing, one line */
    String detail;        /* optional multi-line context for the Raw Log */
    String module;        /* LogEntry.moduleName */
    String function;      /* LogEntry.funcName */
    String file;          /* LogEntry.file */
    String actionPayload; /* turn/tool id for the action, optional */
} TuiLogRecord;

/* Append one structured record.
 *
 * The single bounded store is the C++ ring (5000 records / 2 MB, FIFO); a record that does
 * not fit evicts the oldest and increments the dropped counter the Raw Log renders, so
 * nothing is ever lost silently (NFR-17). `record` may not be NULL; the struct itself is
 * copied field by field, and every String in it is copied during this call.
 *
 * Null-safe: no-op if state or record is NULL.
 */
void tuiAddLogRecord(TuiState* state, const TuiLogRecord* record);

/* Append a record from two strings (the compatibility wrapper).
 *
 * Maps `summary` -> message, `text` -> detail, `level` -> level and leaves every metadata
 * field empty. It exists for the call sites that genuinely have no metadata; a site that
 * does should call tuiAddLogRecord and send the fields.
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiAddLogMessageEx(TuiState* state, TuiLogLevel level, String summary, String text);

/* Append a chat message to the scrollable output display area.
 *
 * The output area has a maximum capacity (1000 lines). When exceeded, the
 * oldest lines are evicted first (FIFO). The `param` fields are inbound
 * Strings — their data is copied internally, so the caller's buffers can be
 * freed or reused immediately after this call returns.
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiAddChatMessage(TuiState* state, ChatMessageParam param);

/* Clear all lines from the output display area.
 *
 * After this call the output area will be empty and self-consistent: any
 * open-group state (collapsed header groups) is reset too, so messages
 * appended afterwards — e.g. replayed after a session switch — render with
 * their header labels, and an in-flight stream message (see
 * tuiUpdateStreamChatMessage / tuiStreamChatMessageClear) is cleared and
 * stops being displayed. The auto-scroll flag is unaffected.
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiClearChatMessages(TuiState* state);

/* Update the message that display what the LLM is currently producing.
 *
 * Only one such message can be displayed on the assumption that the LLM can
 * only work on one message at a time.
 *
 * The summary and type field in param is ignored
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiUpdateStreamChatMessage(TuiState* state, ChatMessageParam param);

/* Clear the stream message and stop displaying it.
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiStreamChatMessageClear(TuiState* state);

/* Update the streaming message for a specific pipeline agent.
 * If agent is not yet registered, it is auto-registered as the latest
 * discovered agent. If MaxPipelineAgents is exceeded, the agent with
 * the oldest lastUpdate time is silently dropped.
 * Updates content, reasoning, role, finishReason, status and the
 * `currentTool`/`contextUsed`/`contextMax` fields for the agent identified by
 * agentId. All String members are inbound — their data is copied internally.
 * The lane row mirrors the numbers and never parses `status` (see
 * PipelineChatMessage); the sentence is an ABI field nothing draws.
 * Null-safe: no-op if state is NULL or agentId is empty.
 */
void tuiPipelineAgentUpdate(TuiState* state, String agentId, PipelineChatMessage msg);

/* Announce a tool call a pipeline agent made: the Agents tab draws it as a
 * row on the agent's transcript.
 * All String members are inbound — their data is copied internally.
 * Null-safe: no-op if state is NULL or agentId is empty.
 */
void tuiPipelineAgentToolCall(TuiState* state, String agentId, TuiToolCallParam call);

/* Announce a tool call's result: the Agents tab draws it under the call's
 * row. `ok` is the run's own success reading (1 = success, 0 = failure).
 * All String members are inbound — their data is copied internally.
 * Null-safe: no-op if state is NULL or agentId is empty.
 */
void tuiPipelineAgentToolResult(TuiState* state, String agentId, TuiToolResultParam result);

/* Mark a pipeline agent's current message as done.
 * Moves the current streaming content/reasoning/role to `finished`
 * and clears the streaming content. The lastUpdate timestamp is NOT
 * refreshed — done agents are evicted first (LRU policy) if capacity
 * is exceeded. This allows the TUI to display both the last finished
 * message and any new streaming content.
 * Null-safe: no-op if state is NULL or agentId is empty.
 */
void tuiPipelineAgentDone(TuiState* state, String agentId);

/* Clear all pipeline agent stream messages.
 * Called when the entire pipeline completes.
 * Null-safe: no-op if state is NULL.
 */
void tuiPipelineClear(TuiState* state);

/* TuiRuntimeStatus — the run's status as numbers.
 *
 * The contract is numbers, not a sentence: the UI formats them once per update and never
 * parses them back. `tokensPerSecond == 0` and `contextMax == 0` are the
 * *absence* values (the row draws `—`, not `0 tok/s` or `42k/0`), and an empty `model` is
 * "no name reported". `phase` is the ordinal of the UI's phase vocabulary
 * (`idle|thinking|tooling|writing|waiting|error|offline` = 0..6); an ordinal this build does not
 * know renders as idle, never as a neighbouring phase.
 *
 * `detail` is the current step (a tool name, a model step), one line. `compactAt` is the
 * threshold D's `needCompression()` compares `contextUsed` with. `elapsedMs` is the run's clock -
 * monotonic from the run's start, never process uptime. It is a sampled value: the UI shows it as
 * the settled duration and runs the frame's own clock while a run is in flight, so the row and the
 * top bar cannot disagree between two status updates.
 *
 * `model` is the model display name the chips show ("none" when D has no model configured).
 * It travels with the status because it is the status row's fourth fact - the UI has no other
 * source for it and must not parse one out of a sentence.
 *
 * Sender contract (D-STATUS): the tick stream stays at or below 20 Hz, with two documented
 * exceptions - one update when a turn starts doing work (so the row does not keep the previous
 * turn's phase through a slow first token) and one flush at a request boundary when the window
 * swallowed the newest values. Always send the final one when the turn ends. "Run" means the
 * user's turn, not one LLM request: a tool-calling turn is many requests, and an `idle` published
 * at a request boundary would be a lie for the rest of it - the flush carries the phase the last
 * tick had.
 */
typedef struct TuiRuntimeStatus {
    int32_t phase; /* 0 idle,1 thinking,2 tooling,3 writing,4 waiting,5 error,6 offline */
    String detail; /* current tool / model, one line; copied during the call */
    String model;  /* model display name ("none" when none); copied during the call */
    int64_t contextUsed, contextMax, compactAt;
    double tokensPerSecond;
    int64_t elapsedMs;
    /* Forward-compat field: transported, copied and round-trip-tested, but no D
     * sender sets it and no view reads it yet - it is the hook for the run's
     * pending question count. Keep it 0 until that consumer exists. */
    int32_t pendingQuestions;
    int32_t reserved; /* zero; keeps 8-byte alignment explicit */
} TuiRuntimeStatus;

/* Set the run's structured status, or clear it.
 *
 * Passing NULL clears it: the chips fall back to their defaults, which is also what a
 * session switch does, so a status can never keep describing a session the user left (§5.4).
 *
 * `phase == 6` (offline) is the one fact with a consequence beyond the row - it also drives the
 * thread's offline card and the composer's disabled state, so it is the single trigger for
 * "the model is unreachable".
 *
 * A clear is always honoured - so a status set before a session switch is still dropped rather
 * than kept describing a session the user left.
 *
 * Every String in the struct is copied during the call. Frame-thread only, like every
 * mutating entry point. Null-safe: no-op if state or status is NULL.
 */
void tuiSetRuntimeStatus(TuiState* state, const TuiRuntimeStatus* status);

/* Get the current content of the user's input buffer as an owned String.
 *
 * This returns whatever the user has typed into the input area so far,
 * regardless of whether they have pressed Enter. Use tuiIsSubmitReady()
 * and tuiGetSubmitQuery() to retrieve the query after submission instead.
 *
 * Returns an owned String — caller must free the result with String_Free().
 * Null-safe: returns {NULL, 0} if state is NULL.
 */
String tuiGetInput(TuiState* state);

/* Clear the user's input buffer.
 *
 * Empties the multiline input field. This does not affect the submission
 * flag or the stored submit query.
 *
 * Null-safe: returns 0 if state is NULL.
 */
int tuiIsSubmitReady(TuiState* state);

/* Reset the submission flag after processing a user's query.
 *
 * Submission occurs when the user presses Enter in the input area. This sets
 * the submission flag (checkable via tuiIsSubmitReady) and captures the
 * current input text into the submit query (retrievable via tuiGetSubmitQuery).
 *
 * After you have processed the query, call this function to reset the flag
 * so that the next Enter press will be detected. This also clears the
 * stored submit query text.
 *
 * Typical usage pattern:
 *
 *   if (tuiIsSubmitReady(state)) {
 *       String query = tuiGetSubmitQuery(state);
 *       // ... process query ...
 *       String_Free(query);
 *       tuiResetSubmit(state);   // acknowledge and clear for next input
 *   }
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiResetSubmit(TuiState* state);

/* Get the last submitted query text as an owned String.
 *
 * Returns a snapshot of the input text at the moment the user pressed Enter.
 * This is a read-only copy — modifying it does not affect the TUI state.
 * The query persists until tuiResetSubmit() is called.
 *
 * Null-safe: returns {NULL, 0} if state is NULL.
 * Caller must free the result with String_Free().
 */
String tuiGetSubmitQuery(TuiState* state);

/* Get the current auto-scroll setting.
 *
 * When auto-scroll is enabled, the output area automatically follows new
 * content as it is appended. Manual scrolling up disables auto-scroll.
 * Pressing the End key re-enables it. Returns 1 if enabled, 0 if disabled.
 *
 * Null-safe: returns 0 if state is NULL.
 */
int tuiGetAutoScroll(TuiState* state);

/* Set the auto-scroll flag.
 *
 * Pass 1 to enable, 0 to disable. When enabled, the output area automatically
 * scrolls to the bottom whenever new lines are appended via tuiAddOutputLine().
 * When disabled, the user's current scroll position is preserved.
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiSetAutoScroll(TuiState* state, int enabled);

/* Set the ready flag.
 *
 * Pass 1 to signal ready and 0 for busy.
 */
void tuiReadyStatus(TuiState* state, int ready);

/* Session sidebar */

/* Replace the session snapshot shown in the sidebar panel.
 *
 * `items` is an array of `count` SessionItem structs. The panel copies all
 * inbound strings during the call, so the caller's buffers may be freed or
 * reused immediately after this call returns (same contract as
 * tuiInitQueryHistory / tuiAddChatMessage).
 *
 * The active id is recomputed from the snapshot (the entry with
 * isActive != 0; at most one such entry is expected - the last one wins
 * defensively).
 *
 * Null-safe: no-op if state is NULL. `items` may be NULL when count == 0
 * (empty list); a NULL items pointer with count > 0 is treated as empty.
 */
void tuiSetSessionList(TuiState* state, const SessionItem* items, size_t count);

/* Check whether the sidebar has a pending session action.
 *
 * Pure check - does not consume anything. Returns 1 iff at least one
 * action is queued, 0 otherwise.
 *
 * Null-safe: returns 0 if state is NULL.
 */
int tuiIsSessionActionReady(TuiState* state);

/* Pop one pending session action from the front of the sidebar queue.
 *
 * Consume-on-read: each call returns exactly one action, or the None
 * sentinel when the queue is empty. The returned strings are owned
 * (malloc'd) and MUST be freed with String_Free() when no longer needed.
 * An empty sessionId/title field is returned as {NULL, 0}.
 *
 * Null-safe: returns {TuiSessionAction_None, {NULL, 0}, {NULL, 0}} if
 * state is NULL.
 */
SessionAction tuiGetSessionAction(TuiState* state);

/* Drop chips: the attachments a window drop queued and the row the composer
 * draws. One pop-one getter per queue and one by-id setter, all frame-thread
 * only (the composer's own click handling is the only other writer).
 *
 * The chip ids: append-only, monotonic, never reused; 0 is "none" and never a
 * real id. A report for an unknown or removed id is a silent no-op, so a late
 * reply can never resurrect a chip the user cancelled.
 */

/* Pop one queued dropped path.
 *
 * Consume-on-read: each call returns exactly one drop, or the {0, {NULL, 0}}
 * sentinel when the queue is empty. A multi-path drop arrives as several
 * consecutive pops, one per frame poll, never batched.
 *
 * `chipId` is the id of the Pending chip the drop created: report the chip
 * back through tuiSetChipStatus with this id.
 *
 * Ownership: `path` is owned (malloc'd) and MUST be freed with String_Free().
 * An empty queue answers {0, {NULL, 0}}.
 *
 * Null-safe: returns the sentinel if state is NULL.
 */
TuiDroppedPath tuiGetDroppedPath(TuiState* state);

/* Report one chip's outcome by id.
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiSetChipStatus(TuiState* state, int64_t chipId, int32_t status, String path, String display);

/* How many chips the row currently shows (the Send-time snapshot's count).
 *
 * Null-safe: returns 0 if state is NULL.
 */
size_t tuiGetChipCount(TuiState* state);

/* The chip at `index` of the row snapshot (creation order).
 *
 * An index past the end answers the {0, TuiChipStatus_Pending, {NULL, 0}}
 * sentinel. The path is the chip's workarea-relative path on Done and
 * FailedRaw; {NULL, 0} otherwise.
 *
 * Ownership: `path` is owned (malloc'd) and MUST be freed with String_Free().
 *
 * Null-safe: returns the sentinel if state is NULL.
 */
TuiChipParam tuiGetChipAt(TuiState* state, size_t index);

/* Empty the chip row: the Send handshake's clear, after the snapshot.
 *
 * The queues are untouched: a drop or a cancel that arrived between the
 * snapshot and this clear still drains afterwards.
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiClearChipRow(TuiState* state);

/* Pop one queued cancel click (a chip the user X-removed).
 *
 * Consume-on-read: each call returns exactly one id, oldest first, or 0 when
 * the queue is empty. Forward it to the worker as stop(chipId); an unknown id
 * is a no-op on the D side.
 *
 * Null-safe: returns 0 if state is NULL.
 */
int64_t tuiGetChipCancel(TuiState* state);

/* Run cancellation.
 *
 * The composer's Stop button (and Esc while a run is active)
 * asks the UI loop to stop the running agent. There is no agent cancel
 * API: the flag is consumed once per frame by the D UI loop, which calls the
 * same stopAgent() the typed "/stop" command uses (D-CANCEL), so partial
 * output is kept by the existing stop path rather than by a second one.
 *
 * Frame-thread only (no mutex): the flag is written and read on the thread
 * that drives the frame loop. Debug builds assert it.
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiRequestCancel(TuiState* state);

/* Take one pending cancel request.

 * Consume-on-read: returns 1 once per request and then 0 again. A second
 * request while one is still pending is a no-op, so a double click can never
 * queue two stops.
 *
 * Null-safe: returns 0 if state is NULL.
 */
int tuiTakeCancel(TuiState* state);

/* Raw Log access. */

/* Open or close the Raw Log surface.
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiSetRawLogOpen(TuiState* state, int open);

/* Is the Raw Log surface up?
 *
 * D polls this once per frame: the logger is raised to the
 * developer level only while the surface is open *and* the floor asks for trace, and the
 * previous level is restored on close and on quit.
 *
 * Null-safe: returns 0 if state is NULL.
 */
int tuiIsRawLogOpen(const TuiState* state);

/* The Raw Log's display floor severity.
 *
 * The user's `UiLog` preference itself (trace=0, info=2, warn=3, error=4).
 *
 * Null-safe: returns 0 (trace) if state is NULL.
 */
int tuiRawLogFloor(const TuiState* state);

#ifdef __cplusplus
}
#endif
