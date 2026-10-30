// test_table.cpp
//
// The TUI text-grid table renderer (task 36, design section 4.14, D-TABLE / FR-20): parse
// GFM tables, allocate widths, wrap, draw - and degrade to a vertical key-value layout
// instead of blanking or overrunning.
//
// What is pinned here, case by case:
//
//   1. the source      `ui/table.cpp` asks no profile question of its own (`tuiIsTextGrid`),
//                      builds no colour (`ImVec4(`), reads no display size, names no v4
//                      type and includes no view header - the layering every `ui/` file is
//                      held to by `tools/gate_ratchet.txt`.
//   2. the parse       header, delimiter alignment (`:---`, `:--:`, `---:`), body, escaped
//                      pipes, inline code as opaque text (an unmatched backtick is literal, so
//                      it cannot fold a row), optional outer pipes, short rows padded and long
//                      ones dropped, CRLF, and every shape that is *not* a table (a setext
//                      heading, a header without a delimiter, a delimiter of the wrong width,
//                      a bad alignment cell, an interior blank line). A rejected block leaves
//                      the caller's table untouched.
//   3. plain cells     the inline markdown a model writes reduced to what one cell holds:
//                      markers dropped, link labels kept, code kept verbatim, escapes honoured
//                      (a `Path` cell keeps its backslashes) - and a lone `*` or `~` (a glob in
//                      a tool name) kept.
//   4. the detection   tables in any container: fenced code (backticks and tildes) is skipped,
//                      an unterminated fence swallows what follows, a fence four columns past
//                      its container is code, a pipe inside inline code is not a separator, a
//                      lone backtick in the header does not hide the table, a CRLF span ends at
//                      the newline, and two tables come back in order.
//   5. the widths      the documented arithmetic: chrome `kTableBorderWidth()`, a floor of
//                      `kMinTableWidth`, `kMinColumnWidth` per column, the equal-share cap,
//                      the spare handed back to the columns that wrap, CJK measured in cells
//                      (not bytes, not codepoints), and the three degradations.
//   6. the measure     height = two rules + one line per wrapped cell line + the header
//                      rule, so a caller can reserve the rows before drawing - and a hand-built
//                      ragged `Table` (a public aggregate) clamps instead of reading past a row,
//                      down to the narrowest row a `Table` can hold, one with no cells at all.
//   7. the drawing     a real 103x40 cell screen read back cell by cell: corners and rules on
//                      the exact cells, text inside the box (never on a rule), alignment left
//                      / centre / right at the arithmetic offsets, **no filled cell at all**,
//                      nothing painted outside the rect, and a table taller than the rect
//                      clipped instead of overrunning.
//   8. the ASCII column `+ - |` on a surface whose glyph set is ASCII, same geometry.
//   9. the fallback    too many columns and too narrow a rect are both text: key-value lines
//                      with no border glyphs and nothing outside the rect, and an empty table
//                      (a failed parse) draws nothing at all.
//  10. the goldens     one screen per reference size (103x82, 110x30, 120x40) plus an ASCII
//                      and a key-value variant, pinning the glyphs *and* the inks and fills.
//  11. the reference   the three sizes the acceptance names, drawn without the golden's caption
//      sizes           and checked cell by cell: a rule carries no text (no header artifact), the
//                      table paints inside its rect (no blanking) and nothing outside it.
//
// The grid is the real text backend (the `TScreen` pattern of `test_session_filter_smoke.cpp`),
// so a golden is what the terminal would show, not what the drawing code believes it drew.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = usage error.
//
//   ./test_table [-v] [--update]
//
// `--update` rewrites the fixtures; do that only for a deliberate rendering change, and commit
// the diff with it. They are captured with `LLMFUN_ASCII=0` and the true-colour theme, so a CJK
// locale, `TERM=linux` or a `COLORTERM` cannot change a golden (the ASCII variant sets the
// variable itself).

#include "ui/icons.h"
#include "ui/metrics.h"
#include "ui/table.h"
#include "ui/text.h"
#include "ui/tokens.h"

#include "imtui/imtui.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ui = llmfun::tui::ui;

namespace {

int g_checks = 0;
int g_failures = 0;
bool g_verbose = false;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) {
        if (g_verbose)
            std::printf("ok   %s\n", what.c_str());
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

[[noreturn]] void usage(const char* argv0, const char* problem) {
    std::fprintf(stderr, "usage: %s [-v] [--update]   (%s)\n", argv0, problem);
    std::exit(2);
}

// ---------------------------------------------------------------------------
// The cell grid the text backend writes
// ---------------------------------------------------------------------------

ImTui::TScreen g_screen;

std::string utf8Of(uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

constexpr uint32_t kBoxVertical = 0x2502;    // │
constexpr uint32_t kBoxHorizontal = 0x2500;  // ─
constexpr uint32_t kBoxTopLeft = 0x250C;     // ┌
constexpr uint32_t kBoxTopMid = 0x252C;      // ┬
constexpr uint32_t kBoxTopRight = 0x2510;    // ┐
constexpr uint32_t kBoxMidLeft = 0x251C;     // ├
constexpr uint32_t kBoxMidMid = 0x253C;      // ┼
constexpr uint32_t kBoxMidRight = 0x2524;    // ┤
constexpr uint32_t kBoxBottomLeft = 0x2514;  // └
constexpr uint32_t kBoxBottomMid = 0x2534;   // ┴
constexpr uint32_t kBoxBottomRight = 0x2518; // ┘

struct Grid {
    int nx = 0;
    int ny = 0;
    std::vector<uint32_t> ch;
    std::vector<uint8_t> fg;
    std::vector<uint8_t> bg;

    uint32_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return ch[static_cast<std::size_t>(y) * nx + x];
    }
    uint8_t fgAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return fg[static_cast<std::size_t>(y) * nx + x];
    }
    uint8_t bgAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return bg[static_cast<std::size_t>(y) * nx + x];
    }
    /// A cell nothing was drawn in: no character and no fill.
    bool untouched(int x, int y) const { return at(x, y) == 0 && bgAt(x, y) == 0; }

    std::string rowText(int y) const {
        std::string text;
        for (int x = 0; x < nx; ++x) {
            const uint32_t cp = at(x, y);
            text += cp == 0 ? std::string(" ") : utf8Of(cp);
        }
        return text;
    }

    std::string span(int x, int y, int cells) const {
        std::string text;
        for (int i = 0; i < cells; ++i) {
            const uint32_t cp = at(x + i, y);
            text += cp == 0 ? std::string(" ") : utf8Of(cp);
        }
        return text;
    }

    bool rowContains(int y, const std::string& needle) const {
        return rowText(y).find(needle) != std::string::npos;
    }

    bool contains(const std::string& needle) const {
        for (int y = 0; y < ny; ++y)
            if (rowContains(y, needle))
                return true;
        return false;
    }

    int paintedInside(int x0, int y0, int x1, int y1) const {
        int count = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                count += untouched(x, y) ? 0 : 1;
        return count;
    }

    int paintedOutside(int x0, int y0, int x1, int y1) const {
        int count = 0;
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x) {
                const bool inside = x >= x0 && x < x1 && y >= y0 && y < y1;
                if (!inside && !untouched(x, y))
                    ++count;
            }
        return count;
    }

    void rowGlyphs(int y, std::vector<uint32_t>& out) const {
        out.clear();
        for (int x = 0; x < nx; ++x)
            if (at(x, y) != 0)
                out.push_back(at(x, y));
    }
};

Grid capture() {
    Grid grid;
    grid.nx = g_screen.nx;
    grid.ny = g_screen.ny;
    const std::size_t cells = static_cast<std::size_t>(grid.nx) * grid.ny;
    grid.ch.resize(cells);
    grid.fg.resize(cells);
    grid.bg.resize(cells);
    for (std::size_t i = 0; i < cells; ++i) {
        grid.ch[i] = g_screen.data[i].ch;
        grid.fg[i] = g_screen.data[i].fg;
        grid.bg[i] = g_screen.data[i].bg;
    }
    return grid;
}

/// One frame of the text grid: a decoration-free root window filling the display, the body
/// drawing inside it, then the frame rendered through the real backend.
Grid frame(int screenW, int screenH, const std::function<void(const ui::Metrics&)>& body) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(screenW), static_cast<float>(screenH));
    io.DeltaTime = 1.0f / 60.0f;
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(screenW), static_cast<float>(screenH)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("table-frame", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    body(ui::metricsFor(true, 1.0f, 1.0f));
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    return capture();
}

ui::Rect cellRect(float x, float y, float w, float h) {
    ui::Rect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    r.cells = true;
    return r;
}

