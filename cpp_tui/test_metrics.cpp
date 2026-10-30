// test_metrics.cpp
//
// Unit test for the metrics module (task 13, D-METRICS, design §3.4) and for the
// gate ledger it installs.
//
// `ui/metrics.h` is where every length the shell draws with comes from, and the
// shell is correct only if those lengths are exact. This binary pins what that
// rests on:
//
//   0. the types      the default Rect claims no unit and has no area, its
//                     edges are derived from x/y/w/h, and a default Metrics is
//                     inert - the states a view must not draw as if measured;
//
//   1. rounding       roundCells: 0.5 -> 0, 1.5 -> 1, 2.5 -> 2 (halves round
//                     *down*, never up), a whole count is unchanged, every value
//                     stays within half a unit of its answer, and a poisoned
//                     value (NaN, infinity) answers 0 instead of propagating
//                     into a layout;
//   2. text grid      every field is a whole number of cells and equals the
//                     design's cell counts: gu/row 1, pads 1/2/3, radius 0,
//                     border 1, top bar 1 row, rail 26, collapsed rail 7,
//                     inspector 28, now bar 1 row, composer 4/11;
//   3. GUI 100 %      gu 8 / row 24 px, rail 208 px, collapsed rail 56 px,
//                     inspector 224 px, now bar 28 px, top bar 48 px, composer
//                     50/218 px, pads from the space tokens (8/12/16), radius 6;
//   4. GUI 150 %      one scale applied once: gu 12 / row 36 px and the regions
//                     scale with them (collapsed rail 84 px, inspector 336 px,
//                     now bar 42 px, composer 74/326 px, hairline still 1 px);
//   5. scale rules    the text grid ignores the font and DPI scale, the two
//                     factors multiply (order-free), a bad factor is 100 %, and
//                     the product is capped so it cannot overflow into 0;
//   6. live profile   ui::metricsFor() answers the profile that is actually up -
//                     the pixel profile with no backend, the one-cell grid after
//                     ImTui_ImplText_Init() - and reads the live ImGui scale
//                     factors, so a view that asks it can never disagree with
//                     what the backend is drawing;
//   7. gate ledger    tools/gate_ratchet.txt states exactly what the tree
//                     contains: no unlisted render-profile branch, hand-made
//                     colour, v4 payload type or display-size read, no row that
//                     grew, no profile branch or ImVec4 in `views/`, and no
//                     `tui.h` under `ui/`;
//   8. vocabulary     no comment calls back a renderer that task 41 removed: the
//                     banned phrases (`kBanned`) may appear only where the same
//                     sentence names `store/` or marks the history (`task 41`),
//                     because a reader who believes a second renderer exists will
//                     write code for it.
//
// Exit codes: 0 = all checks passed; 1 = at least one failed; 2 = usage error.
//
// Headless (no display, no terminal). Run from the build dir:
//   ./test_metrics [-v]

#include "imtui/imtui.h" // imgui + ImTui_ImplText_Init/Shutdown

#include "ui/metrics.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ui = llmfun::tui::ui;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
bool g_verbose = false;

