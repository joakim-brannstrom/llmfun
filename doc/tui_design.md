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

---

## Internal C++ API

The internal C++ API (`tui.h` / `tui.cpp`) is used by `tui_api.cpp` and `main.cpp` (via the C API). All functions are in the `llmfun::tui` namespace.

See `tui.h` for the complete function declarations.

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