/// A table drawn at `rect` on a `screenW` x `screenH` grid.
Grid drawTable(const ui::Table& table, int screenW, int screenH, const ui::Rect& rect) {
    return frame(screenW, screenH, [&](const ui::Metrics& m) { ui::renderTable(table, rect, m); });
}

/// A scene: a caption on row 1 (so "no blanking" is visible in a golden) and one table under
/// it, filling the rest of the screen.
Grid drawScene(const char* caption, const ui::Table& table, int screenW, int screenH) {
    return frame(screenW, screenH, [&](const ui::Metrics& m) {
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(1.0f, 1.0f), ImGui::GetColorU32(ui::color(ui::ColorToken::textMid)), caption);
        ui::renderTable(
            table,
            cellRect(1.0f, 3.0f, static_cast<float>(screenW - 2), static_cast<float>(screenH - 3)),
            m);
    });
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

ui::Table parseOrFail(const char* text, const std::string& what) {
    ui::Table table;
    const bool ok = ui::parseGfmTable(text, table);
    check(ok, "parses: " + what);
    if (!ok)
        check(false, "the parsed table is usable: " + what);
    return table;
}

/// The three-column tool table every block in the design's own sketch shows.
const char* kToolTable = "| Tool | Calls | Time |\n"
                         "|------|------:|-----:|\n"
                         "| read_file | 3 | 240ms |\n"
                         "| executeCommand | 1 | 9.1s |\n";

std::string cellText(const ui::Table& table, std::size_t row, std::size_t column) {
    if (row >= table.rows.size() || column >= table.rows[row].size())
        return "<out of range>";
    return table.rows[row][column].text;
}

// ---------------------------------------------------------------------------
// 1. The source obeys the layering rules
// ---------------------------------------------------------------------------

int countOccurrences(const std::string& haystack, const std::string& needle) {
    int count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size()))
        ++count;
    return count;
}

void checkSource() {
    const char* path = TEST_TABLE_SOURCE;
    std::ifstream in(path);
    check(in.good(), std::string("ui/table.cpp is readable (") + path + ")");
    if (!in.good())
        return;
    std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    check(source.find("tuiIsTextGrid") == std::string::npos,
          "ui/table.cpp asks no profile question of its own (no tuiIsTextGrid)");
    check(source.find("ImVec4(") == std::string::npos,
          "ui/table.cpp builds no colour (both inks come from ui::color())");
    check(source.find("DisplaySize") == std::string::npos,
          "ui/table.cpp reads no display size (the rect is the geometry)");
    check(source.find("ChatMessage") == std::string::npos &&
              source.find("RenderGroup") == std::string::npos,
          "ui/table.cpp names no v4 type");
    check(source.find("#include \"tui.h\"") == std::string::npos &&
              source.find("#include \"views/") == std::string::npos,
          "ui/table.cpp includes neither tui.h nor a view header");
    check(source.find("#include \"ui/table.h\"") != std::string::npos,
          "ui/table.cpp implements the header it documents");
    check(countOccurrences(source, "ColorToken::") >= 3,
          "the three inks are named tokens (border, header, body)");
}

// ---------------------------------------------------------------------------
// 2. The parse
// ---------------------------------------------------------------------------

void checkParse() {
    const ui::Table tool = parseOrFail(kToolTable, "the tool table");
    check(tool.rows.size() == 3, "three rows: a header and two body rows");
    check(tool.columns() == 3, "three columns");
    check(cellText(tool, 0, 0) == "Tool" && cellText(tool, 0, 1) == "Calls" &&
              cellText(tool, 0, 2) == "Time",
          "the header cells");
    check(cellText(tool, 1, 0) == "read_file" && cellText(tool, 1, 1) == "3" &&
              cellText(tool, 2, 2) == "9.1s",
          "the body cells");
    check(tool.rows[0][0].header && !tool.rows[1][0].header, "only the first row is the header");
    check(tool.rows[0][0].align == ui::kAlignLeft && tool.rows[0][1].align == ui::kAlignRight &&
              tool.rows[0][2].align == ui::kAlignRight,
          "`------:` is right, `------` is left");
    check(tool.rows[2][1].align == ui::kAlignRight, "a body cell carries its column's alignment");

    // Optional outer pipes, and no spaces around the separators.
    const ui::Table bare = parseOrFail("a | b\n--- | ---\n1 | 2", "a row with no outer pipes");
    check(bare.columns() == 2 && cellText(bare, 0, 0) == "a" && cellText(bare, 1, 1) == "2",
          "the outer pipes are optional");

    // Centre alignment, and a header with no body rows.
    const ui::Table centre = parseOrFail("| a | b | c |\n|:--|:-:|--:|\n", "a header-only table");
    check(centre.rows.size() == 1, "a header-only table is a table");
    check(centre.rows[0][0].align == ui::kAlignLeft &&
              centre.rows[0][1].align == ui::kAlignCenter &&
              centre.rows[0][2].align == ui::kAlignRight,
          "`:--` left, `:-:` centre, `--:` right");

    // Escaped pipes stay in the cell; the escape is resolved by the plain-text pass.
    const ui::Table escaped = parseOrFail("| a \\| b | c |\n|---|---|\n", "an escaped pipe");
    check(escaped.columns() == 2, "an escaped pipe does not split a cell");
    check(cellText(escaped, 0, 0) == "a | b", "an escaped pipe becomes a pipe");

    // Inline code is opaque: its pipe is text, not a separator.
    const ui::Table code = parseOrFail("| `x|y` | b |\n|---|---|\n", "inline code with a pipe");
    check(code.columns() == 2, "a pipe inside backticks does not split a cell");
    check(cellText(code, 0, 0) == "x|y", "inline code keeps its content");

    // A backtick run that nothing closes is literal: it must not swallow the row's separators.
    const ui::Table loneTick = parseOrFail("| 1 | `x | y |\n|---|---|---|\n", "a lone backtick");
    check(loneTick.columns() == 3, "a lone backtick does not fold the remaining columns into one");
    check(cellText(loneTick, 0, 1) == "`x", "the unmatched backtick is kept as text");
    check(cellText(loneTick, 0, 2) == "y", "the column after it is still its own cell");
    const ui::Table loneTickHeader =
        parseOrFail("| a ` b | c |\n|---|---|\n| 1 | 2 |\n", "a lone backtick in the header");
    check(loneTickHeader.columns() == 2, "a lone backtick in the header keeps the column count");
    check(cellText(loneTickHeader, 0, 0) == "a ` b", "the header cell keeps the backtick");
    check(cellText(loneTickHeader, 1, 1) == "2", "the body row lines up with the header");
    const ui::Table unclosedTick =
        parseOrFail("| unclosed `tick |\n|---|\n", "an unclosed backtick");
    check(cellText(unclosedTick, 0, 0) == "unclosed `tick",
          "the closing pipe is a separator, not part of the cell");

    // Short rows pad, long rows drop the surplus: a column index is valid on every row.
    const ui::Table ragged =
        parseOrFail("| a | b | c |\n|---|---|---|\n| 1 |\n| 1 | 2 | 3 | 4 |", "a ragged table");
    check(ragged.columns() == 3, "the header decides the column count");
    check(cellText(ragged, 1, 1).empty() && cellText(ragged, 1, 2).empty(),
          "a short row is padded with empty cells");
    check(cellText(ragged, 2, 2) == "3" && ragged.rows[2].size() == 3,
          "a long row drops the surplus cell");

    // CRLF, which is what a body read from a file looks like.
    const ui::Table crlf = parseOrFail("| a | b |\r\n|---|---|\r\n| 1 | 2 |\r\n", "a CRLF table");
    check(cellText(crlf, 0, 1) == "b" && cellText(crlf, 1, 1) == "2",
          "a trailing carriage return is stripped");

    // Not tables. Each one is a shape a markdown body really contains.
    struct Reject {
        const char* text;
        const char* what;
    };
    const Reject rejects[] = {
        {"", "an empty block"},
        {"| a | b |", "a header with no delimiter row"},
        {"a | b\n--- | --- | ---\n", "a delimiter of the wrong width"},
        {"| a |\n| : |\n", "a delimiter cell that is not an alignment"},
        {"| a |\n| a |\n", "a delimiter cell that is not hyphens"},
        {"Title\n---\n", "a setext heading"},
        {"| a |\n\n| --- |\n", "an interior blank line"},
        {"plain text\nwith no pipes\n", "prose"},
    };
    for (const Reject& reject : rejects) {
        ui::Table table;
        check(!ui::parseGfmTable(reject.text, table), std::string("rejects ") + reject.what);
        check(table.empty(),
              std::string("a rejected block leaves the table empty: ") + reject.what);
    }

    // A failed parse must not touch the caller's table.
    ui::Table kept = tool;
    check(!ui::parseGfmTable("not a table", kept), "a failed parse answers false");
    check(kept.rows.size() == tool.rows.size() && cellText(kept, 0, 0) == "Tool",
          "a failed parse leaves the caller's table untouched");
}