void check(bool condition, const std::string& what) {
    if (condition) {
        if (g_verbose)
            std::fprintf(stderr, "ok   %s\n", what.c_str());
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

std::string number(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", static_cast<double>(value));
    return text;
}

void expect(float got, float want, const std::string& what) {
    check(got == want, what + ": got " + number(got) + ", expected " + number(want));
}

// ---------------------------------------------------------------------------
// 0. The types
// ---------------------------------------------------------------------------

/// Every length in `Metrics`, through the module's own visitor: one inventory in
/// `ui/metrics.h` (`visitLengths()`, guarded by a `static_assert` on the struct
/// size) instead of a copy here that a new field could silently miss.
std::vector<std::pair<std::string, float>> metricFields(const ui::Metrics& m) {
    std::vector<std::pair<std::string, float>> fields;
    ui::visitLengths(
        m, [&fields](const char* name, float value) { fields.emplace_back(name, value); });
    return fields;
}

void checkTypes() {
    // The default Rect is "the layout has not answered yet": it must claim no
    // unit and enclose no area, so a view that draws it before the tiler runs
    // collapses instead of painting a plausible rectangle in the wrong unit.
    const ui::Rect empty;
    expect(empty.x, 0.0f, "default Rect: x is 0");
    expect(empty.y, 0.0f, "default Rect: y is 0");
    expect(empty.w, 0.0f, "default Rect: w is 0");
    expect(empty.h, 0.0f, "default Rect: h is 0");
    check(!empty.cells, "default Rect: claims no unit (cells is false)");
    expect(empty.right(), 0.0f, "default Rect: right() is x + w");
    expect(empty.bottom(), 0.0f, "default Rect: bottom() is y + h");

    // The edges are derived, not stored, so a consumer that asserts `cells` first
    // sees the same numbers the tiler computed - and the flag survives a
    // hand-filled rectangle, which is how the layout engine builds them.
    ui::Rect cellRect;
    cellRect.x = 2.0f;
    cellRect.y = 3.0f;
    cellRect.w = 10.0f;
    cellRect.h = 4.0f;
    cellRect.cells = true;
    expect(cellRect.right(), 12.0f, "Rect::right() is x + w");
    expect(cellRect.bottom(), 7.0f, "Rect::bottom() is y + h");
    check(cellRect.cells, "a filled Rect keeps the unit it was given");

    // A default Metrics is inert, which `metrics.h` promises: a view that forgot
    // to ask for the metrics lays out nothing rather than a plausible wrong size.
    const ui::Metrics m;
    check(!m.textGrid, "default Metrics: textGrid is false");
    bool inert = true;
    for (const auto& field : metricFields(m))
        inert = inert && field.second == 0.0f;
    check(inert, "default Metrics: every length is 0");
}

// ---------------------------------------------------------------------------
// 1. Rounding
// ---------------------------------------------------------------------------

void checkRounding() {
    // The three values the grid actually produces, and the documented answer.
    expect(ui::roundCells(0.5f), 0.0f, "roundCells(0.5) = 0");
    expect(ui::roundCells(1.5f), 1.0f, "roundCells(1.5) = 1");
    expect(ui::roundCells(2.5f), 2.0f, "roundCells(2.5) = 2");
    // Halves go down, everything else goes to the nearest whole unit.
    expect(ui::roundCells(0.0f), 0.0f, "roundCells(0) = 0");
    expect(ui::roundCells(0.49f), 0.0f, "roundCells(0.49) = 0");
    expect(ui::roundCells(0.51f), 1.0f, "roundCells(0.51) = 1");
    expect(ui::roundCells(1.49f), 1.0f, "roundCells(1.49) = 1");
    expect(ui::roundCells(1.51f), 2.0f, "roundCells(1.51) = 2");
    expect(ui::roundCells(12.5f), 12.0f, "roundCells(12.5) = 12");
    expect(ui::roundCells(26.0f), 26.0f, "roundCells leaves a whole count alone");
    // A poisoned length must die here, not in a SetNextWindowSize call.
    expect(ui::roundCells(std::numeric_limits<float>::quiet_NaN()), 0.0f, "roundCells(NaN) = 0");
    expect(ui::roundCells(std::numeric_limits<float>::infinity()), 0.0f, "roundCells(+inf) = 0");
    expect(ui::roundCells(-std::numeric_limits<float>::infinity()), 0.0f, "roundCells(-inf) = 0");

    // Below zero the same rule rounds an exact half away from zero: the mirror of
    // the positive rule, and the reason the contract is total (`metrics.h`). No
    // design length is negative; this is here so a later signed length behaves.
    expect(ui::roundCells(-0.5f), -1.0f, "roundCells(-0.5) = -1");
    expect(ui::roundCells(-1.5f), -2.0f, "roundCells(-1.5) = -2");
    expect(ui::roundCells(-2.0f), -2.0f, "roundCells(-2) = -2");
    expect(ui::roundCells(-0.4f), 0.0f, "roundCells(-0.4) = 0");

    // Whole counts are fixed points - the text grid's identity property.
    bool identity = true;
    for (int cells = 0; cells <= 64; ++cells)
        identity =
            identity && ui::roundCells(static_cast<float>(cells)) == static_cast<float>(cells);
    check(identity, "every whole cell count is a fixed point of roundCells");

    // Nearest whole unit, ties down: |answer - value| <= 0.5, and an exact half
    // never rounds up. A round-half-up formula fails the tie branch (0.5 -> 1),
    // which is the overshoot that breaks a cell-exact layout.
    bool nearest = true;
    bool tiesDown = true;
    for (int quarter = 0; quarter <= 400; ++quarter) {
        const float value = static_cast<float>(quarter) * 0.25f;
        const float answer = ui::roundCells(value);
        if (!(std::fabs(answer - value) <= 0.5f))
            nearest = false;
        if (std::fabs(answer - value) == 0.5f && answer > std::floor(value))
            tiesDown = false;
    }
    check(nearest, "roundCells stays within half a unit of the value it rounds");
    check(tiesDown, "an exact half rounds down, never up");
}

// ---------------------------------------------------------------------------
// 2-4. The two profile tables
// ---------------------------------------------------------------------------

/// One row of the design's metrics table (metrics.h states the source of every
/// number). Spelled out per profile on purpose: the test is the second reader of
/// that table, so a drifting constant shows up as a failed expectation instead
/// of as a layout that quietly moved.
struct Expected {
    const char* what;
    float gu;
    float row;
    float pad1;
    float pad2;
    float pad3;
    float radius;
    float border;
    float topBarH;
    float railW;
    float railCollapsedW;
    float inspectorW;
    float nowBarH;
    float composerMinH;
    float composerMaxH;
};

void expectMetrics(const ui::Metrics& m, const Expected& e) {
    const std::string at = std::string(e.what) + ": ";
    expect(m.gu, e.gu, at + "gu");
    expect(m.row, e.row, at + "row");
    expect(m.pad1, e.pad1, at + "pad1");
    expect(m.pad2, e.pad2, at + "pad2");
    expect(m.pad3, e.pad3, at + "pad3");
    expect(m.radius, e.radius, at + "radius");
    expect(m.border, e.border, at + "border");
    expect(m.topBarH, e.topBarH, at + "top bar height");
    expect(m.railW, e.railW, at + "rail width");
    expect(m.railCollapsedW, e.railCollapsedW, at + "collapsed rail width");
    expect(m.inspectorW, e.inspectorW, at + "inspector width");
    expect(m.nowBarH, e.nowBarH, at + "now bar height");
    expect(m.composerMinH, e.composerMinH, at + "composer minimum height");
    expect(m.composerMaxH, e.composerMaxH, at + "composer maximum height");
}

void expectAllWhole(const ui::Metrics& m, const std::string& what) {
    // A cell is the smallest unit a terminal has: a fraction of one cannot be
    // drawn, and a layout that carries one drifts a column at a time.
    for (const auto& field : metricFields(m))
        check(std::fmod(field.second, 1.0f) == 0.0f,
              what + ": " + field.first + " is a whole number (" + number(field.second) + ")");
}

void checkTextGridMetrics() {
    const Expected e{
        /*what*/ "text grid",
        /*gu*/ 1.0f,
        /*row*/ 1.0f,
        /*pad1*/ 1.0f,
        /*pad2*/ 2.0f,
        /*pad3*/ 3.0f,
        /*radius*/ 0.0f,
        /*border*/ 1.0f,
        /*topBarH*/ 1.0f,
        /*railW*/ 26.0f,
        /*railCollapsedW*/ 7.0f,
        /*inspectorW*/ 28.0f,
        /*nowBarH*/ 1.0f,
        /*composerMinH*/ 4.0f,
        /*composerMaxH*/ 11.0f,
    };
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    check(m.textGrid, "text grid: metricsFor(true, ...) reports the text grid");
    expectMetrics(m, e);
    expectAllWhole(m, "text grid");
    // A rounded corner needs a curve and a cell has none; a radius on the grid
    // would draw a square with a missing cell.
    expect(m.radius, 0.0f, "text grid: the radius is square");
}

void checkGuiMetricsAt100() {
    const Expected e{
        /*what*/ "GUI 100 %",
        /*gu*/ 8.0f,
        /*row*/ 24.0f,
        /*pad1*/ 8.0f,
        /*pad2*/ 12.0f,
        /*pad3*/ 16.0f,
        /*radius*/ 6.0f,
        /*border*/ 1.0f,
        /*topBarH*/ 48.0f,
        /*railW*/ 208.0f,
        /*railCollapsedW*/ 56.0f,
        /*inspectorW*/ 224.0f,
        /*nowBarH*/ 28.0f,
        /*composerMinH*/ 50.0f,
        /*composerMaxH*/ 218.0f,
    };
    const ui::Metrics m = ui::metricsFor(false, 1.0f, 1.0f);
    check(!m.textGrid, "GUI 100 %: metricsFor(false, ...) reports the pixel profile");
    expectMetrics(m, e);
    expectAllWhole(m, "GUI 100 %");
    // The render regions are the design's, not a multiple of gu: the rail is
    // 26 cells wide at 8 px per cell, the now bar is 28 px for a 24 px line.
    expect(m.railW, m.gu * 26.0f, "GUI 100 %: the rail is 26 grid units wide");
    expect(m.inspectorW, m.gu * 28.0f, "GUI 100 %: the inspector is 28 grid units wide");
}

void checkGuiMetricsAt150() {
    const Expected e{
        /*what*/ "GUI 150 %",
        /*gu*/ 12.0f,
        /*row*/ 36.0f,
        /*pad1*/ 12.0f,
        /*pad2*/ 18.0f,
        /*pad3*/ 24.0f,
        /*radius*/ 9.0f,
        /*border*/ 1.0f, // a 1.5 px hairline rounds down: 1 px, never 2
        /*topBarH*/ 72.0f,
        /*railW*/ 312.0f,
        /*railCollapsedW*/ 84.0f,
        /*inspectorW*/ 336.0f,
        /*nowBarH*/ 42.0f,
        /*composerMinH*/ 74.0f,
        /*composerMaxH*/ 326.0f,
    };
    const ui::Metrics m = ui::metricsFor(false, 1.5f, 1.0f);
    expectMetrics(m, e);
    expectAllWhole(m, "GUI 150 %");
    // The composer is the *resolved* anatomy, not the 100 % total times the
    // scale: the two rows grow to 36 px but the hairline does not (1 px), so
    // 2 x 36 + 2 x 1 = 74, where 50 px x 1.5 would answer 75 for a container
    // whose parts add up to 74. Pinned as an identity so a later "simplification"
    // back to one scaled constant fails here.
    expect(m.composerMinH, 2.0f * m.row + 2.0f * m.border,
           "GUI 150 %: the composer is rows + meta row + two hairlines");
    expect(m.composerMaxH, 9.0f * m.row + 2.0f * m.border,
           "GUI 150 %: the full composer is nine rows + two hairlines");
}

// ---------------------------------------------------------------------------
// 5. Scale rules
// ---------------------------------------------------------------------------

void checkScaleRules() {
    // The text grid is measured in cells the terminal chooses: no scale reaches
    // it, which is what makes the cell-exact assertions above hold at any zoom.
    const ui::Metrics plain = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::Metrics scaled = ui::metricsFor(true, 1.5f, 2.0f);
    const Expected same{
        "text grid at 150 %",
        plain.gu,
        plain.row,
        plain.pad1,
        plain.pad2,
        plain.pad3,
        plain.radius,
        plain.border,
        plain.topBarH,
        plain.railW,
        plain.railCollapsedW,
        plain.inspectorW,
        plain.nowBarH,
        plain.composerMinH,
        plain.composerMaxH,
    };
    check(scaled.textGrid, "text grid at 150 %: still the text grid");
    expectMetrics(scaled, same);

    // The two factors multiply, in either order.
    const ui::Metrics font = ui::metricsFor(false, 1.5f, 2.0f);
    const ui::Metrics dpi = ui::metricsFor(false, 2.0f, 1.5f);
    expect(font.gu, dpi.gu, "the font and DPI scales are interchangeable (gu)");
    expect(font.row, dpi.row, "the font and DPI scales are interchangeable (row)");
    expect(font.gu, 24.0f, "150 % * 200 % is 300 %: gu");

    // A factor that cannot be meant (a hand-edited setting, a monitor query that
    // answered 0) is 100 %, never a NaN layout.
    const ui::Metrics hundred = ui::metricsFor(false, 1.0f, 1.0f);
    const float bad[] = {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::infinity()};
    bool sane = true;
    for (const float factor : bad)
        sane = sane && ui::metricsFor(false, factor, factor).gu == hundred.gu;
    check(sane, "a non-positive or non-finite scale falls back to 100 %");

    // Two legitimate factors can still overflow the product, which would round
    // every length to 0 - an inert layout that draws nothing and looks like a
    // crash. The product is capped at 800 % instead, so a hand-edited settings
    // file can never blank the window.
    const ui::Metrics huge = ui::metricsFor(false, 1e20f, 1e20f);
    expect(huge.gu, 8.0f * 8.0f, "the scale product is capped at 800 % (gu)");
    expect(huge.row, 24.0f * 8.0f, "the scale product is capped at 800 % (row)");
    expect(huge.railW, 208.0f * 8.0f, "the scale product is capped at 800 % (rail)");
    // The widest supported scale is still a live layout, never the zero one.
    check(huge.gu > 0.0f && huge.row > 0.0f && huge.railW > 0.0f,
          "an overflowing scale never collapses the layout to zero");
}

// ---------------------------------------------------------------------------
// 6. The live profile
// ---------------------------------------------------------------------------

void checkLiveProfile() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = ImVec2(100.0f, 30.0f);
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontScaleMain = 1.0f;
    style.FontScaleDpi = 1.0f;

    // No backend: the pixel profile, at 100 %.
    ui::Metrics m = ui::metricsFor();
    check(!m.textGrid, "no backend: the live profile is the pixel profile");
    expect(m.gu, 8.0f, "no backend: gu is 8 px");
    expect(m.row, 24.0f, "no backend: row is 24 px");

    // The accessor reads the live scale factors rather than a constant.
    style.FontScaleMain = 1.5f;
    expect(ui::metricsFor().gu, 12.0f, "the font-scale setting reaches the live gu");
    expect(ui::metricsFor().row, 36.0f, "the font-scale setting reaches the live row");
    style.FontScaleMain = 1.0f;
    style.FontScaleDpi = 2.0f;
    expect(ui::metricsFor().gu, 16.0f, "the monitor DPI scale reaches the live gu");
    style.FontScaleDpi = 1.0f;

    // The text backend is what flips the profile - the flag is the backend's,
    // not a setting.
    ImTui_ImplText_Init();
    style.FontScaleMain = 1.5f; // a stale scalar must not reach the grid
    style.FontScaleDpi = 2.0f;
    m = ui::metricsFor();
    check(m.textGrid, "ImTui_ImplText_Init(): the live profile is the text grid");
    expect(m.gu, 1.0f, "text grid: one cell per grid unit whatever the scale says");
    expect(m.row, 1.0f, "text grid: one row per grid unit whatever the scale says");
    expect(m.railCollapsedW, 7.0f, "text grid: the collapsed rail is 7 cells");
    expect(m.inspectorW, 28.0f, "text grid: the inspector is 28 cells");
    ImTui_ImplText_Shutdown();

    style.FontScaleMain = 1.0f;
    style.FontScaleDpi = 1.0f;
    check(!ui::metricsFor().textGrid,
          "ImTui_ImplText_Shutdown(): the live profile is the pixel profile again");

    ImGui::DestroyContext();

    // Without a context the accessor must answer 100 % rather than read a style
    // that does not exist: a harness calls it between CreateContext and the first
    // frame, and ImGui::GetStyle() is not safe there.
    m = ui::metricsFor();
    check(!m.textGrid, "no ImGui context: the pixel profile, read without a style");
    expect(m.gu, 8.0f, "no ImGui context: gu is the unscaled 8 px");
    expect(m.row, 24.0f, "no ImGui context: row is the unscaled 24 px");
}

// ---------------------------------------------------------------------------
// 7. The gate ledger
// ---------------------------------------------------------------------------

struct GateRow {
    int grid = 0;
    int imvec4 = 0;
    int v4 = 0;
    int display = 0;
};

struct Counts {
    int grid = 0;
    int imvec4 = 0;
    int v4 = 0;
    int display = 0;

    bool empty() const { return grid == 0 && imvec4 == 0 && v4 == 0 && display == 0; }
    std::string describe() const {
        return "tuiIsTextGrid=" + std::to_string(grid) + " ImVec4=" + std::to_string(imvec4) +
               " v4=" + std::to_string(v4) + " DisplaySize=" + std::to_string(display);
    }
    bool operator==(const Counts& other) const {
        return grid == other.grid && imvec4 == other.imvec4 && v4 == other.v4 &&
               display == other.display;
    }
};

/// The repository root: the directory that holds `cpp_tui/ui/metrics.h`. The
/// binary runs from `build/tui`, the tree is two levels up; the other candidates
/// let it also run from the repo root or from a nested build directory.
fs::path findRepoRoot() {
    const std::array<const char*, 4> candidates{"", "..", "../..", "../../.."};
    for (const char* candidate : candidates) {
        const fs::path root = candidate[0] == '\0' ? fs::path(".") : fs::path(candidate);
        std::error_code error;
        if (fs::exists(root / "cpp_tui" / "ui" / "metrics.h", error))
            return fs::weakly_canonical(root, error);
    }
    return {};
}

std::string trimmed(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos)
        return {};
    const std::size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

/// The counts of the four tokens in one file, line by line: a line that names the
/// token is a line of gate, so a comment counts too (the token's presence is what
/// the ledger makes visible; a comment that mentions it is a place a later edit
/// will copy from). An unreadable file fails the check instead of counting 0,
/// which would hide every gate in it.
Counts countGates(const fs::path& file) {
    Counts counts;
    std::ifstream in(file);
    check(in.good(), "cannot read " + file.string() + " (a file that cannot be read must not " +
                         "silently count 0 gates)");
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("tuiIsTextGrid") != std::string::npos)
            ++counts.grid;
        if (line.find("ImVec4(") != std::string::npos)
            ++counts.imvec4;
        if (line.find("ChatMessage") != std::string::npos ||
            line.find("RenderGroup") != std::string::npos)
            ++counts.v4;
        if (line.find("DisplaySize") != std::string::npos)
            ++counts.display;
    }
    return counts;
}

