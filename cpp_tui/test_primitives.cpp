// test_primitives.cpp
//
// Unit test for the primitive library (task 16, design §4.15, plan §8.6): fourteen
// widgets, each with a `measure()` that answers the size its renderer draws in, and
// the real button that replaces the blank-button-plus-overlaid-text hack.
//
// What is pinned here, case by case:
//
//   0. the source      no colour literal in `ui/primitives.cpp` - the module must
//                      resolve every ink and fill through `ui::color()`, so the
//                      shipped file is read and searched for the two shapes a colour
//                      literal takes (`ImVec4(` and `IM_COL32(`). The grep is on the
//                      file that ships, not on the test's idea of it.
//
//   1. the arithmetic  every `measure*()` answers whole, finite, non-negative units;
//                      the chrome minimums hold (a panel cannot be smaller than two
//                      hairlines, two `pad2` and one unit of content); primitives
//                      that share an anatomy agree (a pill is a chip plus a glyph
//                      slot, a row is a chip, a disclosure is the glyph plus air);
//                      and the same call in the two profiles answers cell counts on
//                      the grid and pixels at 100 % and 150 %.
//
//   2. mixed units     a rect whose `cells` flag disagrees with the metrics paints
//                      nothing at all - in the grid *and* in the vertex list. A
//                      28-cell width read as 28 px is a legal-looking number that
//                      lays out wrong; the primitive refuses to draw it instead of
//                      drawing a plausible lie.
//
//   3. render == size  the core claim, checked the only way it can be: each
//                      primitive is rendered alone at exactly its measured rect and
//                      nothing - not one cell, not one vertex - lands outside it,
//                      while something lands inside. For button, chip, meter and
//                      disclosure (the four the task names) the fill or the glyph run
//                      is then measured cell by cell, so "the render uses the measure"
//                      is an equality rather than a bound. The pixel profile is
//                      checked the same way on the vertex bounding box, which is the
//                      pixel analogue of a cell.
//
//   4. the real button the hit box *is* the measured rect: a real
//                      `ImGui::InvisibleButton` submitted by `button()` answers a
//                      press anywhere inside the rect (the corners, the far edge, the
//                      centre) and nowhere a whole unit outside; a disabled button
//                      answers nothing; a rect no button was drawn at answers nothing;
//                      two buttons in one frame keep their own answers *and* their own
//                      ImGui item ids (label + position is the identity, so two rows
//                      with the same label are two buttons); the label is drawn by the
//                      primitive; the item advances the cursor but adds nothing to a
//                      scrolled container's content extent; and a hovered button keeps
//                      the resting one's geometry (only its fill colour moves).
//
//   5. the spinner     four frames at 150 ms with the design's `◐ ◓ ◑ ◒`, the ASCII
//                      column `| / - \`, a negative / NaN / infinite phase meaning
//                      "no motion" (frame 0, the glyph table's own `◐`), and a
//                      rendered mark that is one cell wide.
//
//   6. the meter       a segment lights only when it is full (0.99 -> 9 of 10), the
//                      fraction is clamped and a poisoned one answers empty, and the
//                      rendered bar is exactly `segments` cells wide with the filled
//                      prefix the numbers say.
//
//   7. contrast        every ink/fill pair the primitives can produce clears the
//                      design's bar - 4.5:1 where a glyph sits on a fill, 3:1 for a
//                      boundary - with the same formula `test_tokens` holds the theme
//                      to, including both ways a grid can render a translucent fill
//                      (composited, and imtui's alpha-multiplied form).
//
//   8. the goldens     one 100x30 text screen per primitive in `test/golden/`, each
//                      captioned with the size it measured. `--update` rewrites
//                      them; without it a diff fails the run. The caption is what
//                      makes a golden pin the *numbers* as well as the pixels, so a
//                      formula change and a rendering change are both visible in the
//                      same file.
//
// Exit codes: 0 = every check passed; 1 = at least one failed; 2 = usage error.
// Run from the build dir:  ./test_primitives [-v] [--update]
//
// The text grid is driven through the real ImTui text backend (no ncurses, no PTY,
// no display): `ImTui_ImplText_RenderDrawData` writes the cell grid the assertions
// read. `LLMFUN_ASCII=0` is pinned at start-up so the glyph column is the same on
// every machine - a golden captured on a CJK terminal must not differ from one
// captured on an ASCII one.

#include "imtui/imtui.h"

// The content-extent check reads `DC.CursorMaxPos` directly: the property it pins ("an
// absolute item claims no flow space") has no public accessor, and a test may look at the
// mechanism when the symptom is a scroll range that only moves one frame later.
#include "imgui/imgui_internal.h"

#include "ui/icons.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/text.h"
#include "ui/tokens.h"

#include <algorithm>
#include <cmath>
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

int g_failures = 0;
int g_checks = 0;
bool g_verbose = false;

/// The screen the grid half renders into: the design's reference size, and the size
/// the goldens are captured at.
constexpr int kScreenW = 100;
constexpr int kScreenH = 30;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) {
        if (g_verbose)
            std::fprintf(stderr, "ok   %s\n", what.c_str());
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

[[noreturn]] void usage(const char* argv0, const char* problem) {
    std::fprintf(stderr, "usage: %s [-v] [--update]   (%s)\n", argv0, problem);
    std::exit(2);
}

// UTF-8 helpers, used by the grid reader below.
std::string utf8Of(uint32_t cp);
uint32_t codepointOf(const std::string& utf8);

/// `4`, not `4.0`: every measured length is a whole number of cells or pixels, so a
/// fractional one is a bug and must look like one in a failure message.
std::string num(float v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.3f", double(v));
    std::string text = buffer;
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text;
}

std::string sizeText(ui::Size s) { return num(s.w) + "x" + num(s.h); }
std::string describe(ui::Size s) { return "{" + num(s.w) + ", " + num(s.h) + "}"; }

ui::Rect rectCells(float x, float y, ui::Size s) {
    ui::Rect r;
    r.x = x;
    r.y = y;
    r.w = s.w;
    r.h = s.h;
    r.cells = true;
    return r;
}

ui::Rect rectPixels(float x, float y, ui::Size s) {
    ui::Rect r = rectCells(x, y, s);
    r.cells = false;
    return r;
}

// ---------------------------------------------------------------------------
// The cell grid the text backend writes
// ---------------------------------------------------------------------------

ImTui::TScreen g_screen;

struct Grid {
    int nx = 0;
    int ny = 0;
    std::vector<uint32_t> ch;
    std::vector<uint8_t> chwidth;
    std::vector<uint8_t> bg;

    uint32_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return ch[static_cast<std::size_t>(y) * nx + x];
    }
    uint8_t bgAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return bg[static_cast<std::size_t>(y) * nx + x];
    }
    /// True when nothing at all touched the cell: a painted fill leaves `' '` and a
    /// painted glyph leaves its character, so only an untouched cell reads 0.
    bool blank(int x, int y) const { return at(x, y) == 0; }
    int cellWidth(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return chwidth[static_cast<std::size_t>(y) * nx + x];
    }
    std::string rowText(int y) const {
        std::string text;
        for (int x = 0; x < nx; ++x) {
            const uint32_t cp = at(x, y);
            text += cp == 0 ? std::string(" ") : utf8Of(cp);
        }
        return text;
    }
    /// The row's text starting at `x`, `cells` wide - the way a label is read back
    /// out of the grid instead of being compared with the string the drawing code
    /// used.
    std::string span(int x, int y, int cells) const {
        std::string text;
        for (int i = 0; i < cells; ++i) {
            const uint32_t cp = at(x + i, y);
            text += cp == 0 ? std::string(" ") : utf8Of(cp);
        }
        return text;
    }
};

Grid capture() {
    Grid grid;
    grid.nx = g_screen.nx;
    grid.ny = g_screen.ny;
    const std::size_t cells = static_cast<std::size_t>(grid.nx) * grid.ny;
    grid.ch.resize(cells);
    grid.chwidth.resize(cells);
    grid.bg.resize(cells);
    for (std::size_t i = 0; i < cells; ++i) {
        grid.ch[i] = g_screen.data[i].ch;
        grid.chwidth[i] = g_screen.data[i].chwidth;
        grid.bg[i] = g_screen.data[i].bg;
    }
    return grid;
}

void dumpGrid(const Grid& grid, const char* why) {
    std::fprintf(stderr, "grid %dx%d (%s):\n", grid.nx, grid.ny, why);
    for (int y = 0; y < grid.ny; ++y) {
        std::string ascii;
        for (int x = 0; x < grid.nx; ++x) {
            const uint32_t cp = grid.at(x, y);
            ascii.push_back(cp >= 32 && cp < 127 ? char(cp) : (cp == 0 ? '.' : '#'));
        }
        std::fprintf(stderr, "%2d |%s|\n", y, ascii.c_str());
    }
}

int paintedInside(const Grid& grid, const ui::Rect& r) {
    int count = 0;
    for (int y = static_cast<int>(r.y); y < static_cast<int>(r.bottom()); ++y)
        for (int x = static_cast<int>(r.x); x < static_cast<int>(r.right()); ++x)
            count += grid.blank(x, y) ? 0 : 1;
    return count;
}

int paintedOutside(const Grid& grid, const ui::Rect& r) {
    int count = 0;
    for (int y = 0; y < grid.ny; ++y)
        for (int x = 0; x < grid.nx; ++x) {
            const bool inside = x >= static_cast<int>(r.x) && x < static_cast<int>(r.right()) &&
                                y >= static_cast<int>(r.y) && y < static_cast<int>(r.bottom());
            if (!inside && !grid.blank(x, y))
                ++count;
        }
    return count;
}

/// The horizontal extent of the painted cells in one row: the leftmost column and
/// the rightmost one. -1 when the row is untouched.
struct Span {
    int first = -1;
    int last = -1;
    int count = 0;
};

Span paintedSpan(const Grid& grid, int y) {
    Span span;
    for (int x = 0; x < grid.nx; ++x) {
        if (grid.blank(x, y))
            continue;
        if (span.first < 0)
            span.first = x;
        span.last = x;
        ++span.count;
    }
    return span;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

ui::Metrics gridMetrics() { return ui::metricsFor(true, 1.0f, 1.0f); }
ui::Metrics guiMetrics() { return ui::metricsFor(false, 1.0f, 1.0f); }

/// One frame of the text grid at the reference size.
///
/// The body runs inside a decoration-free root window filling the display, i.e. the
/// shape the shell has: ImGui always has a current window (it opens an implicit
/// fallback one at the end of `NewFrame`), so a primitive draws on a window's list,
/// and a harness that wants whole-display coordinates has to supply the window.
/// `inject` runs *before* `NewFrame`, because that is when ImGui consumes its input
/// queue (an event added after `NewFrame` is only seen by the next frame).
Grid frame(const std::function<void(ImDrawList*, const ui::Metrics&)>& body,
           const std::function<void(ImGuiIO&)>& inject = nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kScreenW), static_cast<float>(kScreenH));
    io.DeltaTime = 1.0f / 60.0f;
    if (inject)
        inject(io);
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    const ui::Metrics m = gridMetrics();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kScreenW), static_cast<float>(kScreenH)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("primitives-frame", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    body(ImGui::GetWindowDrawList(), m);
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    return capture();
}