// ---------------------------------------------------------------------------
// 3. Plain cell text
// ---------------------------------------------------------------------------

void checkPlainText() {
    const ui::Table table = parseOrFail(
        "| **bold** | *em* | `code` | ~~gone~~ |\n"
        "|---|---|---|---|\n"
        "| [label](http://example.com/x) | ![alt](img.png) | a \\* b | read_file(*.cpp) |\n",
        "inline markdown in cells");
    check(cellText(table, 0, 0) == "bold", "`**bold**` loses its markers");
    check(cellText(table, 0, 1) == "em", "`*em*` loses its markers");
    check(cellText(table, 0, 2) == "code", "inline code keeps its content");
    check(cellText(table, 0, 3) == "gone", "`~~struck~~` loses its markers");
    check(cellText(table, 1, 0) == "label", "a link keeps its label");
    check(cellText(table, 1, 1) == "alt", "an image keeps its alt text");
    check(cellText(table, 1, 2) == "a * b", "an escaped marker is the marker");
    check(cellText(table, 1, 3) == "read_file(*.cpp)",
          "a lone `*` and intraword `_` are text (a tool name stays readable)");

    const ui::Table markers =
        parseOrFail("| x ~ y | a_b | 50% |\n|---|---|---|\n", "lone markers and underscores");
    check(cellText(markers, 0, 0) == "x ~ y", "a single `~` is not strikethrough");
    check(cellText(markers, 0, 1) == "a_b", "an intraword `_` is not emphasis");

    // A backslash escapes ASCII punctuation only (CommonMark): a path keeps its separators,
    // including before a byte a naive rule would have eaten.
    const ui::Table paths = parseOrFail("| p | q |\n|---|---|\n"
                                        "| C:\\Users\\me\\new | path\\to\\thing |\n",
                                        "backslashes in a path");
    check(cellText(paths, 1, 0) == "C:\\Users\\me\\new",
          "`C:\\Users\\me\\new` keeps its backslashes (a letter is not escapable)");
    check(cellText(paths, 1, 1) == "path\\to\\thing", "`path\\to\\thing` keeps its backslashes");
    const ui::Table escapedPipe =
        parseOrFail("| a \\| b | c |\n|---|---|\n", "an escaped pipe again");
    check(cellText(escapedPipe, 0, 0) == "a | b", "`\\|` still resolves to a pipe");

    // A link destination may carry balanced parentheses; an unbalanced one stays text.
    const ui::Table links = parseOrFail("| [x](https://e/x_(y)) | [z](u |\n|---|---|\n",
                                        "a link with parentheses in the URL");
    check(cellText(links, 0, 0) == "x", "a URL's own parentheses do not end the destination");
    check(cellText(links, 0, 1) == "[z](u", "an unclosed destination stays plain text");

    // An escaped paren is part of the destination, not the end of it.
    const ui::Table escapedParen =
        parseOrFail("| [x](a\\)b) | [y](a\\) |\n|---|---|\n", "a link with an escaped parenthesis");
    check(cellText(escapedParen, 0, 0) == "x",
          "an escaped `)` does not end the destination, so the label is still kept");
    check(cellText(escapedParen, 0, 1) == "[y](a)",
          "a destination that never closes stays plain text, with the escape resolved");
}

// ---------------------------------------------------------------------------
// 4. The detection
// ---------------------------------------------------------------------------

std::string spanText(std::string_view body, const ui::GfmTableSpan& span) {
    return std::string(body.substr(span.begin, span.end - span.begin));
}

void checkDetect() {
    const std::string body = "Intro line.\n"
                             "\n"
                             "| Tool | Calls |\n"
                             "|------|------:|\n"
                             "| read_file | 3 |\n"
                             "\n"
                             "After.\n";
    const std::vector<ui::GfmTableSpan> spans = ui::findGfmTables(body);
    check(spans.size() == 1, "one table in a body with prose around it");
    if (spans.size() == 1) {
        check(spanText(body, spans[0]) == "| Tool | Calls |\n|------|------:|\n| read_file | 3 |",
              "the span covers the table's lines and not the newline after them");
    }

    // Two tables, in order.
    const std::string two = "| a |\n|---|\n| 1 |\n\nmid\n\n| b |\n|---|\n| 2 |\n";
    const std::vector<ui::GfmTableSpan> pair = ui::findGfmTables(two);
    check(pair.size() == 2, "two tables in one body");
    if (pair.size() == 2) {
        check(spanText(two, pair[0]).find("| a |") == 0, "the first span is the first table");
        check(spanText(two, pair[1]).find("| b |") == 0, "the second span is the second table");
        check(pair[0].end < pair[1].begin, "the spans do not overlap");
    }

    // Fenced code is skipped - the model writes example tables inside fences.
    const std::string fenced =
        "```\n| a | b |\n|---|---|\n| 1 | 2 |\n```\n\n| c | d |\n|---|---|\n| 3 | 4 |\n";
    const std::vector<ui::GfmTableSpan> afterFence = ui::findGfmTables(fenced);
    check(afterFence.size() == 1, "a table inside a backtick fence is not a table");
    if (afterFence.size() == 1)
        check(spanText(fenced, afterFence[0]).find("| c | d |") == 0,
              "the table after the fence is found");

    const std::string tildeFence = "~~~\n| a | b |\n|---|---|\n~~~\n";
    check(ui::findGfmTables(tildeFence).empty(), "a tilde fence is skipped too");

    const std::string unterminated = "```\n| a | b |\n|---|---|\n| 1 | 2 |\n";
    check(ui::findGfmTables(unterminated).empty(),
          "an unterminated fence swallows what follows it");
    // A fence more than three columns past its container is an indented code block, so it does
    // not open a fence and does not swallow the table after it.
    const std::string indentedFence = "    ```\n    code\n\n| a | b |\n|---|---|\n";
    check(ui::findGfmTables(indentedFence).size() == 1,
          "a fence four columns in is an indented code block, not a fence");
    // ...while a fence at a list item's four-column content column is still a fence.
    const std::string listFence =
        "10. Results:\n\n    ```\n    | a | b |\n    |---|---|\n    ```\n";
    check(ui::findGfmTables(listFence).empty(), "a fence at the item's content column is a fence");

    const std::string inlineCode = "`a|b`\n|---|---|\n";
    check(ui::findGfmTables(inlineCode).empty(), "a pipe inside inline code is not a separator");

    // A header with a lone backtick is still a table: before the splitter learned that an
    // unmatched run is literal, this body was left unrewritten (and rich_md drew its
    // artifacts), because the header folded into one cell and the delimiter looked wrong.
    const std::string loneTick = "| a ` b | c |\n|---|---|\n| 1 | 2 |\n";
    const std::vector<ui::GfmTableSpan> tickSpans = ui::findGfmTables(loneTick);
    check(tickSpans.size() == 1, "a lone backtick in the header does not hide the table");
    if (tickSpans.size() == 1) {
        ui::Table table;
        check(ui::parseGfmTable(spanText(loneTick, tickSpans[0]), table) && table.columns() == 2,
              "and the detected span parses into two columns");
    }

    // A CRLF body: the span ends after the last line's CR, so a byte-range replacement cannot
    // strand a carriage return in front of the fence it writes.
    const std::string crlf = "intro\r\n| a | b |\r\n|---|---|\r\n| 1 | 2 |\r\nafter\r\n";
    const std::vector<ui::GfmTableSpan> crlfSpans = ui::findGfmTables(crlf);
    check(crlfSpans.size() == 1, "a CRLF body is scanned");
    if (crlfSpans.size() == 1) {
        check(crlfSpans[0].end < crlf.size() && crlf[crlfSpans[0].end] == '\n',
              "the span ends at the newline, not before the carriage return");
        ui::Table table;
        check(ui::parseGfmTable(spanText(crlf, crlfSpans[0]), table) && table.columns() == 2,
              "the CRLF span parses");
    }

    check(ui::findGfmTables("just prose\nwith a | pipe but no delimiter\n").empty(),
          "no table in prose with a lone pipe");
    check(ui::findGfmTables("").empty(), "no table in an empty body");
}

