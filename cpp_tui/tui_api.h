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
 * behavior depends on it (the contract tests pin the current value with
 * static_assert so a bump stays deliberate). Bump it (with a comment
 * listing the additions) whenever the C API surface grows.
 *
 * Version history:
 *   1 - original API
 *   2 - session sidebar: SessionItem, TuiSessionActionType, SessionAction,
 *       tuiSetSessionList, tuiIsSessionActionReady, tuiGetSessionAction
 *   3 - max width: tuiSetMaxWidth (cap the rendered width in columns;
 *       0 = unlimited). No signature or struct-layout changes to existing
 *       symbols.
 *   4 - single handle: TuiBackendMode/TuiBackendKind, tuiInit(state, mode),
 *       per-state frame calls, None-safe tuiRender, tuiBackendActive/Note,
 *       destroy-tears-down; TuiScreen + tuiShutdown removed.
 *   5 - MVP additive surface: tuiSetUiGeneration (1); tuiRequestCancel/
 *       tuiTakeCancel (3); TuiLogRecord/tuiAddLogRecord + tuiAddLogMessageEx
 *       wrapper (32); TuiRuntimeStatus/tuiSetRuntimeStatus (39);
 *       SessionItem.updatedAtMs/flags (24); tuiSetRawLogOpen/tuiIsRawLogOpen
 *       (33); PipelineChatMessage.currentTool/contextUsed/contextMax/reserved
 *       (39a). One bump for the whole workbench MVP: the later symbols land
 *       inside v5 without re-bumping (task 32 asserts the pin is still 5).
 *   6 - legacy renderer removed (task 41): TuiUiGeneration and
 *       tuiSetUiGeneration are gone, so a caller that still sets a generation
 *       fails to build instead of silently rendering nothing. The workbench is
 *       the only renderer; every other symbol is unchanged. This is the one
 *       deliberate *removal* in the header's history - the v5 note above still
 *       records what v5 added.
 */
#define TUI_API_VERSION 6

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
 *     tuiSetStatusText, etc.) are NOT copied; the caller must ensure the
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

/* ChatMessageParam — Bundles all parameters for a chat message.
 *
 * Layout (64-bit): 3x String{ptr(8) + size_t(8)} + enum(4) = 52 bytes
 * Each String is 16 bytes. Total struct size: 52 bytes (+ 4-byte tail padding).
 * All fields are naturally aligned.
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
    String summary;          /* offset 0,  size 16 */
    String text;             /* offset 16, size 16 */
    String thinking;         /* offset 32, size 16  (NEW) */
    TuiChatMessageType type; /* offset 48, size 4 */
} ChatMessageParam;          /* total size: 52 bytes (+ 4-byte tail padding) */

/* PipelineChatMessage — one streaming update for a pipeline agent lane (the Agents tab's
 * per-agent row, design §5.21).
 *
 * Ownership: every String is inbound (non-owning); `tuiPipelineAgentUpdate` copies their data
 * during the call, so the caller's buffers may be reused or freed as soon as it returns.
 *
 * Layout (64-bit): 6x String{ptr(8) + size_t(8)} + 3x int64(8) = 120 bytes, no tail padding.
 * All fields are naturally aligned.
 *
 * The last four fields are the v5 additions (task 39a). They are *appended*, so every earlier
 * offset is unchanged and a caller recompiled against this header that leaves them zero still
 * sends a valid update (no tool name, no context reading).
 *
 * `status` is the v4 status sentence. It is kept because removing it would move every offset
 * below it (the v6 note's "every other symbol is unchanged"), and nothing in C++ reads it since
 * task 41 removed the renderer that showed it: this UI reads the numbers below it and never
 * parses that text (D-STATUS: numbers are the contract).
 *
 *   * `currentTool` — the newest tool this agent has called in the response ("" = none yet);
 *   * `contextUsed` / `contextMax` — tokens in the context, and the model's context size. A
 *     `contextUsed` above 0 is drawn whatever the ceiling is; the ceiling is what makes a *zero*
 *     unambiguous for the sender: `0/8192` is a real zero, `0/0` is no reading at all.
 *     `contextUsed` alone cannot tell those apart, which is why the pair is sent instead of the
 *     count on its own.
 *   * `reserved` — zero. It is a slot a later producer can use instead of growing the struct
 *     again (it buys no alignment: 112 bytes is already 8-aligned).
 */