/// A frame with no rendering: the body draws inside a generously sized window and the
/// caller inspects the vertices it produced. The pixel profile's geometry is checked
/// this way - a vertex is a pixel's worth of drawing, so no display is involved - and the
/// window is large enough that no rect of ours is near an edge (it is the vertex
/// positions that are inspected, not the clip rect). `inject` runs before `NewFrame`, so
/// an input event (a hover) is seen by the frame that is measured.
struct VertexBox {
    bool any = false;
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    int count = 0;
    /// Every vertex position, in order: two draws that differ only in colour have the same
    /// list, and a draw that changed shape has not.
    std::vector<float> xy;

    bool hasVertexAt(float x, float y) const {
        for (std::size_t i = 0; i + 1 < xy.size(); i += 2) {
            if (std::fabs(xy[i] - x) < 0.01f && std::fabs(xy[i + 1] - y) < 0.01f)
                return true;
        }
        return false;
    }
};

VertexBox vertexFrame(const std::function<void(ImDrawList*, const ui::Metrics&)>& body,
                      const ui::Metrics& m, const std::function<void(ImGuiIO&)>& inject = nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    // The whole virtual space is the display, so a window can be large and its clip
    // rect cannot cut a glyph out of the vertex list (the text path culls glyphs
    // against the window's clip rect while building vertices).
    io.DisplaySize = ImVec2(4000.0f, 4000.0f);
    io.DeltaTime = 1.0f / 60.0f;
    if (inject)
        inject(io);
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(4000.0f, 4000.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("primitives-vertices", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int before = dl->VtxBuffer.Size;
    body(dl, m);
    VertexBox box;
    box.count = dl->VtxBuffer.Size - before;
    box.xy.reserve(static_cast<std::size_t>(box.count) * 2);
    for (int i = before; i < dl->VtxBuffer.Size; ++i) {
        const ImVec2& pos = dl->VtxBuffer[i].pos;
        box.xy.push_back(pos.x);
        box.xy.push_back(pos.y);
        if (!box.any) {
            box.any = true;
            box.x0 = box.x1 = pos.x;
            box.y0 = box.y1 = pos.y;
        } else {
            box.x0 = std::min(box.x0, pos.x);
            box.y0 = std::min(box.y0, pos.y);
            box.x1 = std::max(box.x1, pos.x);
            box.y1 = std::max(box.y1, pos.y);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::Render();
    return box;
}

// ---------------------------------------------------------------------------
// 0. The source ships no colour literal
// ---------------------------------------------------------------------------

int countOccurrences(const std::string& haystack, const std::string& needle) {
    int count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size()))
        ++count;
    return count;
}

void checkSource() {
    const char* path = TEST_PRIMITIVES_SOURCE;
    std::ifstream in(path);
    check(in.good(), std::string("ui/primitives.cpp is readable (") + path + ")");
    if (!in.good())
        return;
    std::string source;
    std::string line;
    while (std::getline(in, line)) {
        source += line;
        source += '\n';
    }
    // The shapes a colour literal takes. A resolved `ImU32` local is not a literal - it
    // holds what `ui::color()` answered - so only building a value is banned: the
    // constructor, a brace-init, a packed constant, or a raw hex handed to GetColorU32.
    check(countOccurrences(source, "ImVec4(") == 0 && countOccurrences(source, "ImVec4{") == 0,
          "ui/primitives.cpp builds no ImVec4 value: colours come from ui::color()");
    check(countOccurrences(source, "IM_COL32(") == 0,
          "ui/primitives.cpp writes no packed colour literal");
    check(countOccurrences(source, "GetColorU32(0x") == 0,
          "ui/primitives.cpp hands no raw hex colour to GetColorU32");
    check(countOccurrences(source, "tuiIsTextGrid") == 0,
          "ui/primitives.cpp takes the profile from Metrics, never from tuiIsTextGrid()");
    check(countOccurrences(source, "DisplaySize") == 0,
          "ui/primitives.cpp lays out inside the rect it is given, not against the display");
}

// ---------------------------------------------------------------------------
// 1b. A GUI measure before the first frame
// ---------------------------------------------------------------------------

/// A GUI measure before the first frame fails closed. `GetCurrentContext()` is non-null
/// the moment a context exists, but `CalcTextSize` needs the current *font*, which only
/// `NewFrame` installs - the state that segfaults inside `ImFont::GetFontBaked` if it is
/// not checked. The text then contributes nothing and the chrome still measures, so a
/// caller sees a degenerate width rather than a crash or a plausible-looking number.
void checkMeasureBeforeFirstFrame() {
    const ui::Metrics gui = ui::metricsFor(false, 1.0f, 1.0f);
    const ui::Size chip = ui::measureChip(gui, "Plan");
    const ui::Size button = ui::measureButton(gui, {"Send", true, false, false});
    check(chip.w == 2.0f * gui.pad1 && chip.h == gui.row,
          "a GUI chip measured before the first frame is its chrome only (" + describe(chip) + ")");
    check(button.w == 2.0f * gui.pad2 && button.h == gui.row,
          "a GUI button measured before the first frame is its chrome only (" + describe(button) +
              ")");
}

// ---------------------------------------------------------------------------
// 1. The arithmetic
// ---------------------------------------------------------------------------

/// Every measure answers whole, finite, non-negative units - the properties the
/// layout engine relies on to place a primitive before it is drawn.
void checkMeasureShape(const ui::Metrics& m, const char* profile,
                       const std::vector<std::pair<std::string, ui::Size>>& sizes) {
    (void)m;
    for (const auto& entry : sizes) {
        const ui::Size s = entry.second;
        check(s.w == ui::roundCells(s.w) && s.h == ui::roundCells(s.h),
              std::string(profile) + ": " + entry.first + " measures whole units " + describe(s));
        check(s.w >= 0.0f && s.h >= 0.0f, std::string(profile) + ": " + entry.first +
                                              " measures a non-negative size " + describe(s));
        check(std::isfinite(s.w) && std::isfinite(s.h),
              std::string(profile) + ": " + entry.first + " measures a finite size " + describe(s));
    }
}

void checkArithmeticText() {
    const ui::Metrics m = gridMetrics();
    check(m.textGrid && m.gu == 1.0f && m.row == 1.0f && m.pad1 == 1.0f && m.pad2 == 2.0f &&
              m.pad3 == 3.0f && m.border == 1.0f && m.radius == 0.0f,
          "the text grid's metrics are the cell counts the primitive arithmetic assumes");

    const ui::ButtonSpec send{"Send", true, false, false};
    const std::vector<std::pair<std::string, ui::Size>> sizes = {
        {"panel", ui::measurePanel(m, {20.0f, 5.0f})},
        {"card", ui::measureCard(m, {20.0f, 5.0f})},
        {"rule", ui::measureRule(m, 30.0f)},
        {"button", ui::measureButton(m, send)},
        {"iconButton", ui::measureIconButton(m)},
        {"chip", ui::measureChip(m, "Plan")},
        {"pill", ui::measurePill(m, "3 skills")},
        {"row", ui::measureRow(m, "Title")},
        {"icon", ui::measureIconMark(m)},
        {"meter", ui::measureMeter(m, 10)},
        {"spinner", ui::measureSpinner(m)},
        {"disclosure", ui::measureDisclosure(m, "WORK")},
        {"skeleton", ui::measureSkeleton(m, {20.0f, 1.0f})},
        {"tooltip", ui::measureTooltip(m, "Ready")},
    };
    checkMeasureShape(m, "grid", sizes);

    // Panel: the wish is kept when it fits its chrome and grown when it does not.
    check(ui::measurePanel(m, {20.0f, 8.0f}).w == 20.0f &&
              ui::measurePanel(m, {20.0f, 8.0f}).h == 8.0f,
          "measurePanel keeps a wish that fits its chrome");
    const ui::Size minimum = ui::measurePanel(m, {0.0f, 0.0f});
    check(minimum.w == 7.0f && minimum.h == 7.0f,
          "measurePanel grows an empty wish to the chrome minimum " + describe(minimum));
    check(ui::measurePanel(m, {4.0f, 4.0f}).w == 7.0f &&
              ui::measurePanel(m, {4.0f, 4.0f}).h == 7.0f,
          "measurePanel grows a too-small wish to the minimum");
    check(ui::measurePanel(m, {-3.0f, -3.0f}).w == minimum.w,
          "measurePanel treats a negative wish as the minimum, not as a negative size");
    check(ui::measureCard(m, {7.0f, 2.0f}).w == ui::measurePanel(m, {7.0f, 2.0f}).w &&
              ui::measureCard(m, {7.0f, 2.0f}).h == ui::measurePanel(m, {7.0f, 2.0f}).h,
          "a card and a panel share one anatomy");

    // Rule: the thin axis is one hairline, whichever way it runs.
    check(ui::measureRule(m, 30.0f, true).w == 30.0f && ui::measureRule(m, 30.0f, true).h == 1.0f,
          "a rule is one hairline thick when it runs horizontally");
    check(ui::measureRule(m, 30.0f, false).w == 1.0f && ui::measureRule(m, 30.0f, false).h == 30.0f,
          "a vertical rule is one hairline wide");
    check(ui::measureRule(m, 0.0f, true).w == 0.0f,
          "a zero-length rule measures nothing wide (the layout asks, the rect decides)");

    // Button / chip / row / pill / disclosure: the anatomy the design states.
    check(ui::measureButton(m, send).w == 8.0f && ui::measureButton(m, send).h == 1.0f,
          "\"Send\" is 4 cells of label + 2 of padding on each side, one row tall");
    check(ui::measureButton(m, {"", true, false, false}).w == 4.0f,
          "an empty button is its padding - never a zero-width target");
    const ui::ButtonSpec longer{"a much longer label", true, false, false};
    check(ui::measureButton(m, longer).w > ui::measureButton(m, send).w,
          "a longer label measures a wider button");
    check(ui::measureChip(m, "Plan").w == 6.0f && ui::measureChip(m, "Plan").h == 1.0f,
          "a chip is its label plus `pad1` on each side, one row tall");
    check(ui::measureRow(m, "Title").h == 1.0f && ui::measureRow(m, "Title").w == 7.0f,
          "a row is its text plus `pad1` on each side, one row tall");
    check(ui::measurePill(m, "3 skills").w == ui::measureChip(m, "3 skills").w + m.gu + m.pad1,
          "a pill is a chip plus a glyph slot and one unit of air");
    check(ui::measureDisclosure(m, "WORK").w == m.gu + m.pad1 + 4.0f,
          "a disclosure is the glyph, one unit of air, then the label");
    check(ui::measureIconButton(m).w == 1.0f && ui::measureIconButton(m).h == 1.0f,
          "an icon button is square (one cell on the grid)");
    check(ui::measureIconMark(m).w == 1.0f, "an icon mark is one cell wide");
    check(ui::measureSpinner(m).w == 1.0f, "a spinner is one cell wide");

    // Meter: segments butt together on the grid (a cell cannot be subdivided).
    check(ui::measureMeter(m, 10).w == 10.0f && ui::measureMeter(m, 10).h == 1.0f,
          "a 10-segment meter is 10 cells wide and one cell tall");
    check(ui::measureMeter(m, 0).w == 1.0f,
          "a meter with no segments still measures one segment (a bar you can see)");
    check(ui::measureMeter(m, -5).w == 1.0f,
          "a negative segment count is clamped, not measured into the past");

    // Skeleton and tooltip.
    check(ui::measureSkeleton(m, {20.0f, 1.0f}).w == 20.0f &&
              ui::measureSkeleton(m, {20.0f, 1.0f}).h == 1.0f,
          "a skeleton keeps its width and is one row tall");
    check(ui::measureSkeleton(m, {0.0f, 0.0f}).w == 1.0f,
          "a zero-width skeleton is still one cell wide");
    check(ui::measureTooltip(m, "Ready").w == 9.0f && ui::measureTooltip(m, "Ready").h == 3.0f,
          "a tooltip is its text plus padding, one row plus padding tall");
}

void checkArithmeticGui() {
    const ui::Metrics m = guiMetrics();
    check(!m.textGrid && m.gu == 8.0f && m.row == 24.0f && m.pad1 == 8.0f && m.pad2 == 12.0f &&
              m.border == 1.0f && m.radius == 6.0f,
          "the pixel profile's metrics at 100 % are the lengths the primitive arithmetic "
          "assumes");

    const ui::ButtonSpec send{"Send", true, false, false};
    const std::vector<std::pair<std::string, ui::Size>> sizes = {
        {"panel", ui::measurePanel(m, {120.0f, 60.0f})},
        {"card", ui::measureCard(m, {120.0f, 60.0f})},
        {"rule", ui::measureRule(m, 240.0f)},
        {"button", ui::measureButton(m, send)},
        {"iconButton", ui::measureIconButton(m)},
        {"chip", ui::measureChip(m, "Plan")},
        {"pill", ui::measurePill(m, "3 skills")},
        {"row", ui::measureRow(m, "Title")},
        {"icon", ui::measureIconMark(m)},
        {"meter", ui::measureMeter(m, 10)},
        {"spinner", ui::measureSpinner(m)},
        {"disclosure", ui::measureDisclosure(m, "WORK")},
        {"skeleton", ui::measureSkeleton(m, {120.0f, 24.0f})},
        {"tooltip", ui::measureTooltip(m, "Ready")},
    };
    checkMeasureShape(m, "gui", sizes);

    // The vertical unit is the row, in pixels: 24 px at 100 %, 36 px at 150 %.
    check(ui::measureButton(m, send).h == 24.0f, "a GUI button is 24 px (one row) tall");
    check(ui::measureChip(m, "Plan").h == 24.0f && ui::measureRow(m, "Title").h == 24.0f,
          "a GUI chip and a GUI row are one row (24 px) tall");
    check(ui::measureIconButton(m).w == 24.0f && ui::measureIconButton(m).h == 24.0f,
          "a GUI icon button is the 24 x 24 px target the design asks for");
    check(ui::measurePanel(m, {0.0f, 0.0f}).w == 34.0f &&
              ui::measurePanel(m, {0.0f, 0.0f}).h == 50.0f,
          "a GUI panel's chrome minimum is 34 x 50 px (2 hairlines, 2 x 12 px, one unit)");
    check(ui::measureRule(m, 240.0f).h == 1.0f, "a GUI rule is a 1 px hairline");

    // A meter is 3/4 of a unit tall (6 px) and pays a 1 px gap between segments.
    const ui::Size bar = ui::measureMeter(m, 10);
    check(bar.w == 89.0f && bar.h == 6.0f,
          "a GUI 10-segment meter is 10 x 8 px + 9 hairlines wide and 6 px tall, got " +
              describe(bar));

    // The label is measured in pixels, not in cells: a chip is its label in pixels
    // plus its padding, and the two profiles necessarily disagree.
    const float labelPx = ImGui::CalcTextSize("Plan").x;
    check(ui::measureChip(m, "Plan").w == ui::roundCells(labelPx + 2.0f * m.pad1),
          "a GUI chip is its label in pixels plus `pad1` on each side");
    check(ui::measureChip(m, "Plan").w != ui::measureChip(gridMetrics(), "Plan").w,
          "the two profiles measure the same chip differently (cells vs pixels)");

    // 150 %: one scale applied to every primitive, and the hairline stays 1 px.
    const ui::Metrics scaled = ui::metricsFor(false, 1.5f, 1.0f);
    check(ui::measureButton(scaled, send).h == 36.0f, "a GUI button is 36 px tall at 150 %");
    check(ui::measureIconButton(scaled).w == 36.0f, "an icon button is 36 px wide at 150 %");
    check(ui::measureMeter(scaled, 10).h == 9.0f,
          "a GUI meter is 9 px tall at 150 % (3/4 of a 12 px unit)");
    check(ui::measureMeter(scaled, 10).w == 129.0f,
          "a GUI meter pays 12 px per segment and one 1 px hairline per gap at 150 %");
    check(ui::measurePanel(scaled, {0.0f, 0.0f}).h == 74.0f,
          "a GUI panel's chrome minimum is 74 px tall at 150 % (2 + 36 + 36)");
}

// ---------------------------------------------------------------------------
// 2. Mixed units paint nothing
// ---------------------------------------------------------------------------

void checkMixedUnits() {
    const ui::Metrics gridProfile = gridMetrics();
    const ui::Metrics guiProfile = guiMetrics();
    const ui::ButtonSpec spec{"Send", true, false, false};

    struct Call {
        std::string name;
        std::function<void(ImDrawList*, const ui::Metrics&, const ui::Rect&)> draw;
    };
    const std::vector<Call> calls = {
        {"panel", [](ImDrawList* dl, const ui::Metrics& m,
                     const ui::Rect& r) { ui::panel(dl, m, r, ui::ColorToken::surface, true); }},
        {"card", [](ImDrawList* dl, const ui::Metrics& m,
                    const ui::Rect& r) { ui::card(dl, m, r, ui::ColorToken::raised); }},
        {"rule", [](ImDrawList*, const ui::Metrics& m,
                    const ui::Rect& r) { ui::rule(m, r, ui::ColorToken::lineStrong); }},
        {"button",
         [spec](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) { ui::button(spec, m, r); }},
        {"iconButton", [](ImDrawList*, const ui::Metrics& m,
                          const ui::Rect& r) { ui::iconButton(m, r, ui::Icon::Ok, false); }},
        {"chip",
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) {
             ui::chip(m, r, "Plan", ui::ColorToken::textHi, ui::ColorToken::raised);
         }},
        {"pill",
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) {
             ui::pill(m, r, ui::Icon::Running, "running", ui::ColorToken::textHi,
                      ui::ColorToken::raised);
         }},
        {"row", [](ImDrawList*, const ui::Metrics& m,
                   const ui::Rect& r) { ui::row(m, r, "Title", true, true); }},
        {"icon",
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) {
             ui::iconMark(m, r, ui::Icon::Ok, ui::ColorToken::stateOk);
         }},
        {"meter", [](ImDrawList*, const ui::Metrics& m,
                     const ui::Rect& r) { ui::meter(m, r, 10, 0.5f, ui::ColorToken::accent); }},
        {"spinner",
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) { ui::spinner(m, r, 0.2); }},
        {"disclosure", [](ImDrawList*, const ui::Metrics& m,
                          const ui::Rect& r) { ui::disclosure(m, r, "WORK", true); }},
        {"skeleton",
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) { ui::skeleton(m, r, 0.5f); }},
    };

    const ui::Rect cellRect = rectCells(2.0f, 2.0f, {40.0f, 4.0f});
    const ui::Rect pixelRect = rectPixels(20.0f, 20.0f, {200.0f, 80.0f});
    for (const Call& call : calls) {
        // A cell rect handed to the pixel metrics.
        const VertexBox wrong = vertexFrame(
            [&](ImDrawList* dl, const ui::Metrics&) { call.draw(dl, guiProfile, cellRect); },
            guiProfile);
        check(wrong.count == 0,
              call.name + " draws no vertex for a cell rect in the pixel profile");

        // A pixel rect handed to the grid metrics.
        const Grid grid0 = frame(
            [&](ImDrawList* dl, const ui::Metrics&) { call.draw(dl, gridProfile, pixelRect); });
        int painted = 0;
        for (int y = 0; y < grid0.ny; ++y)
            for (int x = 0; x < grid0.nx; ++x)
                painted += grid0.blank(x, y) ? 0 : 1;
        check(painted == 0, call.name + " paints no cell for a pixel rect on the text grid");

        // And the same call with the flag it was measured for does draw, so the two
        // checks above are about the flag and not about a broken primitive.
        const VertexBox good = vertexFrame(
            [&](ImDrawList* dl, const ui::Metrics&) { call.draw(dl, guiProfile, pixelRect); },
            guiProfile);
        check(good.count > 0, call.name + " draws its vertices once the unit matches");
    }
}