// ---------------------------------------------------------------------------
// 5. The widths
// ---------------------------------------------------------------------------

void checkWidths() {
    check(ui::kTableBorderWidth(1) == 4 && ui::kTableBorderWidth(3) == 10 &&
              ui::kTableBorderWidth(8) == 25,
          "the chrome identity is 3*cols + 1");

    const ui::Table tool = parseOrFail(kToolTable, "the tool table");
    const ui::TableWidths wide = ui::allocateTableWidths(tool, 103);
    check(wide.fits, "three columns fit 103 cells");
    check(wide.colCells.size() == 3, "one width per column");
    check(wide.colCells[0] == 14 && wide.colCells[1] == 5 && wide.colCells[2] == 5,
          "each column is as wide as its widest cell (executeCommand is 14)");
    check(wide.widthCells == ui::kTableBorderWidth(3) + 24,
          "the table is the chrome plus the content");
    check(wide.widthCells == 34, "the table takes only what its columns need");

    // Below the floor, and per-column minimums that do not fit.
    check(!ui::allocateTableWidths(tool, ui::kMinTableWidth - 1).fits,
          "narrower than kMinTableWidth degrades");
    check(!ui::allocateTableWidths(tool, 20).fits,
          "three columns need kMinColumnWidth each (20 is not enough)");
    check(ui::allocateTableWidths(tool, 22).fits, "22 is enough for three columns");

    // Too many columns.
    std::string many;
    many += "|";
    for (int c = 0; c < ui::kMaxTableColumns + 1; ++c)
        many += " h" + std::to_string(c) + " |";
    many += "\n|";
    for (int c = 0; c < ui::kMaxTableColumns + 1; ++c)
        many += "---|";
    many += "\n|";
    for (int c = 0; c < ui::kMaxTableColumns + 1; ++c)
        many += " v" + std::to_string(c) + " |";
    many += "\n";
    const ui::Table tooMany = parseOrFail(many.c_str(), "a nine-column table");
    check(tooMany.columns() == ui::kMaxTableColumns + 1, "the parser keeps all nine columns");
    check(!ui::allocateTableWidths(tooMany, 400).fits,
          "more than kMaxTableColumns columns degrades to key-value");

    // The cap: one long cell cannot take the whole width, but the spare comes back to it
    // once the other columns have what they need.
    std::string oneLong = "| a | b |\n|---|---|\n| x | ";
    oneLong += std::string(200, 'y');
    oneLong += " |\n";
    const ui::Table longTable = parseOrFail(oneLong.c_str(), "a table with one very long cell");
    const ui::TableWidths capped = ui::allocateTableWidths(longTable, 40);
    check(capped.fits, "two columns fit 40 cells");
    check(capped.colCells[0] == ui::kMinColumnWidth, "the short column keeps its minimum");
    check(capped.colCells[1] == 40 - ui::kTableBorderWidth(2) - ui::kMinColumnWidth,
          "the long column gets the cap and then the spare");
    check(capped.widthCells == 40, "the table fills the room it was given");

    // CJK is measured in cells: three ideographs are six cells, not three and not nine.
    const ui::Table cjk =
        parseOrFail("| \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E | b |\n|---|---|\n", "a CJK cell");
    check(ui::displayWidth(cellText(cjk, 0, 0)) == 6, "the test's own measure agrees (6 cells)");
    const ui::TableWidths cjkWidths = ui::allocateTableWidths(cjk, 60);
    check(cjkWidths.fits && cjkWidths.colCells[0] == 6,
          "a CJK column is six cells wide, not three codepoints");

    // Every column keeps the minimum whenever the layout fits.
    for (int avail = ui::kMinTableWidth; avail <= 120; ++avail) {
        const ui::TableWidths widths = ui::allocateTableWidths(tool, avail);
        if (!widths.fits)
            continue;
        bool ok = true;
        for (const int cells : widths.colCells)
            ok = ok && cells >= ui::kMinColumnWidth;
        if (!ok) {
            check(false, "a column fell below kMinColumnWidth at " + std::to_string(avail));
            return;
        }
        if (widths.widthCells > avail) {
            check(false, "a table overran its room at " + std::to_string(avail));
            return;
        }
    }
    check(true, "every fitting width keeps the minimum and stays inside the room");
}

// ---------------------------------------------------------------------------
// 6. The measure
// ---------------------------------------------------------------------------

void checkMeasure() {
    const ui::Table tool = parseOrFail(kToolTable, "the tool table");
    const ui::TableSize size = ui::measureTable(tool, 103);
    check(size.w == 34, "the measured width is the table's own width");
    check(size.h == 6, "two rules, three rows and the header rule");

    // A wrapping cell raises the height by exactly the lines it wraps to.
    std::string longTable = "| a | b |\n|---|---|\n| x | word ";
    for (int i = 0; i < 12; ++i)
        longTable += "word ";
    longTable += "|\n";
    const ui::Table table = parseOrFail(longTable.c_str(), "a wrapping cell");
    const ui::TableWidths widths = ui::allocateTableWidths(table, 40);
    check(widths.fits, "the wrapping table fits 40 cells");
    const int wrappedLines =
        static_cast<int>(ui::wrap(cellText(table, 1, 1), widths.colCells[1]).size());
    check(wrappedLines > 1, "the long cell really wraps");
    const ui::TableSize measured = ui::measureTable(table, 40);
    check(measured.h == 2 + 1 + wrappedLines + 1,
          "the height is two rules, the header, its rule and the wrapped lines");

    // The key-value fallback measures itself from its lines.
    const std::vector<std::string> lines = ui::tableKeyValueLines(tool);
    check(lines.size() == 7, "one line per body cell (two rows of three) plus the row gap");
    check(lines[0] == "Tool: read_file" && lines[1] == "Calls: 3" && lines[2] == "Time: 240ms",
          "a key-value line is `header: value`");
    check(lines[3].empty(), "two rows are separated by one empty line");
    check(lines[4] == "Tool: executeCommand" && lines[6] == "Time: 9.1s",
          "the second row is written the same way");
    const ui::TableSize narrow = ui::measureTable(tool, 12);
    check(narrow.h == static_cast<int>(lines.size()), "the fallback is one row per line");
    check(narrow.w <= 12, "the fallback is clipped to the room it has");
}

/// `Table` is a public aggregate, so a hand-built row can be shorter than the header (the parser
/// never produces one), and it can be empty. Reading `row[c]` for every column, or `row.front()`
/// at all, would be a crash rather than a layout: allocation, measure, the key-value fallback and
/// draw all clamp to the shorter of the two.
void checkRaggedTable() {
    ui::Table table;
    table.rows.push_back({ui::TableCell{"a", true, ui::kAlignLeft},
                          ui::TableCell{"b", true, ui::kAlignLeft},
                          ui::TableCell{"c", true, ui::kAlignLeft}});
    table.rows.push_back({ui::TableCell{"1", false, ui::kAlignLeft}});

    const ui::TableWidths widths = ui::allocateTableWidths(table, 60);
    check(widths.fits && widths.colCells.size() == 3,
          "a ragged table still allocates a width per column");
    const ui::TableSize size = ui::measureTable(table, 60);
    check(size.h == 5, "a ragged table measures two rules, a header, its rule and one body line");
    const Grid grid = drawTable(table, 60, 12, cellRect(0.0f, 0.0f, 60.0f, 12.0f));
    check(grid.span(2, 1, 1) == "a", "a ragged table draws its header");
    check(grid.span(2, 3, 1) == "1", "and the cells its short row has");
    check(grid.paintedOutside(0, 0, widths.widthCells, size.h) == 0,
          "a ragged table paints nothing outside its own width");

    // The vertical fallback is the other way into a hand-built row, so it clamps too: one line
    // per header cell, a missing cell drawn as an empty value, and no read past the row.
    const std::vector<std::string> fallback = ui::tableKeyValueLines(table);
    check(fallback.size() == 3, "the key-value fallback keeps a line per header cell");
    check(fallback[0] == "a: 1" && fallback[1] == "b: " && fallback[2] == "c: ",
          "a cell the short row does not have is an empty value, not a read past the row");
    const ui::TableSize narrow = ui::measureTable(table, 12);
    check(narrow.w == 4 && narrow.h == 3,
          "a room too narrow for a grid measures the fallback instead of overrunning");

    // A row with no cells at all is the shortest a hand-built row can be: the header ink comes
    // from the row index, so drawing it does not read `cells.front()`.
    ui::Table emptyRow;
    emptyRow.rows.push_back(
        {ui::TableCell{"a", true, ui::kAlignLeft}, ui::TableCell{"b", true, ui::kAlignLeft}});
    emptyRow.rows.push_back({});
    const ui::TableSize emptySize = ui::measureTable(emptyRow, 60);
    check(emptySize.h == 5, "a row with no cells still measures one line for itself");
    const Grid emptyGrid = drawTable(emptyRow, 60, 12, cellRect(0.0f, 0.0f, 60.0f, 12.0f));
    check(emptyGrid.span(2, 1, 1) == "a" && emptyGrid.span(9, 1, 1) == "b",
          "a row with no cells still draws the header beside it");
    check(ui::tableKeyValueLines(emptyRow).size() == 2,
          "a row with no cells is two empty values in the fallback");
}