typedef struct PipelineChatMessage {
    String content;      /* offset 0,   size 16 */
    String reasoning;    /* offset 16,  size 16 */
    String role;         /* offset 32,  size 16 */
    String finishReason; /* offset 48,  size 16 */
    String status;       /* offset 64,  size 16 - the v4 sentence; unread since task 41 */
    String currentTool;  /* offset 80,  size 16 - newest tool of this response; "" = none yet */
    int64_t contextUsed; /* offset 96,  size 8  - tokens in context; 0 is a value */
    int64_t contextMax;  /* offset 104, size 8  - the model's ceiling; 0 = unknown. It is only
                          * needed to make a ZERO unambiguous (0/8192 = a real zero, 0/0 = no
                          * reading at all); a contextUsed above 0 is drawn either way */
    int64_t reserved;    /* offset 112, size 8  - zero; a slot for a later producer, no more */
} PipelineChatMessage;   /* total size: 120 bytes */

/* The size is a wire contract with `llm/tui/package.d`, which pins the same 120 bytes: a layout
 * change has to fail the build here, at the definition, in every TU that includes this header.
 * (The per-field offsets and the "the fields arrive unchanged" round-trip are pinned by
 * `./test_tui_api_v4` case 1f; headers are exempt from the `./test_metrics` gate ledger, so this
 * pin costs the ledger nothing.)
 */
#if defined(__cplusplus)
static_assert(sizeof(PipelineChatMessage) == 120, "PipelineChatMessage is a 120-byte contract");
#else
_Static_assert(sizeof(PipelineChatMessage) == 120, "PipelineChatMessage is a 120-byte contract");
#endif

/* SessionItem - One row of the session sidebar snapshot.
 *
 * Ownership: all Strings are inbound (non-owning). The panel copies their
 * data during tuiSetSessionList, so the caller's buffers may be reused or
 * freed immediately after the call returns.
 *
 * Layout (64-bit): 3x String{ptr(8) + size_t(8)} + size_t(8) + int(4) + 4 bytes
 *   of padding + int64(8) + 2x int32(4) = 80 bytes, no tail padding. All fields are
 *   naturally aligned; the padding after `isActive` is what lifts `updatedAtMs` to
 *   its 8-byte boundary.
 *
 * The three fields after `isActive` are the v5 additions (task 24). They are
 * *appended*, so every pre-v5 offset is unchanged and a caller that is
 * recompiled against this header and leaves them zero (a `{0}`-style
 * initializer) still produces a valid snapshot:
 * `updatedAtMs == 0` means "unknown", which the rail renders as a flat list
 * with no fabricated dates, and `flags == 0` means no row is marked running.
 *
 * That compatibility is **source-level**. `TUI_API_VERSION` is documentation
 * only (nothing checks it at run time) and D and C++ ship together (C-02, C-11),
 * so an object file compiled against the pre-v5 64-byte struct and linked
 * against this library would stride 64 bytes through an 80-byte array. "Append
 * only" means no *recompiled* caller changes behaviour; it is not a promise that
 * a stale binary keeps working.
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
    String id;           /* offset 0,  size 16 - immutable session id */
    String title;        /* offset 16, size 16 */
    String preview;      /* offset 32, size 16 - first user message, truncated by D */
    size_t messageCount; /* offset 48, size 8 */
    int isActive;        /* offset 56, size 4 - 1 = active, 0 = not */
    int64_t updatedAtMs; /* offset 64, size 8 - unix ms; 0 = unknown */
    int32_t flags;       /* offset 72, size 4 - TuiSessionFlag_* bits, 0 = none */
    int32_t reserved;    /* offset 76, size 4 - must be 0 (keeps the size 80) */
} SessionItem;           /* total size: 80 bytes */

/* SessionItem.flags - what the session snapshot may say about a row.
 *
 * Only `Running` crosses the ABI. `unread` and `pinned` are UI-local by design
 * (FR-03, D-19): unread is "updatedAt advanced while this session was not
 * active during this process lifetime" and pinned is a `ui.conf` preference, so
 * neither is a fact the store has. Bits above 0 are reserved for later
 * additive flags and must be zero, exactly like `reserved`.
 */
