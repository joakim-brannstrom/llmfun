#!/usr/bin/env python3
"""
Unit tests for imgui_markdown inline code span parsing.

Static-analysis tests in the style of test_code_block.py: they pin the
structural invariants that make the single-pass CodeSpan state machine
correct (design 5.1-5.5). Behavioral verification of the rendered byte
stream lives in test_inline_code_runtime.cpp (design section 6).
"""

import re
import sys
import os

# ------------------------------------------------------------------
# Use script directory to resolve paths regardless of CWD
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
HEADER_PATH = os.path.join(SCRIPT_DIR, "..", "..", "imgui_markdown", "imgui_markdown.h")
TUI_PATH = os.path.join(SCRIPT_DIR, "..", "..", "..", "cpp_tui", "tui.cpp")


def load_header():
    """Load imgui_markdown.h source."""
    with open(HEADER_PATH, "r") as f:
        return f.read()


def normalize_ws(s):
    """Collapse all whitespace runs to single space for flexible matching."""
    return re.sub(r'\s+', ' ', s)


def extract_definition(src, func_name):
    """Extract a function body by brace-matching from its *definition*.

    Uses rfind so a forward declaration of the same name near the top of
    the header is skipped (e.g. Markdown, RenderLine, and
    defaultMarkdownFormatCallback are all forward-declared before their
    definitions).
    """
    start = src.rfind("inline void %s(" % func_name)
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


def strip_cpp_comments(src):
    """Remove C-style // and /* */ comments from source."""
    # Remove single-line comments
    src = re.sub(r'//.*?$', '', src, flags=re.MULTILINE)
    # Remove multi-line comments
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.DOTALL)
    return src


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
#  Test 1: INLINE_CODE enum value
# ------------------------------------------------------------------
def test_enum_value():
    tc = test_start("INLINE_CODE enum value")
    src = load_header()
    enum_start = src.find("enum class MarkdownFormatType {")
    enum_end = src.find("};", enum_start)
    tc.check(enum_start >= 0 and enum_end > enum_start,
             "MarkdownFormatType enum should be found")
    enum_body = src[enum_start:enum_end]
    tc.check("INLINE_CODE," in enum_body,
             "INLINE_CODE should be a MarkdownFormatType value")
    tc.check(enum_body.find("CODE_BLOCK,") < enum_body.find("INLINE_CODE,"),
             "CODE_BLOCK should still precede INLINE_CODE")
    tc.finish()


# ------------------------------------------------------------------
#  Test 2: CodeSpan struct declaration
# ------------------------------------------------------------------
def test_codespan_struct():
    tc = test_start("CodeSpan struct declaration")
    src = load_header()
    m = re.search(r"struct CodeSpan \{[^}]*\}", src, re.DOTALL)
    tc.check(m is not None, "CodeSpan struct should be declared")
    if m is not None:
        body = m.group(0)
        tc.check("bool active = false;" in body,
                 "CodeSpan should have an active flag")
        tc.check("int textStart = 0;" in body,
                 "CodeSpan should have a textStart index")
    tc.finish()


# ------------------------------------------------------------------
#  Test 3: Opening guard conjunction (lone-backtick rule)
# ------------------------------------------------------------------
# The whole opening condition must appear in one place, including the
# prev-char lone-backtick guard (revision M2): a backtick at index 0 may
# open a span, and any backtick preceded by another backtick may not.
OPEN_GUARD = (
    "} else if (c == '`' && (i == 0 || markdown_[i - 1] != '`') && "
    "link.state == Link::NO_LINK && em.state == Emphasis::NONE && "
    "!line.isHeading && (int)markdownLength_ > i + 1 && "
    "markdown_[i + 1] != '`' && markdown_[i + 1] != '\\n' && "
    "markdown_[i + 1] != '\\r') {"
)


def test_opening_guard():
    tc = test_start("Opening guard conjunction (lone backtick)")
    src = load_header()
    nsrc = normalize_ws(src)
    tc.check(nsrc.count(OPEN_GUARD) == 1,
             "The full opening-guard conjunction should appear in exactly one place")
    tc.finish()