// ---------------------------------------------------------------------------
// 7. The drawing
// ---------------------------------------------------------------------------

bool isTopGlyph(uint32_t cp) {
    return cp == kBoxTopLeft || cp == kBoxTopMid || cp == kBoxTopRight || cp == kBoxHorizontal;
}
bool isMidGlyph(uint32_t cp) {
    return cp == kBoxMidLeft || cp == kBoxMidMid || cp == kBoxMidRight || cp == kBoxHorizontal;
}
bool isBottomGlyph(uint32_t cp) {
    return cp == kBoxBottomLeft || cp == kBoxBottomMid || cp == kBoxBottomRight ||
           cp == kBoxHorizontal;
}

void checkRulesOf(const Grid& grid, int y, bool (*isRule)(uint32_t), const std::string& what) {
    std::vector<uint32_t> glyphs;
    grid.rowGlyphs(y, glyphs);
    bool ok = !glyphs.empty();
    for (const uint32_t cp : glyphs)
        ok = ok && isRule(cp);
    check(ok, what);
}

/// The whole-cell checks every drawn grid must pass: corners, rules, borders on every text
/// row, no filled cell, nothing outside the rect, and the content present.
void checkTableGeometry(const Grid& grid, const ui::Rect& r, const std::vector<int>& left, int rows,
                        const std::string& what) {
    const int x0 = static_cast<int>(r.x);
    const int y0 = static_cast<int>(r.y);
    check(grid.at(x0, y0) == kBoxTopLeft, what + ": the top-left corner");
    check(grid.at(x0 + left.back(), y0) == kBoxTopRight, what + ": the top-right corner");
    check(grid.at(x0, y0 + rows - 1) == kBoxBottomLeft, what + ": the bottom-left corner");
    check(grid.at(x0 + left.back(), y0 + rows - 1) == kBoxBottomRight,
          what + ": the bottom-right corner");
    checkRulesOf(grid, y0, isTopGlyph, what + ": the top rule carries no text");
    checkRulesOf(grid, y0 + rows - 1, isBottomGlyph, what + ": the bottom rule carries no text");
    int headerRules = 0;
    for (int row = 1; row < rows - 1; ++row) {
        if (grid.at(x0, y0 + row) == kBoxMidLeft) {
            ++headerRules;
            checkRulesOf(grid, y0 + row, isMidGlyph,
                         what + ": the header rule carries no text either");
            continue;
        }
        bool ok = true;
        for (const int offset : left)
            ok = ok && grid.at(x0 + offset, y0 + row) == kBoxVertical;
        check(ok, what + ": every column edge on row " + std::to_string(row));
    }
    check(headerRules == (rows > 3 ? 1 : 0), what + ": exactly one header rule, above the body");
    bool noFill = true;
    for (int y = 0; y < grid.ny; ++y)
        for (int x = 0; x < grid.nx; ++x)
            noFill = noFill && grid.bgAt(x, y) == 0;
    check(noFill, what + ": no cell is filled (the table paints glyphs only)");
    check(grid.paintedOutside(x0, y0, x0 + left.back() + 1, y0 + rows) == 0,
          what + ": nothing is painted outside the table");
    check(grid.paintedInside(x0, y0, x0 + left.back() + 1, y0 + rows) > 0,
          what + ": the table is not blank");
}

void checkRender() {
    const ui::Table tool = parseOrFail(kToolTable, "the tool table");
    const ui::TableWidths widths = ui::allocateTableWidths(tool, 103);
    const ui::Rect rect = cellRect(2.0f, 1.0f, 103.0f, 20.0f);
    const Grid grid = drawTable(tool, 103, 40, rect);

    // The column offsets the renderer must have used, recomputed from the documented
    // anatomy: one border, one cell of padding, the content, one cell of padding.
    std::vector<int> left;
    left.push_back(0);
    for (const int cells : widths.colCells)
        left.push_back(left.back() + 1 + ui::kCellPadding + cells + ui::kCellPadding);
    check(left.back() + 1 == widths.widthCells, "the column offsets add up to the table width");

    const ui::TableSize size = ui::measureTable(tool, 103);
    checkTableGeometry(grid, rect, left, size.h, "the tool table");

    const int x0 = static_cast<int>(rect.x);
    const int y0 = static_cast<int>(rect.y);
    check(grid.span(x0 + 2, y0 + 1, 4) == "Tool", "the first header cell is inside the box");
    check(grid.rowText(y0 + 1).find("Calls") != std::string::npos, "the second header cell");
    check(grid.span(x0 + 2, y0 + 3, 9) == "read_file", "the first body cell");
    check(grid.at(x0 + left[1], y0 + 3) == kBoxVertical, "the column edge inside the body");

    // Right alignment: `3` ends at the last content cell of a 5-cell column.
    const int callsRightEdge = x0 + left[2] - 1 - ui::kCellPadding;
    check(grid.at(callsRightEdge, y0 + 3) == '3',
          "a right-aligned number ends at the column's right padding");
    check(grid.at(callsRightEdge - 1, y0 + 3) == 0 || grid.at(callsRightEdge - 1, y0 + 3) == ' ',
          "a right-aligned number is padded on the left");

    // Centre alignment: a one-cell value in a four-cell column starts one cell in (the slack
    // is three, and a half rounds down).
    const ui::Table centred = parseOrFail("| h |\n|:-:|\n| x |\n", "a centred column");
    const Grid centredGrid = drawTable(centred, 40, 12, cellRect(0.0f, 0.0f, 40.0f, 12.0f));
    check(centredGrid.at(2 + 1, 3) == 'x', "a centred cell sits at half the slack");
    check(centredGrid.at(2, 3) == 0 || centredGrid.at(2, 3) == ' ', "the cell is not left-aligned");

    // The header is drawn in the accent ink and the body in the primary ink; the rules are
    // neither (so a rule that borrowed the header's ink fails a golden).
    const uint8_t headerInk = grid.fgAt(x0 + 2, y0 + 1);
    const uint8_t bodyInk = grid.fgAt(x0 + 2, y0 + 3);
    check(headerInk != 0 && bodyInk != 0 && headerInk != bodyInk,
          "the header and the body draw in different inks");

    // Wider than the table: the spare stays to the right, unpainted.
    check(grid.untouched(x0 + widths.widthCells, y0 + 1),
          "the table does not stretch across the room it was given");

    // A table taller than the rect is clipped, not drawn over the next block.
    const Grid clipped = drawTable(tool, 103, 40, cellRect(2.0f, 1.0f, 103.0f, 4.0f));
    check(clipped.paintedOutside(2, 1, 2 + widths.widthCells, 5) == 0,
          "a clipped table paints nothing below its rect");
    check(clipped.rowContains(4, "read_file"), "the rows that fit are still drawn");

    // No room at all draws nothing (the caller asked for zero rows).
    const Grid none = drawTable(tool, 103, 40, cellRect(2.0f, 1.0f, 103.0f, 0.0f));
    check(none.paintedInside(0, 0, none.nx, none.ny) == 0, "a rect with no rows draws nothing");

    // A pixel rect is a different renderer's job (design section 6.8): fail closed.
    ui::Rect pixel = rect;
    pixel.cells = false;
    const Grid gui = frame(103, 40, [&](const ui::Metrics&) {
        ui::renderTable(tool, pixel, ui::metricsFor(false, 1.0f, 1.0f));
    });
    check(gui.paintedInside(0, 0, gui.nx, gui.ny) == 0,
          "a pixel rect draws nothing (the GUI keeps rich_md's own tables)");
}

