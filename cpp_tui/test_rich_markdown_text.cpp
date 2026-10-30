// test_rich_markdown_text.cpp
//
// Headless rich-markdown text-mode smoke test for the imgui_rich_md
// migration. Drives a REAL llmfun::tui::TuiState through the real
// tuiRender() on the imtui TEXT backend — no ncurses, no terminal, no PTY
// (same harness pattern as test_tui_maxwidth) — with the real text-mode
// markdown profile (initMarkdownTextMode() BEFORE
// ImTui_ImplText_Init(), the 1.0f bake warm-up, no selection, hard/soft
// breaks, OnImage = nullptr), and pins the reference rendering:
//
//   Case A: the reference document (heading, inline emphasis/strong/code,
//           link, wrapping paragraph, list, quote, fenced code block). The
//           rows appear in top-to-bottom order (chrome-agnostic scan, NO
//           absolute indices); the two inline-formatted paragraphs
//           re-join byte-exactly to their source text (single spaces
//           between the wrapped rows); the long paragraph takes >=2 rows;
//           "Title One", the two list rows and "a quote block" match the
//           trimmed row EXACTLY (the pinned rendered form is
//           "- item one" / "- item two", with the marker's space restored
//           and the quote's first character present; the source markdown
//           is "- item one" / "> a quote block");
//           the code block's right-aligned "Copy" button row (containment)
//           precedes the two code rows (the code block's child window
//           sizes over two passes — asserted only after >=2 frames).
//   Case B: the wide/emoji row reconstructs byte-exactly to
//           "part one part two café 中 文  🚀  done", and the row's cells
//           for U+4E2D, U+6587 and U+1F680 are present, each followed by a
//           blank/pad cell (2-cell glyphs, no overlap with the following
//           text).
//   Case C: the features document (table, MISSING image, mermaid, text)
//           renders without crashing; "Before." and "After." render; no "not loaded" /
//           "( Image" fallback text surfaces anywhere (the image
//           contributes no text row); table/mermaid art is NOT asserted
//           (accepted degradation).
//
// The wrapper's own contract (ui/markdown.* — the only file that may name
// RichMd::) is pinned by five more cases, each a tripwire for one rule:
//
//   Case D: `RenderRaw` only. A real file is named as a transclusion embed
//           (alone on its line, the only form `Render` resolves): the grid must
//           show the SOURCE and never the file's content, because `Render`
//           reads the file and model output must not be able to make the
//           renderer open one.
//   Case E: the block scope. Two frames that open the same block id enter the
//           same ImGui id, two blocks enter different ones — the identity
//           rich_md's per-frame `ID(fragmentCounter++)` relies on, and the one
//           a frame counter in `beginBlock()` would break. Also the guards
//           around a call that has nothing to draw into: no frame, and no ImGui
//           context at all (a rich context can outlive it), are dropped and
//           counted, never dereferenced; and destroying a state that never had
//           a context leaves the owner's installed.
//   Case F: the token bridge. `RichMd::Style` carries the inks, the gaps, the
//           sub/sup scale and the grid's bar corrections; rich_md's untouched
//           defaults are alpha -1 ("automatic"), so an unbridged field fails
//           the case rather than silently keeping rich_md's own colours.
//   Case G: the selection policy and the published counters. On the app path
//           the last render ran with the grid's answer, inside a block scope,
//           with a context, and the theme was not re-applied per frame.
//   Case H: `store/`, `views/` and `model/` name no RichMd symbol at all, so
//           the wrapper cannot be bypassed by a second seam.
//
// stderr is captured across ALL phases (init, every frame, teardown) and
// must contain NONE of: "call InitializeMarkdown first" (context missing),
// "font atlas is not built" (warm-up order broken), "rich_md: UploadRgba"
// (only surfaces with rich_md's DEFAULT OnImage; this profile
// sets OnImage = nullptr), or "PushSelectableText" (rich_md's own report of an
// unbalanced selection push — the wrapper must keep the pair together).
//
// Tripwire: this test FAILS if the warm-up recipe is reverted (init after
// the backend / no bakes -> the rich fonts resolve to the 16px bake and
// the spans render invisible/garbled) or if the LLMFUN imgui measurement
// patch is reverted (wrap/measure count font advances instead of cells ->
// the pinned wrapped rows stop matching).
//
// Row reconstruction: a row is the cells' code points (ch == 0 -> ' '),
// then "cleaned" — outer runs of whitespace and non-ASCII glyphs are
// stripped (repeatedly), because in the real TUI the rows carry UI chrome
// that shares the grid rows with the markdown content: the menu-bar row,
// the 8-column "Open" session strip (closed by white-box state setup for
// layout determinism), the framed tree node's border column, and the root
// window border. The cleaned row is the row's content text. Assertions
// scan top-to-bottom without pinning absolute indices.
//
// Debug: RICHMD_TEST_DEBUG=<path> makes the test dump every grid row
// (cleaned text + per-cell x:U+XXXX(w<chwidth>)) for each case to <path>.
//
// Exit codes: 0 = all cases pass; 1 = first assertion failure (the grid is
// dumped to the real stderr); 2 = environment error (init/display size).
//
// Headless (text backend only) — run from the build dir:
//   ./test_rich_markdown_text < /dev/null

#include "imtui/imtui.h" // imgui + ImTui_ImplText_* (imtui-impl-text.h)

#include "tui.h" // llmfun::tui: TuiState, tuiRender, markdown text-mode lifecycle

#include "ui/markdown.h" // the rich_md seam this file also pins (cases D-G)
#include "ui/tokens.h"   // the inks and gaps the markdown theme is bridged from

#include "imgui_rich_md/rich_md.h" // RichMd::Style, read back by the theme case

