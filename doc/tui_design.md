# TUI System — Implementation Description

## Overview

The llmfun TUI is a C++17 user interface that renders either into a terminal (built on the **imtui** library, a terminal-based ImGui wrapper at `llmfun/vendor/imtui`) or into a graphical window via the GLFW + OpenGL3 backend (see "Graphical backend (GLFW + OpenGL3)" below). It provides a full-screen chat interface for interacting with an LLM: a scrollable chat output area with typed/color-coded messages, a multiline input area, and a status line. The TUI is self-contained in the `llmfun/cpp_tui/` directory.

 A pure C API layer (`tui_api.h` / `tui_api.cpp`) wraps the internal C++ implementation, enabling D to link against the TUI without C++ name mangling. D imports `tui_api.h` directly.

## File Structure

```
llmfun/cpp_tui/
├── CMakeLists.txt   # Build configuration (CMake 3.10+, C++17); driven by ../../tui.mak
├── main.cpp         # Standalone/dry-run entry (`--tui|--gui --frames N`), not the app entry point
├── tui.h / tui.cpp  # TuiState, render dispatch, theme, data feeds
├── tui_api.h / tui_api.cpp  # Pure C API for D (extern "C"); bridges to tui.h/tui.cpp
├── tui_backend.h    # Backend seam: init/newFrame/renderFrame/shutdown + kind/shouldClose
├── tui_backend_ncurses.cpp  # Terminal backend (ImTui/ncurses; text-only popen clipboard)
├── tui_backend_gui.cpp      # GUI backend (GLFW window + OpenGL3; platform clipboard)
├── tui_backend_null.cpp     # Inert Null backend (headless/tests)
├── tui_chat.h / tui_chat.cpp  # Chat/log cluster: message widgets, input area, status line
├── tui_common.h / tui_common.cpp  # Shared helpers: Log, whitespace test, multiline text
├── tui_widgets.h / tui_widgets.cpp  # Shared widgets: renderButton, separator, style guard
├── session_panel.h / session_panel.cpp  # Session sidebar: rows, filter, rename, delete
├── session_fuzzy.h  # Pure fzf-style matcher for the session filter (stdlib-only)
├── probe_margin.c   # PTY probe: asserts no cell lands at/beyond the max-width cap
├── test_*.cpp       # Headless test binaries (session filter, fuzzy, max width, v4 API, ...)
└── test/            # Dev probes (keyboard/mouse/PTY scratch programs)
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
│  Main loop: tuiCreateState → tuiInit(state, mode) →              │
│    tuiBackendNewFrame(state) → tuiRender(state) →                │
│    tuiBackendRender(state) → ... → tuiDestroyState               │
│                                                                  │
├──────────────────────────────────────────────────────────────────┤
│              C API Boundary (tui_api.h) — extern "C"             │
│                                                                  │
│  C header with extern "C" linkage:                               │
│    String (POD struct, explicit ownership)                        │
│    ChatMessageParam (bundles summary, text, thinking, type)       │
│    TuiState* (single opaque handle)                               │
│    TuiBackendMode in → TuiBackendKind out (v4)                    │
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
│    - None-safe: frame calls are inert without a backend (v4)     │
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
├──────────────────────────────────────────────┤  ← y=H-5
│  > User input line 1                         │
│    (multiline input; 4-row minimum frame)    │
├──────────────────────────────────────────────┤  ← y=H-1
│  Context: 0/0 tokens | Model: none | Ready   │
└──────────────────────────────────────────────┘  ← y=H
```

All windows use `ImGuiCond_Always` for stable positioning. Minimum terminal size is **40 columns × 15 rows**; below this, an error message is rendered instead of the normal UI.

The bottom regions are sized from the input's effective height: `inputHeight = max(4, 1 + textLineHeight*inputBufLines + 2*FramePadding.y)` (the 4-row floor is imgui's hard child minimum, `IMGUI_WINDOW_HARD_MIN_SIZE`), the output reserve is `DisplaySize.y - 2 - inputHeight`, and the input frame occupies `H-1-inputHeight .. H-2` (`H-5 .. H-2` for the default 4-row frame); the status line is pinned to row `H-1`. See "Rendering Internals" below for why the 4-row minimum matters.

Accepted minimum-size behavior (documented, no guards or clamps added). At 40 columns with the session panel open, the input group's width collapses to 9 cells, so the field's computed width is 0 and Dear ImGui substitutes its default item width (`CalcItemWidth()`, ~65% of the window ≈ 26 cells, imgui.cpp:11840/8197): the field renders wider than its column and is clipped at the window's right edge (only its 10-cell right column is visible), the Send/Prev/Next stack is pushed off-window, and the status line is cut off after `Context: ` — the input nonetheless stays focusable and typable (the field is auto-focused in the text backend; a click into the field band keeps it active and typed characters reach the input buffer). Collapsing the panel to the 8-cell strip restores a 21-cell field and the on-screen button stack. At 15 rows, a tall input buffer (≥14 newlines ⇒ group height ≥16) drives the group's top above the window: `groupPos.y = ny - groupH` is negative and deliberately unclamped, so the status row — and, for taller buffers, the field's leading rows — are clipped at the top while the group's bottom stays flush with the window bottom, and the output child keeps its existing 1-row minimum clamp (`max(1.0f, …)`). None of these configurations asserts or crashes.

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

## Graphical backend (GLFW + OpenGL3)

The same C++ core, C API and D front end can render either into a terminal
(the imtui text grid, the original mode) or into a real window: the **GUI
backend** is a GLFW window with the stock Dear ImGui OpenGL3 renderer
(`imgui_impl_glfw.cpp` + `imgui_impl_opengl3.cpp` from the vendored imgui —
imtui is bypassed entirely in GUI mode). Both profiles live in the single
`llmfun` binary and are selected through the same C API.

### Backends and the `Backend` seam