void checkAsciiBorders() {
    ::setenv("LLMFUN_ASCII", "1", 1);
    const ui::Table tool = parseOrFail(kToolTable, "the tool table");
    const ui::TableWidths widths = ui::allocateTableWidths(tool, 103);
    const Grid grid = drawTable(tool, 103, 20, cellRect(0.0f, 0.0f, 103.0f, 20.0f));
    check(grid.at(0, 0) == '+' && grid.at(widths.widthCells - 1, 0) == '+',
          "the ASCII column draws `+` corners");
    check(grid.at(0, 1) == '|', "the ASCII column draws `|` edges");
    check(grid.at(1, 0) == '-', "the ASCII column draws `-` rules");
    check(!grid.contains("\xE2\x94\x82"), "no box-drawing glyph reaches an ASCII surface");
    check(grid.span(2, 1, 4) == "Tool", "the ASCII table draws the same cells");
    ::setenv("LLMFUN_ASCII", "0", 1);
}

// ---------------------------------------------------------------------------
// 9. The fallback
// ---------------------------------------------------------------------------

void checkFallback() {
    // A nine-column table (more than kMaxTableColumns) becomes key-value text.
    std::string many = "|";
    for (int c = 0; c < 9; ++c)
        many += " h" + std::to_string(c) + " |";
    many += "\n|";
    for (int c = 0; c < 9; ++c)
        many += "---|";
    many += "\n|";
    for (int c = 0; c < 9; ++c)
        many += " v" + std::to_string(c) + " |";
    many += "\n";
    const ui::Table wide = parseOrFail(many.c_str(), "a nine-column table");
    const Grid grid = drawTable(wide, 60, 20, cellRect(0.0f, 0.0f, 60.0f, 20.0f));
    check(grid.rowContains(0, "h0: v0"), "the fallback writes `header: value`");
    check(grid.rowContains(8, "h8: v8"), "every column of the row is written");
    check(!grid.contains("\xE2\x94\x82") && !grid.contains("+--"), "the fallback draws no borders");
    check(grid.paintedOutside(0, 0, 60, 20) == 0, "the fallback stays inside the rect");

    // A narrow rect degrades too, and a line longer than the rect is cut, never overrun.
    const ui::Table tool = parseOrFail(kToolTable, "the tool table");
    const Grid narrow = drawTable(tool, 60, 20, cellRect(0.0f, 0.0f, 12.0f, 20.0f));
    check(narrow.rowContains(0, "Tool: re"), "a narrow rect falls back to key-value");
    check(narrow.paintedOutside(0, 0, 12, 20) == 0, "a narrow fallback never overruns its rect");

    // A failed parse leaves nothing to draw - and the source-only fallback (R-02) needs no
    // renderer of its own: the block's own text is drawn by the code-block path, so an empty
    // table here is the only thing this renderer can be asked for.
    const Grid source = frame(60, 20, [](const ui::Metrics&) {
        ui::Table empty;
        ui::renderTable(empty, cellRect(0.0f, 0.0f, 60.0f, 20.0f),
                        ui::metricsFor(true, 1.0f, 1.0f));
    });
    check(source.paintedInside(0, 0, 60, 20) == 0, "an empty table paints nothing at all");
    check(ui::tableKeyValueLines(ui::Table{}).empty(),
          "an empty table has no key-value lines either");
}

// ---------------------------------------------------------------------------
// 10. The goldens (the writer and the scenes)
// ---------------------------------------------------------------------------

/// What a screen pins: the glyphs (a text dump), the *inks* (a foreground change is a role or
/// a state, so it must fail a golden exactly like a moved glyph) and the fills (which must all
/// be empty - a table paints no background).
std::string goldenText(const char* name, int screenW, int screenH, const Grid& grid) {
    std::string text = "name: ";
    text += name;
    text += "\nscreen: ";
    text += std::to_string(screenW);
    text += "x";
    text += std::to_string(screenH);
    text += "\ntext:\n";
    std::vector<std::string> rows(static_cast<std::size_t>(grid.ny));
    for (int y = 0; y < grid.ny; ++y) {
        std::string row = grid.rowText(y);
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
        rows[static_cast<std::size_t>(y)] = row;
    }
    // Trailing empty rows are not part of what a screen says; a regression that blanks the
    // screen fails the geometry checks above, not the diff.
    int kept = grid.ny;
    while (kept > 0 && rows[static_cast<std::size_t>(kept - 1)].empty())
        --kept;
    for (int y = 0; y < kept; ++y) {
        text += rows[static_cast<std::size_t>(y)];
        text += '\n';
    }
    text += "inks:\n";
    for (int y = 0; y < kept; ++y) {
        std::string runs;
        int start = -1;
        int index = -1;
        for (int x = 0; x <= grid.nx; ++x) {
            const int value =
                x < grid.nx && grid.at(x, y) != 0 ? static_cast<int>(grid.fgAt(x, y)) : -1;
            const bool same = start >= 0 && value == index;
            if (start >= 0 && !same) {
                runs += (runs.empty() ? "" : ", ") + std::to_string(start) + "-" +
                        std::to_string(x - 1) + "=" + std::to_string(index);
                start = -1;
            }
            if (value >= 0 && start < 0) {
                start = x;
                index = value;
            } else if (value < 0) {
                start = -1;
            }
        }
        if (!runs.empty()) {
            text += std::to_string(y);
            text += ": ";
            text += runs;
            text += '\n';
        }
    }
    text += "fills:\n";
    for (int y = 0; y < kept; ++y) {
        std::string runs;
        int start = -1;
        int index = -1;
        for (int x = 0; x <= grid.nx; ++x) {
            const int value = x < grid.nx ? static_cast<int>(grid.bgAt(x, y)) : -1;
            const bool same = start >= 0 && value == index;
            if (start >= 0 && !same) {
                runs += (runs.empty() ? "" : ", ") + std::to_string(start) + "-" +
                        std::to_string(x - 1) + "=" + std::to_string(index);
                start = -1;
            }
            if (value > 0 && start < 0) {
                start = x;
                index = value;
            } else if (value <= 0) {
                start = -1;
            }
        }
        if (!runs.empty()) {
            text += std::to_string(y);
            text += ": ";
            text += runs;
            text += '\n';
        }
    }
    return text;
}

struct Scene {
    const char* name;
    const char* caption;
    int w;
    int h;
    const char* body;
    bool ascii;
};

std::vector<Scene> scenes() {
    static const char* kSixColumns =
        "| Tool | Calls | Time | State | Notes | Owner |\n"
        "|------|------:|-----:|:-----:|-------|-------|\n"
        "| read_file | 3 | 240ms | ok | read 42 lines | agent |\n"
        "| executeCommand | 1 | 9.1s | ok | `npm test` in the workspace root, then a summary | "
        "agent |\n"
        "| web_search | 12 | 1.4s | warn | rate limited twice | agent |\n";
    static const char* kWrapping =
        "| Tool | Calls | Time | Notes |\n"
        "|------|------:|-----:|:-----|\n"
        "| read_file | 3 | 240ms | A cell long enough that it has to wrap inside its column "
        "instead of overrunning the table. |\n"
        "| executeCommand | 1 | 9.1s | short |\n";
    static const char* kCjk =
        "| \xE5\xB7\xA5\xE5\x85\xB7 | \xE8\xB0\x83\xE7\x94\xA8 | \xE7\x8A\xB6\xE6\x80\x81 |\n"
        "|--------|------:|------:|\n"
        "| read_file | 3 | 240ms |\n"
        "| \xE6\x89\xA7\xE8\xA1\x8C\xE5\x91\xBD\xE4\xBB\xA4 | 1 | 9.1s |\n";
    static const char* kNineColumns = "| a | b | c | d | e | f | g | h | i |\n"
                                      "|---|---|---|---|---|---|---|---|---|\n"
                                      "| 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |\n"
                                      "| 10 | 20 | 30 | 40 | 50 | 60 | 70 | 80 | 90 |\n";
    return {
        {"table-grid-103x82", "tool calls (103x82)", 103, 82, kSixColumns, false},
        {"table-grid-110x30", "a wrapping cell (110x30)", 110, 30, kWrapping, false},
        {"table-grid-120x40", "wide glyphs (120x40)", 120, 40, kCjk, false},
        {"table-ascii-110x30", "ascii borders (110x30)", 110, 30, kWrapping, true},
        {"table-keyvalue-40x20", "key-value fallback (40x20)", 40, 20, kNineColumns, false},
    };
}

// ---------------------------------------------------------------------------
// 11. The reference sizes the acceptance names: 103x82, 110x30, 120x40
// ---------------------------------------------------------------------------

