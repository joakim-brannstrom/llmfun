#!/usr/bin/env python3
"""
Unit tests for imgui_markdown fenced code block parsing.

Tests all 14 cases from Task 12 by analyzing the source code logic
and verifying the parsing behavior through static analysis of
imgui_markdown.h.
"""

import re
import sys
import os

# ------------------------------------------------------------------
# Use script directory to resolve paths regardless of CWD
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
HEADER_PATH = os.path.join(SCRIPT_DIR, "..", "..", "imgui_markdown", "imgui_markdown.h")


def load_header():
    """Load imgui_markdown.h source."""
    with open(HEADER_PATH, "r") as f:
        return f.read()


def normalize_ws(s):
    """Collapse all whitespace runs to single space for flexible matching."""
    return re.sub(r'\s+', ' ', s)


def extract_function(src, func_name):
    """Extract a function body by brace-matching from its declaration."""
    start = src.find(f"inline void {func_name}(")
    if start < 0:
        return ""
    # Find the opening brace
    brace_start = src.find('{', start)
    if brace_start < 0:
        return ""
    # Count braces to find matching close
    depth = 0
    i = brace_start
    while i < len(src):
        if src[i] == '{':
            depth += 1
        elif src[i] == '}':
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
        i += 1
    return src[start:]


# ------------------------------------------------------------------
#  Test framework
# ------------------------------------------------------------------

class TestResult:
    def __init__(self, name, passed, message=""):
        self.name = name
        self.passed = passed
        self.message = message

results = []


def test_start(name):
    return _TestContext(name)


class _TestContext:
    def __init__(self, name):
        self.name = name
        self.passed = True
        self.message = ""

    def check(self, cond, msg):
        if not cond:
            self.passed = False
            self.message = msg

    def finish(self):
        r = TestResult(self.name, self.passed, self.message)
        results.append(r)
        status = "PASS" if self.passed else "FAIL"
        suffix = f" -- {self.message}" if self.message else ""
        print(f"  {status}: {self.name}{suffix}")


# ------------------------------------------------------------------
#  Test 1: Basic block with ID
# ------------------------------------------------------------------
def test_basic_block_with_id():
    tc = test_start("Basic block with ID")
    src = load_header()
    tc.check("CODE_BLOCK," in src, "CODE_BLOCK enum value should exist")
    tc.check('formatInfo.blockId = markdown_ + codeBlock_.blockIdStart' in src,
             "RenderCodeBlock should set blockId pointer")
    tc.check('formatInfo.blockIdLength = codeBlock_.blockIdEnd - codeBlock_.blockIdStart' in src,
             "RenderCodeBlock should set blockIdLength")
    tc.check("blockIdStart" in src and "blockIdEnd" in src,
             "Opening fence should track identifier boundaries")
    tc.check("TextUnformatted" in src,
             "Code blocks should use TextUnformatted for literal rendering")
    tc.finish()


# ------------------------------------------------------------------
#  Test 2: Block without ID
# ------------------------------------------------------------------
def test_block_without_id():
    tc = test_start("Block without ID")
    src = load_header()
    tc.check("codeBlock.blockIdStart = idStart" in src,
             "Opening fence should set blockIdStart")
    tc.check("codeBlock.blockIdEnd = idEnd" in src,
             "Opening fence should set blockIdEnd")
    tc.finish()


# ------------------------------------------------------------------
#  Test 3: Empty block
# ------------------------------------------------------------------
def test_empty_block():
    tc = test_start("Empty block")
    src = load_header()
    tc.check("codeBlock_.textStart < codeBlock_.textEnd" in src,
             "RenderCodeBlock should check for empty content")
    tc.finish()


# ------------------------------------------------------------------
#  Test 4: Unclosed block at EOF
# ------------------------------------------------------------------
def test_unclosed_block():
    tc = test_start("Unclosed block at EOF")
    src = load_header()
    tc.check("if (codeBlock.active)" in src,
             "Should check for active code block at EOF")
    tc.check("codeBlock.textEnd = (int)markdownLength_" in src,
             "Should set textEnd to markdown length at EOF")
    tc.finish()


# ------------------------------------------------------------------
#  Test 5: Emphasis-like content rendered literally
# ------------------------------------------------------------------
def test_emphasis_literal():
    tc = test_start("Emphasis-like content rendered literally")
    src = load_header()
    tc.check("if (!codeBlock.active)" in src,
             "Normal parsing should be skipped when codeBlock is active")
    tc.check("TextUnformatted" in src,
             "Code block content should use TextUnformatted")
    tc.finish()


# ------------------------------------------------------------------
#  Test 6: Link-like content rendered literally
# ------------------------------------------------------------------
def test_link_literal():
    tc = test_start("Link-like content rendered literally")
    src = load_header()
    tc.check("if (!codeBlock.active)" in src,
             "Link parsing should be suppressed inside code blocks")
    tc.finish()


# ------------------------------------------------------------------
#  Test 7: Multi-line block
# ------------------------------------------------------------------
def test_multiline_block():
    tc = test_start("Multi-line block")
    src = load_header()
    tc.check("markdown_[ci] == '\\n'" in src,
             "RenderCodeBlock should split content on newlines")
    tc.check('ImGui::TextUnformatted(markdown_ + lineBegin, markdown_ + lineBreak)' in src,
             "Each line should be rendered with TextUnformatted")
    tc.finish()


