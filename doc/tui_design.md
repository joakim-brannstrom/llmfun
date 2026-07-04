# TUI System — Implementation Description

## Overview

The llmfun TUI is a terminal-based user interface built in C++17 on top of the **imtui** library (a terminal-based ImGui wrapper at `llmfun/vendor/imtui`). It provides a full-screen, three-region layout for interacting with an LLM: a scrollable output area, a multiline input area, and a status line. The TUI is self-contained in the `llmfun/cpp_tui/` directory with six source files.

A pure C API layer (`tui_api.h` / `tui_api.cpp`) wraps the internal C++ implementation, enabling D (and other languages) to link against the TUI without C++ name mangling or ABI issues. D imports the C header directly via `extern(C)`.

## File Structure

```
llmfun/cpp_tui/
├── CMakeLists.txt   # Build configuration (CMake 3.10+, C++17)
├── main.cpp         # Entry point, uses C API (~60 lines)
├── tui.h            # Internal C++ TuiState struct and API declarations (~79 lines)
├── tui.cpp          # All TUI logic: render, theme, init/shutdown, data feeds (~302 lines)
├── tui_api.h        # Pure C API header with extern "C" linkage (~270 lines)
└── tui_api.cpp      # C++ implementation of C API, bridges to tui.h/tui.cpp (~293 lines)
```

### D Bindings

```
llmfun/source/llm/tui/
└── package.d        # D module llm.tui, imports llmfun_tui (~3 lines)
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
│    TuiState* (opaque handle, wraps ::llmfun::tui::TuiState*)     │
│    TuiScreen* (opaque handle, wraps ImTui::TScreen*)             │
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
│    - Calls existing C++ functions (tuiAddOutputLine, etc.)       │
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

### Three-Region Layout

The terminal is divided vertically into three fixed-position regions:

```
┌──────────────────────────────────────────────┐  ← y=0
│                                              │
│           OUTPUT DISPLAY AREA                │
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

| Region | Position | Height | ImGui Widget |
|--------|----------|--------|--------------|
| Output area | `(0, 0)` | `DisplaySize.y - 3` | `BeginChild("output", ...)` with `HorizontalScrollbar` |
| Input area | `(0, H-3)` | `2` rows | `BeginChild("input", ...)` containing `InputTextMultiline` |
| Status line | `(0, H-1)` | `1` row | `BeginChild("status", ...)` with all decorations disabled |

All windows use `ImGuiCond_Always` for stable positioning. Minimum terminal size is **40 columns × 15 rows**; below this, an error message is rendered instead of the normal UI.

### State Management

All TUI state is encapsulated in a single `TuiState` struct (non-copyable, non-movable due to `std::mutex`):

```cpp
struct TuiState {
    std::deque<std::string> outputLines;   // Bounded output (deque for O(1) FIFO eviction)
    static constexpr size_t MAX_OUTPUT_LINES = 10000;

    bool autoScroll = true;                // Auto-scroll flag (main-thread-only)

    std::string inputBuf;                  // Dynamic input buffer
    std::string draftBuf;                  // Saved input when navigating history

    bool submitReady = false;              // Submission flag
    std::string submitQuery;               // Captured query on Enter (read-only snapshot)

    std::vector<std::string> inputHistory; // Input history
    int historyPos = -1;                   // History navigation cursor
    static constexpr size_t MAX_HISTORY = 500;

    std::string statusText;                // Status line text

    mutable std::mutex outputMutex;        // Protects: outputLines, statusText, inputBuf, submitReady, submitQuery
};
```

Key design decisions:
- **`std::deque`** for `outputLines` for O(1) FIFO eviction.
- **`draftBuf`** field added beyond the original plan to preserve the current input when entering history navigation, allowing full restore when navigating back past the newest history entry.
- **Mutex scope**: Only `outputLines`, `statusText`, `inputBuf`, `submitReady`, and `submitQuery` are mutex-protected. `autoScroll`, `historyPos`, `draftBuf`, and `inputHistory` are main-thread-only and unlocked for performance.

---

## C API Layer

The C API (`tui_api.h` / `tui_api.cpp`) provides a language-agnostic interface. D imports it directly via `extern(C)` — no `extern(C++)` name mangling, no module declarations.

### String Type

A plain old data (POD) struct representing a string slice:

```c
typedef struct String {
    const char* data;
    size_t len;
} String;
```

**Ownership rules**:
- **Inbound (D → C++)**: D constructs `String` from a local string slice. C++ reads it without taking ownership. Never call `String_Free` on these.
- **Outbound (C++ → D)**: C++ allocates via `String_New()` / `String_NewBuf()`. D must call `String_Free()` when done.