// ---------------------------------------------------------------------------
// 3. The render uses the measured size
// ---------------------------------------------------------------------------

/// One primitive rendered alone at its measured rect: nothing outside, something
/// inside. `fillCovers` additionally asserts that every cell of the rect was
/// painted, which is what a filled primitive owes its measure.
void checkRenderFits(const std::string& name, ui::Size measured, const ui::Rect& r,
                     const std::function<void(ImDrawList*, const ui::Metrics&)>& draw,
                     bool fillCovers) {
    check(measured.w == r.w && measured.h == r.h,
          name + " is rendered at the size it measures (" + sizeText(measured) + ")");
    const Grid grid = frame(draw);
    const int outside = paintedOutside(grid, r);
    if (outside != 0)
        dumpGrid(grid, name.c_str());
    check(outside == 0, name + " paints nothing outside its measured rect (" +
                            std::to_string(outside) + " stray cell(s))");
    check(paintedInside(grid, r) > 0, name + " paints something inside its measured rect");
    if (fillCovers) {
        int covered = 0;
        for (int y = static_cast<int>(r.y); y < static_cast<int>(r.bottom()); ++y)
            for (int x = static_cast<int>(r.x); x < static_cast<int>(r.right()); ++x)
                covered += grid.bgAt(x, y) != 0 ? 1 : 0;
        const int cells = static_cast<int>(r.w * r.h);
        check(covered == cells, name + " fills exactly its measured rect (" +
                                    std::to_string(covered) + " of " + std::to_string(cells) +
                                    " cells)");
    }
}