#define TuiSessionFlag_Running 1 /* bit 0: a run is in flight for this session */

/* TuiSessionActionType - Kind of a session sidebar action.
 * Append-only ordering: existing values must never be renumbered or
 * reused, so future actions (Fork, Export, Archive, Search, ...) can be
 * added without breaking the D-side mapping.
 */
typedef enum TuiSessionActionType {
    TuiSessionAction_None = 0,   /* sentinel - no action (empty queue) */
    TuiSessionAction_Select = 1, /* switch to the session */
    TuiSessionAction_New = 2,    /* create a new session */
    TuiSessionAction_Rename = 3, /* rename the session (title payload) */
    TuiSessionAction_Delete = 4  /* delete the session (the UI never re-asks) */
} TuiSessionActionType;

/* SessionAction - One queued sidebar action, consumed by tuiGetSessionAction.
 *
 * Ownership: the Strings returned by tuiGetSessionAction are owned
 * (malloc'd) and MUST be freed with String_Free() when no longer needed.
 * An empty field is returned as {NULL, 0}.
 *
 * Layout (64-bit): enum(4) + 4-byte padding + 2x String{ptr(8) + size_t(8)}
 *   = 40 bytes. All fields are naturally aligned.
 *
 * Fields:
 *   sessionId - target session id; empty ({NULL, 0}) for New.
 *   title     - new title for Rename; empty for all other types.
 */
typedef struct SessionAction {
    TuiSessionActionType type; /* offset 0,  size 4 */
    String sessionId;          /* offset 8,  size 16 */
    String title;              /* offset 24, size 16 */
} SessionAction;               /* total size: 40 bytes */

/* TuiState — Opaque handle to the internal TUI state; the single lifecycle
 * object of the v4 API.
 * Created via tuiCreateState(), a backend is attached via tuiInit(), and the
 * whole state is torn down via tuiDestroyState().
 */
typedef struct TuiState TuiState;

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
 * None-safety (deliberate asymmetry): tuiRender(NULL) returns 0 (the v3 exit
 * convention), while a valid state with no backend attached is inert and
 * returns 1 (continue). No ImGui access happens in either case.
 */
int tuiRender(TuiState* state);

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

/* Set the file imgui persists its settings to (io.IniFilename); the GUI
    backend stores the window size in settingsPath(filename) (imgui2.ini -> imgui2.conf).
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

/* Structured log records (v5, task 32, design §5.2 / §6.4)
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
    int32_t reserved;     /* zero; keeps the struct layout stable */
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
 * Updates content, reasoning, role, finishReason, status and the v5
 * `currentTool`/`contextUsed`/`contextMax` fields for the agent identified by
 * agentId. All String members are inbound — their data is copied internally.
 * The lane row mirrors the numbers and never parses `status` (see
 * PipelineChatMessage); the sentence is an ABI field nothing draws.
 * Null-safe: no-op if state is NULL or agentId is empty.
 */
void tuiPipelineAgentUpdate(TuiState* state, String agentId, PipelineChatMessage msg);

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

/* TuiRuntimeStatus — the run's status as numbers (v5, task 39 / D-STATUS).
 *
 * The contract is numbers, not a sentence: the UI formats them once per update and never
 * parses them back (design §6.3). The structured path and the status-sentence fallback agree
 * chip for chip on the numbers both can express; where they cannot, the structured path is the
 * stricter one and says so - `tokensPerSecond == 0` and `contextMax == 0` are the *absence*
 * values (the row draws `—`, not `0 tok/s` or `42k/0`), while the sentence renders whatever it
 * was given. `phase` is the ordinal of the UI's phase vocabulary
 * (`idle|thinking|tooling|writing|waiting|error|offline` = 0..6); an ordinal this build does not
 * know renders as idle, never as a neighbouring phase.
 *
 * `detail` is the current step (a tool name, a model step), one line. `compactAt` is the
 * threshold D's `needCompression()` compares `contextUsed` with. `elapsedMs` is the run's clock -
 * monotonic from the run's start, never process uptime. It is a sampled value: the UI shows it as
 * the settled duration and runs the frame's own clock while a run is in flight, so the row and the
 * top bar cannot disagree between two status updates.
 *
 * The struct carries no model name: a model changes when the user changes it, not per status
 * tick, so that one chip keeps coming from the status sentence.
 *
 * Sender contract (D-STATUS): the tick stream stays at or below 20 Hz, with two documented
 * exceptions - one update when a turn starts doing work (so the row does not keep the previous
 * turn's phase through a slow first token) and one flush at a request boundary when the window
 * swallowed the newest values. Always send the final one when the turn ends. "Run" means the
 * user's turn, not one LLM request: a tool-calling turn is many requests, and an `idle` published
 * at a request boundary would be a lie for the rest of it - the flush carries the phase the last
 * tick had. A sender that has not been converted keeps working - the sentence stays the fallback.
 */