**String functions**:
- `String String_New(const char* cstr)` — allocates from null-terminated C string (copies data).
- `String String_NewBuf(const char* data, size_t len)` — allocates from raw buffer (copies data).
- `void String_Free(String s)` — frees an owned String. No-op if `data` is null. Sets error on double-free or wrong-allocator misuse.

**Safety features**:
- `String_NewBuf` allocates `len + 1` bytes to guarantee null-termination for safe C-string interop.
- `String_NewBuf` returns `{"", 0}` for zero-length input (distinguishable from error `{nullptr, 0}`).
- Ownership tracking via `std::unordered_set<const char*>` detects double-free and wrong-allocator misuse.
- Atexit cleanup frees any leaked owned strings on program exit.

### Opaque Handles

Two opaque handle types hide internal C++ types from D:

```c
typedef struct TuiState TuiState;     // Wraps ::llmfun::tui::TuiState*
typedef struct TuiScreen TuiScreen;   // Wraps ImTui::TScreen*
```

In the C++ implementation, these are completed as:

```cpp
struct TuiScreen {
    ImTui::TScreen* screen;
};

struct TuiState {
    ::llmfun::tui::TuiState* inner;
};
```

### Error Handling

All fallible functions report errors via a thread-local mechanism:

```c
String tuiLastError(void);
```

Returns an owned `String` with the last error message. Thread-local: each thread gets its own error. The error is **consumed** (cleared) on the first call, so stale errors don't persist. Returns `{nullptr, 0}` if no error was set. Caller must free the result with `String_Free()`.

### Thread Safety

| Function | Thread Safety |
|----------|---------------|
| `tuiInit()` / `tuiShutdown()` | Main thread only, not reentrant |
| `tuiCreateState()` / `tuiDestroyState()` | Thread-safe (no shared state) |
| `tuiBackendNewFrame()` / `tuiBackendRender()` | Main thread only |
| `tuiRender()` | Main thread only |
| `tuiAddOutputLine()` | Thread-safe (mutex) |
| `tuiClearOutput()` | Thread-safe (mutex) |
| `tuiSetStatusText()` | Thread-safe (mutex) |
| `tuiGetInput()` | Thread-safe (mutex) |
| `tuiClearInput()` | Thread-safe (mutex) |
| `tuiIsSubmitReady()` | Thread-safe (mutex) |
| `tuiResetSubmit()` | Thread-safe (mutex) |
| `tuiGetSubmitQuery()` | Thread-safe (mutex) |
| `tuiGetAutoScroll()` | Main thread only (no mutex) |
| `tuiSetAutoScroll()` | Main thread only (no mutex) |
| `tuiLastError()` | Thread-local (safe) |
| `String_New()` / `String_NewBuf()` / `String_Free()` | Thread-safe (mutex) |

### Backend Initialization Guard

A `std::atomic<bool>` flag prevents calling backend functions before `tuiInit()` or after `tuiShutdown()`. `tuiBackendNewFrame()` and `tuiBackendRender()` check this flag and return early with an error if the backend is not initialized.

### Complete C API

```c
/* Lifecycle */
TuiScreen* tuiInit(void);
void tuiShutdown(TuiScreen* screen);
TuiState* tuiCreateState(void);
void tuiDestroyState(TuiState* state);

/* Backend frame loop (main-thread only) */
void tuiBackendNewFrame(void);
void tuiBackendRender(TuiScreen* screen);

/* Rendering */
int tuiRender(TuiState* state);           // Returns 0 to exit, 1 otherwise

/* Output (thread-safe) */
void tuiAddOutputLine(TuiState* state, String line);
void tuiClearOutput(TuiState* state);

/* Status (thread-safe) */
void tuiSetStatusText(TuiState* state, String text);

/* Input (thread-safe) */
String tuiGetInput(TuiState* state);
void tuiClearInput(TuiState* state);

/* Submission (thread-safe) */
int tuiIsSubmitReady(TuiState* state);     // Returns 1 if ready, 0 otherwise
void tuiResetSubmit(TuiState* state);
String tuiGetSubmitQuery(TuiState* state);

/* Auto-scroll (main-thread only) */
int tuiGetAutoScroll(TuiState* state);      // Returns 1 if enabled, 0 otherwise
void tuiSetAutoScroll(TuiState* state, int enabled);
```

All API functions are null-safe: they check for null pointers and return early (with an error message for fallible functions).

---