void checkRenderGridCases() {
    const ui::Metrics m = gridMetrics();

    // The four primitives the task names, with the equality spelled out.
    {
        const ui::ButtonSpec spec{"Send", true, false, false};
        const ui::Size s = ui::measureButton(m, spec);
        const ui::Rect r = rectCells(4.0f, 3.0f, s);
        checkRenderFits(
            "button", s, r, [&](ImDrawList*, const ui::Metrics& mm) { ui::button(spec, mm, r); },
            true);
        const Grid grid =
            frame([&](ImDrawList*, const ui::Metrics& mm) { ui::button(spec, mm, r); });
        check(grid.span(6, 3, 4) == "Send", "the button's label sits inside its measured rect, "
                                            "one cell clear of the fill's edge");
    }
    {
        const ui::Size s = ui::measureChip(m, "Plan");
        const ui::Rect r = rectCells(4.0f, 3.0f, s);
        checkRenderFits(
            "chip", s, r,
            [&](ImDrawList*, const ui::Metrics& mm) {
                ui::chip(mm, r, "Plan", ui::ColorToken::textHi, ui::ColorToken::raised);
            },
            true);
        const Grid grid = frame([&](ImDrawList*, const ui::Metrics& mm) {
            ui::chip(mm, r, "Plan", ui::ColorToken::textHi, ui::ColorToken::raised);
        });
        check(grid.span(5, 3, 4) == "Plan",
              "the chip's label sits `pad1` inside its measured rect");
    }
    {
        const ui::Size s = ui::measureMeter(m, 10);
        const ui::Rect r = rectCells(4.0f, 3.0f, s);
        const Grid grid = frame([&](ImDrawList*, const ui::Metrics& mm) {
            ui::meter(mm, r, 10, 0.4f, ui::ColorToken::accent);
        });
        const Span span = paintedSpan(grid, 3);
        check(span.first == 4 && span.last == 13,
              "the meter's rendered run is exactly its measured 10 cells (span " +
                  std::to_string(span.first) + ".." + std::to_string(span.last) + ")");
        check(paintedOutside(grid, r) == 0, "the meter paints nothing outside its measured rect");
    }
    {
        const ui::Size s = ui::measureDisclosure(m, "WORK");
        const ui::Rect r = rectCells(4.0f, 3.0f, s);
        const Grid grid = frame(
            [&](ImDrawList*, const ui::Metrics& mm) { ui::disclosure(mm, r, "WORK", false); });
        check(grid.at(4, 3) ==
                  codepointOf(ui::icon(ui::Icon::DisclosureClosed, ui::GlyphSet::Glyphs)),
              "the disclosure's glyph is the table's collapsed mark");
        check(grid.span(4, 3, static_cast<int>(s.w)) ==
                  std::string(ui::icon(ui::Icon::DisclosureClosed, ui::GlyphSet::Glyphs)) + " WORK",
              "the disclosure is glyph, one cell of air, then the label");
        check(paintedOutside(grid, r) == 0,
              "the disclosure paints nothing outside its measured rect");
    }

    // The rest of the vocabulary: the same two bounds at the measured rect.
    {
        const ui::Size s = ui::measurePanel(m, {24.0f, 6.0f});
        const ui::Rect r = rectCells(2.0f, 2.0f, s);
        checkRenderFits(
            "panel", s, r,
            [&](ImDrawList* dl, const ui::Metrics& mm) {
                ui::panel(dl, mm, r, ui::ColorToken::surface, true);
            },
            true);
        const Grid grid = frame([&](ImDrawList* dl, const ui::Metrics& mm) {
            ui::panel(dl, mm, r, ui::ColorToken::surface, true);
        });
        check(grid.bgAt(2, 2) != grid.bgAt(3, 3),
              "the panel's hairline is drawn inside the rect, over the fill");
        checkRenderFits(
            "card", s, r,
            [&](ImDrawList* dl, const ui::Metrics& mm) {
                ui::card(dl, mm, r, ui::ColorToken::raised);
            },
            true);
    }
    {
        const ui::Size s = ui::measureRule(m, 30.0f);
        const ui::Rect r = rectCells(2.0f, 2.0f, s);
        checkRenderFits(
            "rule", s, r,
            [&](ImDrawList*, const ui::Metrics& mm) {
                ui::rule(mm, r, ui::ColorToken::lineStrong);
            },
            true);
    }
    {
        const ui::Size s = ui::measureIconButton(m);
        const ui::Rect r = rectCells(2.0f, 2.0f, s);
        checkRenderFits(
            "iconButton", s, r,
            [&](ImDrawList*, const ui::Metrics& mm) { ui::iconButton(mm, r, ui::Icon::Ok, false); },
            true);
    }
    {
        const ui::Size s = ui::measurePill(m, "3 skills");
        const ui::Rect r = rectCells(2.0f, 2.0f, s);
        checkRenderFits(
            "pill", s, r,
            [&](ImDrawList*, const ui::Metrics& mm) {
                ui::pill(mm, r, ui::Icon::Ok, "3 skills", ui::ColorToken::textHi,
                         ui::ColorToken::raised);
            },
            true);
    }
    {
        const ui::Size s = ui::measureRow(m, "Plan: modern UI");
        const ui::Rect r = rectCells(2.0f, 2.0f, s);
        checkRenderFits(
            "row", s, r,
            [&](ImDrawList*, const ui::Metrics& mm) {
                ui::row(mm, r, "Plan: modern UI", true, true);
            },
            true);
        const Grid plain = frame([&](ImDrawList*, const ui::Metrics& mm) {
            ui::row(mm, r, "Plan: modern UI", false, true);
        });
        check(plain.bgAt(2, 2) == 0, "an unselected row paints no fill");
        check(paintedOutside(plain, r) == 0, "an unselected row stays inside its rect");
    }
    {
        const ui::Size s = ui::measureIconMark(m);
        const ui::Rect r = rectCells(2.0f, 2.0f, s);
        checkRenderFits(
            "icon", s, r,
            [&](ImDrawList*, const ui::Metrics& mm) {
                ui::iconMark(mm, r, ui::Icon::Ok, ui::ColorToken::stateOk);
            },
            false);
    }
    {
        const ui::Size s = ui::measureSpinner(m);
        const ui::Rect r = rectCells(2.0f, 2.0f, s);
        checkRenderFits(
            "spinner", s, r, [&](ImDrawList*, const ui::Metrics& mm) { ui::spinner(mm, r, 0.0); },
            false);
    }
    {
        const ui::Size s = ui::measureSkeleton(m, {24.0f, 1.0f});
        const ui::Rect r = rectCells(2.0f, 2.0f, s);
        checkRenderFits(
            "skeleton", s, r,
            [&](ImDrawList*, const ui::Metrics& mm) { ui::skeleton(mm, r, 0.3f); }, true);
    }

    // A label longer than the rect it was measured for is cut to its rect instead of
    // running over the neighbour - the caller may shrink the rect after measuring.
    {
        const ui::ButtonSpec spec{"a label far wider than the rect", true, false, false};
        ui::Rect r = rectCells(2.0f, 2.0f, ui::measureButton(m, spec));
        r.w = 6.0f;
        const Grid grid =
            frame([&](ImDrawList*, const ui::Metrics& mm) { ui::button(spec, mm, r); });
        check(paintedOutside(grid, r) == 0,
              "a label too long for its rect is cut, not drawn over the neighbour");
        check(grid.at(static_cast<int>(r.right()), 2) == 0,
              "the cell just past a cut label stays empty");
    }
}

void checkRenderPixelCases() {
    const ui::Metrics gui = guiMetrics();
    struct PixelCase {
        std::string name;
        ui::Size size;
        std::function<void(ImDrawList*, const ui::Metrics&, const ui::Rect&)> draw;
        bool reachesEdges;
    };
    const std::vector<PixelCase> cases = {
        {"panel", ui::measurePanel(gui, {120.0f, 60.0f}),
         [](ImDrawList* dl, const ui::Metrics& m, const ui::Rect& r) {
             ui::panel(dl, m, r, ui::ColorToken::surface, true);
         },
         true},
        {"card", ui::measureCard(gui, {120.0f, 60.0f}),
         [](ImDrawList* dl, const ui::Metrics& m, const ui::Rect& r) {
             ui::card(dl, m, r, ui::ColorToken::raised);
         },
         true},
        {"rule", ui::measureRule(gui, 240.0f),
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) {
             ui::rule(m, r, ui::ColorToken::lineStrong);
         },
         true},
        {"button", ui::measureButton(gui, {"Send", true, false, false}),
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) {
             ui::button({"Send", true, true, true}, m, r);
         },
         true},
        {"iconButton", ui::measureIconButton(gui),
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) {
             ui::iconButton(m, r, ui::Icon::Ok, true);
         },
         true},
        {"chip", ui::measureChip(gui, "Plan"),
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) {
             ui::chip(m, r, "Plan", ui::ColorToken::textHi, ui::ColorToken::raised);
         },
         true},
        {"pill", ui::measurePill(gui, "3 skills"),
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) {
             ui::pill(m, r, ui::Icon::Ok, "3 skills", ui::ColorToken::textHi,
                      ui::ColorToken::raised);
         },
         true},
        {"row", ui::measureRow(gui, "Title"),
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) {
             ui::row(m, r, "Title", true, true);
         },
         true},
        {"icon", ui::measureIconMark(gui),
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) {
             ui::iconMark(m, r, ui::Icon::Ok, ui::ColorToken::stateOk);
         },
         false},
        {"meter", ui::measureMeter(gui, 10),
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) {
             ui::meter(m, r, 10, 0.5f, ui::ColorToken::accent);
         },
         true},
        {"spinner", ui::measureSpinner(gui),
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) { ui::spinner(m, r, 0.3); },
         false},
        {"disclosure", ui::measureDisclosure(gui, "WORK"),
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) {
             ui::disclosure(m, r, "WORK", true);
         },
         false},
        {"skeleton", ui::measureSkeleton(gui, {120.0f, 24.0f}),
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) { ui::skeleton(m, r, 0.5f); },
         true},
    };
    for (const PixelCase& item : cases) {
        const ui::Rect r = rectPixels(10.0f, 10.0f, item.size);
        const VertexBox box =
            vertexFrame([&](ImDrawList* dl, const ui::Metrics& m) { item.draw(dl, m, r); }, gui);
        check(box.count > 0, "the pixel " + item.name + " draws something");
        const bool inside = box.x0 >= r.x - 0.01f && box.y0 >= r.y - 0.01f &&
                            box.x1 <= r.right() + 0.01f && box.y1 <= r.bottom() + 0.01f;
        check(inside, "the pixel " + item.name + " stays inside its measured rect (" + num(box.x0) +
                          "," + num(box.y0) + ")-(" + num(box.x1) + "," + num(box.y1) +
                          ") against " + sizeText(item.size) + " at (" + num(r.x) + "," + num(r.y) +
                          ")");
        if (item.reachesEdges) {
            const bool edges = std::fabs(box.x0 - r.x) < 0.01f && std::fabs(box.y0 - r.y) < 0.01f &&
                               std::fabs(box.x1 - r.right()) < 0.01f &&
                               std::fabs(box.y1 - r.bottom()) < 0.01f;
            check(edges, "the pixel " + item.name + " fills its measured rect exactly");
        }
    }
}

// ---------------------------------------------------------------------------
// 3b. The pixel path's states, corners and shrink behaviour
// ---------------------------------------------------------------------------

