# imtui Tests

This directory contains the test suite for `llmfun/vendor/imtui` and its dependencies.

## Test Suite Overview

| Test | File | Description |
|------|------|-------------|
| Code Block Tests | `test_code_block.py` | 16 tests for fenced code block parsing in `imgui_markdown` |
| Inline Code Tests | `test_inline_code.py` | 10 static-analysis tests for inline code span parsing |
| Inline Code Runtime Harness | `test_inline_code_runtime.cpp` | Rendered byte-stream checks for every edge-case matrix row |
| Unicode Cell-Width Unit Test | `test_wcwidth.cpp` | Verifies `imtui_wcwidth` vectors (emoji-presentation = 2, zero-width format/VS/combining = 0) |
| UTF-8 Grid Regression Test | `test_utf8_grid.cpp` | Headless grid-invariant regression test for the UTF-8 width fix (CMake target `imtui_utf8_grid_test`) |
| ncurses Fold-Row Rewrite Test | `test_ncurses_fold_redraw.cpp` + `test_ncurses_fold_redraw.py` | PTY-level regression test for the VS16-fold row-rewrite corruption (CMake target `imtui_ncurses_fold_redraw_test`; the Python driver replays the raw ncurses stream against a VS16-clustering terminal model) |
| PTY Verification | `pty_utf8_check.py` | Terminal-level gate: parses a `tmux capture-pane -p -e` capture and asserts scrollbar-block alignment for the six bug-report table rows (see `utf8_verification.md`) |

## Prerequisites

- **build-essential** (gcc, g++, make)
- **cmake** (>= 3.10)
- **libncurses-dev** (for imtui-ncurses backend)
- **Python 3** (for the test scripts)

Install on Debian/Ubuntu:
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libncurses-dev
```

## Quick Start

From the project root directory:

```bash
python3 llmfun/vendor/imtui/test/build_test.py
```

This will build `llmfun_tui` (verifying compilation) and run all tests.

## Build and Run Separately

```bash
# Build only (compilation check)
python3 llmfun/vendor/imtui/test/build_test.py --build

# Run only (tests)
python3 llmfun/vendor/imtui/test/build_test.py --run
```

## Test Details: Inline Code Tests

The `test_inline_code.py` file contains 10 static-analysis tests that pin the
structural invariants of the inline code span state machine: the `INLINE_CODE`
enum value, the `CodeSpan` struct, the lone-backtick opening guard, the
active-span literal skip and newline abort, the close-branch ordering, the
`RenderLine` branch ordering, default-callback push/pop balance, the TUI
callback case, brace balance, and the `CodeSpan` reset sites.

The `test_inline_code_runtime.cpp` harness closes the gap that static tests
cannot: bookkeeping off-by-ones that silently duplicate or drop text. It
compiles `imgui_markdown.h` standalone against a fake `ImGui` namespace whose
stubs record every rendered byte range, feeds each edge-case matrix row
(`plan/system_design.md` section 5.6) plus regression rows through
`ImGui::Markdown`, and asserts the exact rendered byte stream and the
`INLINE_CODE` callback pair ranges. `build_test.py` compiles it with `g++`
and runs it as part of the test run.

## Manual TUI Smoke Test (Inline Code)

The runtime harness cannot exercise the real terminal renderer. Once per
change, verify visually:

1. Build and run `llmfun_tui`.
2. Send a canned assistant message containing the edge-case matrix, e.g.:

       text `code` here and `*not em*` stays literal.
       Adjacent spans: `a` `b`. Unclosed span `oops
       ```cpp
       int x;
       ```
       Emphasis still works: *italic* and **strong**.

3. Confirm:
   - Closed spans render in the `MarkdownStyle::inlineCode` color with
     literal backtick markers around them (the terminal has no monospace
     face).
   - Markdown characters inside a span (`*not em*`) render literally.
   - The unclosed span's opening backtick renders as literal text.
   - Fenced code blocks render unchanged (gray text, no markers).
   - Emphasis markers `_`/`**` still render.
   - No text is missing or duplicated.

## Test Details: Unicode Cell-Width Unit Test

The `test_wcwidth.cpp` unit test pins the `imtui_wcwidth` /
`ImFontIMTuiCellWidth` table in `imgui_draw.cpp`. It defines `IMTUI` and
`IMGUI_USE_WCHAR32` (exactly like the `llmfun_tui` build) and includes
`imgui_draw.cpp` by TU inclusion, so the static width helpers are tested
without exporting new production symbols. `build_test.py` compiles it with
`g++ -std=c++11 -O0 -ffunction-sections -Wl,--gc-sections` (unreferenced
imgui functions are garbage-collected) and runs it; it exits non-zero if
any width vector mismatches. Expected widths: emoji-presentation codepoints
(✅ U+2705, ❓ U+2753, ⭐ U+2B50, ⬛ U+2B1B, ...) are 2, zero-width
format/VS/combining codepoints (U+FE0F, U+200D, U+0301, ...) are 0.
This test fails against the pre-fix width table
(✅/❓/⭐ measured as 1, VS16/ZWJ/combining as 1, regional indicators as 2).