## D Bindings

D imports the C API directly via `extern(C)`:

```d
module llm.tui;

import llmfun_tui;
```

The D-side provides helper utilities (per the system design):

- **`toApiString()`**: Converts a D string to an inbound API `String` (no allocation).
- **`OwnedString`**: RAII wrapper for owned strings returned by the C API. Automatically calls `String_Free` in destructor. Disables copy and move constructors to prevent double-free.

### D Usage Example

```d
import llm.tui;

void main() {
    auto screen = tuiInit();
    auto state = tuiCreateState();

    tuiSetStatusText(state, "Ready".toApiString);
    tuiAddOutputLine(state, "Welcome to llmfun".toApiString);

    while (tuiRender(state) != 0) {
        if (tuiIsSubmitReady(state) != 0) {
            OwnedString query = tuiGetSubmitQuery(state);
            // process query.toString()
            tuiResetSubmit(state);
        }
    }

    tuiDestroyState(state);
    tuiShutdown(screen);
}
```

---

## Internal C++ API

The internal C++ API (`tui.h` / `tui.cpp`) is used by `tui_api.cpp` and `main.cpp` (via the C API). All functions are in the `llmfun::tui` namespace.

| Function | Description | Thread-safe |
|----------|-------------|-------------|
| `tuiInit(ImTui::TScreen** screen)` | Initialize terminal, ImGui context, and dark theme | — |
| `tuiShutdown(ImTui::TScreen* screen)` | Clean up all resources | — |
| `tuiRender(TuiState& state)` | Render one frame; returns `false` to exit | — |
| `tuiAddOutputLine(TuiState&, const string&)` | Append line with FIFO eviction | Yes (mutex) |
| `tuiClearOutput(TuiState&)` | Clear all output lines | Yes (mutex) |
| `tuiSetStatusText(TuiState&, const string&)` | Set status line text | Yes (mutex) |
| `tuiGetInput(const TuiState&)` | Get current input buffer | Yes (mutex) |
| `tuiClearInput(TuiState&)` | Clear input buffer | Yes (mutex) |
| `tuiIsSubmitReady(const TuiState&)` | Check if input is ready to submit | Yes (mutex) |
| `tuiResetSubmit(TuiState&)` | Reset submission flag and clear `submitQuery` | Yes (mutex) |
| `tuiGetSubmitQuery(const TuiState&)` | Get last submitted query (read-only snapshot) | Yes (mutex) |

## Main Event Loop (`main.cpp`)

The entry point uses the C API and follows a standard ImGui frame loop:

1. **Initialization**: Call `tuiInit()`, create state via `tuiCreateState()`, set initial status text and welcome message.
2. **Frame loop**:
   - `tuiBackendNewFrame()` — processes backend input and starts new ImGui frame
   - `tuiRender(state)` — renders all three regions, handles keyboard shortcuts. Returns `0` to exit.
   - **Submission check**: If `tuiIsSubmitReady(state)`, extract the query via `tuiGetSubmitQuery()`, echo it to output as `> query`, and reset submit flag. The input buffer is already cleared inside `tuiRender()` (same-frame clearing), so no explicit `tuiClearInput()` call is needed.
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
| `Ctrl+Up` | Navigate backward in input history | Input widget active |
| `Ctrl+Down` | Navigate forward in input history | Input widget active |

Note: History navigation uses `Ctrl+Up`/`Ctrl+Down` instead of plain `Up`/`Down` to avoid conflicting with `InputTextMultiline`'s internal cursor movement.

### Output Area

- Renders inside a `BeginChild("output")` with `ImGuiWindowFlags_HorizontalScrollbar`.
- **Lock-minimization**: Copies `outputLines` to a local vector under mutex, then renders from the copy without holding the lock. This eliminates deadlock risk from ImGui internals calling back into the TUI API.
- **Auto-scroll**: Automatically follows new content when `autoScroll` is `true`. Manual scroll (scrolling up) disables auto-scroll. Pressing `End` re-enables it.
- Auto-scroll detection compares `GetScrollY()` against `GetScrollMaxY() - 1.0f`.

### Input Area