/// `tools/gate_ratchet.txt`: `<path> grid=<n> imvec4=<n> v4=<n> display=<n>` per
/// row, `#` starts a comment. Every row needs all four keys; a typo is a hard
/// failure, because a ledger that silently ignores a column would let that column
/// grow.
bool readLedger(const fs::path& file, std::map<std::string, GateRow>& rows) {
    std::ifstream in(file);
    if (!in) {
        check(false, "cannot read " + file.string() + " (the ledger is part of the test)");
        return false;
    }
    std::string raw;
    int lineNumber = 0;
    while (std::getline(in, raw)) {
        ++lineNumber;
        const std::size_t comment = raw.find('#');
        const std::string line =
            trimmed(comment == std::string::npos ? raw : raw.substr(0, comment));
        if (line.empty())
            continue;
        const std::string where =
            file.filename().string() + ":" + std::to_string(lineNumber) + ": ";
        std::vector<std::string> fields;
        std::string field;
        for (const char c : line) {
            if (c == ' ' || c == '\t') {
                if (!field.empty())
                    fields.push_back(field);
                field.clear();
            } else {
                field.push_back(c);
            }
        }
        if (!field.empty())
            fields.push_back(field);
        if (fields.size() < 5) {
            check(false, where + "expected '<path> grid=<n> imvec4=<n> v4=<n> display=<n>'");
            continue;
        }
        GateRow row;
        int seen = 0;
        for (std::size_t i = 1; i < fields.size(); ++i) {
            const std::size_t equals = fields[i].find('=');
            if (equals == std::string::npos) {
                check(false, where + "'" + fields[i] + "' is not key=value");
                continue;
            }
            const std::string key = fields[i].substr(0, equals);
            const std::string value = fields[i].substr(equals + 1);
            char* end = nullptr;
            const long parsed = std::strtol(value.c_str(), &end, 10);
            if (end == value.c_str() || *end != '\0' || parsed < 0) {
                check(false, where + "'" + fields[i] + "' is not a count");
                continue;
            }
            if (key == "grid") {
                row.grid = static_cast<int>(parsed);
                ++seen;
            } else if (key == "imvec4") {
                row.imvec4 = static_cast<int>(parsed);
                ++seen;
            } else if (key == "v4") {
                row.v4 = static_cast<int>(parsed);
                ++seen;
            } else if (key == "display") {
                row.display = static_cast<int>(parsed);
                ++seen;
            } else {
                check(false, where + "unknown key '" + key + "'");
            }
        }
        check(seen == 4, where + "names each of grid/imvec4/v4/display exactly once");
        if (fields[0].rfind("cpp_tui/", 0) != 0) {
            check(false, where + "'" + fields[0] + "' is not a cpp_tui/ path");
            continue;
        }
        check(rows.count(fields[0]) == 0, where + "'" + fields[0] + "' is listed twice");
        rows[fields[0]] = row;
    }
    check(!rows.empty(), "tools/gate_ratchet.txt lists at least one file");
    return true;
}

