# TUI System — Implementation Description

## Overview

The llmfun TUI is a terminal-based user interface built in C++17 on top of the **imtui** library (a terminal-based ImGui wrapper at `llmfun/vendor/imtui`). It provides a full-screen chat interface for interacting with an LLM: a scrollable chat output area with typed/color-coded messages, a multiline input area, and a status line. The TUI is self-contained in the `llmfun/cpp_tui/` directory.

 A pure C API layer (`tui_api.h` / `tui_api.cpp`) wraps the internal C++ implementation, enabling D to link against the TUI without C++ name mangling. D imports `tui_api.h` directly.

## File Structure

```
llmfun/cpp_tui/
├── CMakeLists.txt   # Build configuration (CMake 3.10+, C++17)
├── main.cpp         # Lightweight test/dry-run for the TUI (not the main application entry point)
├── tui.h            # Internal C++ structs and API declarations
├── tui.cpp          # All TUI logic: render, theme, init/shutdown, data feeds
├── tui_api.h        # Pure C API header — entry point for D code (extern "C" linkage)
└── tui_api.cpp      # C++ implementation of C API, bridges to tui.h/tui.cpp
```

### D Bindings

```
llmfun/source/llm/tui/
└── package.d        # D module llm.tui, imports llmfun_tui, provides helpers
```

## Architecture

### Three-Layer Design