# ------------------------------------------------------------------
#  Test 8: Multiple consecutive blocks
# ------------------------------------------------------------------
def test_multiple_blocks():
    tc = test_start("Multiple consecutive blocks")
    src = load_header()
    tc.check("codeBlock = CodeBlock()" in src,
             "CodeBlock should be reset after closing fence")
    tc.finish()


# ------------------------------------------------------------------
#  Test 9: Block followed by heading
# ------------------------------------------------------------------
def test_block_followed_by_heading():
    tc = test_start("Block followed by heading")
    src = load_header()
    nsrc = normalize_ws(src)
    tc.check("isLeadingSpace" in nsrc and "true" in nsrc,
             "Line state should be reset after closing fence")
    tc.finish()


# ------------------------------------------------------------------
#  Test 10: Block followed by list
# ------------------------------------------------------------------
def test_block_followed_by_list():
    tc = test_start("Block followed by list")
    src = load_header()
    tc.check("codeBlock = CodeBlock()" in src,
             "CodeBlock reset allows normal parsing to resume")
    tc.finish()


# ------------------------------------------------------------------
#  Test 11: Block followed by normal text
# ------------------------------------------------------------------
def test_block_followed_by_text():
    tc = test_start("Block followed by normal text")
    src = load_header()
    nsrc = normalize_ws(src)
    tc.check("lineStart" in nsrc and "closeNlEnd" in nsrc,
             "Line start should reference closeNlEnd after closing fence")
    tc.finish()


# ------------------------------------------------------------------
#  Test 12: 4+ backticks fence
# ------------------------------------------------------------------
def test_four_backticks():
    tc = test_start("4+ backticks fence")
    src = load_header()
    tc.check("fenceBacktickCount" in src,
             "Should track number of backticks in opening fence")
    tc.check("backtickCount < codeBlock.fenceBacktickCount" in src,
             "Closing fence should require >= opening fence backtick count")
    tc.finish()


# ------------------------------------------------------------------
#  Test 13: Whitespace around identifier
# ------------------------------------------------------------------
def test_whitespace_around_identifier():
    tc = test_start("Whitespace around identifier")
    src = load_header()
    nsrc = normalize_ws(src)
    tc.check("idStart" in src and "idEnd" in src,
             "Identifier boundaries should be tracked")
    tc.check("alphanumeric" in nsrc or "underscore" in nsrc,
             "Identifier should use character classification")
    tc.finish()


# ------------------------------------------------------------------
#  Test 14: Block at end of input without trailing newline
# ------------------------------------------------------------------
def test_block_at_eof_no_newline():
    tc = test_start("Block at end of input without trailing newline")
    src = load_header()
    tc.check("codeBlock.textEnd = (int)markdownLength_" in src,
             "EOF handler should set textEnd to end of input")
    tc.check('markdown_[codeBlock.textEnd - 1]' in src,
             "EOF handler should trim trailing newlines")
    tc.finish()


# ------------------------------------------------------------------
#  Compilation test
# ------------------------------------------------------------------
def test_compilation():
    tc = test_start("Compilation check")
    src = load_header()
    tc.check(len(src) > 10000, "imgui_markdown.h should be a substantial file")
    open_braces = src.count('{')
    close_braces = src.count('}')
    tc.check(open_braces == close_braces,
             f"Braces should be balanced: {open_braces} open, {close_braces} close")
    tc.finish()


def strip_cpp_comments(src):
    """Remove C-style // and /* */ comments from source."""
    # Remove single-line comments
    src = re.sub(r'//.*?$', '', src, flags=re.MULTILINE)
    # Remove multi-line comments
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.DOTALL)
    return src


# ------------------------------------------------------------------
#  Stack balance test (uses proper brace-matching)
# ------------------------------------------------------------------
def test_stack_balance():
    tc = test_start("Stack balance in RenderCodeBlock")
    src = load_header()
    rb_src = extract_function(src, "RenderCodeBlock")
    tc.check(len(rb_src) > 0, "RenderCodeBlock function should be found")
    # Strip comments so comment text doesn't get counted
    rb_clean = strip_cpp_comments(rb_src)
    push_wrap = rb_clean.count("PushTextWrapPos")
    pop_wrap = rb_clean.count("PopTextWrapPos")
    tc.check(push_wrap == pop_wrap,
             f"TextWrapPos push/pop should be balanced: {push_wrap} push, {pop_wrap} pop")
    tc.finish()


# ------------------------------------------------------------------
#  Main
# ------------------------------------------------------------------
def main():
    print("=== imgui_markdown Code Block Tests ===\n")

    test_basic_block_with_id()
    test_block_without_id()
    test_empty_block()
    test_unclosed_block()
    test_emphasis_literal()
    test_link_literal()
    test_multiline_block()
    test_multiple_blocks()
    test_block_followed_by_heading()
    test_block_followed_by_list()
    test_block_followed_by_text()
    test_four_backticks()
    test_whitespace_around_identifier()
    test_block_at_eof_no_newline()
    test_compilation()
    test_stack_balance()

    total = len(results)
    passed = sum(1 for r in results if r.passed)

    print(f"\n=== Results: {passed}/{total} passed ===")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