/// A hovered button must keep the resting one's geometry: only the fill *colour* moves.
/// The hover fill is derived from a token rather than named by one, which is exactly how a
/// rounded fill can silently become a square one as the pointer enters.
void checkGuiButtonStates() {
    const ui::Metrics gui = guiMetrics();
    const ui::ButtonSpec spec{"Send", true, false, false};
    const ui::Rect r = rectPixels(100.0f, 100.0f, ui::measureButton(gui, spec));
    const auto draw = [&](ImDrawList* dl, const ui::Metrics& m) { ui::button(spec, m, r); };

    // Settle the pointer outside the rect first, so the hover frame is a transition and not
    // the first frame in which ImGui has ever seen a mouse.
    vertexFrame(draw, gui, [&](ImGuiIO& io) { io.AddMousePosEvent(10.0f, 10.0f); });
    const VertexBox rest = vertexFrame(draw, gui);
    vertexFrame(draw, gui, [&](ImGuiIO& io) { io.AddMousePosEvent(r.x + 4.0f, r.y + 4.0f); });
    const VertexBox hover = vertexFrame(draw, gui);

    // The control: the hover is real. On the grid (radius 0, so geometry cannot mask it) the
    // fill's colour index moves when the pointer enters.
    const ui::Metrics grid = gridMetrics();
    const ui::Rect cellRect = rectCells(10.0f, 5.0f, ui::measureButton(grid, spec));
    const auto cellDraw = [&](ImDrawList*, const ui::Metrics& m) { ui::button(spec, m, cellRect); };
    const Grid away = frame(cellDraw, [&](ImGuiIO& io) { io.AddMousePosEvent(2.0f, 20.0f); });
    const Grid over = frame(
        cellDraw, [&](ImGuiIO& io) { io.AddMousePosEvent(cellRect.x + 2.0f, cellRect.y + 0.5f); });
    check(away.bgAt(11, 5) != over.bgAt(11, 5), "hovering a button changes its fill colour (" +
                                                    std::to_string(int(away.bgAt(11, 5))) + " -> " +
                                                    std::to_string(int(over.bgAt(11, 5))) + ")");

    check(hover.count == rest.count && hover.xy == rest.xy,
          "a hovered button keeps the resting one's geometry (only its colour moves); " +
              std::to_string(rest.count) + " vs " + std::to_string(hover.count) + " vertices");
}

/// A rounded primitive must not have a square corner: with a token radius the first vertex
/// of a fill (and of the border) is inset along the corner arc, so no vertex sits exactly on
/// the rect's top-left corner. A set of square edge bars over a rounded fill - or a square
/// hover fill - puts one there.
void checkRoundedCorners() {
    const ui::Metrics gui = guiMetrics();
    const ui::ButtonSpec spec{"Send", true, false, false};
    struct Case {
        const char* name;
        ui::Size size;
        std::function<void(ImDrawList*, const ui::Metrics&, const ui::Rect&)> draw;
    };
    const std::vector<Case> cases = {
        {"panel", ui::measurePanel(gui, {120.0f, 60.0f}),
         [](ImDrawList* dl, const ui::Metrics& m, const ui::Rect& r) {
             ui::panel(dl, m, r, ui::ColorToken::surface, true);
         }},
        {"card", ui::measureCard(gui, {120.0f, 60.0f}),
         [](ImDrawList* dl, const ui::Metrics& m, const ui::Rect& r) {
             ui::card(dl, m, r, ui::ColorToken::raised);
         }},
        {"button", ui::measureButton(gui, spec),
         [&](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) { ui::button(spec, m, r); }},
        {"iconButton", ui::measureIconButton(gui),
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) {
             ui::iconButton(m, r, ui::Icon::Ok, true);
         }},
        {"chip", ui::measureChip(gui, "Plan"),
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) {
             ui::chip(m, r, "Plan", ui::ColorToken::textHi, ui::ColorToken::raised);
         }},
        {"pill", ui::measurePill(gui, "3 skills"),
         [](ImDrawList*, const ui::Metrics& m, const ui::Rect& r) {
             ui::pill(m, r, ui::Icon::Ok, "3 skills", ui::ColorToken::textHi,
                      ui::ColorToken::raised);
         }},
    };
    for (const Case& item : cases) {
        const ui::Rect r = rectPixels(100.0f, 100.0f, item.size);
        const VertexBox box =
            vertexFrame([&](ImDrawList* dl, const ui::Metrics& m) { item.draw(dl, m, r); }, gui);
        check(!box.hasVertexAt(r.x, r.y),
              std::string("the rounded ") + item.name +
                  " has no square corner at its rect's origin (its fill and border follow "
                  "the radius)");
        check(!box.hasVertexAt(r.right(), r.bottom()),
              std::string("the rounded ") + item.name + " has no square corner at its far edge");
    }
}

/// A primitive handed less room than its measure paints the part that fits and nothing
/// outside - a caller may shrink a rect after measuring it (a breakpoint budget, a
/// container that lost a column). `drawLine` and the glyph/segment runs are clipped for
/// exactly this reason.
void checkShrunkRects() {
    const ui::Metrics m = gridMetrics();
    struct Case {
        const char* name;
        ui::Size size;
        std::function<void(ImDrawList*, const ui::Metrics&, const ui::Rect&)> draw;
    };
    const ui::ButtonSpec spec{"a label far wider than the rect", true, false, false};
    const std::vector<Case> cases = {
        {"button", ui::measureButton(m, spec),
         [&](ImDrawList*, const ui::Metrics& mm, const ui::Rect& r) { ui::button(spec, mm, r); }},
        {"chip", ui::measureChip(m, "qwen3-coder"),
         [](ImDrawList*, const ui::Metrics& mm, const ui::Rect& r) {
             ui::chip(mm, r, "qwen3-coder", ui::ColorToken::textHi, ui::ColorToken::raised);
         }},
        {"pill", ui::measurePill(m, "running 00:12"),
         [](ImDrawList*, const ui::Metrics& mm, const ui::Rect& r) {
             ui::pill(mm, r, ui::Icon::Running, "running 00:12", ui::ColorToken::textHi,
                      ui::ColorToken::raised);
         }},
        {"row", ui::measureRow(m, "Plan: modern UI"),
         [](ImDrawList*, const ui::Metrics& mm, const ui::Rect& r) {
             ui::row(mm, r, "Plan: modern UI", true, true);
         }},
        {"icon", ui::measureIconMark(m),
         [](ImDrawList*, const ui::Metrics& mm, const ui::Rect& r) {
             ui::iconMark(mm, r, ui::Icon::Ok, ui::ColorToken::stateOk);
         }},
        {"meter", ui::measureMeter(m, 10),
         [](ImDrawList*, const ui::Metrics& mm, const ui::Rect& r) {
             ui::meter(mm, r, 10, 0.5f, ui::ColorToken::accent);
         }},
        {"spinner", ui::measureSpinner(m),
         [](ImDrawList*, const ui::Metrics& mm, const ui::Rect& r) { ui::spinner(mm, r, 0.3); }},
        {"disclosure", ui::measureDisclosure(m, "WORK"),
         [](ImDrawList*, const ui::Metrics& mm, const ui::Rect& r) {
             ui::disclosure(mm, r, "WORK", true);
         }},
        {"skeleton", ui::measureSkeleton(m, {24.0f, 1.0f}),
         [](ImDrawList*, const ui::Metrics& mm, const ui::Rect& r) { ui::skeleton(mm, r, 0.3f); }},
    };
    for (const Case& item : cases) {
        ui::Rect full = rectCells(2.0f, 2.0f, item.size);
        int paintedFull = 0;
        const Grid whole =
            frame([&](ImDrawList* dl, const ui::Metrics& mm) { item.draw(dl, mm, full); });
        paintedFull = paintedInside(whole, full);
        check(paintedFull > 0, std::string(item.name) + " paints its measured rect");

        ui::Rect shrunk = full;
        shrunk.w = std::max(1.0f, full.w - 4.0f);
        const Grid clipped =
            frame([&](ImDrawList* dl, const ui::Metrics& mm) { item.draw(dl, mm, shrunk); });
        const int outside = paintedOutside(clipped, shrunk);
        if (outside != 0)
            dumpGrid(clipped, item.name);
        check(outside == 0, std::string(item.name) +
                                " paints nothing outside a rect shrunk below its measure (" +
                                std::to_string(outside) + " stray cell(s))");
    }
}

/// An absolutely placed primitive claims no flow space. An item's own contribution to a
/// container's content extent is its position *plus the scroll offset*, so a scrolled
/// region holding absolute items would grow by its own scroll and could never settle.
/// Two assertions: the mechanism (the content extent is untouched) and the symptom (a
/// scrolled window's scroll range does not move, even when the button sits below the
/// content the flow produced).
void checkNoFlowSpace() {
    const ui::Metrics m = gridMetrics();
    const ui::ButtonSpec spec{"Send", true, false, false};
    const ui::Size s = ui::measureButton(m, spec);

    // The mechanism: the extent `ItemSize` would have grown is restored.
    {
        bool extentKept = false;
        frame([&](ImDrawList*, const ui::Metrics& mm) {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            const ImVec2 before = window->DC.CursorMaxPos;
            ui::button(spec, mm, rectCells(2.0f, 2.0f, s));
            const ImVec2 after = window->DC.CursorMaxPos;
            extentKept = before.x == after.x && before.y == after.y;
        });
        check(extentKept, "button() leaves the window's content extent untouched");
    }

    // The symptom: a window whose content is 35 rows in a 30-row viewport, scrolled to its
    // end, with the button placed *below* the flowed content (a pinned footer row, an
    // overlay). Without the restore its extent - and therefore the scroll range - grows.
    const auto maxScrollY = [&](bool withButton) {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(kScreenW), static_cast<float>(kScreenH));
        io.DeltaTime = 1.0f / 60.0f;
        ImTui_ImplText_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kScreenW), kScreenH));
        ImGui::Begin("primitives-scroller", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::Dummy(ImVec2(10.0f, 35.0f)); // five rows of scroll in a 30-row window
        ImGui::SetScrollY(5.0f);
        if (withButton)
            ui::button(spec, m, rectCells(2.0f, 40.0f, s)); // below the flowed content
        const float max = ImGui::GetScrollMaxY();
        ImGui::End();
        ImGui::Render();
        ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
        return max;
    };
    maxScrollY(false); // settle: the content size is measured at the next Begin
    const float baseline = maxScrollY(false);
    maxScrollY(true);
    const float withButton = maxScrollY(true);
    check(std::fabs(baseline - 5.0f) < 1.0f,
          "the scrolled probe window really scrolls by five rows (" + num(baseline) + ")");
    check(std::fabs(withButton - baseline) < 0.5f,
          "an absolutely placed button adds nothing to a scrolled container's extent (" +
              num(baseline) + " -> " + num(withButton) + ")");
}
// ---------------------------------------------------------------------------
// 4. The real button
// ---------------------------------------------------------------------------

struct ClickResult {
    bool pressed = false;
    bool drawn = false;
};