# ------------------------------------------------------------------
#  Test 4: Active-span literal skip and newline abort
# ------------------------------------------------------------------
def test_active_span_handling():
    tc = test_start("Active span: literal skip and newline abort")
    src = load_header()
    md = extract_definition(src, "Markdown")
    tc.check(len(md) > 0, "Markdown definition should be found")
    active_start = md.find("if (codeSpan.active) {")
    tc.check(active_start >= 0, "Active-span block should exist")
    region_end = md.find("} else if (c == '`'", active_start)
    tc.check(region_end > active_start, "Opening guard should follow the active-span block")
    region = md[active_start:region_end]
    tc.check("if (c == '`')" in region, "Close branch should test for a closing backtick")
    # Abort branch: reset on newline, then the literal-skip continue.
    abort = region.find("c == '\\n' || c == '\\r'")
    reset = region.find("codeSpan = CodeSpan();", abort) if abort >= 0 else -1
    skip = region.find("continue;", abort) if abort >= 0 else -1
    tc.check(abort >= 0, "Abort branch should test for \\n or \\r")
    tc.check(abort >= 0 and abort < reset < skip,
             "Abort branch should reset codeSpan and then hit the literal-skip continue")
    tc.finish()


# ------------------------------------------------------------------
#  Test 5: Close-branch ordering
# ------------------------------------------------------------------
def test_close_branch_ordering():
    tc = test_start("Close branch ordering")
    src = load_header()
    md = extract_definition(src, "Markdown")
    active_start = md.find("if (codeSpan.active) {")
    region_end = md.find("} else if (c == '`'", active_start)
    region = md[active_start:region_end]
    # Expected source order: pre-text range computed -> pre-text render ->
    # isInlineCode flag on -> span render -> flag off -> state reset.
    p_line_end = region.find("int lineEnd = codeSpan.textStart - 1;")
    p_pre_render = region.find("RenderLine(markdown_, line, textRegion, mdConfig_);")
    p_span_flag = region.find("line.isInlineCode = true;")
    p_last_pos = region.find("line.lastRenderPosition = codeSpan.textStart - 1;")
    p_span_render = region.find("RenderLine(markdown_, line, textRegion, mdConfig_);",
                                p_span_flag)
    p_flag_off = region.find("line.isInlineCode = false;")
    p_reset = region.find("codeSpan = CodeSpan();", p_span_render)
    ordered = (p_line_end >= 0 and p_pre_render >= 0 and p_span_flag >= 0 and
               p_last_pos >= 0 and p_span_render >= 0 and p_flag_off >= 0 and
               p_reset >= 0 and
               p_line_end < p_pre_render < p_span_flag < p_last_pos < p_span_render <
               p_flag_off < p_reset)
    tc.check(ordered,
             "Close branch should render pre-text, then the span piece, then reset")
    tc.finish()


# ------------------------------------------------------------------
#  Test 6: RenderLine INLINE_CODE branch ordering
# ------------------------------------------------------------------
def test_renderline_branch_ordering():
    tc = test_start("RenderLine INLINE_CODE branch ordering")
    src = load_header()
    rl = extract_definition(src, "RenderLine")
    # Anchor on code, not on the "render inline code" comment, so a comment
    # reword in Task 8 doc cleanup cannot break this test.
    start = rl.find("formatInfo.type = MarkdownFormatType::INLINE_CODE;")
    end = rl.find("} else", start)
    tc.check(start >= 0 and end > start, "INLINE_CODE branch should exist in RenderLine")
    branch = rl[start:end]
    p_type = branch.find("formatInfo.type = MarkdownFormatType::INLINE_CODE;")
    p_text = branch.find("formatInfo.text = text;")
    p_length = branch.find("formatInfo.textLength = textSize;")
    p_cb = branch.find("mdConfig_.formatCallback(formatInfo, true);")
    p_render = branch.find("textRegion_.RenderTextWrapped(text, text + textSize);")
    ordered = (p_type >= 0 and p_text >= 0 and p_length >= 0 and p_cb >= 0 and
               p_render >= 0 and p_type < p_cb and p_text < p_cb and p_length < p_cb and
               p_cb < p_render)
    tc.check(ordered,
             "type/text/textLength should be set before the callback fires, render after")
    tc.finish()