## Test Details: UTF-8 Grid Regression Test

The `test_utf8_grid.cpp` test renders fixed text rows through the REAL
vendored imgui + imtui text backend into an `ImTui::TScreen` grid and asserts
the grid-vs-terminal-width invariants restored by the UTF-8 width fix
(implementation plan Task 3; `plan/system_design.md` §7 Task 3). Unlike
`test_inline_code_runtime.cpp`, it is not a fake-ImGui stub: it links the
actual `imtui` + `imgui-for-imtui` libraries exactly like `llmfun_tui` (whose
imgui TU is compiled with `IMTUI` + `IMGUI_USE_WCHAR32`). It is built by the
normal cmake phase (target `imtui_utf8_grid_test` in `llmfun/cpp_tui/
CMakeLists.txt`) and executed by `build_test.py` from the build directory; it
runs headless (no ncurses, no PTY) and is CI-safe.

The fixture mirrors the bug report's six-row markdown table (shift pattern
0, 0, +1, +1, −1, +1): two plain rows, three rows with ✅/❓, one with ⚠️
(U+26A0 + U+FE0F), plus a probe row "a✅b" and a row with U+0301/U+200D.
Assertions:

- (a) per-row grid extent (rightmost text cell) equals
  `base_col + oracle_sum(row) - 1`, where `oracle_sum` is computed by an
  INDEPENDENT width oracle (adapted Kuhn wcwidth + the pinned emoji-data.txt
  15.1 emoji-presentation list — not a call into `ImFontIMTuiCellWidth`).
  **Fails pre-fix**: ✅/❓ rows are one cell short, the ⚠️ row one cell long.
- (b) the cell after a wide emoji lands at `base_cell + 2`: in "a✅b" the 'b'
  lands at 'a' column + 3 and the ✅ cell reports `chwidth == 2`. **Fails
  pre-fix** ('b' lands at +2).
- (c) no zero-width codepoint from the corpus (U+FE0F, U+200D, U+0301)
  appears as any cell's `ch`, `ch2` holds only an allowed continuation
  (VS16 or a combining mark — never ZWJ), every non-space text cell has
  `chwidth >= 1`, and in default mode `ch2 == 0` everywhere.
- (d) scrollbar cells occupy one constant column across all six table rows,
  and (e) content cells never overwrite the scrollbar column — post-fix
  guards only (they pass pre-fix by construction: the TScreen grid is
  internally consistent even when broken; only the terminal stream diverges,
  which the Task 4 PTY capture observes).
- (f) P3 folding mode only (`build_test.py` runs the binary a second time
  with `LLMFUN_IMTUI_EMOJI_PRESENTATION=1`): row 1's `e` cell carries
  `ch2 == U+0301` (mark merged, width unchanged) and row 6's U+26A0 cell
  carries `ch2 == U+FE0F` with `chwidth == 2` (VS16 promotes the
  text-default base to emoji presentation); per-row extents follow the
  folding-aware oracle (the promoted pair advances the pen 2 cells).

The "fails pre-fix" behavior was demonstrated by building the test against a
pre-fix reconstruction of `imgui_draw.cpp` (see `scratch/make_prefix_sim.py`;
the workspace has no git history, so the verbatim pre-fix file is not
available). Recorded outputs: `scratch/prefix_run.txt` (8 failures: (a) for
the ✅/❓/⚠️ rows, (b), (c) with 3 leaked width-0 cells; (d)/(e) pass by
construction) and `scratch/postfix_run.txt` (all checks pass).

## Test Details: ncurses Fold-Row Rewrite Test

The `test_ncurses_fold_redraw.cpp` binary (CMake target
`imtui_ncurses_fold_redraw_test`) drives the REAL ncurses backend
(`ImTui_ImplNcurses_DrawScreen`) with a markdown-table-style row containing
a folded VS16 pair (`ch = U+26A0, ch2 = U+FE0F, chwidth = 2`) across 3 or 4
frames of content changes. The Python driver
`test_ncurses_fold_redraw.py` (wired into `build_test.py`) spawns the
binary on a real PTY, replays the raw ncurses output stream against a model
of a VS16-clustering terminal (the pair renders 2 cells wide), and asserts
the final row content:

- 3 frames (pair present, row rewritten twice): row 0 must read
  `" | ⚠ | foobarZ!"` with the pair cell at column 3 still 2-wide.
  Pre-fix the frame-2/3 diff patches land 1 column left — `Z` overwrites
  the `r` and the trailing `|` lingers as a shadow character — so the row
  reads `" | ⚠ | foobaZ!|"` and the assertion fails.
- 4 frames (adds the fold-removal transition, pair → plain ⚠): row 0 must
  read `" | ⚠  | foobarZ!"` with the cell back to width 1. This is a
  post-fix guard rather than a pre-fix failing check: removing a fold
  shifts the entire virtual tail, which forces a wide rewrite via
  absolute addressing that self-heals even pre-fix — the prev-grid check
  in the fix is insurance that keeps this transition on the same
  full-line redraw path.
- Both runs: the control row without folds keeps `"foobar"` untouched.

This is the only layer where the bug is observable: the TScreen grid is
internally consistent pre-fix (the headless grid test cannot see it), and
tmux counts the pair as 1 cell like ncurses' own model (a tmux capture
cannot reproduce it either). The recorded "fails pre-fix" demonstration:
temporarily replacing the `wredrawln` call with a no-op and rebuilding
yields the `"foobaZ!|"` corruption above; restoring it makes both runs
pass.

## Test Details: PTY Verification (Terminal-Level Gate)

The `pty_utf8_check.py` script is the only gate that observes the bug the
user actually reported — the real terminal stream. It runs `llmfun_tui`
(seeded with the six-row table) under tmux, captures the pane with `tmux
capture-pane -p -e` (the `-e` keeps SGR attributes: the scrollbar block is
spaces with a background, so a plain `-p` capture cannot see it), and
asserts: (1) all six table rows are found; (2) the rightmost bg-colored cell
(scrollbar block) column is identical across all six rows; (3) the ⚠ row
contains no U+FE0F and the cell right after ⚠ is the row's next visible
character; (4) the plain-ASCII control row is unchanged. The cell grid is
built with an independent width oracle (adapted Kuhn wcwidth + the pinned
emoji-data.txt 15.1 emoji-presentation list), so the check is not
self-referential. It is a manual/PTY procedure (not wired into
`build_test.py`); the full step-by-step procedure, environment, and
recorded before/after results live in `utf8_verification.md`.

## UTF-8 Width Model (invariants and limitations)

The imtui terminal grid and the ncurses stream stay aligned because both use
one width source of truth: the `imtui_wcwidth` table in
`third-party/imgui/imgui/imgui_draw.cpp` (width classes 0 / 1 / 2; pinned to
emoji-data.txt 15.1 + Markus Kuhn's wcwidth; see the LLMFUN PATCH comment
above the table). Rendering invariants:

- **Quad-width-1**: every glyph quad emitted by `ImFont::RenderText` is
  exactly 1 column wide; wide characters advance the pen by their cell width.
- **`avg + 1` cell mapping**: the text backend (`imtui-impl-text.cpp`) maps a
  quad at pen column `x` to grid column `x + 1` (six-vertex average + 1),
  keeping column 0 as the window margin / scrollbar area; its `lastCharX`
  dedup pushes duplicate quads to `lastCharX + 1`.
- **Vertex-color encoding**: quad vertex 1's color carries the codepoint
  (becomes `TCell::ch`), vertex 2's color the cell width (becomes
  `TCell::chwidth`), vertex 3's color the zero-width continuation folded
  into the base (becomes `TCell::ch2`; 0 = none, P3 Task 6); the consumer is
  `ImTui_ImplText_RenderDrawData` (`cell.ch = col1; cell.chwidth =
  (uint8_t)col2; cell.ch2 = ...`).
- **Width-0 skip rule**: zero-width codepoints (VS16 U+FE0F, ZWJ U+200D,
  format/control chars, combining marks) never produce a grid cell — ⚠️
  renders as narrow ⚠, guaranteed aligned on all terminals.