- Uses `ImGui::InputTextMultiline` with a dynamic `std::string` buffer and a resize callback (`InputResizeCallback`, a static function rather than a per-frame lambda).
- **Resize callback**: Implements the standard `imgui_stdlib` pattern — on `ImGuiInputTextFlags_CallbackResize`, resizes the string and updates `data->Buf`.
- **Buffer safety**: Ensures `inputBuf` is non-empty before passing to `InputTextMultiline` to prevent buffer over-read/write before the resize callback fires.
- **Buffer size**: Passes `state.inputBuf.size() + 1` (for null terminator), following standard ImGui patterns.
- **Default focus**: `ImGui::SetKeyboardFocusHere()` is called before `InputTextMultiline` (guarded by a `static bool` so it only runs on the first frame), giving the input field keyboard focus on startup so the user can type immediately without clicking.
- **Same-frame clearing on Enter**: When the user presses Enter, `inputBuf` is captured into `submitQuery` and `inputBuf` is cleared in the same render frame. This provides instant visual feedback — the input field appears empty immediately, with no lag. The main loop retrieves the query via `tuiGetSubmitQuery()` (a read-only snapshot) rather than `tuiGetInput()`.
- **History navigation**:
  - On first `Ctrl+Up`: saves current input to `draftBuf`, pushes it to `inputHistory` (if not duplicate of last entry), then navigates to the entry before it.
  - `Ctrl+Down` walks forward; past the end restores `draftBuf` and resets `historyPos` to -1.
  - On submission: pushes input to history if non-empty, not a duplicate, and not currently in history navigation (`historyPos == -1`).
  - History is bounded to `MAX_HISTORY` (500) entries with FIFO eviction.

### Status Line

- A child window with all decorations disabled (`NoCollapse`, `NoResize`, `NoMove`, `NoTitleBar`, `NoScrollbar`, `NoScrollWithMouse`).
- Falls back to a default status string (`"Context: 0/0 tokens | Model: none | Ready"`) if `statusText` is empty.

## Theme

The `applyTheme()` function starts with `ImGui::StyleColorsDark()` as a base, then overrides specific colors:

| Color Slot | Value | Purpose |
|------------|-------|---------|
| `Text` | `(0.90, 0.90, 0.90, 1.00)` | Light gray text |
| `TextDisabled` | `(0.50, 0.50, 0.50, 1.00)` | Dimmed text |
| `WindowBg` | `(0.06, 0.06, 0.06, 1.00)` | Near-black background |
| `ChildBg` | `(0.06, 0.06, 0.06, 0.00)` | Transparent child bg |
| `Border` | `(0.20, 0.20, 0.20, 1.00)` | Subtle borders |
| `FrameBg` | `(0.16, 0.16, 0.16, 1.00)` | Input frame background |
| `FrameBgHovered` | `(0.26, 0.26, 0.26, 1.00)` | Hovered frame |
| `FrameBgActive` | `(0.26, 0.59, 0.98, 0.65)` | Blue highlight when active |
| `ScrollbarBg` | `(0.05, 0.05, 0.05, 0.54)` | Dark scrollbar background |
| `ScrollbarGrab` | `(0.34, 0.34, 0.34, 0.54)` | Scrollbar grab handle |

## Initialization & Shutdown

### `tuiInit()` (internal C++)
1. Calls `IMGUI_CHECKVERSION()` and `ImGui::CreateContext()`.
2. Applies dark theme via `applyTheme()`.
3. Initializes ncurses backend with mouse support enabled, active FPS at 60.0, idle FPS at 3.0 (CPU saving).
4. Initializes text renderer backend.
5. Returns `false` with error message on failure, cleaning up the ImGui context.

### `tuiShutdown()` (internal C++)
1. Shuts down text renderer (if screen is non-null).
2. Shuts down ncurses backend (if screen is non-null).
3. Destroys ImGui context.

### `tuiInit()` (C API)
Wraps the internal `tuiInit()` and returns an opaque `TuiScreen*` handle. Sets a backend initialization guard (`std::atomic<bool>`) to prevent calling backend functions before initialization.

### `tuiShutdown()` (C API)
Wraps the internal `tuiShutdown()`, clears the backend initialization guard, and deletes the `TuiScreen` wrapper.

## Build System

The `CMakeLists.txt` uses CMake 3.10+ with C++17:
- Brings in imtui as a subdirectory (`add_subdirectory(../vendor/imtui ...)`).
- Disables shared libraries, curl support, and imtui examples.
- **Static library target** (`llmfun_tui_lib`): Contains `tui.cpp` and `tui_api.cpp`. Enables D code (and other languages) to link against the library independently. Exports include directories via `PUBLIC` so both `tui_api.h` and `tui.h` are findable. Links against `imtui-ncurses` (`PUBLIC`) and `Threads::Threads` (`PRIVATE`).
- **Executable target** (`llmfun_tui`): Only `main.cpp` as source. Links against `llmfun_tui_lib`.