/// A settled pointer frame, a press frame, and a release frame. `pressed` is what
/// `buttonPressed()` answered in the frame the press arrived in - the frame in which
/// the button was drawn.
ClickResult clickButton(ui::Rect r, ui::ButtonSpec spec, float x, float y) {
    ClickResult result;
    frame([&](ImDrawList*, const ui::Metrics& m) { ui::button(spec, m, r); },
          [&](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
    const Grid grid = frame(
        [&](ImDrawList*, const ui::Metrics& m) {
            ui::button(spec, m, r);
            result.pressed = ui::buttonPressed(spec, m, r);
        },
        [&](ImGuiIO& io) {
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(0, true);
        });
    result.drawn = paintedInside(grid, r) > 0;
    frame([](ImDrawList*, const ui::Metrics&) {},
          [&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    return result;
}

void checkButtonHitBox() {
    const ui::Metrics m = gridMetrics();
    const ui::ButtonSpec spec{"Send", true, false, false};
    const ui::Size s = ui::measureButton(m, spec);
    const ui::Rect r = rectCells(10.0f, 5.0f, s);
    check(s.w == 8.0f && s.h == 1.0f, "the hit-box button measures 8 x 1 cells");

    struct Point {
        const char* what;
        float x;
        float y;
        bool inside;
    };
    // The corners (the far edge included: ImGui's own hit test is half-open and the
    // item's touch padding reaches the exclusive edge), the centre, and points a
    // whole unit outside on all four sides.
    const Point points[] = {
        {"top-left corner", r.x, r.y, true},
        {"bottom-right corner", r.right() - 1.0f, r.bottom() - 1.0f, true},
        {"far edge", r.right() - 0.5f, r.y + 0.5f, true},
        {"centre", r.x + 4.0f, r.y + 0.5f, true},
        {"far cell's bottom half", r.right() - 0.5f, r.bottom() - 0.5f, true},
        {"two cells left of the rect", r.x - 2.0f, r.y + 0.5f, false},
        {"two rows above the rect", r.x + 4.0f, r.y - 2.0f, false},
        {"two cells right of the rect", r.right() + 2.0f, r.y + 0.5f, false},
        {"two rows below the rect", r.x + 4.0f, r.bottom() + 2.0f, false},
    };
    for (const Point& point : points) {
        const ClickResult got = clickButton(r, spec, point.x, point.y);
        check(got.drawn, std::string("the button is drawn for the ") + point.what + " probe");
        check(got.pressed == point.inside, std::string("buttonPressed is ") +
                                               (point.inside ? "true" : "false") +
                                               " for a click at the " + point.what);
    }

    // A disabled button is not a target at all.
    const ui::ButtonSpec disabled{"Send", false, false, false};
    check(!clickButton(r, disabled, r.x + 2.0f, r.y + 0.5f).pressed,
          "a disabled button answers no press even from the middle of its rect");

    // A rect no button was drawn at answers nothing.
    {
        const ui::Rect other = rectCells(40.0f, 10.0f, s);
        bool answered = true;
        frame([&](ImDrawList*, const ui::Metrics& mm) {
            ui::button(spec, mm, r);
            answered = ui::buttonPressed(spec, mm, other);
        });
        check(!answered, "a rect no button was drawn at answers nothing");
    }

    // Two buttons in one frame keep their own answers - including two with the same
    // label, because the rect is part of the identity.
    {
        const ui::Rect first = rectCells(10.0f, 5.0f, s);
        const ui::Rect second = rectCells(10.0f, 8.0f, s);
        bool firstPressed = true;
        bool secondPressed = false;
        const auto body = [&](ImDrawList*, const ui::Metrics& mm) {
            ui::button(spec, mm, first);
            ui::button(spec, mm, second);
            firstPressed = ui::buttonPressed(spec, mm, first);
            secondPressed = ui::buttonPressed(spec, mm, second);
        };
        frame(body, [&](ImGuiIO& io) { io.AddMousePosEvent(second.x + 2.0f, second.y + 0.5f); });
        frame(body, [&](ImGuiIO& io) {
            io.AddMousePosEvent(second.x + 2.0f, second.y + 0.5f);
            io.AddMouseButtonEvent(0, true);
        });
        frame([](ImDrawList*, const ui::Metrics&) {},
              [&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
        check(!firstPressed && secondPressed,
              "two buttons with the same label answer their own rect only");
    }

    // The button is one real item: it advances the cursor past its own rect (so it
    // participates in the window's content area, which is what a scrolled list needs),
    // and the label it draws is the label it was given, centred in the rect.
    {
        const ui::Rect labelRect = rectCells(4.0f, 6.0f, s);
        bool advanced = false;
        const Grid grid = frame([&](ImDrawList*, const ui::Metrics& mm) {
            ImGui::SetCursorScreenPos(ImVec2(4.0f, 6.0f));
            ui::button(spec, mm, labelRect);
            advanced = ImGui::GetCursorScreenPos().y >= labelRect.bottom();
        });
        check(advanced, "button() advances the cursor past the item it submitted");
        check(grid.span(6, 6, 4) == "Send", "button() draws its own label, centred in the rect");
        check(grid.at(5, 6) == ' ' && grid.at(10, 6) == ' ',
              "the label is centred: the cell before it and the cell after it carry no text");
    }

    // A press is one true per click: the frame after the press does not answer again,
    // so a caller that reads the button once per frame cannot fire twice.
    {
        bool pressedAgain = true;
        frame(
            [&](ImDrawList*, const ui::Metrics& mm) {
                ui::button(spec, mm, r);
                pressedAgain = ui::buttonPressed(spec, mm, r);
            },
            [&](ImGuiIO& io) {
                io.AddMousePosEvent(r.x + 2.0f, r.y + 0.5f);
                io.AddMouseButtonEvent(0, true);
            });
        frame([&](ImDrawList*, const ui::Metrics& mm) {
            ui::button(spec, mm, r);
            pressedAgain = ui::buttonPressed(spec, mm, r);
        });
        check(!pressedAgain, "a press answers once: the next frame with the button still down "
                             "is not a second click");
    }

    // Two buttons with the same label are two ImGui *items*, not one shared id: ImGui
    // derives an `InvisibleButton` id from its label alone, so the primitive pushes its own
    // identity (label + exact position) first. Without that, two rail rows called "Delete"
    // would share `ActiveId` and ImGui's id debugger would report a duplicate.
    {
        const ui::Rect firstNear = rectCells(10.0f, 5.0f, s);
        const ui::Rect secondNear = rectCells(10.0f, 9.0f, s);
        ImGuiID firstId = 0;
        ImGuiID secondId = 0;
        frame([&](ImDrawList*, const ui::Metrics& mm) {
            ui::button(spec, mm, firstNear);
            firstId = ImGui::GetItemID();
            ui::button(spec, mm, secondNear);
            secondId = ImGui::GetItemID();
        });
        check(firstId != 0 && secondId != 0 && firstId != secondId,
              "two same-label buttons are two ImGui items (distinct item ids)");
    }

    // The three button states are distinguishable in the grid: the primary fill, the rest
    // fill and the disabled fill are three different colours, and a primary button's fill
    // is its own edge (no invisible accent border on accent).
    {
        const ui::Rect restRect = rectCells(2.0f, 2.0f, s);
        const ui::Rect primaryRect = rectCells(2.0f, 4.0f, s);
        const ui::Rect disabledRect = rectCells(2.0f, 6.0f, s);
        const ui::Rect focusRect = rectCells(2.0f, 8.0f, s);
        const Grid grid = frame([&](ImDrawList*, const ui::Metrics& mm) {
            ui::button(spec, mm, restRect);
            ui::button({"Send", true, false, true}, mm, primaryRect);
            ui::button({"Send", false, false, false}, mm, disabledRect);
            ui::button({"Send", true, true, false}, mm, focusRect);
        });
        const uint8_t rest = grid.bgAt(6, 2);
        const uint8_t primary = grid.bgAt(6, 4);
        const uint8_t disabledFill = grid.bgAt(6, 6);
        check(rest != 0 && primary != 0 && disabledFill != 0,
              "all three button states paint a fill");
        check(rest != primary && rest != disabledFill && primary != disabledFill,
              "the rest, primary and disabled fills are three different colours (" +
                  std::to_string(int(rest)) + ", " + std::to_string(int(primary)) + ", " +
                  std::to_string(int(disabledFill)) + ")");

        // A one-row button has no interior for a border, so its shape is its fill -
        // and a taller rect gets the ring, which is what makes the focused state
        // visible where a cell grid can hold it.
        const ui::Rect tallBase = rectCells(2.0f, 12.0f, {12.0f, 3.0f});
        const Grid tall = frame([&](ImDrawList*, const ui::Metrics& mm) {
            ui::button({"Send", true, false, false}, mm, tallBase);
            ui::Rect focusedRect = tallBase;
            focusedRect.y = 16.0f;
            ui::button({"Send", true, true, false}, mm, focusedRect);
        });
        check(tall.bgAt(7, 13) != tall.bgAt(2, 12),
              "a three-row button draws its border inside the rect, over the fill");
        check(tall.bgAt(2, 16) != tall.bgAt(2, 12),
              "a focused button's ring uses a different colour from an unfocused one");
        check(tall.bgAt(7, 17) == tall.bgAt(7, 13),
              "the ring is a frame: the interior of a focused button keeps its fill");

        // A primary button has no border (an accent border on an accent fill would be
        // invisible), and its focus ring flips to `canvas` so that it can read.
        const ui::Rect primaryTall = rectCells(2.0f, 20.0f, {12.0f, 3.0f});
        const ui::Rect primaryFocused = rectCells(2.0f, 24.0f, {12.0f, 3.0f});
        const Grid primaryGrid = frame([&](ImDrawList*, const ui::Metrics& mm) {
            ui::button({"Send", true, false, true}, mm, primaryTall);
            ui::button({"Send", true, true, true}, mm, primaryFocused);
        });
        check(primaryGrid.bgAt(2, 20) == primaryGrid.bgAt(7, 21),
              "a primary button has no border: its fill is its edge");
        check(primaryGrid.bgAt(2, 24) != primaryGrid.bgAt(7, 25),
              "a focused primary button's ring differs from its own fill");
    }
}

// ---------------------------------------------------------------------------
// 5. The spinner
// ---------------------------------------------------------------------------

void checkSpinner() {
    const struct {
        double phase;
        int frame;
    } cases[] = {
        {-1.0, 0},  {-0.001, 0}, {0.0, 0},   {0.075, 0}, {0.149, 0},
        {0.150, 1}, {0.225, 1},  {0.299, 1}, {0.300, 2}, {0.449, 2},
        {0.450, 3}, {0.599, 3},  {0.600, 0}, {1.200, 0}, {1.350, 1},
    };
    for (const auto& c : cases) {
        check(ui::spinnerFrame(c.phase) == c.frame, "spinnerFrame(" +
                                                        num(static_cast<float>(c.phase)) +
                                                        ") is frame " + std::to_string(c.frame));
    }
    check(ui::spinnerFrame(std::nan("")) == 0, "a NaN phase answers the static frame");
    check(ui::spinnerFrame(INFINITY) == 0, "an infinite phase answers the static frame");
    check(ui::spinnerFrame(-INFINITY) == 0, "a negative infinite phase answers the static frame");
    check(ui::kSpinnerFrameSeconds == 0.150, "a spinner frame lasts the design's 150 ms");

    // Each frame draws one cell, and frame 0 is the glyph table's own mark: a
    // surface with motion off and a surface at rest draw the same character.
    std::string seen;
    for (int index = 0; index < 4; ++index) {
        const double phase = double(index) * ui::kSpinnerFrameSeconds + 0.001;
        const Grid grid = frame([&](ImDrawList*, const ui::Metrics& mm) {
            ui::spinner(mm, rectCells(2.0f, 2.0f, ui::measureSpinner(mm)), phase);
        });
        check(!grid.blank(2, 2), "spinner frame " + std::to_string(index) + " draws a mark");
        check(grid.cellWidth(2, 2) == 1,
              "spinner frame " + std::to_string(index) + " is one cell wide as rendered");
        seen += utf8Of(grid.at(2, 2));
    }
    check(seen.size() == 12, "the four spinner frames read back as four three-byte marks (" +
                                 std::to_string(seen.size()) + " bytes)");
    if (seen.size() == 12) {
        check(seen.substr(0, 3) ==
                  utf8Of(codepointOf(ui::icon(ui::Icon::Spinner, ui::GlyphSet::Glyphs))),
              "spinner frame 0 is the glyph table's Spinner mark");
        check(seen.substr(0, 3) != seen.substr(3, 3) && seen.substr(3, 3) != seen.substr(6, 3) &&
                  seen.substr(6, 3) != seen.substr(9, 3) && seen.substr(0, 3) != seen.substr(9, 3),
              "the four frames are four different marks");
    }
    check(ui::displayWidth(std::string(ui::icon(ui::Icon::Spinner, ui::GlyphSet::Glyphs))) == 1,
          "the table's spinner mark is one cell wide");
}

// ---------------------------------------------------------------------------
// 6. The meter
// ---------------------------------------------------------------------------

void checkMeter() {
    const ui::Metrics m = gridMetrics();
    const ui::Rect r = rectCells(2.0f, 2.0f, ui::measureMeter(m, 10));
    const uint32_t full = codepointOf("\xE2\x96\x88");  // █
    const uint32_t empty = codepointOf("\xE2\x96\x91"); // ░

    const struct {
        float fraction;
        int filled;
    } cases[] = {
        {-1.0f, 0}, {0.0f, 0},  {0.05f, 0}, {0.1f, 1},  {0.42f, 4},
        {0.5f, 5},  {0.99f, 9}, {1.0f, 10}, {2.0f, 10},
    };
    for (const auto& c : cases) {
        const Grid grid = frame([&](ImDrawList*, const ui::Metrics& mm) {
            ui::meter(mm, r, 10, c.fraction, ui::ColorToken::accent);
        });
        int fullCount = 0;
        int emptyCount = 0;
        for (int x = 2; x < 12; ++x) {
            fullCount += grid.at(x, 2) == full ? 1 : 0;
            emptyCount += grid.at(x, 2) == empty ? 1 : 0;
        }
        check(fullCount == c.filled, "a meter at " + num(c.fraction) + " shows " +
                                         std::to_string(c.filled) + " full segment(s), got " +
                                         std::to_string(fullCount));
        check(emptyCount == 10 - c.filled, "a meter at " + num(c.fraction) + " shows " +
                                               std::to_string(10 - c.filled) + " empty segment(s)");
        check(paintedOutside(grid, r) == 0,
              "a meter at " + num(c.fraction) + " stays inside its measured rect");
    }

    const Grid poisoned = frame([&](ImDrawList*, const ui::Metrics& mm) {
        ui::meter(mm, r, 10, std::nanf(""), ui::ColorToken::accent);
    });
    int fullCount = 0;
    for (int x = 2; x < 12; ++x)
        fullCount += poisoned.at(x, 2) == full ? 1 : 0;
    check(fullCount == 0, "a NaN fraction draws an empty meter rather than a full one");

    // The fill colour is the caller's token, and the empty half is the boundary
    // token: the two runs are visibly different cells.
    const Grid grid = frame([&](ImDrawList*, const ui::Metrics& mm) {
        ui::meter(mm, r, 10, 0.4f, ui::ColorToken::accent);
    });
    const uint8_t filledInk = g_screen.data[2 * g_screen.nx + 2].fg;
    const uint8_t emptyInk = g_screen.data[2 * g_screen.nx + 9].fg;
    check(filledInk != emptyInk, "the meter's filled and empty runs use different tokens");
    check(!grid.blank(2, 2) && !grid.blank(9, 2), "both meter runs are drawn");
}

// ---------------------------------------------------------------------------
// 7. Contrast
// ---------------------------------------------------------------------------

float srgbToLinear(float channel) {
    return channel <= 0.04045f ? channel / 12.92f : std::pow((channel + 0.055f) / 1.055f, 2.4f);
}

float relativeLuminance(const ImVec4& colour) {
    return 0.2126f * srgbToLinear(colour.x) + 0.7152f * srgbToLinear(colour.y) +
           0.0722f * srgbToLinear(colour.z);
}

float contrastRatio(const ImVec4& a, const ImVec4& b) {
    const float la = relativeLuminance(a);
    const float lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

/// Source-over: what the pixel profile does with a translucent fill.
ImVec4 compositeOver(const ImVec4& fg, const ImVec4& bg) {
    const float a = fg.w;
    return ImVec4(fg.x * a + bg.x * (1.0f - a), fg.y * a + bg.y * (1.0f - a),
                  fg.z * a + bg.z * (1.0f - a), 1.0f);
}

/// What the text backend does with one: `rgbToAnsi256` multiplies the channels by
/// the alpha instead of blending them (measured in the vendored imtui), so a
/// translucent token on the grid comes out darker than the canvas under it. Both
/// forms are checked: the ink has to survive whichever the live profile uses.
ImVec4 multiplied(const ImVec4& colour) {
    return ImVec4(colour.x * colour.w, colour.y * colour.w, colour.z * colour.w, 1.0f);
}

void checkContrastPairs() {
    const auto rgb = [](ui::ColorToken token) {
        return ui::token(token, ui::ColorDepth::TrueColor).rgb;
    };
    const ImVec4 canvas = rgb(ui::ColorToken::canvas);
    const ImVec4 surface = rgb(ui::ColorToken::surface);
    const ImVec4 raised = rgb(ui::ColorToken::raised);
    const ImVec4 inset = rgb(ui::ColorToken::inset);
    const ImVec4 accent = rgb(ui::ColorToken::accent);
    const ImVec4 textHi = rgb(ui::ColorToken::textHi);
    const ImVec4 textMid = rgb(ui::ColorToken::textMid);
    const ImVec4 textLow = rgb(ui::ColorToken::textLow);
    const ImVec4 lineStrong = rgb(ui::ColorToken::lineStrong);
    const ImVec4 selection = rgb(ui::ColorToken::selection);

    struct Pair {
        const char* what;
        ImVec4 ink;
        ImVec4 fill;
        float bar;
    };
    const Pair pairs[] = {
        {"a primary button's label on its accent fill", canvas, accent, 4.5f},
        {"a button's label on its raised fill", textHi, raised, 4.5f},
        {"a button's label on the hover step above raised", textHi, ui::stateStep(raised, 1), 4.5f},
        {"a primary button's label on the hover step above accent", canvas,
         ui::stateStep(accent, 1), 4.5f},
        {"a disabled button's label on inset", textLow, inset, 4.5f},
        {"a chip's label on raised", textMid, raised, 4.5f},
        {"a row's label on the canvas", textMid, canvas, 4.5f},
        {"a selected row's label on the composited selection fill", textHi,
         compositeOver(selection, canvas), 4.5f},
        {"a selected row's label on the grid's alpha-multiplied selection", textHi,
         multiplied(selection), 4.5f},
        {"a disabled row's label on the canvas", textLow, canvas, 4.5f},
        {"a disclosure's label on the canvas", textHi, canvas, 4.5f},
        {"a disclosure's glyph on the canvas", textLow, canvas, 4.5f},
        {"the spinner on the canvas", accent, canvas, 4.5f},
        {"a skeleton's static mark on inset", textLow, inset, 4.5f},
        {"an icon button's glyph on raised", textMid, raised, 4.5f},
        {"a meter's filled segment on the canvas", accent, canvas, 3.0f},
        {"a meter's empty segment on the canvas", lineStrong, canvas, 3.0f},
        {"a meter's empty segment on the surface", lineStrong, surface, 3.0f},
        {"a primary button's focus ring on its accent fill", canvas, accent, 4.5f},
        {"a quiet button's focus ring on raised",
         compositeOver(rgb(ui::ColorToken::focusRing), raised), raised, 3.0f},
    };
    for (const Pair& pair : pairs) {
        const float ratio = contrastRatio(pair.ink, pair.fill);
        check(ratio >= pair.bar, std::string(pair.what) + " reaches " + num(pair.bar) +
                                     ":1 (measures " + num(ratio) + ":1)");
    }

    // The one token the design bars from carrying a glyph is genuinely below the bar
    // on the darkest fill too, so "the disabled ink is textLow" is a measurement
    // rather than a preference.
    const ImVec4 textDim = rgb(ui::ColorToken::textDim);
    check(contrastRatio(textDim, inset) < 4.5f,
          "textDim could not carry the disabled label either (it measures " +
              num(contrastRatio(textDim, inset)) + ":1 on inset)");
}

// ---------------------------------------------------------------------------
// 8. Goldens
// ---------------------------------------------------------------------------

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

uint32_t codepointOf(const std::string& utf8) {
    if (utf8.empty())
        return 0;
    const unsigned char* s = reinterpret_cast<const unsigned char*>(utf8.data());
    if (s[0] < 0x80)
        return s[0];
    if ((s[0] & 0xE0) == 0xC0 && utf8.size() >= 2)
        return (uint32_t(s[0] & 0x1Fu) << 6) | uint32_t(s[1] & 0x3Fu);
    if ((s[0] & 0xF0) == 0xE0 && utf8.size() >= 3)
        return (uint32_t(s[0] & 0x0Fu) << 12) | (uint32_t(s[1] & 0x3Fu) << 6) |
               uint32_t(s[2] & 0x3Fu);
    if ((s[0] & 0xF8) == 0xF0 && utf8.size() >= 4)
        return (uint32_t(s[0] & 0x07u) << 18) | (uint32_t(s[1] & 0x3Fu) << 12) |
               (uint32_t(s[2] & 0x3Fu) << 6) | uint32_t(s[3] & 0x3Fu);
    return 0;
}

/// The caption every golden screen starts with: the primitive's name and the size it
/// measured, so the file pins the arithmetic as well as the pixels.
void captionLine(ImDrawList* dl, const char* name, ui::Size s) {
    const std::string text = std::string(name) + " measure " + sizeText(s);
    dl->AddText(ImVec2(1.0f, 0.0f), ImGui::GetColorU32(ui::color(ui::ColorToken::textHi)),
                text.c_str());
}

struct Scene {
    const char* name;
    std::function<void(ImDrawList*, const ui::Metrics&)> draw;
};

std::vector<Scene> scenes() {
    std::vector<Scene> list;
    list.push_back({"panel", [](ImDrawList* dl, const ui::Metrics& m) {
                        const ui::Rect r =
                            rectCells(2.0f, 2.0f, ui::measurePanel(m, {24.0f, 6.0f}));
                        captionLine(dl, "panel", {r.w, r.h});
                        ui::panel(dl, m, r, ui::ColorToken::surface, true);
                    }});
    list.push_back({"card", [](ImDrawList* dl, const ui::Metrics& m) {
                        const ui::Rect r = rectCells(2.0f, 2.0f, ui::measureCard(m, {24.0f, 6.0f}));
                        captionLine(dl, "card", {r.w, r.h});
                        ui::card(dl, m, r, ui::ColorToken::raised);
                    }});
    list.push_back({"rule", [](ImDrawList* dl, const ui::Metrics& m) {
                        const ui::Size s = ui::measureRule(m, 40.0f);
                        captionLine(dl, "rule", s);
                        ui::rule(m, rectCells(2.0f, 2.0f, s), ui::ColorToken::lineStrong);
                        ui::rule(m, rectCells(2.0f, 4.0f, ui::measureRule(m, 20.0f, false)),
                                 ui::ColorToken::lineSoft);
                    }});
    list.push_back({"button", [](ImDrawList* dl, const ui::Metrics& m) {
                        const ui::ButtonSpec rest{"Send", true, false, false};
                        const ui::Size s = ui::measureButton(m, rest);
                        captionLine(dl, "button", s);
                        ui::button(rest, m, rectCells(2.0f, 2.0f, s));
                        ui::button({"Send", true, false, true}, m, rectCells(12.0f, 2.0f, s));
                        ui::button({"Send", false, false, false}, m, rectCells(22.0f, 2.0f, s));
                        ui::button({"Send", true, true, false}, m, rectCells(32.0f, 2.0f, s));
                    }});
    list.push_back({"iconButton", [](ImDrawList* dl, const ui::Metrics& m) {
                        const ui::Size s = ui::measureIconButton(m);
                        captionLine(dl, "iconButton", s);
                        for (int i = 0; i < ui::kIconCount; ++i)
                            ui::iconButton(m, rectCells(2.0f + float(i), 2.0f, s),
                                           static_cast<ui::Icon>(i), i % 3 == 0);
                    }});
    list.push_back({"chip", [](ImDrawList* dl, const ui::Metrics& m) {
                        const ui::Size s = ui::measureChip(m, "Plan");
                        captionLine(dl, "chip", s);
                        ui::chip(m, rectCells(2.0f, 2.0f, s), "Plan", ui::ColorToken::textHi,
                                 ui::ColorToken::raised);
                        ui::chip(m, rectCells(10.0f, 2.0f, ui::measureChip(m, "qwen3-coder")),
                                 "qwen3-coder", ui::ColorToken::textMid, ui::ColorToken::raised);
                        ui::chip(m, rectCells(2.0f, 4.0f, ui::measureChip(m, "3 skills")),
                                 "3 skills", ui::ColorToken::textLow, ui::ColorToken::inset);
                    }});
    list.push_back({"pill", [](ImDrawList* dl, const ui::Metrics& m) {
                        const ui::Size s = ui::measurePill(m, "running 00:12");
                        captionLine(dl, "pill", s);
                        ui::pill(m, rectCells(2.0f, 2.0f, s), ui::Icon::Running, "running 00:12",
                                 ui::ColorToken::textHi, ui::ColorToken::raised);
                        ui::pill(m, rectCells(2.0f, 4.0f, ui::measurePill(m, "Ready")),
                                 ui::Icon::Active, "Ready", ui::ColorToken::textMid,
                                 ui::ColorToken::raised);
                    }});
    list.push_back({"row", [](ImDrawList* dl, const ui::Metrics& m) {
                        const ui::Size s = ui::measureRow(m, "Plan: modern UI");
                        captionLine(dl, "row", s);
                        ui::row(m, rectCells(2.0f, 2.0f, s), "Plan: modern UI", false, true);
                        ui::row(m, rectCells(2.0f, 3.0f, s), "Active session", true, true);
                        ui::row(m, rectCells(2.0f, 4.0f, s), "Disabled row", false, false);
                    }});
    list.push_back({"icon", [](ImDrawList* dl, const ui::Metrics& m) {
                        const ui::Size s = ui::measureIconMark(m);
                        captionLine(dl, "icon", s);
                        for (int i = 0; i < ui::kIconCount; ++i)
                            ui::iconMark(m, rectCells(2.0f + float(i), 2.0f, s),
                                         static_cast<ui::Icon>(i), ui::ColorToken::textMid);
                    }});
    list.push_back({"meter", [](ImDrawList* dl, const ui::Metrics& m) {
                        const ui::Size s = ui::measureMeter(m, 10);
                        captionLine(dl, "meter", s);
                        ui::meter(m, rectCells(2.0f, 2.0f, s), 10, 0.42f, ui::ColorToken::accent);
                        ui::meter(m, rectCells(2.0f, 4.0f, s), 10, 0.0f, ui::ColorToken::accent);
                        ui::meter(m, rectCells(2.0f, 6.0f, s), 10, 1.0f, ui::ColorToken::stateWarn);
                    }});
    list.push_back({"spinner", [](ImDrawList* dl, const ui::Metrics& m) {
                        const ui::Size s = ui::measureSpinner(m);
                        captionLine(dl, "spinner", s);
                        for (int frame = 0; frame < 4; ++frame)
                            ui::spinner(m, rectCells(2.0f + float(frame) * 2.0f, 2.0f, s),
                                        double(frame) * ui::kSpinnerFrameSeconds + 0.001);
                    }});
    list.push_back({"disclosure", [](ImDrawList* dl, const ui::Metrics& m) {
                        const ui::Size s = ui::measureDisclosure(m, "WORK");
                        captionLine(dl, "disclosure", s);
                        ui::disclosure(m, rectCells(2.0f, 2.0f, s), "WORK", false);
                        ui::disclosure(m, rectCells(2.0f, 3.0f, s), "WORK", true);
                    }});
    list.push_back({"skeleton", [](ImDrawList* dl, const ui::Metrics& m) {
                        const ui::Size s = ui::measureSkeleton(m, {24.0f, 1.0f});
                        captionLine(dl, "skeleton", s);
                        ui::skeleton(m, rectCells(2.0f, 2.0f, s), 0.0f);
                        ui::skeleton(m, rectCells(2.0f, 4.0f, s), 0.35f);
                    }});
    list.push_back({"tooltip", [](ImDrawList* dl, const ui::Metrics& m) {
                        // The widget itself is ImGui's own window; what this screen pins
                        // is the anatomy `measureTooltip` answers for it - the box the
                        // layout has to leave room for.
                        const ui::Size s = ui::measureTooltip(m, "compaction at 115k");
                        captionLine(dl, "tooltip", s);
                        const ui::Rect r = rectCells(2.0f, 2.0f, s);
                        ui::panel(dl, m, r, ui::ColorToken::raised, true);
                        dl->AddText(ImVec2(r.x + m.pad2, r.y + m.pad1),
                                    ImGui::GetColorU32(ui::color(ui::ColorToken::textHi)),
                                    "compaction at 115k");
                    }});
    return list;
}

std::string goldenText(const Scene& scene) {
    const Grid grid = frame([&](ImDrawList* dl, const ui::Metrics& m) { scene.draw(dl, m); });
    std::string text = "name: ";
    text += scene.name;
    text += "\nscreen: 100x30\ntext:\n";
    for (int y = 0; y < grid.ny; ++y) {
        std::string row = grid.rowText(y);
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
        text += row;
        text += '\n';
    }
    // A text dump cannot show a fill, and a primitive *is* mostly its fill: so the
    // painted background is recorded as runs of equal colour index, one line per row
    // that has any. That pins each fill's extent and its token - a fill that moved or
    // changed colour fails the golden exactly like a moved glyph.
    text += "fills:\n";
    for (int y = 0; y < grid.ny; ++y) {
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

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path);
    if (!in.good())
        return false;
    out.clear();
    std::string line;
    while (std::getline(in, line)) {
        out += line;
        out += '\n';
    }
    return true;
}

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    if (!out.good())
        return false;
    out << text;
    return out.good();
}

void checkGoldens(bool update) {
    const std::string dir = TEST_PRIMITIVES_GOLDEN_DIR;
    const std::vector<Scene> all = scenes();
    check(all.size() == 14,
          "every primitive has a golden screen (" + std::to_string(all.size()) + " of 14)");
    for (const Scene& scene : all) {
        const std::string path = dir + "/primitives-" + std::string(scene.name) + "-100x30.txt";
        const std::string text = goldenText(scene);
        if (update) {
            check(writeFile(path, text), "golden written: " + path);
            continue;
        }
        std::string expected;
        if (!readFile(path, expected)) {
            check(false, "golden is readable: " + path + " (run with --update to create it)");
            continue;
        }
        if (expected == text) {
            check(true, std::string("the ") + scene.name + " golden matches");
            continue;
        }
        std::size_t line = 1;
        std::size_t at = 0;
        while (at < expected.size() && at < text.size() && expected[at] == text[at]) {
            line += expected[at] == '\n' ? 1 : 0;
            ++at;
        }
        check(false, std::string("the ") + scene.name +
                         " golden matches (first difference on line " + std::to_string(line) +
                         ": " + path + ")");
    }
}

void checkSpinnerAscii() {
    // The ASCII column: `| / - \`. The environment is flipped for these frames and
    // restored before the process ends, so nothing after them can see it.
    ::setenv("LLMFUN_ASCII", "1", 1);
    const char* expected[4] = {"|", "/", "-", "\\"};
    for (int frame0 = 0; frame0 < 4; ++frame0) {
        const Grid grid = frame([&](ImDrawList*, const ui::Metrics& m) {
            ui::spinner(m, rectCells(2.0f, 2.0f, {1.0f, 1.0f}),
                        double(frame0) * ui::kSpinnerFrameSeconds + 0.001);
        });
        check(grid.rowText(2)[2] == *expected[frame0], std::string("ASCII spinner frame ") +
                                                           std::to_string(frame0) + " is '" +
                                                           expected[frame0] + "'");
        check(ui::displayWidth(expected[frame0]) == 1,
              std::string("ASCII spinner frame ") + std::to_string(frame0) + " is one cell wide");
    }
    ::setenv("LLMFUN_ASCII", "0", 1);
    check(ui::textGlyphSet() == ui::GlyphSet::Glyphs,
          "the environment is back on the Unicode column after the ASCII checks");
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

    // The golden screens are captured in one glyph set, whatever terminal the run
    // happens on: `LLMFUN_ASCII=0` outranks TERM and the locale in the detection
    // order, so a CJK locale cannot change a golden.
    ::setenv("LLMFUN_ASCII", "0", 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // Before the backend and before any frame: no font is installed yet, which is the
    // state in which a GUI measure used to segfault inside `CalcTextSize`.
    checkMeasureBeforeFirstFrame();
    ImTui_ImplText_Init();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kScreenW), static_cast<float>(kScreenH));
    io.DeltaTime = 1.0f / 60.0f;

    // One warm-up frame: the pixel-profile measures need a live font, and the first
    // frame is also where the text backend builds its atlas.
    frame([](ImDrawList*, const ui::Metrics&) {});

    checkSource();
    checkArithmeticText();
    checkArithmeticGui();
    checkMixedUnits();
    checkRenderGridCases();
    checkRenderPixelCases();
    checkGuiButtonStates();
    checkRoundedCorners();
    checkShrunkRects();
    checkButtonHitBox();
    checkNoFlowSpace();
    checkSpinner();
    checkMeter();
    checkContrastPairs();
    checkGoldens(update);
    checkSpinnerAscii();

    ImTui_ImplText_Shutdown();
    ImGui::DestroyContext();

    std::printf("%s: %d check(s), %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_checks,
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