/// The scene bodies at their reference sizes, drawn without the caption the golden adds, and
/// checked cell by cell: no header artifact (a rule carries no text), no blanking (the table
/// paints inside its rect) and no overrun (nothing outside it).
void checkReferenceSizes() {
    for (const Scene& scene : scenes()) {
        if (scene.ascii || std::strcmp(scene.name, "table-keyvalue-40x20") == 0)
            continue; // the ASCII variant is checkAsciiBorders' job, the fallback its own
        ::setenv("LLMFUN_ASCII", "0", 1);
        ui::applyTheme(ui::ThemeId::Graphite, ui::ColorDepth::TrueColor);
        const ui::Table table = parseOrFail(scene.body, scene.name);
        const int marginLeft = 1;
        const int marginTop = 3;
        const int avail = scene.w - 2 * marginLeft;
        const Grid grid =
            drawTable(table, scene.w, scene.h,
                      cellRect(static_cast<float>(marginLeft), static_cast<float>(marginTop),
                               static_cast<float>(avail), static_cast<float>(scene.h - marginTop)));
        const ui::TableWidths widths = ui::allocateTableWidths(table, avail);
        check(widths.fits, std::string(scene.name) + ": a grid layout fits the reference size");
        if (!widths.fits)
            continue;
        check(widths.widthCells <= avail, std::string(scene.name) + ": the table fits its room");
        std::vector<int> left;
        left.push_back(0);
        for (const int cells : widths.colCells)
            left.push_back(left.back() + 1 + ui::kCellPadding + cells + ui::kCellPadding);
        const ui::TableSize size = ui::measureTable(table, avail);
        checkTableGeometry(grid,
                           cellRect(static_cast<float>(marginLeft), static_cast<float>(marginTop),
                                    static_cast<float>(avail),
                                    static_cast<float>(scene.h - marginTop)),
                           left, size.h, scene.name);
    }
}