void checkGateLedger() {
    const fs::path root = findRepoRoot();
    if (root.empty()) {
        check(false, "the repository root (cpp_tui/ui/metrics.h) was not found from the working "
                     "directory - run ./test_metrics from build/tui");
        return;
    }

    std::map<std::string, GateRow> ledger;
    if (!readLedger(root / "tools" / "gate_ratchet.txt", ledger))
        return;

    // The tree as it is: every implementation file under cpp_tui/ that is not a
    // harness. Headers are exempt (they declare), the harnesses are exempt (they
    // construct v4 types on purpose to test them).
    std::map<std::string, Counts> actual;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root / "cpp_tui", error), end; it != end;
         it.increment(error)) {
        if (error) {
            check(false, "cannot walk cpp_tui/: " + error.message());
            return;
        }
        const fs::path& path = it->path();
        if (it->is_directory()) {
            if (path.filename() == "test")
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file())
            continue;
        const std::string extension = path.extension().string();
        if (extension != ".cpp" && extension != ".c")
            continue;
        const std::string name = path.filename().string();
        if (name.rfind("test", 0) == 0 || name.rfind("probe", 0) == 0)
            continue;
        actual[fs::relative(path, root, error).generic_string()] = countGates(path);
    }

    // (a) What the tree contains is what the ledger says.
    for (const auto& entry : actual) {
        const std::string& path = entry.first;
        const Counts& counts = entry.second;
        const auto row = ledger.find(path);
        if (row == ledger.end()) {
            check(counts.empty(), path + " carries a gate (" + counts.describe() +
                                      ") but is not in tools/gate_ratchet.txt");
            continue;
        }
        const Counts recorded{row->second.grid, row->second.imvec4, row->second.v4,
                              row->second.display};
        check(counts == recorded, path + " counts " + counts.describe() +
                                      " but tools/gate_ratchet.txt says " + recorded.describe() +
                                      " - lower the row when a gate retires, never raise it");
    }

    // (b) Every row describes a file that still exists, so a rename cannot leave
    // a stale allowance behind.
    for (const auto& entry : ledger) {
        std::error_code existsError;
        check(fs::exists(root / entry.first, existsError),
              "tools/gate_ratchet.txt lists " + entry.first + ", which does not exist");
        check(actual.count(entry.first) != 0,
              "tools/gate_ratchet.txt lists " + entry.first + ", which the scan did not see");
    }

    // (c) A view never decides the profile and never builds a colour (D-METRICS,
    // D-THEME). Its v4 and display counts are allowed until tasks 17/18/25 move
    // the overlay geometry into the layout engine and re-home the palette, which
    // is why those numbers exist in the ledger at all - they are exact, so a new
    // read in a view fails the row check in (a) either way.
    for (const auto& entry : ledger) {
        if (entry.first.rfind("cpp_tui/views/", 0) != 0)
            continue;
        check(entry.second.grid == 0,
              entry.first +
                  " is a view: geometry comes from ui::metricsFor(), not tuiIsTextGrid()");
        check(entry.second.imvec4 == 0,
              entry.first + " is a view: it draws with ui::color() and never builds an ImVec4");
    }

    // (d) The layer below: `ui/` may not reach up into the shell or the v4 core,
    // which is what lets the metrics module be tested on its own.
    error.clear();
    std::vector<std::string> uiOffenders;
    for (fs::recursive_directory_iterator it(root / "cpp_tui" / "ui", error), end; it != end;
         it.increment(error)) {
        if (error || !it->is_regular_file())
            continue;
        const std::string extension = it->path().extension().string();
        if (extension != ".cpp" && extension != ".h")
            continue;
        std::ifstream in(it->path());
        // An unreadable header would look like a clean one; that is the failure
        // mode a layering check must not have.
        check(in.good(), "cannot read " + it->path().string() + " (the ui/ layering scan)");
        std::string line;
        int lineNumber = 0;
        while (std::getline(in, line)) {
            ++lineNumber;
            if (line.find("#include \"tui.h\"") == std::string::npos &&
                line.find("#include \"views/") == std::string::npos)
                continue;
            uiOffenders.push_back(fs::relative(it->path(), root).generic_string() + ":" +
                                  std::to_string(lineNumber));
        }
    }

    std::string offenders;
    for (const std::string& where : uiOffenders)
        offenders += (offenders.empty() ? "" : ", ") + where;
    check(uiOffenders.empty(),
          "ui/ is the layer below the shell and includes neither tui.h nor a view" +
              (offenders.empty() ? std::string() : " (found: " + offenders + ")"));

    if (g_verbose)
        std::fprintf(stderr, "ok   gate ledger: %zu row(s), %zu scanned file(s), root %s\n",
                     ledger.size(), actual.size(), root.string().c_str());
}