```
┌──────────────────────────────────────────────────────────────────┐
│                         D Side (package.d)                       │
│                                                                  │
│  module llm.tui;                                                 │
│  import llmfun_tui;   ← links against llmfun_tui_lib            │
│                                                                  │
│  Main loop: tuiInit → tuiCreateState → tuiBackendNewFrame →     │
│             tuiRender → tuiBackendRender → ... → tuiShutdown     │
│                                                                  │
├──────────────────────────────────────────────────────────────────┤
│              C API Boundary (tui_api.h) — extern "C"             │
│                                                                  │
│  C header with extern "C" linkage:                               │
│    String (POD struct, explicit ownership)                        │
│    ChatMessageParam (bundles summary, text, thinking, type)       │
│    TuiState* (opaque handle)                                      │
│    TuiScreen* (opaque handle)                                     │
│    All functions: pointers only, String by value, null-safe       │
│    Error reporting: tuiLastError()                                │
│                                                                  │
├──────────────────────────────────────────────────────────────────┤
│              C++ Implementation (tui_api.cpp)                    │
│                                                                  │
│  C++ implementation of the C API:                                │
│    - Wraps internal C++ TuiState (::llmfun::tui::TuiState)       │
│    - Implements String_New/String_Free with malloc/free          │
│    - Implements error handling with thread-local storage         │
│    - Calls existing C++ functions (tuiAddChatMessage, etc.)      │
│    - All functions declared with extern "C" linkage              │
│    - Backend init guard prevents calls before tuiInit()          │
│                                                                  │
├──────────────────────────────────────────────────────────────────┤
│              C++ Core (tui.h / tui.cpp)                          │
│                                                                  │
│  TuiState (full struct, hidden from D)                           │
│  Internal std::string usage (implementation detail)               │
│  ImGui / ImTui integration                                        │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### Three-Region Layout (Chat Tab)

The terminal is divided vertically into three fixed-position regions for the chat view:

```
┌──────────────────────────────────────────────┐  ← y=0
│                                              │
│           CHAT OUTPUT DISPLAY AREA           │
│         (scrollable child window)            │
│                                              │
│                                              │
│                                              │
├──────────────────────────────────────────────┤  ← y=H-3
│  > User input line 1                         │
│    User input line 2 (multiline)             │
├──────────────────────────────────────────────┤  ← y=H-1
│  Context: 0/0 tokens | Model: none | Ready   │
└──────────────────────────────────────────────┘  ← y=H
```

All windows use `ImGuiCond_Always` for stable positioning. Minimum terminal size is **40 columns × 15 rows**; below this, an error message is rendered instead of the normal UI.

### Chat Message Types

The TUI supports seven message types, each with distinct color coding. See `ChatMessageType` enum in `tui.h` and `TuiChatMessageType` enum in `tui_api.h`.

 Messages use three distinct color bands for quick visual scanning:
 - **Input** (blue tones): User queries and vision messages
 - **Work trail** (warm tones): Tool calls, tool responses, and system messages showing LLM reasoning
 - **Output** (green tones): Assistant responses and final answers


Messages can contain collapsible **thinking/reasoning** content (the `thinking` field in `ChatMessageParam`), displayed as expandable sections within the message.

### Markdown Support

The TUI includes `imgui_markdown` in the header (`tui.h` includes `imgui_markdown.h`), but markdown rendering is **currently turned off** because the library lacks support for fence blocks (code blocks with triple backticks). The `mdConfig` field in `TuiState` is reserved for when this is re-enabled.

### State Management

All TUI state is encapsulated in a single `TuiState` struct. See `tui.h` for the full definition.

Key design decisions:
- **`std::deque`** for `outputLines` (chat messages) and `logMessages` for O(1) FIFO eviction.
- **Chat message types** with color coding for visual distinction of message roles.
- **Thinking content** stored separately and rendered as collapsible sections.
- **Render groups** (`RenderGroup`) for efficient batched rendering of related messages (UserQuery, FinalAnswer, AssistantWork).
- **`draftBuf`** preserves input when navigating history.
- **Log tab** provides separate view for system log messages.
 - **Bounded storage**: All collections have hard limits to prevent unbounded growth. Chat messages, log messages, and input history are capped (see `MaxChatMessages`, `MaxLogMessages`, and `MAX_HISTORY` in `tui.h`).

---

## C API Layer

 The C API (`tui_api.h` \/ `tui_api.cpp`) provides a language-agnostic interface. D imports it directly (ImportC) — no `extern(C++)` name mangling, no module declarations.


 ### Why a Pure C API?

 D can directly import C header files and use their functions and data structures natively. By providing a pure C API:
 - D imports the C header directly without any shim layer or `extern(C)` declarations.
 - The `String` struct (pointer + length) maps naturally to D's string slice type.


### String Type

A plain old data (POD) struct representing a string slice:

```c
typedef struct String {
    const char* data;
    size_t len;
} String;
```

**Why this works**: D's native string type is essentially `const(char)*` with a `.length` property. The `String` struct maps directly to this layout, allowing zero-copy passing of string data between D and C++.

**Ownership rules**:
- **Inbound (D → C++)**: D constructs `String` from a local string slice. The data is copied internally by the C++ functions, so the caller's buffer must live long enough for the call to complete.
- **Outbound (C++ → D)**: C++ allocates via `String_New()` / `String_NewBuf()` (using `malloc`). D must call `String_Free()` (which calls `free`) when done.

**String functions**: See `tui_api.h` for full declarations.
- `String String_New(const char* cstr)` — allocates from null-terminated C string (copies data).
- `String String_NewBuf(const char* data, size_t len)` — allocates from raw buffer (copies data).
- `void String_Free(String s)` — frees an owned String. No-op if `data` is null.

**Safety features**:
- `String_NewBuf` allocates `len + 1` bytes to guarantee null-termination for safe C-string interop.
- `String_NewBuf` returns `{NULL, 0}` for zero-length input.
- Memory allocator: all strings allocated via `malloc` (C standard library). Must be freed via `String_Free()` which uses `free()`.

### Opaque Handles

Two opaque handle types hide internal C++ types from D:

```c
typedef struct TuiState TuiState;     // Wraps ::llmfun::tui::TuiState*
typedef struct TuiScreen TuiScreen;   // Wraps ImTui::TScreen*
```

### Error Handling

All fallible functions report errors via a thread-local mechanism:

```c
String tuiLastError(void);
```

Returns an owned `String` with the last error message. Thread-local: each thread gets its own error. The error is **consumed** (cleared) on the first call. Returns `{NULL, 0}` if no error was set. Caller must free the result with `String_Free()`.

 ### Threading Model

 The TUI is driven from a single thread (the main/UI thread). All API functions must be called from this thread. No mutexes or locks protect the TUI state.


### API Reference

See `tui_api.h` for the complete C API. The header is self-documented with detailed comments for each function.

### Session API (Phase 2)

The session sidebar added a second API family to `tui_api.h`, and bumped
`TUI_API_VERSION` from 1 to 2. The version macro is a **documentation marker
only** — nothing consumes it at compile time or runtime; the header comment
lists the additions.

New types:

```c
typedef struct SessionItem {
    String id;           /* immutable session id */
    String title;        /* human-readable title */
    String preview;      /* first user message, truncated by D */
    size_t messageCount; /* total entries in the session file */
    int isActive;        /* 1 = active session, 0 = not */
} SessionItem;