# ------------------------------------------------------------------
#  Test 7: Default callback INLINE_CODE push/pop balance
# ------------------------------------------------------------------
def test_default_callback_balance():
    tc = test_start("Default callback INLINE_CODE push/pop balance")
    src = load_header()
    dc = extract_definition(src, "defaultMarkdownFormatCallback")
    case_start = dc.find("case MarkdownFormatType::INLINE_CODE:")
    case_end = dc.find("break;", case_start)
    tc.check(case_start >= 0 and case_end > case_start,
             "INLINE_CODE case should exist in the default callback")
    case_body = dc[case_start:case_end]
    clean = strip_cpp_comments(case_body)
    push = clean.count("PushStyleColor")
    pop = clean.count("PopStyleColor")
    tc.check(push == 1 and pop == 1,
             f"INLINE_CODE case should push/pop exactly once: {push} push, {pop} pop")
    tc.check("ImVec4(0.7f, 0.7f, 0.7f, 1.0f)" in case_body,
             "INLINE_CODE should use the CODE_BLOCK gray color")
    tc.finish()


# ------------------------------------------------------------------
#  Test 8: TUI callback INLINE_CODE case
# ------------------------------------------------------------------
def test_tui_callback_case():
    tc = test_start("TUI callback INLINE_CODE case")
    with open(TUI_PATH, "r") as f:
        tui = f.read()
    case_start = tui.find("case ImGui::MarkdownFormatType::INLINE_CODE:")
    case_end = tui.find("break;", case_start)
    tc.check(case_start >= 0 and case_end > case_start,
             "INLINE_CODE case should exist in markdownFormatCallback (tui.cpp)")
    case_body = tui[case_start:case_end]
    tc.check("style.inlineCode" in case_body,
             "TUI case should style with MarkdownStyle::inlineCode")
    tc.check(case_body.count('TextUnformatted("`")') == 2,
             "TUI case should print literal backtick markers on start and end")
    tc.check("PushStyleColor" in case_body and "PopStyleColor" in case_body,
             "TUI case should push and pop the span color")
    tc.finish()


# ------------------------------------------------------------------
#  Test 9: Brace balance
# ------------------------------------------------------------------
def test_brace_balance():
    tc = test_start("Brace balance")
    src = load_header()
    raw_open = src.count('{')
    raw_close = src.count('}')
    tc.check(raw_open == raw_close,
             f"Braces should be balanced: {raw_open} open, {raw_close} close")
    clean = strip_cpp_comments(src)
    stripped_open = clean.count('{')
    stripped_close = clean.count('}')
    tc.check(stripped_open == stripped_close,
             f"Braces should be balanced after stripping comments: "
             f"{stripped_open} open, {stripped_close} close")
    tc.finish()


# ------------------------------------------------------------------
#  Test 10: CodeSpan hygiene resets in Markdown()
# ------------------------------------------------------------------
def test_codespan_resets():
    tc = test_start("CodeSpan hygiene resets in Markdown()")
    src = load_header()
    md = extract_definition(src, "Markdown")
    count = md.count("codeSpan = CodeSpan()")
    # Six sites: the four code-block hygiene resets (fence open, fence close,
    # per-line accumulation, unclosed block at EOF) plus the close-branch and
    # abort-branch resets of the span state machine itself. Removing any one
    # of them must fail this test, so pin the exact current count.
    tc.check(count == 6,
             f"Markdown() should reset CodeSpan at all 6 sites, found {count}")
    tc.finish()


# ------------------------------------------------------------------
#  Main
# ------------------------------------------------------------------
def main():
    print("=== imgui_markdown Inline Code Tests ===\n")

    test_enum_value()
    test_codespan_struct()
    test_opening_guard()
    test_active_span_handling()
    test_close_branch_ordering()
    test_renderline_branch_ordering()
    test_default_callback_balance()
    test_tui_callback_case()
    test_brace_balance()
    test_codespan_resets()

    total = len(results)
    passed = sum(1 for r in results if r.passed)

    print(f"\n=== Results: {passed}/{total} passed ===")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