`cpp_tui/tui_backend.h` defines the lifecycle interface every backend
implements — `init(error)` / `newFrame()` / `renderFrame()` / `shutdown()`,
plus `kind()` (None/Tui/Gui) and `shouldClose()` (exit request, e.g. a closed
window). `tuiInit` creates one backend through the factories
(`makeNcursesBackend` / `makeGuiBackend` / `makeNullBackend`) and the
per-state `TuiState` owns it:

| Backend | Implementation | Notes |
|---------|----------------|-------|
| `NcursesBackend` | `tui_backend_ncurses.cpp` | the terminal: ImTui text rasteriser + ncurses output; also installs the **popen clipboard** helpers (`xclip` / `wl-copy`) |
| `GuiBackend` | `tui_backend_gui.cpp` | the window: GLFW + OpenGL3; 800x600, title `llmfun`; the **GLFW platform clipboard** (no subprocess) |
| `NullBackend` | `tui_backend_null.cpp` | inert (attaches nothing, `kind() == None`); the seam's placeholder for headless/tests — `tuiInit` never attaches it |

The GUI backend asks for a GL 3.0 context with GLSL `#version 130` and
retries once with GL 2.1 + `#version 120` if window/context creation fails.
Each attempt passes its **own pinned shader string** to
`ImGui_ImplOpenGL3_Init` — never `nullptr`, whose desktop auto-branch can
emit a version the context does not have. GLFW errors are captured by an
error callback and become the init failure reason.

### Selection and fallback contract

`TuiBackendMode` is resolved once at startup and passed to
`tuiInit(state, mode)`:

```
TuiBackendMode_Tui    terminal UI; no GUI attempt
TuiBackendMode_Gui    require the GUI; fail if it cannot start (no fallback)
TuiBackendMode_Auto   default: try the GUI first, then the fallback rule
```

- The `agent` command defaults to Auto. `--tui` / `--gui` (mutually
  exclusive) select Tui / Gui; D resolves this in `resolveTuiBackendMode`.
- `LLMFUN_TUI_BACKEND=auto|gui|tui` selects the mode from the environment when no flag is given
  (precedence: CLI flag > env > Auto); an unknown value warns and falls back to Auto.
- **Auto fallback is resolved inside C++** (the TTY check cannot be faked
  from D): when GUI init fails and **stdin AND stdout are TTYs**, `tuiInit`
  prints one line to stderr — `GUI unavailable: <reason>; falling back to
  terminal UI` — records it as the *backend note* (`tuiBackendNote()`, which
  the D actor reads and uses to seed the status line) and attaches the
  terminal backend. Without a TTY (pipes, CI, headless Auto) there is no
  fallback: the GUI reason is the init error. If the terminal fallback
  itself fails, the combined reason is reported
  (`...; terminal UI also failed: ...`) and no note is recorded.
- Fallback is **init-time only**. Once the frame loop is running, a failure
  is an ordinary error and the backend never changes mid-session.
- Every failure stage reports a precise reason through `tuiLastError()`
  (`glfwInit failed: ...`, `GLFW window creation failed (GL 3.0 and GL 2.1)`,
  `ImGui_ImplOpenGL3_Init(...) failed: OpenGL driver unavailable`, ...).

### Pixel profile (GUI vs text grid)

The text grid's constraints are terminal-cell constraints; they are
runtime-gated with `tuiIsTextGrid()` (which reports
`ImTui_TextEncodingActive`), so one build serves both profiles:

- **text-grid only:** the 40x15 minimum-size screen (small GUI windows keep
  rendering instead), the `maxWidth` clamp (a column cap must not be read as
  pixels), the cell-tuned `GrabMinSize = 3.0` scrollbar grab (GUI keeps
  imgui's 12.0 default), the input-row nav-cursor suppression
  (`suppressInputRowNavCursor`; the GUI uses the upstream nav cursor), and
  the cell-derived label-truncation budgets (GUI measures pixels via
  `CalcTextSize` / `GetContentRegionAvail`).
- **shared:** theme colors (`applyTheme`, applied after the backend init),
  widget semantics (Escape, history, submit), `Ctrl+C` exit, and the
  root-window scroll pinning (`SetNextWindowScroll(0, 0)`), which re-asserts
  the "the root never scrolls" invariant inside every `Begin`: the absolute
  layout would otherwise slide when a startup nav request asks for a centered
  scroll.
- **GUI only:** the root window paints the themed background — the text grid
  deliberately keeps it transparent so the terminal background shows through,
  while the pixel framebuffer would otherwise keep stale pixels after a
  resize; buttons and regions size from imgui metrics (`GetFrameHeight`,
  `GetTextLineHeight`, `ItemSpacing`) instead of one-cell constants.

### Fonts & DPI

The GUI renders with imgui's embedded bitmap font (ProggyClean, 13 px base)
by default — no asset is needed, and the font is pinned explicitly instead of
left to `AddFontDefault()`'s size heuristic. Two optional environment
variables (GUI only; the terminal backend's imtui font path is untouched):

- `LLMFUN_GUI_FONT` — path to a TTF font, replacing the embedded default
  (`io.Fonts->AddFontFromFileTTF`).
- `LLMFUN_GUI_FONT_SIZE` — its pixel size (default 16; unset, empty,
  non-numeric, or out-of-range values keep the default). Without
  `LLMFUN_GUI_FONT` the variable has no effect.

A path that cannot be loaded prints
`LLMFUN_GUI_FONT: cannot load '<path>'; using the embedded font` to stderr
and keeps the default. The 1.92 atlas rasterizes glyphs on demand, so no
glyph ranges are passed (they only matter to legacy backends); a glyph the
font lacks renders as its fallback (`?`) glyph. Without configuration the
rendering is exactly as before on a 1.0-scale monitor.

**DPI:** at init the backend queries the primary monitor's content scale
(`ImGui_ImplGlfw_GetContentScaleForMonitor`) and, when it is not 1.0, scales
the style metrics once (`style.ScaleAllSizes(scale)`) and sets the font scale
(`style.FontScaleDpi` — 1.92's replacement for `io.FontGlobalScale`). The
per-monitor query requires GLFW 3.3+; older GLFW compiles the call out inside
the vendored backend and reports 1.0, so the defaults stay unscaled
(Wayland/macOS report 1.0 as well). The window size is not changed — the
layout scales inside the existing 800x600 window.

### Clipboard

Both profiles copy through `ImGui::SetClipboardText` (the `[c]` buttons and
markdown links call it), but the installed handler differs: the terminal
backend installs the popen helpers (`xclip`, or `wl-copy`/`wl-paste` on
Wayland); the GUI backend keeps the GLFW platform clipboard installed by
`imgui_impl_glfw` — no subprocess runs in GUI mode. Paste is the input
widget's own handling in both profiles; `Ctrl+C` is the exit key in both (not
a copy binding).

### Idle pacing

The GUI is vsynced (`glfwSwapInterval(1)`) and waits for input: each frame
calls `glfwWaitEventsTimeout(1/60 s)`, so the loop blocks between frames
instead of busy-spinning (measured on llvmpipe: event-waiting, no spin, with
the CPU cost being the software rasterisation of ~60 fps redraws). The
terminal backend keeps its ncurses 60/3 FPS pacing; the D actor polls at
10 ms in both profiles. Window close (X / Alt+F4) goes through the backend's
`shouldClose()` into the existing user-terminated path — the same exit as
`Ctrl+C`.

### Startup-failure reporting (`uiStartupFailed`)

An init failure reaches the application as a typed message: the D
`TextUserInterfaceActor` attaches the backend from `onSpawn` — on the actor's
own thread, so the window/GL context are created and driven from one thread
(the app spawns the actor with `Config.detached`) — records any failure reason
there, and dispatches `TUIListener.uiStartupFailed(reason)` instead of arming
its frame tick. `AppAgentActor.uiStartupFailed` logs
`TUI startup failed: <reason>` to stderr and exits the process non-zero — so
`--gui` without a display and headless Auto runs fail deterministically for
scripts. On success the actor logs `TUI backend active: <Tui|Gui>` and, when
a fallback happened, logs `TUI backend note: <note>`.

### Build (GLFW discovery and link flags)

`cpp_tui/CMakeLists.txt` locates GLFW in three steps (normalized to a target
named `glfw`):

1. `find_package(glfw3 3.0 QUIET)` — the GLFW CMake package config;
2. pkg-config (`glfw3`, when pkg-config is installed);
3. `find_path(GLFW/glfw3.h)` + `find_library(glfw glfw3)`.

If all three fail, configure stops with an install hint (`libglfw3-dev` on
Ubuntu/Debian; `glfw-devel` from EPEL on EL7/EL9 — the EL specifics are to
be confirmed by the EL7/EL9 evidence run; building GLFW from source remains
the fallback). The two vendored imgui backend translation units
(`imgui_impl_glfw.cpp`, `imgui_impl_opengl3.cpp`) compile into
`libllmfun_tui_all_lib.a`. The GLFW-including sources — the vendored
`imgui_impl_glfw.cpp` and `tui_backend_gui.cpp` — get `-DGLFW_INCLUDE_NONE`
(the GLFW header otherwise pulls in GL headers), and the build adds the
explicit imgui core + `backends/` include directories.
Nothing links `-lGL`: the OpenGL3 backend loads the driver at run time
through its bundled loader, which is why the executables link
`${CMAKE_DL_LIBS}` (libdl) alongside `glfw`. `dub.sdl` adds `-L-lglfw` and
`-L-ldl` unconditionally on Linux (dub cannot probe libc versions; `libdl` is
a stub after glibc 2.34 — non-glibc toolchains can override the flags).

### Packaging: the `libglfw3` runtime closure

GLFW is linked normally (no dlopen plugin), so `libglfw3` is a **load-time
dependency of the process**: it is needed even for `--tui` runs, and without
it the process cannot start at all. This is the accepted limitation of the
normal-linking design — packagers must depend on the closure.

The rest of the closure is a property of the distro's GLFW build. Derive it
from the distro package metadata (or from `ldd libglfw3` where that build
links its clients directly). Debian/Ubuntu example (26.04, `libglfw3`
3.4-4): the runtime package depends on the X11 client set (`libx11-6`,
`libx11-xcb1`, `libxcursor1`, `libxext6`, `libxi6`, `libxinerama1`,
`libxkbcommon0`, `libxrandr2`, `libxrender1`), the Wayland client set
(`libwayland-client0`, `libwayland-cursor0`, `libwayland-egl1`,
`libdecor-0-0`) and the GL dispatcher trio (`libegl1`, `libglx0`,
`libopengl0`) — this GLFW build loads the display clients lazily (its own
`ldd` shows only libm/libc), so read the package dependency list, not just
`ldd`.

The OpenGL driver is opened at run time by the bundled loader; Mesa's
software rasteriser (llvmpipe) is accepted, so VMs without a GPU run the
GUI. A missing driver surfaces as `ImGui_ImplOpenGL3_Init(...) failed:
OpenGL driver unavailable (bundled loader)`.

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

One opaque handle type hides the internal C++ state from D (v4):

```c
typedef struct TuiState TuiState;   // Wraps ::llmfun::tui::TuiState*
```

`TuiScreen` (the ImTui screen handle) and `tuiShutdown` were removed in API
v4: the screen is an implementation detail of the terminal backend, and
`tuiDestroyState` tears down the whole state — backend included.

### Error Handling

All fallible functions report errors via a thread-local mechanism:

```c
String tuiLastError(void);
```

Returns an owned `String` with the last error message. Thread-local: each thread gets its own error. The error is **consumed** (cleared) on the first call. Returns `{NULL, 0}` if no error was set. Caller must free the result with `String_Free()`.

A second thread-local channel carries the init fallback note (`tuiBackendNote()`, see the API Reference below): an owned `String` that is non-empty only after an Auto init fell back to the terminal backend, and — unlike the error — it is not consumed by reading.

### Threading Model

The TUI is driven from a single thread (the main/UI thread). All API functions must be called from this thread. No mutexes or locks protect the TUI state. The app keeps this rule by spawning `TextUserInterfaceActor` on a dedicated thread (`Config.detached`): the backend attach (`onSpawn`) and every frame run on that one thread.


### API Reference

See `tui_api.h` for the complete C API. The header is self-documented with
detailed comments for each function.

The v4 lifecycle (single `TuiState*` handle):

| Step | Call | Semantics |
|------|------|-----------|
| create | `tuiCreateState()` | creates the state; no backend, no ImGui context |
| attach | `tuiInit(state, mode)` | creates the ImGui context and attaches exactly one backend; `0` = success, otherwise the precise stage reason is in `tuiLastError()`. Single-shot per success; at most one initialized state per process; after a failure the state stays backend-less and a retry (possibly another mode) is allowed. Auto's GUI→TUI fallback is resolved here, in C++ |
| frame | `tuiBackendNewFrame(state)` / `tuiBackendRender(state)` | backend input + ImGui frame start / present the finished frame; no-ops on a backend-less state |
| render | `tuiRender(state)` | draws the widgets; returns `0` when the user requested exit (`Ctrl+C`, or a closed GUI window) |
| inspect | `tuiBackendActive(state)` | the attached `TuiBackendKind` (None/Tui/Gui) |
| note | `tuiBackendNote()` | owned `String` with the fallback note of the most recent `tuiInit` on the calling thread; empty when no fallback happened |
| destroy | `tuiDestroyState(state)` | shuts the backend down, destroys the ImGui context `tuiInit` created (a caller-created context survives), frees the state; null-safe and pre-init-safe |

None-safety is deliberately asymmetric (pinned by `test_tui_api_v4`):
`tuiRender(NULL)` returns `0` (the v3 exit convention) while a valid
backend-less state returns `1` (continue); neither touches ImGui.

### Session API

The session sidebar added a second API family to `tui_api.h`, and bumped
`TUI_API_VERSION` from 1 to 2. The version macro is a **documentation marker
only** — no runtime behavior depends on it, and the contract tests pin its
current value with `static_assert` so a bump stays deliberate; the header
comment lists the additions.

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

### Max Width

Max width caps the TUI's rendered width in terminal columns and bumps
`TUI_API_VERSION` from 2 to 3. As with version 2, the macro is a
**documentation marker only** — no runtime behavior depends on it, and the
contract tests pin its current value with `static_assert`.

New function:

| Function | Semantics |
|----------|-----------|
| `tuiSetMaxWidth(TuiState*, int)` | Cap the rendered width in terminal columns. `0` = unlimited (default, current behavior). Positive values should be in `[40, 10000]`; a positive value below 40 is raised to 40 (below the TUI's `MIN_TERMINAL_WIDTH` it would be stuck on its "Terminal too small!" screen), and negative values are treated as 0 (unlimited). Null-safe. Call after `tuiCreateState` and before the first frame; a late call applies from the next frame. Effective width = `min(terminal width, maxWidth)`. Text backend only (v4): the GUI pixel profile ignores the cap (see below) |

Layout note: the cap is enforced in exactly one place — at the top of
`llmfun::tui::tuiRender` (reading `TuiState.maxWidth`), re-evaluated every
frame before the min-size check and `SetNextWindowSize`. It clamps
`io.DisplaySize.x`, which the vendor `RenderDrawData` consumes to size the
grid, so `DrawScreen` writes at most `maxWidth` columns: no byte reaches a
column at or beyond the cap. The margin right of the cap is **never written**
by the TUI; it is terminal/ncurses-managed (typically blank — the alternate
screen + first-refresh clear). No vendor code and no C↔D render-loop ABI
change.

The v4 gate: the clamp is applied under `tuiIsTextGrid()` in `tuiRender`, so
it binds the terminal backend only — the GUI's `DisplaySize` is in pixels,
and reading a column cap as pixels would truncate the window to a sliver. D
still validates and forwards `TuiConfig.maxWidth` for both modes; the pixel
profile simply ignores it.

The standalone `cpp_tui` executable honors `LLMFUN_TUI_MAX_WIDTH=<cols>` (env
var only; no CLI flag) for PTY debugging and the max-width byte-stream test. Unset,
empty, non-numeric, or negative values are ignored (0 = unlimited); values
above 10000 are clamped to 10000 (mirrors `validateConfig`), and positive
sub-40 caps are raised to 40 by the C API. (The cap is text-backend only:
GUI runs ignore it.)

### API Version History

`TUI_API_VERSION` is a **documentation marker only** — no runtime behavior
depends on it (the contract tests pin its current value with `static_assert`
so a bump stays deliberate); the changelog lives in the `tui_api.h` header
comment. The generations so far:

1. original API: `String`, `ChatMessageParam`, `TuiState` + `TuiScreen`,
   `tuiInit()` + `tuiShutdown(screen)`.
2. session sidebar: `SessionItem`, `TuiSessionActionType`, `SessionAction`,
   `tuiSetSessionList`, `tuiIsSessionActionReady`, `tuiGetSessionAction`.
3. max width: `tuiSetMaxWidth` (cap the rendered width in columns;
   0 = unlimited).
4. single handle: `TuiBackendMode`/`TuiBackendKind`, `tuiInit(state, mode)`,
   per-state frame calls (`tuiBackendNewFrame/Render(state)`), None-safe
   `tuiRender`, `tuiBackendActive`, `tuiBackendNote`, destroy-tears-down;
   `TuiScreen` + `tuiShutdown` removed.

---

## Internal C++ API

The internal C++ API (`tui.h` / `tui.cpp`) is used by `tui_api.cpp` and `main.cpp` (via the C API). All functions are in the `llmfun::tui` namespace.

See `tui.h` for the complete function declarations.

## Session Sidebar

The session sidebar is a left panel in the chat tab that lists all chat
sessions (title, message count, preview) and offers switch / new / rename /
delete. It follows the same three-layer pattern as the query input: the C++
panel owns all UI state and queues actions; the D UI thread polls the queue
once per frame and forwards one action to the agent thread; the agent thread
runs the existing session methods.

### ChatTabSessionPanel

`ChatTabSessionPanel` (`tui.h`) holds the panel state:

```cpp
struct ChatTabSessionPanel {
    ImVec4 activeButton = ImVec4(0.4f, 0.4f, 0.45f, 1.0f); // highlight color
    int panelW = 0;                    // 0 = unset; init to PanelWActivated
                                       // on first render
    static constexpr int PanelWActivated = 30;
    bool panelOpen{true};              // auto-open at startup

    std::vector<SessionEntry> sessions; // full snapshot
    std::string activeId;               // active session id from the snapshot
    std::deque<SessionAction> actions;  // UI -> D queue

    char renameBuf[128] = {};   // rename input; init on row change or
                                // toggle-open, never per frame
    bool renameActive{false};   // rename input visible
    std::string renameRowId;    // row renameBuf was initialized for
    bool renameFocus{false};    // focus the rename input next frame
    int renameSeq{0};           // bumped per open; fresh InputText id
    std::string pendingDeleteId; // two-step delete state
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
(no overlap).

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
  by 0.
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
  title only on row change or toggle-open, never per frame; a title
  longer than the 128-byte buffer initializes the buffer empty, so a blind
  Enter is rejected as empty — no silent truncation. Enter queues
  `{Rename, activeId, typedTitle}` (empty/whitespace-only titles rejected
  in-panel), Escape cancels.
- **Delete**: each row has a `del` button; the first press arms the row
  (`del?`), a second press on the same row queues `{Delete, id}` and clears
  the arm. Pressing another row's delete moves the pending target; any
  non-delete control clears it.
- **Busy gating**: when `!state.readyStatus`, every interactive widget is
  guarded so no action is queued (guard-and-skip: the widgets stay rendered,
  the handlers drop the action). A click already in flight when the busy state
  flips is processed between queries (the mailbox race); see
  `doc/sessions.md` for the observable late-click effect.
- **Scrolling**: the panel child window has no vertical scrollbar yet; rows
  below the terminal height are unreachable.

Sidebar interactions are logged through the shared `Log& log` parameter
(`session panel: ...` lines in the Log tab).

### Filter Input

The panel header carries an fzf-style filter: a single-line
`InputText` between the `Sessions` separator and the `session_rows` child,
so it stays fixed while the rows scroll. It adds one header row; the
rows child is sized to the remaining height, so the panel shows one fewer
row than before. The collapsed 8-wide strip renders no filter.

**Panel state** (`ChatTabSessionPanel`, `tui.h`):

```cpp
    std::array<char, 64> filterBuf = {}; // query; whitespace = no filter
    int filterSeq{0};                    // suffixes the input widget id
    bool filterNonEmptyLastFrame{false}; // end-of-last-frame query snapshot
    ImVec4 matchColor = ImVec4(1.0f, 0.85f, 0.45f, 1.0f); // highlight
```

**Input** (`renderTabChatSessionPanel`, `tui.cpp`):
`SetNextItemWidth(GetContentRegionAvail().x)` +
`ImGui::InputText("##session_filter_" + std::to_string(filterSeq), filterBuf,
sizeof filterBuf, ImGuiInputTextFlags_EnterReturnsTrue)`. Click-to-focus
only: no `SetKeyboardFocusHere`, so the always-rendered input never
steals keyboard focus from the main query input.

**Per-frame visible list** (no caching): a local `std::vector` of
`{index into panel.sessions, score}` (no `SessionEntry` copies). A
whitespace-only query keeps all entries in snapshot order; otherwise
entries with `fuzzyScoreFields(query, title, preview) >= 0` are kept and
`std::stable_sort`ed by score descending (ties keep snapshot order).
`visible` is computed every frame — the filter is a pure function of the
snapshot + `filterBuf`.

**Matcher** (`cpp_tui/session_fuzzy.h`, pure, TUI-independent):

- `fuzzyScore(query, text) -> int`: case-insensitive byte-level subsequence
  (ASCII-only case fold; multi-byte bytes match exactly, never split —
  deliberately not `std::tolower`, which is locale-dependent).
  Leftmost-alignment score: +100/byte, +40 word boundary (start, or after
  space/`-`/`_`/`/`), +25 consecutive, -3/gap byte, -1/first-match position;
  -1 = no match, match score clamped to a floor of 0. Weights are
  named constants (`kFuzzyBase`/`kFuzzyBoundary`/`kFuzzyConsecutive`/
  `kFuzzyGap`/`kFuzzyFirstPos`) for future DP scoring.
- `fuzzyScoreFields(query, title, preview) -> int`: multi-field —
  matches if either field matches; score = `max(titleScore,
  previewScore/2)` (title weighted 2x).
- `fuzzyMatchPositions(query, text, positions&) -> bool`: the leftmost
  alignment's matched byte offsets for highlighting; caller-owned,
  reusable vector (no per-frame allocation churn).

**Escape (clears the filter)**: a global `IsKeyPressed(Escape)` check in
the open-panel branch, before the row loop, gated on `!panel.renameActive`.
The rename box owns Escape while open (its own check runs in the row loop,
active row only), and the rule below guarantees `renameActive` is false
whenever the active row is filtered out, so the two Escape paths are
disjoint on every frame. The check fires when the query is non-empty **or**
`filterRevertedEmpty` (non-empty last frame, empty now): on an *active*
input, ImGui's Escape handling (cancel-edit; `EscapeClearsAll` not set)
reverts the buffer to its activation value during the widget call, which is
rendered above this check — so the end-of-last-frame snapshot is what tells
the handler the user really had a query.
`clearFilter()` empties `filterBuf` and bumps `filterSeq` (see below);
logs `filter cleared (Escape)`.

**Enter (selects the top match)**: the `EnterReturnsTrue` return value
selects `visible[0]` when the visible list is non-empty: already-active →
log-only no-op; ready → queue `{Select, id}`; busy →
`pendingSelectId = id` (flushes as an ordinary Select on the first
ready frame, top of the function). The filter clears at selection time
(`clearFilter()`), independent of the async switch. Enter only fires while
the filter input itself is active, so it cannot race the rename input's
own Enter handling.

**Row clicks**: the existing click handler (queue Select /
pendingSelectId / active-row no-op) plus `clearFilter()` in every branch —
a click on a filtered row clears the filter as well.

**Rename box closes when its row is filtered out**: after computing
`visible`, if `renameActive` and the active row is not in `visible`, the
box closes (`renameActive = renameFocus = false`) and is logged — mirroring
the "active row absent from snapshot" rule at the top of the function and
keeping the Escape branches disjoint.

**Rename-Esc filter restore**: on the frame the rename box closes
via Escape, an *active* filter input reverts its buffer to its activation
value (cancel-edit) in the same frame, which would wipe the query along
with the box. The handler snapshots the pre-frame query
(`filterPreFrame`), and if the buffer changed, restores it and bumps
`filterSeq` (log `filter restored (rename Esc frame)`), so the query
survives a rename-cancel and a later Escape still clears it.

**Highlighting**: with a non-empty filter, `titleMatchRuns` maps
`fuzzyMatchPositions` offsets to label byte ranges — consecutive matches
grouped, snapped to UTF-8 character boundaries (a character is highlighted
iff any of its bytes matched), extended to the whole word enclosing each
match (word = maximal span between the separator bytes), and clipped
to the displayed title prefix (`sessionTitlePortionLen`) — and
`renderTitleButton` over-draws those ranges in `matchColor` on top of the
plain label. Same widget id (`##but` + label) and hover/active colors as
`renderButton`; empty runs render exactly as before (no extra items). The
ellipsis and the ` [N]` count suffix are never highlighted; a preview-only
match has no title runs. The cursor is restored after the overdraw so
`sameLineAfterButton`'s anchor is unaffected.

**No-match indicator**: inside the rows child, when the query is
non-empty, `visible` is empty, and the snapshot is non-empty, a single
dimmed (`previewColor`) `no matches` line replaces the blank area.

**`clearFilter()` and the seq-bump rationale**: `clearFilter(panel)`
fills `filterBuf` with NUL and does `++filterSeq`. The seq suffixes the
InputText widget id, so a programmatic clear changes the id and forces a
fresh InputText state that reads the now-empty buffer. This is robust
against imgui's InputText, which (a) reverts an active edit to its
activation value on Escape (the `revert_edit` path in `imgui_widgets.cpp`)
and (b) can re-assert stale internal edit state from a deactivated widget
on refocus — both bypassed by the id change. (The same pattern powers
`renameSeq`.)

**End-of-frame snapshot**: `filterNonEmptyLastFrame` is set from the final
buffer after the rows child closes, so the rename-Esc restore counts as a
real query for the next frame's filter-persistence check.

**Smoke harness** (`test_session_filter_smoke`): a committed CMake
executable driving the real `TuiState` through the imtui text backend
(same frame pipeline as `main.cpp`, no terminal; 80x24; a 13-session seed
with distinct filterable titles). Scenarios: type/narrow/rank, no-match
indicator, Esc clear, Enter top-match select, row click, busy-defer +
flush (last wins), snapshot refresh with an active filter, rename-box close,
Esc priority (rename wins), rename+filter coexistence (filter restore),
Enter-on-active no-op + clear, multi-byte + whole-word highlight, score
clamp, >64-byte truncation, focus (does not steal from the main query
input), nav stability (arrow keys move neither the active id nor the nav
state while a filter is active), seq-bump re-apply after clear (no
stale-text resurface), close/reopen + pipeline-occupancy persistence,
empty snapshot, and log-line verification. Build/run (glibc environment):

```
make -f tui.mak
cd build/tui
./test_session_filter_smoke < /dev/null
# (or: TERM=xterm-256color COLUMNS=80 LINES=24 timeout 300 ./test_session_filter_smoke < /dev/null)
```

Exits 0 on pass, non-zero on the first failed assertion (full grid dump on
stderr). The binary statically links imtui/ncurses and needs glibc to
execute — run it in a glibc environment (dev box / glibc CI), not a musl
sandbox. `test_session_fuzzy` (matcher unit test, stdlib-only) and the
`llmfun_tui --frames N` dry-run round out the headless coverage.

---

## Main Event Loop (`main.cpp`)

The `main.cpp` file provides a lightweight test/dry-run for the TUI (not the main application entry point). It follows a standard ImGui frame loop:

1. **Initialization**: create the state via `tuiCreateState()`, attach a backend via `tuiInit(state, mode)` (`--tui` / `--gui`; the standalone default is the terminal backend so the headless modes never open a window), set initial status text and welcome message.
2. **Frame loop**:
   - `tuiBackendNewFrame(state)` — processes backend input (terminal input or window events) and starts a new ImGui frame
   - `tuiRender(state)` — renders all three regions, handles keyboard shortcuts. Returns `0` to exit (Ctrl+C, or a closed GUI window).
   - **Submission check**: If `tuiIsSubmitReady(state)`, extract the query via `tuiGetSubmitQuery()`, echo it to output, and reset submit flag.
   - `tuiBackendRender(state)` — renders the ImGui frame through the active backend (terminal grid draw or GL present)
3. **Shutdown**: Call `tuiDestroyState(state)` — it shuts the backend down and frees the state (v4 has no separate screen object).

### Headless Smoke Mode

`main.cpp` accepts `--frames N` (or `--smoke`, an alias for 30 frames) for
headless CI runs: it seeds the session panel with a sample
`tuiSetSessionList` snapshot, runs the normal frame loop for exactly N
frames, verifies the session action queue is empty (`tuiIsSessionActionReady`
== 0 and `tuiGetSessionAction` returns the None sentinel), prints
`smoke ok: ...`, and exits 0. Without the argument the interactive loop is
unchanged. `--frames` requires a non-negative integer; usage errors exit 2.
`--gui --frames N` runs the window backend for N frames and needs a display
(run it under `xvfb-run` where available).
The committed `test_session_filter_smoke` harness (see
[Filter Input](#filter-input) above) covers the session filter panel
flows headlessly the same way.

### Keyboard Shortcuts

| Shortcut | Action | Condition |
|----------|--------|-----------|
| `Ctrl+C` | Exit TUI (return `false`) | Anywhere |
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

---

## Rendering Internals: Vendored imtui / imgui Patches

The TUI renders through a patched fork of **imtui** (`llmfun/vendor/imtui`:
the text rasteriser in `src/imtui-impl-text.cpp`, the ncurses output in
`src/imtui-impl-ncurses.cpp`) built on a vendored **Dear ImGui**
(`vendor/imtui/third-party/imgui`, currently 1.92.9b). The patches are marked
with `LLMFUN PATCH` comments so they can be re-applied after an upstream sync.
They exist because the 1-cell terminal grid exposes assumptions made for pixel
backends (coordinates, stroke widths, half-cell sizes), and because a terminal
keeps what was painted.

### The `ImTui_TextEncodingActive` guard

One imgui build serves both the terminal backend and pixel backends (e.g.
OpenGL): the runtime flag `bool ImTui_TextEncodingActive` (declared
`IMGUI_API` in `imgui/imgui.h`, defined `false` in `imgui/imgui_draw.cpp`)
tells the imgui core that the imtui text path is the active renderer.

- `ImTui_ImplText_Init()` sets it **true**; `ImTui_ImplText_Shutdown()`
  resets it to **false** (restored 2026-10-04 — one imgui build now serves
  both the text and the pixel backend, so a stale `true` would corrupt pixel
  rendering after a text-backend shutdown).
- **Convention:** a patch that exists only because of the text grid must be
  conditional on the flag, and its flag-false branch must keep the upstream
  expression verbatim. Guarded today: `imgui_draw.cpp` (imtui vertex-color
  encoding, unit-cell glyph quads, zero-width/emoji folding, fine-clip
  branch, filled-triangle guard), `imgui_widgets.cpp` (caret line row and
  caret rect) and `imgui.cpp` (`RenderNavCursor`, see below).
- App code that must behave differently per profile uses the runtime
  predicate `tuiIsTextGrid()` (defined as `ImTui_TextEncodingActive` in
  `tui.h`/`tui.cpp`) rather than the core guard — see "Graphical backend
  (GLFW + OpenGL3)" for the gated items; the ncurses-side patches live in an
  always-imtui file.

### Cell-grid rendering model

- **Mapping:** a quad or rect is mapped to the cell containing its pen
  coordinate `(x, y)` — a pen at `x` paints column `x`. Text, rects, caret
  and mouse coordinates all follow this rule (see the off-by-one fix below).
- **Persistence:** the backend paints a persistent screen — cells stay until
  something repaints them. A widget that shows for a single frame leaves its
  cells behind, so transient states (focus rings, streaming banners) must be
  suppressed on every frame, not just in the steady state.
- **Metrics:** the font is a 1px bitmap (`SizePixels = 1`), glyphs occupy
  `[pen.y-0.5, pen.y+0.5]`, `FramePadding = (1, 0)`, `ItemSpacing = (1, 0)`;
  caret and scrollbar geometry are tuned for those values.

### Fixed rendering defects (2026-10-03)

Commits `df96b90` (`tui: fix render bugs`) and `fc7bcf7` (`tui: fix side
scrollbar`), plus the nav-cursor follow-up.

**Text/background one-cell offset.** Every text run painted one cell right of
where imgui placed it, while background rects painted true. Normally
invisible, it showed wherever text abutted the left edge of a background
rect: the `New` button (3-cell label in a 3-cell rect) rendered as `" Ne"w`
with the last glyph outside the grey cell, and the input row's
`Send`/`Prev`/`Next` sat shifted. Cause: the text backend mapped glyph quads
to `trunc(pen_x) + 1` (`int xx = (x) + 1;` — the quad's six-vertex average is
`pen + 0.5`, so the truncation already yields `pen`), while rects, caret and
mouse mapping use the containing cell; the caret patch carried a matching
`+ ImVec2(1, 0)` compensation. Fix: `int xx = (x);` and drop the caret
compensation in the same change; then re-create the intended one-cell inset
explicitly in the affected widgets — `renderButton` / `renderTitleButton`
draw labels at `p0 + FramePadding.x` (like imgui's own buttons),
`sameLineAfterButton` subtracts `FramePadding.x` again, tight button widths
became `label + 2*FramePadding` (`New` 3→5, `Clear` 5→7, `Rename` 6→8), and
session previews / the "no matches" line are inset by `FramePadding.x`.
Visible consequence to remember: plain text now sits at its true coordinate,
so the menu bar, the `Sessions`/`Pipeline` separators, message and header
text, and the status line (column 0) each moved one column left; buttons and
rows keep their previous inset look.

**Output horizontal scrollbar removed; input/status layout.** (a) A long grey
line at the bottom of the chat output appeared for messages with code lines
wider than the viewport: `llm_output` had
`ImGuiWindowFlags_HorizontalScrollbar`, and unwrapped overwide content
surfaced the h-scrollbar as a full-width strip. The flag is gone from
`llm_output` and `llm_log`; overwide content is clipped instead. (b) The
single-line input frame overlapped the status row and lost its last frame
row: the output reserve was computed from `inputBufLines`, but the multiline
input's frame is an imgui child clamped to the 4-row hard minimum
(`IMGUI_WINDOW_HARD_MIN_SIZE`), so a 1-line input is 4 rows tall with its
bottom edge on the display bottom (where the renderer's clip clamp swallows
the frame's last row). Both sizes are now derived from the effective height
(see the Three-Region Layout section): `inputHeight` (4-row minimum) is the
widget size and `DisplaySize.y - 2 - inputHeight` the output reserve; the
input frame occupies `H-1-inputHeight .. H-2` (H-5..H-2 for the default
frame) and the status line is pinned to `H-1`.

**Vertical scrollbar invisible.** (a) `ImTui_ImplText_Init` set
`ScrollbarSize = 0.5f`, and the strip `[col+0.5, col+1)` rounds to 0 or 1
columns purely by parity, so the output child's `x = 78.5` scrollbar never
painted a cell; `ScrollbarSize` is now `1.0f` (one deterministic column).
(b) The patched `ScanLine` DDA stepped x only when its error counter crossed
`m`, which for near-vertical edges (`|dy| >> |dx|`) collapsed the edge onto
one column: the middle rows of a 1-cell-wide rect got zero-width spans, so
even a full-cell scrollbar stayed black in the middle. `ScanLine` now
evaluates the crossing `x` for each row (`x = x1 + (x2-x1)*t`); horizontal
edges contribute to their single row. (c) The base scrollbar colours were
semi-transparent near-blacks that blend into the black background;
`applyTheme()` (`tui.cpp`) now uses solid greys — `ScrollbarBg` (0.15, alpha
1.0), `ScrollbarGrab` (0.45, 1.0), hovered 0.55, active 0.65 — assigned after
the Button/NavCursor aliases so those keep the dimmer grey, and
`GrabMinSize = 3.0` (backend `GrabMinSize` is `1.0f`; a sub-cell grab is a
0-1 row sliver). Result: a full-height track column at `x = W-2` (ANSI 235)
with a 3-cell grab (ANSI 242) — e.g. column 78 at 80x24, column 118 at
120x30. This also fixed `imtui_utf8_grid_test` check (d) (the scrollbar
column), which had been failing since the imgui update.

**Keyboard-nav cursor — the "long grey horizontal bar".** A two-cell-tall
grey band sat a few rows above the input frame, appearing "on" whatever line
was there (the startup help line with `/code <query>`, a thinking trace's
`End ----` separator); a mouse click hid it, keyboard/Tab brought it back.
Root cause: ImGui's *nav cursor* (keyboard focus highlight) for the input,
which the app focuses programmatically (the input starts with
`isSubmitted = true`, so the first frame calls `SetKeyboardFocusHere`;
submissions and the Chat menu refocus it the same way). Upstream
`RenderNavCursor` draws a ring *outside* the item (expanded 4 px, stroked
2 px thick) — on the 1-cell grid that is a 2-cell band 4 cells away from the
item; and because the screen persists, one frame with the cursor visible
leaves the bar. Fix, two layers: (1) `imgui.cpp` `RenderNavCursor`, guarded
by `ImTui_TextEncodingActive` — cell-grid geometry (`thickness = 1`,
`distance = 0`: a 1-cell border hugging the item) and no cursor for the
*active* item (`g.ActiveId == id`, unless `AlwaysDraw`), since the caret /
active colours already show that state; (2) `tui_chat.cpp`
`suppressInputRowNavCursor()` keeps `g.NavCursorVisible = false` while nav or
edit focus is on the input row (`##user_input`, its `##Child` id,
`##llm_send`, `Prev`, `Next`), called before the output area, before and
after the `InputTextMultiline` call (the widget can re-show the cursor during
its own call) and after the button group; focus on any other widget still
shows its cursor. Triage note: the ring used the dim `ScrollbarGrab` alias
(0.34 @ 0.54 alpha → ANSI 236), visually indistinguishable from the input
frame's `FrameBg` (0.16 → ANSI 235) — hence "the same colour as the input
field" in the bug report. Earlier suspects that proved wrong: the output
horizontal scrollbar (a real but different defect, removed above) and the
hidden-label header rows (an experiment targeting those was reverted).

### Verifying TUI rendering changes

- Build the C++ side with `make -f tui.mak build/tui` (CMake into
  `build/tui`); the D application links `build/tui/libllmfun_tui_all_lib.a`
  (`dub build --config=application`).
- Headless checks in `build/tui/` (run with `</dev/null`; ncurses needs a
  glibc environment and, for the ncurses test, `TERM` set):
  `imtui_utf8_grid_test` (grid, fold and scrollbar checks),
  `imtui_arrow_text_channel_test`, `imtui_ncurses_fold_redraw_test`,
  `test_session_filter_smoke`, `test_tui_maxwidth`, `test_clear_output_state`,
  `test_session_fuzzy`, `test_tui_api_v4` (the v4 C-API contract: None-state
  no-ops, the NULL-vs-None `tuiRender` asymmetry, retry-after-failure, and
  the Auto/TTY fallback matrix through child processes).
- PTY: `probe_margin` (`cpp_tui/probe_margin.c`) runs the real binary on a
  pseudo-terminal and asserts no cell is written at/after
  `LLMFUN_TUI_MAX_WIDTH`; `llmfun_tui --frames N` runs the standalone UI
  headlessly — `--tui` / `--gui` pick the backend, the default is the
  terminal, and `--gui --frames N` needs a display (run it under `xvfb-run`
  where available; `MESA_GL_VERSION_OVERRIDE=2.1` exercises the GL 2.1 +
  GLSL 120 retry path).
- GUI smoke on a headless runner (opt-in; NOT part of the default
  `make`/`dub test` gates): `sh cpp_tui/test/xvfb_gui_smoke.sh
  build/tui/llmfun_tui` runs the standalone driver under `xvfb-run` with
  `LIBGL_ALWAYS_SOFTWARE=1` (llvmpipe) as `--gui --frames 30`; exit 0 =
  pass. Where Xvfb is absent it prints `SKIP: xvfb-run not available` and
  exits 77 — skipped, not failed (install it with `apt-get install xvfb`);
  a missing binary skips the same way.
- Init order matters: `ImTui_ImplText_Init()` overwrites style values
  (scrollbar size/grab minimum, the nav-cursor colour — it sets it
  transparent), so `applyTheme()` must run after it; a harness that
  initialises in the other order does not see the shipped style.
- When a stray bar or line is suspected again, check the frame's nav state
  first (`NavCursorVisible`, `ActiveId`, `NavId` — a nav cursor shows as a
  ring around the focused item) before suspecting content or scrollbars. The
  ad-hoc visual probe used during the investigation (a grid dump of painted
  cells with background colours plus the frame's nav state) is not committed
  to the repo.