// ---------------------------------------------------------------------------
// 8. The vocabulary
// ---------------------------------------------------------------------------
/// The text of one line with its comment markers removed and its blanks collapsed,
/// so a phrase wrapped onto the next line still reads as one sentence.
std::string proseOf(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size();) {
        if (raw.compare(i, 3, "///") == 0) {
            i += 3;
            continue;
        }
        if (raw.compare(i, 2, "//") == 0 || raw.compare(i, 2, "/*") == 0 ||
            raw.compare(i, 2, "*/") == 0) {
            i += 2;
            continue;
        }
        out.push_back(raw[i] == '\t' ? ' ' : raw[i]);
        ++i;
    }
    // Trimmed and single-spaced: the join below adds the one space a wrap replaced, so
    // `the legacy` + `renderer` reads as the phrase it is.
    std::string collapsed;
    collapsed.reserve(out.size());
    for (const char c : out) {
        if (c == ' ' && (collapsed.empty() || collapsed.back() == ' '))
            continue;
        collapsed.push_back(c);
    }
    while (!collapsed.empty() && collapsed.back() == ' ')
        collapsed.pop_back();
    return collapsed;
}

/// The words a removed thing may not keep.
///
/// `store/` is a v4 *data adapter* and the only UI is the workbench (task 41), so a
/// sentence that names a renderer or a second generation sends the next reader looking
/// for code that does not exist. The vocabulary rotted through two review rounds
/// because it was swept by hand, one phrase at a time; this is the same rule as a
/// check. `kBanned` lists the phrases, and a line may say one only where it names the
/// directory (`store/`) or marks the history (`task 41`).
void checkVocabulary() {
    // Spelled in two pieces each, so this file does not trip its own scan.
    static constexpr const char* kBanned[] = {
        "legacy "
        "renderer",
        "legacy "
        "generation",
        "legacy "
        "Options",
        "workbench "
        "generation",
        "two "
        "generations",
        "both "
        "generations",
    };
    const fs::path root = findRepoRoot();
    if (root.empty()) {
        check(false, "the repository root was not found from the working directory - run "
                     "./test_metrics from build/tui");
        return;
    }

    std::error_code error;
    for (fs::recursive_directory_iterator it(root / "cpp_tui", error), end; it != end;
         it.increment(error)) {
        if (error || !it->is_regular_file())
            continue;
        const std::string extension = it->path().extension().string();
        if (extension != ".cpp" && extension != ".h")
            continue;
        std::ifstream in(it->path());
        // An unreadable file would look like a clean one: the failure mode a wording
        // check must not have.
        check(in.good(), "cannot read " + it->path().string() + " (the vocabulary scan)");
        std::vector<std::string> prose;
        std::string raw;
        while (std::getline(in, raw))
            prose.push_back(proseOf(raw));
        const std::string where = fs::relative(it->path(), root).generic_string();
        for (std::size_t i = 0; i < prose.size(); ++i) {
            // The window, not the line: a phrase wrapped onto the next line is still one
            // sentence to a reader (and the comment markers between them are not words).
            const std::string window =
                prose[i] + " " + (i + 1 < prose.size() ? prose[i + 1] : std::string());
            if (window.find("store/") != std::string::npos ||
                window.find("task 41") != std::string::npos)
                continue;
            for (const char* phrase : kBanned) {
                if (window.find(phrase) == std::string::npos)
                    continue;
                // Report once, on the first line that holds part of the phrase: a hit
                // that is wholly on the next line belongs to that line's own window.
                if (prose[i].find(phrase) == std::string::npos && i + 1 < prose.size() &&
                    prose[i + 1].find(phrase) != std::string::npos)
                    continue;
                check(false, where + ":" + std::to_string(i + 1) + ": '" + phrase +
                                 "' names a renderer that is gone - say 'store/' or mark it "
                                 "as history ('task 41')");
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0) {
            g_verbose = true;
        } else {
            std::fprintf(stderr, "usage: %s [-v]\n", argv[0]);
            return 2;
        }
    }

    checkTypes();
    checkRounding();
    checkTextGridMetrics();
    checkGuiMetricsAt100();
    checkGuiMetricsAt150();
    checkScaleRules();
    checkLiveProfile();
    checkGateLedger();
    checkVocabulary();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d metrics check(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_metrics: all checks passed (grid + pixel profiles, gate ledger)\n");
    return 0;
}
