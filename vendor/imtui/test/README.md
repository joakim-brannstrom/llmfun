# imtui Tests

This directory contains the test suite for `llmfun/vendor/imtui` and its dependencies.

## Test Suite Overview

| Test | File | Description |
|------|------|-------------|
| Code Block Tests | `test_code_block.py` | 16 tests for fenced code block parsing in `imgui_markdown` |
| Inline Code Tests | `test_inline_code.py` | 10 static-analysis tests for inline code span parsing |
| Inline Code Runtime Harness | `test_inline_code_runtime.cpp` | Rendered byte-stream checks for every edge-case matrix row |

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
└── README.md               # This file
```