typedef enum TuiSessionActionType {
    TuiSessionAction_None = 0,   /* sentinel - no action (empty queue) */
    TuiSessionAction_Select = 1, /* switch to the session */
    TuiSessionAction_New = 2,    /* create a new session */
    TuiSessionAction_Rename = 3, /* rename the session (title payload) */
    TuiSessionAction_Delete = 4  /* delete the session (already confirmed) */
} TuiSessionActionType;

typedef struct SessionAction {
    TuiSessionActionType type; /* offset 0,  size 4 */
    String sessionId;          /* offset 8,  size 16 - target session id; empty for New */
    String title;              /* offset 24, size 16 - new title for Rename; empty otherwise */
} SessionAction;               /* total size: 40 bytes */
```

`TuiSessionActionType` is append-only: existing values are never renumbered
or reused, so future actions (Fork, Export, Archive, Search) extend it without
breaking the D mapping. `tui_api.cpp` has compile-time `static_assert`s tying
the C enum to the internal C++ mirror (`SessionActionType` in `tui.h`).

New functions:

| Function | Semantics |
|----------|-----------|
| `tuiSetSessionList(TuiState*, const SessionItem*, size_t)` | Full replace of the panel snapshot. All inbound strings are copied into `std::string` during the call (caller buffers may be reused/freed immediately). Recomputes the active id (the entry with `isActive != 0`; at most one expected, last wins defensively). Clears the panel's two-step delete confirmation when its id is absent from the new snapshot. Null-safe; `items == NULL` with `count == 0` is an empty list |
| `tuiIsSessionActionReady(TuiState*)` | Pure check: 1 iff at least one action is queued, 0 otherwise. Consumes nothing. Null-safe (0) |
| `tuiGetSessionAction(TuiState*)` | Pops exactly one action from the front of the queue (consume-on-read). Returned strings are malloc'd (via `String_NewBuf`) and MUST be freed with `String_Free`; empty fields are `{NULL, 0}`. Empty queue returns `{TuiSessionAction_None, {NULL, 0}, {NULL, 0}}`. Null-safe |

The ownership contract mirrors `String` exactly: `SessionItem` strings are
inbound (non-owning, copied during the call), `SessionAction` strings are
outbound (owned, `String_Free`). See the header for byte-level layout
comments.

---

## Internal C++ API

The internal C++ API (`tui.h` / `tui.cpp`) is used by `tui_api.cpp` and `main.cpp` (via the C API). All functions are in the `llmfun::tui` namespace.

See `tui.h` for the complete function declarations.

## Session Sidebar (Phase 2)

The session sidebar is a left panel in the chat tab that lists all chat
sessions (title, message count, preview) and offers switch / new / rename /
delete. It follows the same three-layer pattern as the query input: the C++
panel owns all UI state and queues actions; the D UI thread polls the queue
once per frame and forwards one action to the agent thread; the agent thread
runs the Phase 1 session methods.

### ChatTabSessionPanel

`ChatTabSessionPanel` (`tui.h`) holds the panel state:

```cpp
struct ChatTabSessionPanel {
    ImVec4 activeButton = ImVec4(0.4f, 0.4f, 0.45f, 1.0f); // highlight color
    int panelW = 0;                    // 0 = unset; init to PanelWActivated
                                       // on first render (F5)
    static constexpr int PanelWActivated = 30;
    bool panelOpen{true};              // auto-open at startup