- **P3 folding rule (opt-in, `LLMFUN_IMTUI_EMOJI_PRESENTATION=1`)**:
  instead of skipping, RenderText folds the continuation into the previously
  emitted quad at the vertex level (no second quad, so the `lastCharX` dedup
  never fires): VS16 after a text-default emoji base (U+26A0 etc.) promotes
  the base cell to width 2 and adds one extra pen advance; a combining mark
  attaches with the width unchanged. The ncurses backend emits ch + ch2 as
  one `addwstr` run before the `chwidth` cursor compensation so the terminal
  clusters the pair. **Fold-row rewrite fix**: ncurses' internal wcwidth
  counts a folded VS16 pair as 1 cell (grid counts 2), so ncurses' virtual
  screen is 1 column short per pair; rows whose new or previous grid
  contains a fold are therefore forced to a full-line redraw (`wredrawln`)
  instead of ncurses' diff-based patching, which would land 1 column off on
  VS16-clustering terminals (the reported " foobar" -> " ffoobar" bug).
  On terminals that do NOT cluster VS16 (real xterm, tmux) the ⚠️ row still
  shifts −1 (the terminal renders the pair narrow: an unfixable
  terminal-capability mismatch). The combining-mark merge has no width
  change (grid = ncurses = terminal = 1) and is safe. Default is OFF —
  the P0 behavior above stays guaranteed on every terminal.
- **Cursor compensation**: the ncurses backend (`imtui-impl-ncurses.cpp`)
  advances the terminal cursor by exactly `chwidth` per cell, skipping
  `chwidth - 1` grid columns for wide cells.

Known limitations: grapheme clusters
(ZWJ sequences, flag pairs, keycaps, skin tones) remain misaligned; combining
marks are dropped in P0 (é renders as "e") and merged only with the P3 env
gate; emoji presentation via VS16 is opt-in P3 (`LLMFUN_IMTUI_EMOJI_
PRESENTATION=1`; fold-row rewrites are kept aligned by the `wredrawln`
full-line redraw in the ncurses backend — see above); narrow-emoji
terminals show the opposite +1 shift on ✅ rows, mitigated by
`LLMFUN_IMTUI_EMOJI_WIDTH=1` (forces the Emoji_Presentation=Yes class to
width 1). Terminal-level verification (incl. the P3 folding captures) is
documented in `utf8_verification.md`.

Note on the emscripten backend (`imtui-impl-emscripten.cpp`): it is not part
of the llmfun build (EMSCRIPTEN is off), it is unbuildable in this
environment (no emscripten toolchain), and its `get_screen()` has a
pre-existing compile break unrelated to this work — it bit-puns a `TCell`
as an int (`buffer[idx] = cell & 0x000000FF;`), which cannot compile
regardless of the P3 `TCell::ch2` addition (the addition itself is purely
additive). A follow-up fix or removal of the emscripten backend is out of
scope here.

## Test Details: Code Block Tests

The `test_code_block.py` file contains 16 tests covering all fenced code block scenarios:

| # | Test | What it verifies |
|---|------|-----------------|
| 1 | Basic block with ID | `` ```cpp `` fence with language identifier |
| 2 | Block without ID | `` ``` `` fence with no language identifier |
| 3 | Empty block | Opening + closing fence with no content |
| 4 | Unclosed block | Code block at EOF without closing fence |
| 5 | Emphasis literal | `**bold**` inside block renders literally |
| 6 | Link literal | `[link](url)` inside block renders literally |
| 7 | Multi-line block | Multiple lines each rendered on separate line |
| 8 | Multiple blocks | Two consecutive code blocks back-to-back |
| 9 | Block + heading | Code block followed by `# Heading` |
| 10 | Block + list | Code block followed by `* list item` |
| 11 | Block + text | Code block followed by plain paragraph |
| 12 | 4+ backticks | ```` fence with triple backticks inside |
| 13 | Whitespace around ID | `` ```  python  `` trims whitespace |
| 14 | EOF no newline | Block at end of input without trailing `\n` |
| 15 | Compilation | imgui_markdown.h compiles as part of llmfun_tui |
| 16 | Stack balance | PushTextWrapPos/PopTextWrapPos are balanced |

## Approach

The tests use source code analysis of `imgui_markdown.h` to verify parsing logic.
This works because imgui_markdown is a single-header library whose behavior is
determined entirely by its source. A compilation check verifies the header
integrates correctly with the full build.

## Directory Structure

```
vendor/imtui/test/
├── CMakeLists.txt          # CMake stub (integration placeholder)
├── build_test.py           # Build & run script
├── test_code_block.py      # Test cases (16 tests)
├── test_inline_code.py     # Inline code static-analysis tests (10 tests)
├── test_inline_code_runtime.cpp  # Runtime byte-stream harness (fake ImGui)
├── test_wcwidth.cpp      # Unicode cell-width unit test (TU inclusion)
├── test_utf8_grid.cpp    # Headless grid-invariant regression test (real imgui)
├── pty_utf8_check.py     # Terminal-level PTY gate (tmux capture-pane -e parser)
├── utf8_verification.md  # PTY verification procedure + recorded before/after results
└── README.md               # This file
```