#include <array>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#ifdef TEST_RICH_MARKDOWN_SRC_DIR
#include <fstream>
#endif

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Reference documents — the exact contract of the pinned rendering.
// ---------------------------------------------------------------------------

// Case A: the reference document.
static const char* kDoc =
    "# Title One\n\n"
    "Paragraph with *emphasis* and **strong** and `inline code` and a "
    "[link](https://example.com).\n\n"
    "A longer paragraph that must wrap over several lines to show how wrapping behaves: the quick "
    "brown fox jumps over the lazy dog and runs and runs and runs and runs and runs until the line "
    "is definitely long enough to wrap twice.\n\n"
    "- item one\n- item two\n\n"
    "> a quote block\n\n"
    "```\ncode block line one\ncode block line two\n```\n";

// Case A: the two inline-formatted source sentences the rendered (possibly
// wrapped) rows must re-join to, byte-exactly.
static const char* kLinkParagraph =
    "Paragraph with emphasis and strong and inline code and a link.";
static const char* kLongParagraph =
    "A longer paragraph that must wrap over several lines to show how wrapping behaves: the quick "
    "brown fox jumps over the lazy dog and runs and runs and runs and runs and runs until the line "
    "is definitely long enough to wrap twice.";

// Case B: source text and the byte-exact pinned rendering (one space after
// the 2-cell CJK cells, two spaces on each side of the 2-cell emoji).
static const char* kWide = "part one part two cafe\u00e9 \u4e2d\u6587 \U0001F680 done";
static const char* kWideRow = "part one part two cafe\u00e9 \u4e2d \u6587  \U0001F680  done";

// Case C: the features document (table, MISSING image, mermaid, text).
static const char* kFeatures =
    "Before.\n\n"
    "| Left | Right |\n|:-----|------:|\n| a    | 1     |\n| b    | 2     |\n\n"
    "![alt](images/markdown_broken_image.png)\n\n"
    "```mermaid\ngraph TD\nA-->B\n```\n\n"
    "After.";

namespace {

ImTui::TScreen g_screen;
char g_case = '0';
int g_frames = 0;

// A dup of the REAL stderr (captured at main entry): fail() diagnostics
// must reach the terminal even while fd 2 is redirected into the capture
// pipe; g_errOut is the FILE* view of that fd.
int g_realErr = -1;
FILE* g_errOut = nullptr;
std::string g_capturedErr;              // filled when the capture stops
size_t g_dbgLines = 0, g_dbgGroups = 0; // fail() diagnostics (last frame)
void dumpGrid(FILE* f) {
    // Full grid dump (ch 0 printed as '.'), plus the cleaned rows (the
    // content text — keeps the wide glyphs, strips the chrome).
    ::fprintf(f, "grid (%dx%d):\n", g_screen.nx, g_screen.ny);
    for (int y = 0; y < g_screen.ny; ++y) {
        std::string line;
        for (int x = 0; x < g_screen.nx; ++x) {
            unsigned c = g_screen.data[y * g_screen.nx + x].ch;
            line.push_back((c >= 32 && c < 127) ? char(c) : '.');
        }
        ::fprintf(f, "%2d |%s|\n", y, line.c_str());
    }
}

void fail(const std::string& what) {
    ::fprintf(g_errOut, "FAIL [case %c @ frame %d]: %s\n", g_case, g_frames, what.c_str());
    ::fprintf(g_errOut, "debug: outputLines=%zu renderGroups=%zu\n", g_dbgLines, g_dbgGroups);
    dumpGrid(g_errOut);
    if (!g_capturedErr.empty()) {
        ::fprintf(g_errOut, "captured stderr so far:\n%s\n", g_capturedErr.c_str());
    }
    std::exit(1);
}

void expect(bool cond, const std::string& what) {
    if (!cond)
        fail(what);
}

// Append code point cp to s as UTF-8.
void appendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s.push_back(char(cp));
    } else if (cp < 0x800) {
        s.push_back(char(0xC0 | (cp >> 6)));
        s.push_back(char(0x80 | (cp & 63)));
    } else if (cp < 0x10000) {
        s.push_back(char(0xE0 | (cp >> 12)));
        s.push_back(char(0x80 | ((cp >> 6) & 63)));
        s.push_back(char(0x80 | (cp & 63)));
    } else {
        s.push_back(char(0xF0 | (cp >> 18)));
        s.push_back(char(0x80 | ((cp >> 12) & 63)));
        s.push_back(char(0x80 | ((cp >> 6) & 63)));
        s.push_back(char(0x80 | (cp & 63)));
    }
}

// Reconstruct row y: each cell's code point (ch == 0 -> ' '); ch2 is
// ignored (folding is off in this profile, so ch2 is always 0 here).
std::string rowRaw(int y) {
    std::string s;
    for (int x = 0; x < g_screen.nx; ++x) {
        uint32_t ch = g_screen.data[y * g_screen.nx + x].ch;
        if (ch == 0)
            s.push_back(' ');
        else
            appendUtf8(s, ch);
    }
    return s;
}

int utf8SeqLen(unsigned char lead) {
    return lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
}