    std::vector<SessionEntry> sessions; // full snapshot (A1)
    std::string activeId;               // active session id from the snapshot
    std::deque<SessionAction> actions;  // UI -> D queue (A2/A7)

    char renameBuf[128] = {};   // rename input; init on row change or
                                // toggle-open, never per frame
    bool renameActive{false};   // rename input visible (L8)
    std::string renameRowId;    // row renameBuf was initialized for
    bool renameFocus{false};    // focus the rename input next frame
    int renameSeq{0};           // bumped per open; fresh InputText id
    std::string pendingDeleteId; // two-step delete state (A5)
};
```

`SessionEntry` is one snapshot row (`id`, `title`, `preview`,
`messageCount`, `isActive`); the internal `SessionAction` mirrors the C
`SessionAction` (type + sessionId + title).

### Mutual Exclusion with the Pipeline Panel

The chat tab has exactly **one left-panel slot**. The pipeline panel
(`ChatTabLeftPanel`) renders whenever it has agents — open or collapsed —
so it always wins the slot while agents are present. The session panel
renders only when the pipeline is empty; its state is preserved, so it
reappears unchanged when the pipeline clears. `renderTabChatSessionPanel`
starts with the early return `if (!state.left.agents.empty()) return;`
(no overlap, R6/A6/H1).

The output area offsets by the resolved width, kept in one place:

```cpp
int leftPanelWidth(const TuiState& s) {
    return !s.left.agents.empty() ? s.left.panelW
                                  : (s.sessionPanel.panelOpen ? s.sessionPanel.panelW : 8);
}
```

Session panel open = 30 columns, collapsed = an 8-column "Open" strip (so the
output area never covers the Open button), pipeline present = the pipeline
panel's own width. `renderTabChat` calls `renderTabChatSessionPanel` before
`renderTabChatLeftPanel`, and the `outputArea` lambda offsets by
`leftPanelWidth(state)`.

### Panel Behavior

- **First open**: `panelW == 0` is initialized to `PanelWActivated` (mirrors
  `renderTabChatLeftPanel`), so the first frame never offsets the output area
  by 0 (F5).
- **Collapse**: the "Close" button clears the pending delete and rename state
  and sets `panelW = 8`; the collapsed strip shows an "Open" button that
  restores `PanelWActivated`.
- **Rows**: one row per snapshot entry via the shared `renderButton` helper;
  label = title truncated to the row width with a UTF-8-safe ellipsis plus the
  always-kept ` [N]` message count (`sessionRowLabel`); the active row is
  highlighted; a tooltip shows the full title and preview. Clicking a row
  queues `{Select, id}` unless it is already active (a click on the active
  row queues nothing).
- **New**: queues `{New}`.
- **Rename**: a "Rename" toggle on the active row reveals the `InputText`
  (the toggle avoids an always-present tab-focus stop — imtui tab navigation
  does not reach plain buttons). The buffer is initialized from the current
  title only on row change or toggle-open, never per frame (L3); a title
  longer than the 128-byte buffer initializes the buffer empty, so a blind
  Enter is rejected as empty — no silent truncation (L4). Enter queues
  `{Rename, activeId, typedTitle}` (empty/whitespace-only titles rejected
  in-panel), Escape cancels.
- **Delete**: each row has a `del` button; the first press arms the row
  (`del?`), a second press on the same row queues `{Delete, id}` and clears
  the arm. Pressing another row's delete moves the pending target (L1); any
  non-delete control clears it.
- **Busy gating**: when `!state.readyStatus`, every interactive widget is
  guarded so no action is queued (guard-and-skip only — the vendored ImGui
  1.81 has no `BeginDisabled`). A click already in flight when the busy state
  flips is processed between queries (the mailbox race); see
  `doc/sessions.md` for the observable late-click effect.
- **Scrolling**: the panel child window has no vertical scrollbar yet; rows
  below the terminal height are unreachable until Phase 3.

Sidebar interactions are logged through the shared `Log& log` parameter
(`session panel: ...` lines in the Log tab).

---

## Main Event Loop (`main.cpp`)

The `main.cpp` file provides a lightweight test/dry-run for the TUI (not the main application entry point). It follows a standard ImGui frame loop:

1. **Initialization**: Call `tuiInit()`, create state via `tuiCreateState()`, set initial status text and welcome message.
2. **Frame loop**:
   - `tuiBackendNewFrame()` — processes backend input and starts new ImGui frame
   - `tuiRender(state)` — renders all three regions, handles keyboard shortcuts. Returns `0` to exit.
   - **Submission check**: If `tuiIsSubmitReady(state)`, extract the query via `tuiGetSubmitQuery()`, echo it to output, and reset submit flag.
   - `tuiBackendRender(screen)` — renders the ImGui frame to the terminal screen
3. **Shutdown**: Call `tuiDestroyState(state)` and `tuiShutdown(screen)` on exit.

### Headless Smoke Mode

`main.cpp` accepts `--frames N` (or `--smoke`, an alias for 30 frames) for
headless CI runs: it seeds the session panel with a sample
`tuiSetSessionList` snapshot, runs the normal frame loop for exactly N
frames, verifies the session action queue is empty (`tuiIsSessionActionReady`
== 0 and `tuiGetSessionAction` returns the None sentinel), prints
`smoke ok: ...`, and exits 0. Without the argument the interactive loop is
unchanged. `--frames` requires a non-negative integer; usage errors exit 2.

### Keyboard Shortcuts

| Shortcut | Action | Condition |
|----------|--------|-----------|
| `Ctrl+C` | Exit TUI (return `false`) | Anywhere |
| `Ctrl+D` | Exit TUI (return `false`) | Anywhere |
| `Ctrl+L` | Clear output area | Only when no widget has focus |
| `End` | Scroll to bottom, re-enable auto-scroll | Anywhere |
| `Escape` | Clear input buffer | Input widget active |
 | `Ctrl+Up` | Navigate backward in input history (does not work) | Input widget active |
 | `Ctrl+Down` | Navigate forward in input history (does not work) | Input widget active |

Note: History navigation uses `Ctrl+Up`/`Ctrl+Down` instead of plain `Up`/`Down` to avoid conflicting with `InputTextMultiline`'s internal cursor movement.

### Output Area

- **Auto-scroll**: Automatically follows new content when `autoScroll` is `true`. Manual scroll (scrolling up) disables auto-scroll. Pressing `End` re-enables it.

### Input Area

- **History navigation**:
  - On first `Ctrl+Up`: saves current input to `draftBuf`, pushes it to `inputHistory` (if not duplicate of last entry), then navigates to the entry before it.
  - `Ctrl+Down` walks forward; past the end restores `draftBuf` and resets `historyPos` to -1.
  - On submission: pushes input to history if non-empty, not a duplicate, and not currently in history navigation (`historyPos == -1`).
  - History is bounded to `MAX_HISTORY` (500) entries with FIFO eviction.

### Status Line

- A child window with all decorations disabled (`NoCollapse`, `NoResize`, `NoMove`, `NoTitleBar`, `NoScrollbar`, `NoScrollWithMouse`).
- Falls back to a default status string (`"Context: 0/0 tokens | Model: none | Ready"`) if `statusText` is empty.