typedef struct TuiRuntimeStatus {
    int32_t phase; /* 0 idle,1 thinking,2 tooling,3 writing,4 waiting,5 error,6 offline */
    String detail; /* current tool / model, one line; copied during the call */
    int64_t contextUsed, contextMax, compactAt;
    double tokensPerSecond;
    int64_t elapsedMs;
    int32_t pendingQuestions;
    int32_t reserved; /* zero; keeps 8-byte alignment explicit */
} TuiRuntimeStatus;

/* Set the run's structured status, or hand the chips back to the status sentence.
 *
 * Passing NULL clears it: the sentence splitter owns the chips again, which is also what a
 * session switch does, so a status can never keep describing a session the user left (§5.4).
 *
 * `phase == 6` (offline) is the one fact the sentence cannot express - it also drives the
 * thread's offline card and the composer's disabled state, so it is the single trigger for
 * "the model is unreachable".
 *
 * A state that will never draw the numbers must not claim a structured status, and a clear is
 * always honoured - so a status set before a session switch is still dropped rather than kept
 * describing a session the user left.
 *
 * Every String in the struct is copied during the call. Frame-thread only, like every
 * mutating entry point. Null-safe: no-op if state or status is NULL.
 */
void tuiSetRuntimeStatus(TuiState* state, const TuiRuntimeStatus* status);

/* Set the text displayed in the status line at the bottom of the terminal.
 *
 * The status line is a single row below the input area, typically used for
 * context information (token counts, model name, status messages). The
 * `text` parameter is an inbound String — its data is copied internally.
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiSetStatusText(TuiState* state, String text);

/* Input */

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

/* Reports whether the frame we just rendered consumed at least one input
 * event (mouse, key, ...). If so, more input is likely to arrive: the next
 * frame should be rendered immediately instead of waiting for the refresh
 * interval.
 *
 * Returns 1 if the last rendered frame consumed input, 0 otherwise.
 * */
int tuiHasMoreEvents();

/* Run cancellation (MVP, task 3).

 * The composer's Stop button (and, from task 11, Esc while a run is active)
 * asks the UI loop to stop the running agent. The MVP adds no agent cancel
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

/* Raw Log access (v5, task 33). */

/* Open or close the Raw Log surface.
 *
 * `open` is 1 to open and 0 to close. Opening is what `--debug-ui` does before the first frame,
 * so the overlay is up on the frame the window appears; the state is never persisted, so every
 * start is closed (D-RAWLOG) and a restart cannot reopen it.
 *
 * This is deliberately the *only* bridge the access paths need: `/rawlog`, the palette's
 * Developer row and `Ctrl+Shift+L` are all inside the UI (they write the same flag), and the
 * logger policy (raise on open, restore on close) is driven by polling `tuiIsRawLogOpen()` once
 * per frame. The call does not touch the log ring and does not enable the archive: the surface
 * is a view (R-08).
 *
 * Frame-thread only, like every mutating entry point.
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiSetRawLogOpen(TuiState* state, int open);

/* Is the Raw Log surface up?
 *
 * D polls this once per frame to raise the in-process logger to the developer level while the
 * surface is open and to restore the previous level on close **and** on quit (D-RAWLOG). It
 * reflects the flag the UI's own access paths write, so a `/rawlog` in the composer is visible
 * to the very next poll.
 *
 * Null-safe: returns 0 if state is NULL.
 */
int tuiIsRawLogOpen(const TuiState* state);

#ifdef __cplusplus
}
#endif