bool isRowSpace(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// The row's content text: strip outer runs of whitespace and non-ASCII
// glyphs (window / tree-node border cells quantized to one cell), repeated
// until stable — e.g. "  |  code line  |  " -> "code line".
std::string cleanRow(int y) {
    std::string s = rowRaw(y);
    size_t a = 0, b = s.size();
    bool changed = true;
    while (changed) {
        changed = false;
        while (a < b && isRowSpace((unsigned char)s[a])) {
            ++a;
            changed = true;
        }
        if (a < b && (unsigned char)s[a] >= 0x80) {
            a += (size_t)utf8SeqLen((unsigned char)s[a]);
            changed = true;
        }
        while (b > a && isRowSpace((unsigned char)s[b - 1])) {
            --b;
            changed = true;
        }
        if (b > a && (unsigned char)s[b - 1] >= 0x80) {
            // Walk back over the trailing glyph's continuation bytes; the byte the walk
            // stops on is the glyph's *lead* byte, so it goes with them. Dropping it is what
            // makes the strip terminate on a row that ends with a multi-byte glyph (`| ... |`
            // of a drawn table): keeping it leaves a lone lead byte, which measures as "still
            // non-ASCII" on every pass - an endless loop, not a wrong string.
            size_t st = b;
            while (st > a && ((unsigned char)s[st - 1] & 0xC0) == 0x80)
                --st;
            b = st > a ? st - 1 : a;
            changed = true;
        }
    }
    return s.substr(a, b - a);
}

// First row y >= from whose content contains sub; -1 if none.
int findRow(const std::string& sub, int from) {
    for (int y = from; y < g_screen.ny; ++y)
        if (cleanRow(y).find(sub) != std::string::npos)
            return y;
    return -1;
}
// First row y >= from whose trimmed content equals row exactly; -1 if none.
int findRowExact(const std::string& row, int from) {
    for (int y = from; y < g_screen.ny; ++y)
        if (cleanRow(y) == row)
            return y;
    return -1;
}

// Walk the consecutive non-blank rows from startY and re-join them to
// expected (the source sentence): each row must be the next un-consumed
// (space-skip) prefix run of expected. Sets outEndRow to the first row
// after the sentence, *outMatched to the matched byte count, and returns
// the row count. Stops at the first row that does not continue the
// sentence (or a blank row once started).
int joinSentence(int startY, const std::string& expected, int& outEndRow, size_t* outMatched) {
    int y = startY;
    while (y < g_screen.ny && cleanRow(y).empty())
        ++y; // chrome/blank rows before the sentence
    size_t cur = 0;
    int rows = 0;
    for (; y < g_screen.ny; ++y) {
        std::string row = cleanRow(y);
        if (row.empty())
            break; // the paragraph's rows are consecutive non-blank
        size_t k = cur;
        while (k < expected.size() && expected[k] == ' ')
            ++k;
        if (k == expected.size())
            break;
        if (k + row.size() <= expected.size() && expected.compare(k, row.size(), row) == 0) {
            cur = k + row.size();
            ++rows;
        } else {
            break; // this row does not continue the sentence
        }
    }
    outEndRow = y;
    if (outMatched)
        *outMatched = cur;
    return rows;
}

// Run exactly one frame on the text backend (same pipeline as
// test_tui_maxwidth): NewFrame pair, the app render (owns EndFrame), then
// the draw data into the TScreen grid.
void frame(llmfun::tui::TuiState& state) {
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    if (!llmfun::tui::tuiRender(state))
        fail("tuiRender returned false (quit)");
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    g_dbgLines = state.chat.outputLines.size();
    g_dbgGroups = state.chat.renderGroups.size();
    ++g_frames;
}

// Fresh case setup: clear the output, close the session panel (white-box
// state setup — the 30-column open panel would share grid rows 1-3 with
// the markdown content; the closed 8-column "Open" strip keeps the layout
// deterministic), re-arm the simulated 80x24 terminal, add ONE assistant
// message with the case document.
void setupCase(llmfun::tui::TuiState& state, const char* doc) {
    llmfun::tui::tuiClearOutput(state);
    ImGui::GetIO().DisplaySize = ImVec2(80.0f, 24.0f);
    llmfun::tui::ChatMessage m;
    m.summary = "Assistant";
    m.text = doc;
    m.type = llmfun::tui::ChatMessageType::Assistant;
    // Monotonic id exactly like the C API (tuiAddChatMessage): buildRenderGroups
    // caches on (first,last) message ids with a (0,0) sentinel, so an id-0
    // message would be treated as "unchanged" and never grouped.
    m.id = state.chat.outputLineNextId++;
    llmfun::tui::tuiAddOutputLine(state, m);
}

// Optional per-case grid dump for debugging pinned values: set
// RICHMD_TEST_DEBUG to a file path.
void debugDump(llmfun::tui::TuiState& state, const char* phase) {
    const char* path = std::getenv("RICHMD_TEST_DEBUG");
    if (!path || !*path)
        return;
    FILE* f = ::fopen(path, std::strcmp(phase, "A") == 0 ? "w" : "a");
    if (!f)
        return;
    ::fprintf(f, "=== case %s (%dx%d, frame %d) outputLines=%zu renderGroups=%zu ===\n", phase,
              g_screen.nx, g_screen.ny, g_frames, state.chat.outputLines.size(),
              state.chat.renderGroups.size());
    for (int y = 0; y < g_screen.ny; ++y) {
        std::string cells;
        for (int x = 0; x < g_screen.nx; ++x) {
            const ImTui::TCell& c = g_screen.data[y * g_screen.nx + x];
            if (c.ch == 0 || c.ch == ' ')
                continue;
            std::array<char, 64> tmp{};
            ::snprintf(tmp.data(), tmp.size(), "%d:U+%04X(w%u) ", x, c.ch, c.chwidth);
            cells += tmp.data();
        }
        ::fprintf(f, "%2d |%s| cells: %s\n", y, cleanRow(y).c_str(), cells.c_str());
    }
    ::fclose(f);
}

// stderr capture: redirect fd 2 into a pipe around all render phases; the
// captured text is returned on stop().
struct ErrCapture {
    int saved = -1;
    int pipeR = -1;
    bool active = false;

    void start() {
        ::fflush(stderr);
        ::fflush(g_errOut);
        std::array<int, 2> fds{};
        saved = ::dup(2);
        bool piped = ::pipe(fds.data()) == 0;
        if (saved < 0 || !piped || ::dup2(fds[1], 2) != 2) {
            // Close everything opened so far so a partial failure leaks nothing.
            if (saved >= 0)
                ::close(saved);
            if (piped) {
                ::close(fds[0]);
                ::close(fds[1]);
            }
            return;
        }
        ::close(fds[1]);
        pipeR = fds[0];
        active = true;
    }

    void stop(std::string& out) {
        if (!active)
            return;
        active = false;
        ::fflush(stderr); // buffered libc writes -> the pipe
        ::close(2);
        std::string s;
        std::array<char, 4096> buf;
        ssize_t n;
        while ((n = ::read(pipeR, buf.data(), buf.size())) > 0)
            s.append(buf.data(), (size_t)n);
        ::close(pipeR);
        pipeR = -1;
        ::dup2(saved, 2);
        ::close(saved);
        saved = -1;
        out.swap(s);
    }
};

// Harness init/shutdown: mirror the real app's tuiInit order exactly —
// context, then the text-mode markdown profile (fonts + 1-cell warm-up
// bakes) BEFORE the text backend init, because ImTui_ImplText_Init() builds
// and locks the font atlas (a later GetFontBaked() would resolve the 16px
// bake instead of the exact 1-cell bake), then theme, then pin the default
// font by name.
void harnessInit(llmfun::tui::TuiState& state) {
    std::setlocale(LC_ALL, "");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    llmfun::tui::initMarkdownTextMode(state); // BEFORE the text backend init
    ImTui_ImplText_Init();
    llmfun::tui::applyUiTheme();
    llmfun::tui::finishMarkdownTextMode(state);
    ImGui::GetIO().DisplaySize = ImVec2(80.0f, 24.0f);
}

void harnessShutdown(llmfun::tui::TuiState& state) {
    llmfun::tui::destroyMarkdown(state);
    ImTui_ImplText_Shutdown();
    ImGui::DestroyContext();
}

// ---------------------------------------------------------------------------
// Cases (assertions on the LAST of >=2 frames: the code block's child
// window sizes over two passes).
// ---------------------------------------------------------------------------

void caseA(llmfun::tui::TuiState& state) {
    g_case = 'A';
    setupCase(state, kDoc);
    for (int i = 0; i < 4; ++i)
        frame(state);

    // The rows in top-to-bottom order (chrome-agnostic, no indices).
    int y = findRowExact("Title One", 0);
    expect(y >= 0, "case A: 'Title One' row not found");

    int end = 0;
    size_t matched = 0;
    joinSentence(y + 1, kLinkParagraph, end, &matched);
    expect(matched == std::strlen(kLinkParagraph),
           "case A: link paragraph rows do not rejoin byte-exactly to the source text");

    int rows = joinSentence(end, kLongParagraph, end, &matched);
    expect(rows >= 2, "case A: long paragraph must wrap to >=2 rows");
    expect(matched == std::strlen(kLongParagraph),
           "case A: long paragraph rows do not rejoin byte-exactly to the source text");

    // The rendered (trimmed) forms after the BLOCK_LI SameLine/Indent fix:
    // the marker's gap and the quote's first character are no longer eaten
    // ("-item one" -> "- item one", "quote" row keeps its "a").
    int y1 = findRowExact("- item one", end);
    expect(y1 >= 0, "case A: 'item one' list row not found");
    int y2 = findRowExact("- item two", y1 + 1);
    expect(y2 >= 0, "case A: 'item two' list row not found");
    int y3 = findRowExact("a quote block", y2 + 1);
    expect(y3 >= 0, "case A: 'quote block' row not found");
    int y4 = findRow("Copy", y3 + 1);
    expect(y4 >= 0, "case A: code-block 'Copy' button row not found");
    int y5 = findRow("code block line one", y4);
    expect(y5 >= 0, "case A: 'code block line one' row not found");
    int y6 = findRow("code block line two", y5 + 1);
    expect(y6 >= 0, "case A: 'code block line two' row not found");

    debugDump(state, "A");
}

void caseB(llmfun::tui::TuiState& state) {
    g_case = 'B';
    setupCase(state, kWide);
    for (int i = 0; i < 4; ++i)
        frame(state);

    // The pinned row, byte-exact.
    int y = -1;
    for (int yy = 0; yy < g_screen.ny; ++yy)
        if (cleanRow(yy) == kWideRow) {
            y = yy;
            break;
        }
    expect(y >= 0, "case B: pinned wide row 'part one part two cafe'+'\\u00e9' ... not found "
                   "byte-exact (see grid dump)");

    // Cell check: the wide glyphs are present (once each) and each is
    // followed by a blank/pad cell — the 2-cell span must not overlap the
    // following text (the next non-blank cell is >= 2 columns away).
    int nU4E2D = 0, nU6587 = 0, nU1F680 = 0;
    for (int x = 0; x < g_screen.nx; ++x) {
        const ImTui::TCell& c = g_screen.data[y * g_screen.nx + x];
        if (c.ch == 0x4E2D)
            ++nU4E2D;
        else if (c.ch == 0x6587)
            ++nU6587;
        else if (c.ch == 0x1F680)
            ++nU1F680;
    }
    expect(nU4E2D == 1 && nU6587 == 1 && nU1F680 == 1,
           "case B: each wide glyph (U+4E2D, U+6587, U+1F680) must appear exactly once");
    for (int x = 0; x < g_screen.nx; ++x) {
        const ImTui::TCell& c = g_screen.data[y * g_screen.nx + x];
        if (c.ch != 0x4E2D && c.ch != 0x6587 && c.ch != 0x1F680)
            continue;
        expect(c.chwidth >= 1 && c.chwidth <= 2,
               "case B: wide glyph cell must carry a sane chwidth (1 or 2)");
        expect(x + 1 < g_screen.nx, "case B: wide glyph cell must not be the last column");
        const ImTui::TCell& next = g_screen.data[y * g_screen.nx + x + 1];
        expect(next.ch == 0 || next.ch == ' ',
               "case B: wide glyph must be followed by a blank/pad cell (no overlap)");
    }

    debugDump(state, "B");
}

void caseC(llmfun::tui::TuiState& state) {
    g_case = 'C';
    setupCase(state, kFeatures);
    // 80x30, not the harness default 80x24: the table is drawn by the text-grid renderer
    // (task 37's bridge), which spends three rows rich_md's native table did not - a top rule,
    // the header separator and a bottom rule. In 24 rows the document no longer fits, the
    // thread follows the newest content and "Before." scrolls above the fold; the case is
    // about what the document renders, not about where a full thread starts.
    ImGui::GetIO().DisplaySize = ImVec2(80.0f, 30.0f);
    for (int i = 0; i < 4; ++i)
        frame(state);

    // "Before." renders immediately above the (unasserted) table art; both
    // text rows pin exact trimmed equality.
    int yb = findRowExact("Before.", 0);
    expect(yb >= 0, "case C: 'Before.' row not found (document crashed or truncated?)");
    int y = findRowExact("After.", 0);
    expect(y >= 0, "case C: 'After.' row not found (document crashed or truncated?)");

    // The missing image must contribute no fallback text row anywhere.
    for (int yy = 0; yy < g_screen.ny; ++yy) {
        std::string row = rowRaw(yy);
        expect(row.find("not loaded") == std::string::npos,
               "case C: image fallback text 'not loaded' surfaced on the grid");
        expect(row.find("( Image") == std::string::npos,
               "case C: image fallback text '( Image' surfaced on the grid");
    }

    debugDump(state, "C");
}

/// One frame that draws exactly one probe window. The block-scope and theme
/// assertions need a frame of their own: `beginBlock()` answers its id only
/// inside one (that is where ImGui has a window to hash against), and a probe
/// window keeps the app's own layout out of the measurement.
std::size_t drawBlockProbe(std::size_t blockId, const char* text, bool selectable) {
    const llmfun::tui::ui::MarkdownTheme theme =
        llmfun::tui::ui::markdownThemeFromTokens(/*textGrid=*/true);
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##block_probe", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar);
    const std::size_t scope = llmfun::tui::ui::beginBlock(blockId);
    llmfun::tui::ui::renderModelText(text, theme, selectable);
    llmfun::tui::ui::endBlock();
    ImGui::End();
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    ++g_frames;
    return scope;
}

bool sameColor(const ImVec4& a, const ImVec4& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

/// D: `RenderRaw` only. A transclusion is a *file read* in rich_md's `Render`, so
/// the tripwire is a real file: if the wrapper ever calls `Render`, the probe's
/// content appears on the grid and its path disappears.
///
/// The probe lives under the system temp directory rather than the working
/// directory: `fail()` exits the process, so a red case must not be able to leave
/// a file behind in `build/tui`.
void caseTransclusion(llmfun::tui::TuiState& state) {
    g_case = 'D';
    const std::filesystem::path probePath =
        std::filesystem::temp_directory_path() / "llmfun-md-probe.txt";
    const std::string probeName = probePath.filename().string();
    const char* kProbeBody = "SECRET_BODY_SHOULD_NEVER_BE_RENDERED";
    {
        FILE* f = ::fopen(probePath.c_str(), "w");
        expect(f != nullptr, "case D: could not create the transclusion probe file");
        ::fprintf(f, "%s\n", kProbeBody);
        ::fclose(f);
    }
    // The message names the file the way rich_md's transclusion syntax does: an
    // embed alone on its own line (that is the only form `Render` resolves).
    const std::string doc = std::string("before\n\n![[") + probePath.string() + "]]\n\nafter";
    setupCase(state, doc.c_str());
    for (int i = 0; i < 3; ++i)
        frame(state);

    bool sourceShown = false;
    for (int yy = 0; yy < g_screen.ny; ++yy) {
        const std::string row = rowRaw(yy);
        expect(row.find(kProbeBody) == std::string::npos,
               "case D: the transclusion target's CONTENT was rendered - the wrapper used "
               "Render() instead of RenderRaw() (model output must never read a file)");
        // The file NAME, not the whole embed: the absolute path may be wrapped
        // across two rows, the name is short enough to stay on one.
        if (row.find(probeName) != std::string::npos)
            sourceShown = true;
    }
    expect(sourceShown, "case D: the transclusion source text did not render as text");
    std::error_code ignored;
    std::filesystem::remove(probePath, ignored);
    debugDump(state, "D");
}

/// E, second half: a rich context with NO ImGui context. `ImGui::GetCurrentWindowRead()`
/// is `(*GImGui).CurrentWindow`, so a guard shaped "no context, or no window" reads the
/// missing ImGui context and is itself the null dereference it is meant to prevent.
/// The measurement runs in a child process: destroying the ImGui context would take the
/// rest of the suite with it, and an exit status reports a missing guard as a failure
/// instead of a dead suite (the old order crashes, so the crash is the evidence).
void expectRenderWithoutImGuiContextIsDropped() {
    const pid_t pid = ::fork();
    expect(pid >= 0, "case E: fork() failed - cannot test the no-ImGui-context guard");
    if (pid == 0) {
        ImGui::DestroyContext(); // the state under test: rich context installed, no ImGui one
        const llmfun::tui::ui::MarkdownCounters before = llmfun::tui::ui::markdownCounters();
        llmfun::tui::ui::renderModelText("x", llmfun::tui::ui::markdownThemeFromTokens(true),
                                         false);
        const llmfun::tui::ui::MarkdownCounters after = llmfun::tui::ui::markdownCounters();
        const bool dropped =
            after.droppedRenders == before.droppedRenders + 1 && after.renders == before.renders;
        if (!dropped)
            ::fprintf(g_errOut,
                      "case E child: a render with a rich context and no ImGui context was not "
                      "dropped (droppedRenders %u -> %u, renders %u -> %u)\n",
                      before.droppedRenders, after.droppedRenders, before.renders, after.renders);
        ::_exit(dropped ? 0 : 1);
    }
    int status = 0;
    expect(::waitpid(pid, &status, 0) == pid, "case E: waitpid() failed");
    expect(!WIFSIGNALED(status),
           "case E: a render with a rich context but no ImGui context crashed (signal " +
               std::to_string(WTERMSIG(status)) +
               ") - the frame guard reads the ImGui context instead of testing for it");
    expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "case E: a render with a rich context but no ImGui context was not dropped");
}

/// E: the block scope. Two frames that open the same block must enter the same
/// ImGui id, and two blocks must enter different ones - which is exactly what a
/// per-frame counter would break, and what rich_md's `ID(fragmentCounter++)`
/// needs from its caller.
void caseBlockScope() {
    g_case = 'E';
    const std::size_t first = drawBlockProbe(7, "alpha", false);
    const std::size_t again = drawBlockProbe(7, "alpha", false);
    const std::size_t other = drawBlockProbe(8, "alpha", false);
    expect(first != 0, "case E: beginBlock() answered no id inside a frame");
    expect(first == again, "case E: the same block id entered a different ImGui scope on the "
                           "next frame - rich_md's fragment ids would drift");
    expect(first != other, "case E: two block ids shared one ImGui scope");
    // Outside a frame there is no window to push an id onto: the call answers 0 and
    // leaves the stack alone (a PushID there would dereference a null window), and
    // the matching endBlock() is then a no-op.
    expect(llmfun::tui::ui::beginBlock(7) == 0,
           "case E: beginBlock() outside a frame must answer no id");
    llmfun::tui::ui::endBlock();
    // ... and the scope it did not open is neither counted nor drawn into: a render
    // with no frame is dropped, and an unmatched endBlock() must not leave a phantom
    // depth behind that would make the next render look scoped.
    const llmfun::tui::ui::MarkdownCounters before = llmfun::tui::ui::markdownCounters();
    llmfun::tui::ui::renderModelText("x", llmfun::tui::ui::markdownThemeFromTokens(true), false);
    const llmfun::tui::ui::MarkdownCounters after = llmfun::tui::ui::markdownCounters();
    expect(after.droppedRenders == before.droppedRenders + 1 && after.renders == before.renders,
           "case E: a render outside a frame must be dropped, not drawn into a null window");
    expect(after.unscopedRenders == before.unscopedRenders,
           "case E: endBlock() without a scope left a phantom block depth behind");
    // The wrapper's context slot is per process, a state is not: destroying one that
    // never had a context (every headless state) must leave the owner's in place, or
    // the owner silently falls back to plain text for the rest of its life.
    expect(llmfun::tui::ui::markdownContext() != nullptr,
           "case E: the harness state has no rich context to protect");
    llmfun::tui::TuiState headless;
    llmfun::tui::destroyMarkdown(headless);
    expect(llmfun::tui::ui::markdownContext() != nullptr,
           "case E: destroying a headless state cleared the rich context of the state that owns "
           "one - that state would draw plain text from then on");
    // And the guard that has no window to test against.
    expectRenderWithoutImGuiContextIsDropped();
}

/// F: the token bridge, field by field. `RichMd::Style` is the sink that
/// `ImGuiCol_*` mapping cannot reach, and rich_md ships values of its own there -
/// the "automatic" alpha -1 markers *and* a hard-coded amber `==mark==` fill that
/// is used without a resolve step. So every colour the bridge owns is compared
/// against the token it must be, and every distance against the value the token
/// arithmetic produces; an alpha check alone would miss the amber.
void caseTheme() {
    g_case = 'F';
    using llmfun::tui::ui::ColorToken;
    const llmfun::tui::ui::MarkdownTheme grid =
        llmfun::tui::ui::markdownThemeFromTokens(/*textGrid=*/true);
    const llmfun::tui::ui::MarkdownTheme pixel =
        llmfun::tui::ui::markdownThemeFromTokens(/*textGrid=*/false);

    // Theme field -> the token it must be (six, i.e. every colour in the theme).
    struct ThemeToken {
        const char* name;
        ImVec4 value;
        ColorToken token;
    };
    const ThemeToken kThemeTokens[] = {
        {"link", grid.link, ColorToken::accent},
        {"inlineCode", grid.inlineCode, ColorToken::roleTool},
        {"quoteBar", grid.quoteBar, ColorToken::lineStrong},
        {"admonitionNote", grid.admonitionNote, ColorToken::accent},
        {"admonitionWarn", grid.admonitionWarn, ColorToken::stateWarn},
        {"markFill", grid.markFill, ColorToken::selection},
    };
    for (const ThemeToken& row : kThemeTokens)
        expect(sameColor(row.value, llmfun::tui::ui::color(row.token)),
               std::string("case F: theme.") + row.name + " is not its token");

    expect(grid.blockGap > 0.0f && grid.blockGap < 1.0f,
           "case F: the grid's block gap must stay below one cell, or it eats a row the "
           "1-row-per-line layout does not budget");
    // rich_md draws `(7 - level) * headerGapStep`, so an h1 above the first block is
    // the widest step and the one that would eat a row first.
    expect(6.0f * grid.headerGap < 1.0f,
           "case F: the grid's widest heading gap (h1 = 6 steps) must stay below one cell");
    expect(pixel.blockGap == grid.blockGap,
           "case F: a gap is a fraction of the drawn size, so both profiles resolve the same "
           "em value");
    expect(grid.subSupScale == 1.0f, "case F: sub/sup must stay one cell tall on the grid");
    expect(pixel.subSupScale > 0.0f && pixel.subSupScale < 1.0f,
           "case F: the pixel profile scales sub/sup down by a type step");

    llmfun::tui::ui::applyMarkdownTheme(pixel);
    const RichMd::Style& style = RichMd::GetStyle();
    // The style field each theme colour must have reached.
    struct StyleField {
        const char* name;
        ImVec4 actual;
        ImVec4 expected;
    };
    const StyleField kStyleColours[] = {
        {"linkColor", style.linkColor, pixel.link},
        {"codeColor", style.codeColor, pixel.inlineCode},
        {"quoteBar", style.quoteBar, pixel.quoteBar},
        {"markBackground", style.markBackground, pixel.markFill},
    };
    for (const StyleField& row : kStyleColours)
        expect(sameColor(row.actual, row.expected),
               std::string("case F: RichMd::Style.") + row.name + " not bridged from the theme");
    // All five admonition slots, each on its class: a future split of Note/Tip/
    // Important must not be able to slip past a two-slot spot check.
    for (int i = 0; i < 5; ++i) {
        const ImVec4 want = i < 3 ? pixel.admonitionNote : pixel.admonitionWarn;
        expect(sameColor(style.admonitionColors[i], want),
               "case F: admonition slot " + std::to_string(i) + " carries the wrong class");
    }
    expect(style.blockGap == pixel.blockGap, "case F: RichMd::Style.blockGap not bridged");
    expect(style.headerGapStep == pixel.headerGap,
           "case F: RichMd::Style.headerGapStep not bridged");
    expect(style.subSupScale == pixel.subSupScale, "case F: RichMd::Style.subSupScale not bridged");
    expect(style.listItemGap == 0.0f && style.fragmentGapTop == 0.0f,
           "case F: a list item / fragment gap must be a stated zero, not a library default");
    // The tripwire for "ImGuiCol_* alone": rich_md's untouched colours are alpha -1
    // ("automatic"), so an unbridged field shows up as a negative alpha even where
    // the token happens to be close to the library's own value.
    expect(style.linkColor.w >= 0.0f && style.codeColor.w >= 0.0f && style.quoteBar.w >= 0.0f &&
               style.admonitionColors[0].w >= 0.0f && style.markBackground.w >= 0.0f,
           "case F: a RichMd::Style colour still carries rich_md's 'automatic' marker");
    expect(!sameColor(style.linkColor, llmfun::tui::ui::color(llmfun::tui::ui::ColorToken::textHi)),
           "case F: a link must not be drawn in body ink");

    // The grid's own corrections, and the profile restored for the cases after.
    llmfun::tui::ui::applyMarkdownTheme(grid);
    const RichMd::Style& gridStyle = RichMd::GetStyle();
    expect(gridStyle.quoteBarThickness == 1.0f && gridStyle.admonitionBarThickness == 1.0f,
           "case F: a grid bar must be exactly one cell (a 2-3 px bar erases the content's "
           "first cells)");
    expect(gridStyle.subSupScale == 1.0f, "case F: the grid's sub/sup scale was not restored");

    // applyUiTheme() is the sink that follows the live profile and the colour
    // depth, so it must re-apply the markdown style: the text profile builds its
    // rich context *before* the backend, i.e. before the depth is known, and
    // rich_md's style would otherwise be the only thing in the app that does not
    // follow the theme. Leaving the pixel theme in place is the failure this
    // checks - the pixel profile's sub/sup scale is not the grid's.
    llmfun::tui::ui::applyMarkdownTheme(pixel);
    llmfun::tui::applyUiTheme();
    const llmfun::tui::ui::MarkdownTheme live =
        llmfun::tui::ui::markdownThemeFromTokens(llmfun::tui::tuiIsTextGrid());
    const RichMd::Style& afterTheme = RichMd::GetStyle();
    expect(afterTheme.subSupScale == live.subSupScale &&
               sameColor(afterTheme.linkColor, live.link) &&
               sameColor(afterTheme.markBackground, live.markFill),
           "case F: applyUiTheme() left the markdown style on another profile or depth");
}

/// G: the selection policy and the counters the wrapper publishes. The app path
/// is what is measured here, not the policy alone: the last render of a real
/// message must have run with the grid's answer, inside a block scope, with a
/// context.
void caseSelectionAndCounters(llmfun::tui::TuiState& state) {
    g_case = 'G';
    expect(!llmfun::tui::ui::markdownSelectable(/*textGrid=*/true),
           "case G: the text grid must not select model text (Ctrl+C is quit there)");
    expect(llmfun::tui::ui::markdownSelectable(/*textGrid=*/false),
           "case G: the pixel profile keeps rich_md's selection");
    const llmfun::tui::ui::MarkdownTheme gridTheme =
        llmfun::tui::ui::markdownThemeFromTokens(/*textGrid=*/true);
    const llmfun::tui::ui::MarkdownTheme guiTheme =
        llmfun::tui::ui::markdownThemeFromTokens(/*textGrid=*/false);
    expect(gridTheme.textGrid && !llmfun::tui::ui::markdownSelectable(gridTheme.textGrid),
           "case G: the text grid's theme and its selection policy disagree");
    expect(!guiTheme.textGrid && llmfun::tui::ui::markdownSelectable(guiTheme.textGrid),
           "case G: the pixel profile's theme and its selection policy disagree");

    setupCase(state, "one **markdown** body");
    const llmfun::tui::ui::MarkdownCounters before = llmfun::tui::ui::markdownCounters();
    for (int i = 0; i < 3; ++i)
        frame(state);
    const llmfun::tui::ui::MarkdownCounters after = llmfun::tui::ui::markdownCounters();

    expect(after.renders > before.renders, "case G: the app path rendered no model text");
    expect(after.droppedRenders == before.droppedRenders,
           "case G: a render found no rich context (a call site skipped markdownReady())");
    expect(after.unscopedRenders == before.unscopedRenders,
           "case G: a model render ran outside beginBlock() - its rich_md ids are not stable");
    expect(after.lastSelectable == false,
           "case G: the app rendered model text as selectable on the text grid");
    expect(after.styleApplies <= before.styleApplies + 1,
           "case G: the token theme was re-applied per frame instead of once per theme");
}

#ifdef TEST_RICH_MARKDOWN_SRC_DIR
/// H: `ui/markdown.*` is the only seam. `store/`, `views/` and `model/` may name
/// no rich_md symbol at all: a second seam would bypass `RenderRaw`, the block
/// scope and the theme, and nothing would notice until a file was read or an id
/// drifted.
void caseSingleSeam() {
    g_case = 'H';
    namespace fs = std::filesystem;
    const fs::path root(TEST_RICH_MARKDOWN_SRC_DIR);
    int scanned = 0;
    for (const char* dir : {"store", "views", "model"}) {
        for (const auto& entry : fs::directory_iterator(root / dir)) {
            if (!entry.is_regular_file())
                continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".cpp" && ext != ".h")
                continue;
            std::ifstream in(entry.path(), std::ios::binary);
            expect(in.good(), std::string("case H: cannot read ") + entry.path().string());
            std::string source((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
            ++scanned;
            expect(source.find("RichMd") == std::string::npos,
                   "case H: " + entry.path().string() +
                       " names RichMd - rich_md must only be reachable through ui/markdown.*");
        }
    }
    expect(scanned >= 8, "case H: the seam scan found almost no sources (bad source dir?)");
    // And the wrapper itself must be the file that renders, or the scan above
    // would pass vacuously with rich_md gone from the tree.
    const fs::path wrapper = root / "ui" / "markdown.cpp";
    std::ifstream in(wrapper, std::ios::binary);
    expect(in.good(), "case H: cannot read ui/markdown.cpp");
    const std::string source((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    expect(source.find("RenderRaw") != std::string::npos,
           "case H: ui/markdown.cpp does not render - the seam moved");
}
#endif

} // namespace

int main() {
    // Diagnostics channel that survives the stderr capture below.
    g_realErr = ::dup(2);
    if (g_realErr < 0) {
        ::fprintf(stderr, "env error: dup(2) failed\n");
        return 2;
    }
    g_errOut = ::fdopen(g_realErr, "w");
    if (!g_errOut) {
        ::fprintf(stderr, "env error: fdopen(dup(2)) failed\n");
        ::close(g_realErr);
        return 2;
    }
    ::setvbuf(g_errOut, nullptr, _IONBF, 0);

    // Core state, driven through the C++ core API with the real text-mode
    // markdown profile; no chat sessions (panel closed for layout
    // determinism), no input, no timing.
    llmfun::tui::TuiState state;
    state.iniFilename = ""; // no ini persistence

    ErrCapture cap;
    cap.start();
    // A failed capture (dup/pipe) must not let the clean-stderr contract
    // below degrade into a trivially-passing no-op.
    expect(cap.active, "stderr capture could not be started (env error)");

    harnessInit(state);
    // The TScreen grid is resized by the first ImTui_ImplText_RenderDrawData
    // from the current DisplaySize (80x24); render one warm-up frame (empty
    // state) before checking the size.
    frame(state);
    if (g_screen.nx != 80 || g_screen.ny != 24) {
        ::fprintf(g_errOut, "unexpected display size %dx%d (want 80x24)\n", g_screen.nx,
                  g_screen.ny);
        dumpGrid(g_errOut);
        cap.stop(g_capturedErr);
        harnessShutdown(state);
        return 2;
    }

    caseA(state);
    caseB(state);
    caseC(state);
    caseTransclusion(state);
    caseBlockScope();
    caseTheme();
    caseSelectionAndCounters(state);
#ifdef TEST_RICH_MARKDOWN_SRC_DIR
    caseSingleSeam();
#endif

    harnessShutdown(state);
    cap.stop(g_capturedErr);

    // Clean-stderr contract (the three tripwire signatures).
    expect(g_capturedErr.find("call InitializeMarkdown first") == std::string::npos,
           "stderr: 'call InitializeMarkdown first' — the markdown context is missing");
    expect(g_capturedErr.find("font atlas is not built") == std::string::npos,
           "stderr: 'font atlas is not built' — the warm-up order is broken");
    expect(g_capturedErr.find("rich_md: UploadRgba") == std::string::npos,
           "stderr: 'rich_md: UploadRgba' — an image upload was attempted (OnImage must be "
           "nullptr in this profile)");
    // rich_md reports an unbalanced selection push itself; the wrapper's only job
    // is to keep the pair together, and this is the check that it does.
    expect(g_capturedErr.find("PushSelectableText") == std::string::npos,
           "stderr: rich_md reported a PushSelectableText() that was never popped");

    ::fprintf(g_errOut, "OK test_rich_markdown_text (frames=%d, captured stderr %zu bytes%s)\n",
              g_frames, g_capturedErr.size(), g_capturedErr.empty() ? "" : " — see dump above");
    return 0;
}
