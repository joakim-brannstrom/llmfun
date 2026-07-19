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
 *       struct TuiScreen {}
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
 */

#define TUI_API_VERSION 1

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

typedef struct PipelineChatMessage {
    String content;
    String reasoning;
    String role;
    String finishReason;
} PipelineChatMessage;

/* TuiState — Opaque handle to the internal TUI state.
 * Created via tuiCreateState(), destroyed via tuiDestroyState().
 */
typedef struct TuiState TuiState;

/* TuiScreen — Opaque handle to the terminal screen.
 * Created via tuiInit(), destroyed via tuiShutdown().
 */
typedef struct TuiScreen TuiScreen;

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

/* Initialize the TUI terminal backend.
 *
 * Sets up the terminal (ncurses), creates the ImGui context, applies the
 * dark theme, and initializes the ImTui backend. After this call succeeds,
 * the terminal is in a controlled state and you must call tuiShutdown()
 * to restore it before the program exits.
 *
 * Must be called before any other API function (except String_New / String_NewBuf).
 * Returns an opaque TuiScreen* on success, NULL on failure (check tuiLastError).
 *
 * active will likely crash.
 */
TuiScreen* tuiInit(void);

/* Shutdown the TUI terminal backend and restore terminal state.
 *
 * Cleans up the ImTui backend, destroys the ImGui context, and restores
 * the terminal to its original state (ncurses end). After this call, the
 * TuiScreen* handle is invalid and must not be used again.
 *
 * Null-safe: passing NULL is a no-op.
 */
void tuiShutdown(TuiScreen* screen);

/* Create a new TUI state object.
 *
 * Allocates and initializes a TuiState instance containing empty output,
 * empty input buffer, disabled submission flag, and default auto-scroll.
 * The state is independent — you can create multiple states and pass them
 * to API functions to manage separate TUI sessions.
 *
 * Returns an opaque TuiState* on success, NULL on failure (e.g. out of memory).
 */
TuiState* tuiCreateState(void);

/* Destroy a TUI state object and free all associated memory.
 *
 * After this call the TuiState* handle is invalid and must not be used again.
 *
 * Null-safe: passing NULL is a no-op.
 */
void tuiDestroyState(TuiState* state);

/* Process backend input and start a new ImGui frame.
 *
 * This function encapsulates the three backend calls required to begin a
 * frame: reads terminal input (ncurses), initializes the text renderer,
 * and creates a new ImGui frame. Call this at the start of each iteration
 * of your main loop, before calling tuiRender().
 */
void tuiBackendNewFrame(void);

/* Render the current ImGui frame to the terminal screen.
 *
 * This function encapsulates the three backend calls required to end a
 * frame: renders the ImGui draw list, sends it to the text renderer,
 * and draws the result to the terminal. Call this at the end of each
 * iteration of your main loop, after calling tuiRender().
 *
 * Typical frame loop:
 *
 *   tuiBackendNewFrame();
 *   if (tuiRender(state) == 0) break;  // user requested exit
 *   // ... process input, update state ...
 *   tuiBackendRender(screen);
 */
void tuiBackendRender(TuiScreen* screen);

/* Render one TUI frame using the given state.
 *
 * Draws the three UI regions: the scrollable output area, the multiline
 * input field, and the status line. Handles keyboard shortcuts internally
 * (Ctrl+C/D to exit, Ctrl+L to clear output, End to scroll to bottom,
 * Escape to clear input, Ctrl+Up/Down for history navigation).
 *
 * Returns 0 if the user requested exit (pressed Ctrl+C, Ctrl+D, or Escape
 * in certain contexts), 1 otherwise. The caller should break the main loop
 * when this returns 0.
 *
 * Null-safe: returns 0 if state is NULL.
 */
int tuiRender(TuiState* state);

/* Set the logging to on/off. Must be done before tuiRender is called. */
void tuiSetLogging(TuiState* state, int onOff);

/* Set the filename that imgui save window settings to.
    By default it is in the "$cwd/imgui.ini".
    Call this **after** `tuiInit` but **before** your main loop starts calling `tuiBackendNewFrame`.
 */
void tuiSetIniFilename(TuiState* state, String filename);

/* Initialize the history in the input field */
void tuiInitQueryHistory(TuiState* state, const String* history, size_t count);

/* Append a line to the scrollable output display area.
 *
 * The output area has a maximum capacity (10000 lines). When exceeded, the
 * oldest lines are evicted first (FIFO). The `line` parameter is an inbound
 * String — its data is copied internally, so the caller's buffer can be
 * freed or reused immediately after this call returns.
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiAddLogMessage(TuiState* state, String summary, String text);

/* Append a chat message to the scrollable output display area.
 *
 * The output area has a maximum capacity (10000 lines). When exceeded, the
 * oldest lines are evicted first (FIFO). The `param` fields are inbound
 * Strings — their data is copied internally, so the caller's buffers can be
 * freed or reused immediately after this call returns.
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiAddChatMessage(TuiState* state, ChatMessageParam param);

/* Clear all lines from the output display area.
 *
 * After this call the output area will be empty. The auto-scroll flag is
 * unaffected.
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
 * Updates content, reasoning, role, and finishReason for the agent
 * identified by agentId. All String parameters are inbound — their data
 * is copied internally.
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

/* Set the text displayed in the status line at the bottom of the terminal.
 *
 * The status line is a single row below the input area, typically used for
 * context information (token counts, model name, status messages). The
 * `text` parameter is an inbound String — its data is copied internally.
 *
 * Null-safe: no-op if state is NULL.
 */
void tuiSetStatusText(TuiState* state, String text);

/* ---- Input ---- */

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

#ifdef __cplusplus
}
#endif