void checkGoldens(bool update) {
    for (const Scene& scene : scenes()) {
        ::setenv("LLMFUN_ASCII", scene.ascii ? "1" : "0", 1);
        ui::applyTheme(ui::ThemeId::Graphite, ui::ColorDepth::TrueColor);
        const ui::Table table = parseOrFail(scene.body, scene.name);
        const Grid grid = drawScene(scene.caption, table, scene.w, scene.h);
        const std::string text = goldenText(scene.name, scene.w, scene.h, grid);
        const std::string path = std::string(TEST_TABLE_GOLDEN_DIR) + "/" + scene.name + ".txt";
        if (update) {
            std::ofstream out(path);
            out << text;
            out.close();
            check(out.good(), "golden written: " + path);
            continue;
        }
        std::ifstream in(path);
        check(in.good(), "golden is readable: " + path + " (run with --update to create it)");
        if (!in.good())
            continue;
        const std::string want((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        check(want == text, std::string("the ") + scene.name + " golden matches");
        if (want != text && g_verbose) {
            std::size_t at = 0;
            while (at < want.size() && at < text.size() && want[at] == text[at])
                ++at;
            int line = 1;
            for (std::size_t k = 0; k < at; ++k)
                line += want[k] == '\n' ? 1 : 0;
            std::fprintf(stderr, "  first difference on line %d\n", line);
        }
    }
    ::setenv("LLMFUN_ASCII", "0", 1);
    ui::applyTheme(ui::ThemeId::Graphite, ui::ColorDepth::TrueColor);
}

// ---------------------------------------------------------------------------
// 12. The bridge (design section 6.8, task 37)
// ---------------------------------------------------------------------------
//
// The store adapter calls `bridgeTables()` on a body on its way to the renderer, and
// `ui/markdown.cpp` draws what it wrote. Everything that can go wrong with a text rewrite is
// pinned here, without a frame: the body's other bytes, code fences, both fence languages, the
// round trip back through the parser, and the two properties the caller relies on (a body with
// no table costs no copy, and a second pass changes nothing).

std::string bridged(const std::string& body, bool showSource = false) {
    std::string out;
    if (!ui::bridgeTables(body, showSource, out))
        return std::string("<unchanged>");
    return out;
}

void checkBridge() {
    // The fence's spelling, and that everything around it is byte-identical.
    const std::string body = "Intro line.\n"
                             "\n"
                             "| Tool | Calls |\n"
                             "|------|------:|\n"
                             "| read_file | 3 |\n"
                             "\n"
                             "After.\n";
    const std::string want = "Intro line.\n"
                             "\n"
                             "```llmfun-table\n"
                             "| Tool | Calls |\n"
                             "|------|------:|\n"
                             "| read_file | 3 |\n"
                             "```\n"
                             "\n"
                             "After.\n";
    check(bridged(body) == want, "one table bridges into a llmfun-table fence, byte for byte");

    // A body with no table is not rewritten at all - and the caller is told, so it can skip
    // the copy it would otherwise make (the whole point of the bool).
    std::string untouched = "sentinel";
    check(!ui::bridgeTables("just prose\nwith a | pipe\n", false, untouched) &&
              untouched == "sentinel",
          "a body with no table answers false and leaves the caller's string untouched");
    check(!ui::bridgeTables("", false, untouched), "an empty body answers false");

    // Two tables, prose between them: both are fenced, in document order, and the text
    // between them survives.
    const std::string two = "| a |\n|---|\n| 1 |\n\nmid\n\n| b |\n|---|\n| 2 |\n";
    const std::string twoWant = "```llmfun-table\n| a |\n|---|\n| 1 |\n```\n"
                                "\nmid\n\n"
                                "```llmfun-table\n| b |\n|---|\n| 2 |\n```\n";
    check(bridged(two) == twoWant, "two tables are both fenced, with the text between them kept");

    // A table the model wrote *as example code* is not a table: fenced code is skipped by the
    // detection, so the bridge leaves it alone (the markdown the renderer sees is unchanged).
    const std::string fenced = "```\n| a | b |\n|---|---|\n| 1 | 2 |\n```\n";
    check(bridged(fenced) == "<unchanged>", "a table inside a fenced code block is not rewritten");
    const std::string tilde = "~~~\n| a | b |\n|---|---|\n~~~\n";
    check(bridged(tilde) == "<unchanged>", "a tilde fence is skipped as well");
    // A fence inside a list item is still a fence when it sits at the item's content column,
    // so its table is example code too.
    check(bridged("10. Results:\n\n    ```\n    | a | b |\n    |---|---|\n    ```\n") ==
              "<unchanged>",
          "a fenced table inside a four-column item is skipped");

    // The `s` toggle: the same body, a source fence instead. The content is identical, so the
    // renderer never has to be told which one it is drawing - the language says it.
    const std::string sourceWant = "Intro line.\n"
                                   "\n"
                                   "```llmfun-table-source\n"
                                   "| Tool | Calls |\n"
                                   "|------|------:|\n"
                                   "| read_file | 3 |\n"
                                   "```\n"
                                   "\n"
                                   "After.\n";
    check(bridged(body, /*showSource=*/true) == sourceWant,
          "showSource writes the source language, with the same bytes inside");

    // The language names are the contract between this module and the renderer registration.
    check(std::string(ui::kTableFenceLanguage) == "llmfun-table" &&
              std::string(ui::kTableSourceFenceLanguage) == "llmfun-table-source",
          "the fence languages are the documented names");

    // CRLF: the fence is written with LF (a carriage return is a line ending, never content),
    // the table's own CRs stop at the fence, and the neighbours keep theirs. The span ends at
    // the newline that follows the table's last line, so the fence's own closing line ends that
    // line - no blank line is inserted between the fence and what follows it.
    const std::string crlf = "intro\r\n| a | b |\r\n|---|---|\r\n| 1 | 2 |\r\nafter\r\n";
    const std::string crlfBridged = bridged(crlf);
    const std::string crlfWant =
        "intro\r\n```llmfun-table\n| a | b |\n|---|---|\n| 1 | 2 |\n```\nafter\r\n";
    check(crlfBridged == crlfWant,
          "a CRLF body bridges into an LF fence between its untouched neighbours");
    check(crlfBridged.find('\r') == 5 && crlfBridged.find('\r', 6) == crlfBridged.size() - 2,
          "the only carriage returns left are the two that were already there");

    // What the bridge writes is markdown: the fence's content parses back into the same table,
    // which is what makes the grid a rendering of the source rather than a second document.
    ui::Table before;
    check(ui::parseGfmTable("| Tool | Calls |\n|------|------:|\n| read_file | 3 |", before),
          "the fixture parses");
    const std::string fenceBody = bridged(body);
    const std::size_t fenceAt = fenceBody.find("```llmfun-table\n");
    const std::size_t closeAt = fenceBody.find("\n```", fenceAt);
    check(fenceAt != std::string::npos && closeAt != std::string::npos,
          "the fence has an opening and a closing line");
    if (fenceAt != std::string::npos && closeAt != std::string::npos) {
        const std::size_t openCells = std::string("```llmfun-table\n").size();
        const std::size_t innerAt = fenceAt + openCells;
        const std::string inner = fenceBody.substr(innerAt, closeAt - innerAt);
        ui::Table after;
        check(ui::parseGfmTable(inner, after), "the fenced content parses as a table");
        check(after.columns() == before.columns() && after.rows.size() == before.rows.size(),
              "the fenced content is the same table");
        if (after.columns() == before.columns() && after.rows.size() == before.rows.size()) {
            bool same = true;
            for (std::size_t r = 0; r < before.rows.size(); ++r)
                for (int c = 0; c < before.columns(); ++c)
                    same = same &&
                           after.rows[r][static_cast<std::size_t>(c)].text ==
                               before.rows[r][static_cast<std::size_t>(c)].text &&
                           after.rows[r][static_cast<std::size_t>(c)].align ==
                               before.rows[r][static_cast<std::size_t>(c)].align;
            check(same, "every cell's text and alignment survived the round trip");
        }
    }

    // Idempotence: the body renderer and the block copy both call the transform, and a second
    // pass must not fence a fence. The detection skips fenced code, so it answers false.
    std::string twice = "sentinel";
    check(!ui::bridgeTables(bridged(body), false, twice) && twice == "sentinel",
          "a bridged body bridges to nothing (the transform is idempotent)");

    // A body that is nothing but a table: no leading or trailing text to lean on.
    check(bridged("| a |\n|---|\n") == "```llmfun-table\n| a |\n|---|\n```\n",
          "a body that is nothing but a table bridges with no neighbours");

    // The live shape of a streamed body: a table that ends the body with no newline after its
    // last line. The closing fence is still written, and it is what ends that line (there is no
    // trailing "\n" to lean on, and md4c closes an unterminated fence at end of text anyway -
    // but the grid must be drawn from a *terminated* fence, not from a lucky parse).
    check(bridged("| a |\n|---|") == "```llmfun-table\n| a |\n|---|\n```",
          "a table that ends the body without a newline still closes its fence");

    // GFM's indented code block is counted from the container, not from the body. At the top
    // level four columns is code, so a sample stays a sample.
    check(bridged("    | a | b |\n    |---|---|\n    | 1 | 2 |\n") == "<unchanged>",
          "a table four columns in at the top level stays an indented code block");
    check(bridged("\t| a | b |\n\t|---|---|\n") == "<unchanged>",
          "a tab-indented table is four columns too");
    // ...while one to three spaces is a legal table.
    check(bridged("  | a |\n  |---|\n") == "```llmfun-table\n  | a |\n  |---|\n```\n",
          "a table indented two columns is still a table");
    // ...and four columns *past a list item's content column* is content, not code. The fence
    // is written at that column, so the item (and anything after the table in it) survives.
    check(bridged("- Results:\n\n    | a | b |\n    |---|---|\n") ==
              "- Results:\n\n  ```llmfun-table\n    | a | b |\n    |---|---|\n  ```\n",
          "a four-column table under a bullet is bridged at the item's content column");
    check(bridged("- Results:\n\n  | a | b |\n  |---|---|\n") ==
              "- Results:\n\n  ```llmfun-table\n  | a | b |\n  |---|---|\n  ```\n",
          "a two-column table under a bullet keeps the fence at the item's column too");
    check(bridged("1. Results:\n\n    | a | b |\n    |---|---|\n") ==
              "1. Results:\n\n   ```llmfun-table\n    | a | b |\n    |---|---|\n   ```\n",
          "a four-column table under a numbered item is bridged");
    check(bridged("10. Results:\n\n    | a | b |\n    |---|---|\n") ==
              "10. Results:\n\n    ```llmfun-table\n    | a | b |\n    |---|---|\n    ```\n",
          "a fence at a four-column content column is still a fence");
    check(bridged("1. outer\n\n    - inner\n\n        | a | b |\n        |---|---|\n") ==
              "1. outer\n\n    - inner\n\n      ```llmfun-table\n        | a | b |\n        "
              "|---|---|\n      ```\n",
          "a nested list's content column moves the table's container again");
    // The delimiter row is a paragraph continuation for md4c, so it may be indented further
    // than the header; the body rows are not (four columns past the container is code).
    check(bridged("| a | b |\n        |---|---|\n| 1 | 2 |\n") ==
              "```llmfun-table\n| a | b |\n        |---|---|\n| 1 | 2 |\n```\n",
          "a delimiter row may be indented further than its header");
    check(bridged("1. Results:\n\n    | a | b |\n        |---|---|\n    | 1 | 2 |\n") ==
              "1. Results:\n\n   ```llmfun-table\n    | a | b |\n        |---|---|\n    | 1 | 2 "
              "|\n   ```\n",
          "a nested delimiter may be indented past the container too");
    check(bridged("1. Results:\n\n    | a | b |\n|---|---|\n") == "<unchanged>",
          "a delimiter behind the item does not start a table");
    check(
        bridged("1. Results:\n\n    | a | b |\n    |---|---|\n| 1 | 2 |\n") ==
            "1. Results:\n\n   ```llmfun-table\n    | a | b |\n    |---|---|\n   ```\n| 1 | 2 |\n",
        "a body row behind the item ends the table");
    check(bridged("1. Results:\n\n    Something.\n\n    | a | b |\n    |---|---|\n") ==
              "1. Results:\n\n    Something.\n\n   ```llmfun-table\n    | a | b |\n    |---|---|\n "
              "  ```\n",
          "a paragraph inside the item does not hide the table after it");
    check(bridged("-  Results:\n\n      | a | b |\n      |---|---|\n") ==
              "-  Results:\n\n   ```llmfun-table\n      | a | b |\n      |---|---|\n   ```\n",
          "two spaces after the marker move the content column to three");
    check(bridged("-  Results:\n\n       | a | b |\n       |---|---|\n") == "<unchanged>",
          "one column further is an indented code block again");
    check(bridged("- Results:\n\nparagraph\n\n    | a | b |\n    |---|---|\n") == "<unchanged>",
          "a paragraph that ends the list makes the four-column table code again");
    check(bridged("1. Results:\n\n    | a | b |\n    |---|---|\n\n    After.\n") ==
              "1. Results:\n\n   ```llmfun-table\n    | a | b |\n    |---|---|\n   ```\n\n    "
              "After.\n",
          "the text after the table stays in the item");
    // The nested output is markdown too: its fence is skipped by a second pass.
    std::string nestedTwice = "sentinel";
    check(!ui::bridgeTables(bridged("1. Results:\n\n    | a | b |\n    |---|---|\n"), false,
                            nestedTwice) &&
              nestedTwice == "sentinel",
          "a bridged nested table does not bridge again");
    // A body whose only table is code answers false, so the caller copies nothing.
    std::string untouchedIndent = "sentinel";
    check(!ui::bridgeTables("intro\n\n    | a |\n    |---|\n", false, untouchedIndent) &&
              untouchedIndent == "sentinel",
          "an indented-code-only table leaves the caller's string untouched");

    // The contract allows `body` and `out` to be the same object (the replacement is built
    // before either is written), which is what a caller that owns the body would want.
    std::string aliased = "intro\n\n| a |\n|---|\n";
    check(ui::bridgeTables(aliased, false, aliased) &&
              aliased == "intro\n\n```llmfun-table\n| a |\n|---|\n```\n",
          "a caller may pass its own body as the output");
}

} // namespace

int main(int argc, char** argv) {
    bool update = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0)
            g_verbose = true;
        else if (std::strcmp(argv[i], "--update") == 0)
            update = true;
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
            usage(argv[0], "print every check with -v, rewrite the goldens with --update");
        else
            usage(argv[0], "unknown argument");
    }

    // One glyph set and one colour depth for the run, whatever terminal it starts on.
    ::setenv("LLMFUN_ASCII", "0", 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImTui_ImplText_Init();
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ui::applyTheme(ui::ThemeId::Graphite, ui::ColorDepth::TrueColor);

    // One warm-up frame: the text backend builds its atlas on the first frame.
    frame(40, 15, [](const ui::Metrics&) {});

    checkSource();
    checkParse();
    checkPlainText();
    checkDetect();
    checkWidths();
    checkMeasure();
    checkRaggedTable();
    checkRender();
    checkAsciiBorders();
    checkFallback();
    checkReferenceSizes();
    checkGoldens(update);
    checkBridge();

    if (g_failures == 0) {
        std::printf("test_table: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "test_table: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
