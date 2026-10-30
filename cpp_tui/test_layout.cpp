// test_layout.cpp
//
// The layout engine (task 17, design §3.3, D-METRICS / D-GEOM / D-OVERLAY).
//
// `computeShell()` is the one function that turns a display size into rectangles,
// so the shell's correctness is a property of *its* output and can be proven
// headless, over thousands of sizes, without a terminal, a display or a frame.
// This binary pins:
//
//   0. the types      the breaks per profile, the overlay minimums, the z-order
//                     names, `inset()` and the unit check;
//   1. the regions    the six tiled rects at the documented reference sizes
//                     (grid 120x36, 100x30, 80x24, 60x20, 40x15; pixel 1280x820,
//                     1000x700, 640x480) - exact numbers, so a moved region is a
//                     failure and not a screenshot;
//   2. tiling         the tiled regions are inside the display, pairwise disjoint,
//                     sum to the display's area and cover every row band exactly -
//                     with the Raw Log drawer open and closed (it is the one
//                     overlay that participates, in the pixel profile);
//   3. minimums       thread >= 3 rows, composer text >= 1 row, now bar = 1 row,
//                     `Load earlier` non-empty and the thread's first row, the
//                     composer reserved before the thread and never covered by the
//                     drawer, and the thread's width floor (a column that does not
//                     fit collapses, then becomes an overlay);
//   4. breakpoints    compact below 60 cells / 640 px (top bar folded, K groups),
//                     the collapsed rail from the grid's 100 cells and the pixel
//                     profile's 900 px, the inspector overlay from 100 cells /
//                     1200 px, and the same display size meaning different things
//                     in the two profiles (the engine reads units, not constants);
//   5. overlays       each open overlay is inside the display and at least its
//                     minimum size, the palette/settings sheets are centred, the
//                     inspector panel is anchored right, and a closed overlay has
//                     no rectangle at all;
//   6. z-order        palette > settings > Raw Log > inspector, resolved in
//                     `ShellLayout::top` (what the Esc chain closes first);
//   7. sweep          every invariant above, over a grid sweep (40..200 cells wide,
//                     15..60 rows), a pixel sweep (640..1920 x 480..1200) at 100 %
//                     and 150 %, and below-floor sizes - with and without the
//                     drawer - aggregated, so one defect prints one line with its
//                     first offenders;
//   8. purity         no ImGui context is created and none is read: the same
//                     arguments answer the same rectangles, a poisoned display
//                     collapses the layout, and an unmetered `Metrics` lays out
//                     nothing;
//   9. the unit stamp every returned rect carries `cells == metrics.textGrid`, the
//                     check that catches mixed-unit arithmetic, and a mismatched
//                     rect is reported (not silently accepted);
//  10. no view reads the display size (`grep -rn "DisplaySize" views/` is empty) -
//                     the reason the palette's overlay geometry moved here.
//  11. focus mode      the chrome hidden, the composer and the now bar kept, and the transcript in
//                     a centred column of at most 90 gu - at the reference sizes and over a sweep,
//                     with the tiling and minimum-content invariants still holding (task 43).
//
// Exit codes: 0 = all checks passed; 1 = at least one failed; 2 = usage error.
//
// Headless (no display, no terminal, no ImGui context). Run from the build dir:
//   ./test_layout [-v]

#include "ui/layout.h"
#include "ui/metrics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
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

std::string rectText(const ui::Rect& r) {
    return "{" + number(r.x) + "," + number(r.y) + " " + number(r.w) + "x" + number(r.h) +
           (r.cells ? " cells" : " px") + "}";
}

void expectRect(const ui::Rect& got, ui::Rect want, const std::string& what) {
    check(got.x == want.x && got.y == want.y && got.w == want.w && got.h == want.h,
          what + ": got " + rectText(got) + ", expected " + rectText(want));
}

// ---------------------------------------------------------------------------
// Independent geometry. The engine ships only what a consumer needs, so the
// properties below are computed here with their own arithmetic - a test that
// grades a module with the module's own ruler proves nothing.
// ---------------------------------------------------------------------------

constexpr float kTol = 1e-3f;

bool hasArea(const ui::Rect& r) { return r.w > 0.0f && r.h > 0.0f; }

bool rectsEqual(const ui::Rect& a, const ui::Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h && a.cells == b.cells;
}

bool layoutsEqual(const ui::ShellLayout& a, const ui::ShellLayout& b) {
    return rectsEqual(a.topBar, b.topBar) && rectsEqual(a.rail, b.rail) &&
           rectsEqual(a.thread, b.thread) && rectsEqual(a.threadColumn, b.threadColumn) &&
           rectsEqual(a.inspector, b.inspector) && rectsEqual(a.composer, b.composer) &&
           rectsEqual(a.nowBar, b.nowBar) && rectsEqual(a.rawLogOverlay, b.rawLogOverlay) &&
           rectsEqual(a.paletteOverlay, b.paletteOverlay) &&
           rectsEqual(a.settingsOverlay, b.settingsOverlay) &&
           rectsEqual(a.loadEarlier, b.loadEarlier) && a.inspectorOverlay == b.inspectorOverlay &&
           a.railOverlay == b.railOverlay && a.rawLogOpen == b.rawLogOpen &&
           a.compact == b.compact && a.compactGroups == b.compactGroups && a.top == b.top;
}

bool overlaps(const ui::Rect& a, const ui::Rect& b) {
    if (!hasArea(a) || !hasArea(b))
        return false;
    return a.x + kTol < b.x + b.w && b.x + kTol < a.x + a.w && a.y + kTol < b.y + b.h &&
           b.y + kTol < a.y + a.h;
}

float area(const ui::Rect& r) { return r.w * r.h; }

bool inside(const ui::Rect& inner, const ui::Rect& outer) {
    return inner.x >= outer.x - kTol && inner.y >= outer.y - kTol &&
           inner.x + inner.w <= outer.x + outer.w + kTol &&
           inner.y + inner.h <= outer.y + outer.h + kTol;
}

/// A failed property, aggregated over a sweep: one line on failure, nothing on
/// success, and the first few offenders inline so the line says *where*.
struct Failures {
    int count = 0;
    std::vector<std::string> examples;

    void note(const std::string& what) {
        ++count;
        if (examples.size() < 4)
            examples.push_back(what);
    }

    void merge(const Failures& other) {
        count += other.count;
        for (const std::string& example : other.examples) {
            if (examples.size() < 4)
                examples.push_back(example);
        }
    }

    void report(const std::string& property) {
        std::string detail;
        for (const std::string& example : examples)
            detail += (detail.empty() ? "" : "; ") + example;
        check(count == 0, property + ": " + std::to_string(count) + " violation(s)" +
                              (detail.empty() ? std::string() : " [" + detail + "]"));
    }
};

std::string sizeText(ImVec2 display, bool textGrid) {
    return std::string(textGrid ? "grid " : "px ") + number(display.x) + "x" + number(display.y);
}

// ---------------------------------------------------------------------------
// Prefs presets
// ---------------------------------------------------------------------------

ui::LayoutPrefs dockedPrefs() {
    ui::LayoutPrefs p;
    p.railOpen = true;
    p.inspectorOpen = true;
    return p;
}

ui::LayoutPrefs barePrefs() { return ui::LayoutPrefs{}; }

ui::LayoutPrefs allOpenPrefs() {
    ui::LayoutPrefs p = dockedPrefs();
    p.paletteOpen = true;
    p.settingsOpen = true;
    p.rawLogOpen = true;
    return p;
}

/// Focus mode (task 43) with both side regions *requested*: the mode hides them, so the same
/// preferences that dock two columns in the ordinary layout must produce a single full-width
/// thread here - that is the check, not a convenience.
ui::LayoutPrefs focusPrefs() {
    ui::LayoutPrefs p = dockedPrefs();
    p.focusMode = true;
    return p;
}

/// The four overlay surfaces of a layout, with their layer id.
std::array<std::pair<ui::OverlayLayer, const ui::Rect*>, 4>
overlaysOf(const ui::ShellLayout& layout) {
    return {{{ui::OverlayLayer::Palette, &layout.paletteOverlay},
             {ui::OverlayLayer::Settings, &layout.settingsOverlay},
             {ui::OverlayLayer::RawLog, &layout.rawLogOverlay},
             {ui::OverlayLayer::Inspector, &layout.inspector}}};
}

bool overlayIsUp(ui::OverlayLayer layer, const ui::ShellLayout& layout,
                 const ui::LayoutPrefs& prefs) {
    switch (layer) {
    case ui::OverlayLayer::Palette:
        return prefs.paletteOpen;
    case ui::OverlayLayer::Settings:
        return prefs.settingsOpen;
    case ui::OverlayLayer::RawLog:
        return prefs.rawLogOpen;
    case ui::OverlayLayer::Inspector:
        return prefs.inspectorOpen && layout.inspectorOverlay;
    case ui::OverlayLayer::Rail:
        return prefs.railOpen && layout.railOverlay;
    case ui::OverlayLayer::None:
        break;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 0. The types
// ---------------------------------------------------------------------------

void checkTypes() {
    // A default layout has no geometry and no decisions: the state a view must
    // not draw as if the tiler had answered.
    const ui::ShellLayout empty;
    for (const ui::Rect* r : {&empty.topBar, &empty.rail, &empty.thread, &empty.inspector,
                              &empty.composer, &empty.nowBar, &empty.rawLogOverlay,
                              &empty.paletteOverlay, &empty.settingsOverlay, &empty.loadEarlier})
        check(!hasArea(*r), "default ShellLayout: a rect encloses no area (" + rectText(*r) + ")");
    check(!empty.inspectorOverlay && !empty.railOverlay && !empty.rawLogOpen && !empty.compact,
          "default ShellLayout: no region is overlaid and nothing is compact");
    check(empty.compactGroups == 0, "default ShellLayout: the group window is 0 (unknown)");
    check(empty.top == ui::OverlayLayer::None, "default ShellLayout: nothing is on top");

    // The breakpoints, in gu units: cells on the grid, 8-px columns in the pixel
    // profile (1200/900/640 px), which is the design's table in one number.
    const ui::Metrics grid = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::Metrics pixel = ui::metricsFor(false, 1.0f, 1.0f);
    const ui::Metrics pixel150 = ui::metricsFor(false, 1.5f, 1.0f);

    const ui::LayoutBreaks gridBreaks = ui::layoutBreaks(grid);
    expect(gridBreaks.wide, 100.0f, "grid breakpoints: wide is 100 cells");
    expect(gridBreaks.medium, 80.0f, "grid breakpoints: medium is 80 cells");
    expect(gridBreaks.narrow, 60.0f, "grid breakpoints: narrow is 60 cells");

    const ui::LayoutBreaks pixelBreaks = ui::layoutBreaks(pixel);
    expect(pixelBreaks.wide, 150.0f, "pixel breakpoints: wide is 1200 px / 8");
    expect(pixelBreaks.medium, 112.5f, "pixel breakpoints: medium is 900 px / 8");
    expect(pixelBreaks.narrow, 80.0f, "pixel breakpoints: narrow is 640 px / 8");

    const ui::LayoutBreaks scaled = ui::layoutBreaks(pixel150);
    check(scaled.wide == pixelBreaks.wide && scaled.medium == pixelBreaks.medium &&
              scaled.narrow == pixelBreaks.narrow,
          "the breakpoints are scale-free (a 150 % font asks for more pixels, not for "
          "different columns)");

    // The overlay minimums (cells/rows on the grid; gu/rows in the pixel profile).
    expect(ui::overlayMinimumW(grid, ui::OverlayLayer::Palette), 20.0f,
           "minimum: the palette is 20 cells wide on the grid");
    expect(ui::overlayMinimumH(grid, ui::OverlayLayer::Palette), 7.0f,
           "minimum: the palette is 7 rows tall on the grid");
    expect(ui::overlayMinimumW(grid, ui::OverlayLayer::Settings), 20.0f,
           "minimum: the settings sheet is 20 cells wide");
    expect(ui::overlayMinimumH(grid, ui::OverlayLayer::Settings), 7.0f,
           "minimum: the settings sheet is 7 rows tall");
    expect(ui::overlayMinimumW(grid, ui::OverlayLayer::RawLog), 20.0f,
           "minimum: the Raw Log is 20 cells wide");
    expect(ui::overlayMinimumH(grid, ui::OverlayLayer::RawLog), 4.0f,
           "minimum: the Raw Log drawer is 4 rows tall");
    expect(ui::overlayMinimumW(grid, ui::OverlayLayer::Inspector), 28.0f,
           "minimum: the inspector overlay is its docked 28 cells");
    expect(ui::overlayMinimumH(grid, ui::OverlayLayer::Inspector), 10.0f,
           "minimum: the inspector overlay is 10 rows tall");
    expect(ui::overlayMinimumW(pixel, ui::OverlayLayer::Inspector), 224.0f,
           "minimum: the inspector overlay is its docked 224 px");
    expect(ui::overlayMinimumH(pixel, ui::OverlayLayer::Inspector), 240.0f,
           "minimum: the inspector overlay is 10 x 24 px tall");
    expect(ui::overlayMinimumW(grid, ui::OverlayLayer::None), 0.0f,
           "minimum: no surface has no minimum width");
    expect(ui::overlayMinimumH(grid, ui::OverlayLayer::None), 0.0f,
           "minimum: no surface has no minimum height");
    expect(ui::overlayMinimumW(grid, ui::OverlayLayer::Rail), 0.0f,
           "minimum: the rail panel keeps its column width and has no minimum of its own");
    expect(ui::overlayMinimumH(grid, ui::OverlayLayer::Rail), 0.0f,
           "minimum: the rail panel has no minimum height");

    // The z-order names (used by the Esc chain's log line and by task 41's tests).
    check(std::strcmp(ui::overlayLayerName(ui::OverlayLayer::Palette), "palette") == 0,
          "z-order: the palette's name");
    check(std::strcmp(ui::overlayLayerName(ui::OverlayLayer::Settings), "settings") == 0,
          "z-order: the settings sheet's name");
    check(std::strcmp(ui::overlayLayerName(ui::OverlayLayer::RawLog), "rawLog") == 0,
          "z-order: the Raw Log's name");
    check(std::strcmp(ui::overlayLayerName(ui::OverlayLayer::Inspector), "inspector") == 0,
          "z-order: the inspector's name");
    check(std::strcmp(ui::overlayLayerName(ui::OverlayLayer::Rail), "rail") == 0,
          "z-order: the rail panel's name");
    check(std::strcmp(ui::overlayLayerName(ui::OverlayLayer::None), "none") == 0,
          "z-order: the empty stack's name");
    // inset(): the border band a hairline is drawn in lives *inside* the rect.
    const ui::Rect outer{10.0f, 5.0f, 20.0f, 10.0f, true};
    const ui::Rect inner = ui::inset(outer, 1.0f);
    expectRect(inner, ui::Rect{11.0f, 6.0f, 18.0f, 8.0f, true}, "inset: one unit inside");
    check(inside(inner, outer), "inset: the content box is inside the region");
    check(inner.cells, "inset: the unit flag survives");
    const ui::Rect squeezed = ui::inset(outer, 40.0f);
    check(squeezed.w == 0.0f && squeezed.h == 0.0f && squeezed.x == 30.0f && squeezed.y == 15.0f,
          "inset: a band wider than the rect collapses to a zero size at the far corner, "
          "inside the source rect (" +
              rectText(squeezed) + ")");
    check(inside(squeezed, outer), "inset: even the collapsed box stays inside the region");

    // The unit check.
    check(ui::rectMatchesProfile(ui::Rect{0, 0, 1, 1, true}, grid),
          "unit check: a cells rect in the grid profile matches");
    check(!ui::rectMatchesProfile(ui::Rect{0, 0, 1, 1, false}, grid),
          "unit check: a pixel rect in the grid profile is rejected");
    check(ui::rectMatchesProfile(ui::Rect{0, 0, 1, 1, false}, pixel),
          "unit check: a pixel rect in the pixel profile matches");
    check(!ui::rectMatchesProfile(ui::Rect{0, 0, 1, 1, true}, pixel),
          "unit check: a cells rect in the pixel profile is rejected");

    // Input hygiene for the one caller-supplied length: a rail width that is not a
    // length is no request (not a zero-width column), and a request narrower than the
    // collapsed rail is floored there - in both shapes (docked and panel).
    const ImVec2 wide(120.0f, 36.0f);
    const ImVec2 narrow(65.0f, 22.0f);
    ui::LayoutPrefs railPrefs;
    railPrefs.railOpen = true;
    railPrefs.railW = std::numeric_limits<float>::infinity();
    expect(ui::computeShell(grid, wide, railPrefs).rail.w, 26.0f,
           "railW: an infinite request falls back to the profile's width");
    railPrefs.railW = std::numeric_limits<float>::quiet_NaN();
    expect(ui::computeShell(grid, wide, railPrefs).rail.w, 26.0f,
           "railW: a NaN request falls back to the profile's width");
    railPrefs.railW = -5.0f;
    expect(ui::computeShell(grid, wide, railPrefs).rail.w, 26.0f,
           "railW: a negative request falls back to the profile's width");
    railPrefs.railW = 3.0f;
    expect(ui::computeShell(grid, wide, railPrefs).rail.w, 7.0f,
           "railW: a request narrower than the collapsed rail is floored there");
    railPrefs.railW = 40.0f;
    expect(ui::computeShell(grid, wide, railPrefs).rail.w, 40.0f,
           "railW: a usable request is honoured");
    expect(ui::computeShell(grid, narrow, railPrefs).rail.w, 40.0f,
           "railW: the panel shape honours the same request");
    railPrefs.railW = 3.0f;
    expect(ui::computeShell(grid, narrow, railPrefs).rail.w, 7.0f,
           "railW: the panel shape floors it the same way");
}

// ---------------------------------------------------------------------------
// Tiling and minimum content: the invariants the sweep reuses
// ---------------------------------------------------------------------------

/// The tiled rects of one layout: the six regions, minus the ones that became
/// overlays or are closed, plus the GUI Raw Log drawer (which is why it exists -
/// it takes rows from the thread and must be accounted for, or the area sum would
/// have a hole where the drawer sits).
std::vector<std::pair<std::string, ui::Rect>>
tiledRects(const ui::ShellLayout& layout, const ui::LayoutPrefs& prefs, const ui::Metrics& m) {
    std::vector<std::pair<std::string, ui::Rect>> rects;
    rects.emplace_back("topBar", layout.topBar);
    if (prefs.railOpen && !layout.railOverlay)
        rects.emplace_back("rail", layout.rail);
    rects.emplace_back("thread", layout.thread);
    if (prefs.inspectorOpen && !layout.inspectorOverlay)
        rects.emplace_back("inspector", layout.inspector);
    if (prefs.rawLogOpen && !m.textGrid)
        rects.emplace_back("rawLogDrawer", layout.rawLogOverlay);
    rects.emplace_back("composer", layout.composer);
    rects.emplace_back("nowBar", layout.nowBar);
    return rects;
}

/// The x spans that cover one [y0, y1) band, sorted: a partition of [0, W).
std::vector<std::pair<float, float>>
bandSpans(const std::vector<std::pair<std::string, ui::Rect>>& rects, float y0, float y1) {
    std::vector<std::pair<float, float>> spans;
    for (const auto& entry : rects) {
        const ui::Rect& r = entry.second;
        if (r.h <= 0.0f)
            continue;
        if (r.y <= y0 + kTol && r.y + r.h >= y1 - kTol)
            spans.emplace_back(r.x, r.x + r.w);
    }
    std::sort(spans.begin(), spans.end());
    return spans;
}

/// Every length the engine returns is whole (`ui/layout.h`: no fraction survives
/// into a `SetNextWindow*` call).
bool isWhole(float v) { return std::fabs(v - std::round(v)) <= 0.0f; }

/// The weak invariants: every rect states its unit, is finite, non-negative and
/// whole, the tiled regions are inside the display, disjoint, sum to its area and
/// cover every row band exactly. These hold at *any* size, floor or not.
void checkTilingCore(const ui::ShellLayout& layout, const ui::LayoutPrefs& prefs,
                     const ui::Metrics& m, ImVec2 display, Failures& failures) {
    const std::string where = sizeText(display, m.textGrid) + (prefs.rawLogOpen ? " +drawer" : "");
    const ui::Rect screen{0.0f, 0.0f, display.x, display.y, m.textGrid};
    const std::vector<std::pair<std::string, ui::Rect>> rects = tiledRects(layout, prefs, m);

    // The whole-unit claim covers the overlays and the Load earlier row too, not just
    // the regions that partition the display.
    std::vector<std::pair<std::string, ui::Rect>> checked = rects;
    checked.emplace_back("rawLogBand", layout.rawLogBand);
    checked.emplace_back("paletteOverlay", layout.paletteOverlay);
    checked.emplace_back("settingsOverlay", layout.settingsOverlay);
    checked.emplace_back("loadEarlier", layout.loadEarlier);
    checked.emplace_back("threadColumn", layout.threadColumn);

    for (const auto& entry : checked) {
        const ui::Rect& r = entry.second;
        if (!ui::rectMatchesProfile(r, m))
            failures.note(where + ": " + entry.first + " does not carry the profile's unit " +
                          rectText(r));
        if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.w) ||
            !std::isfinite(r.h))
            failures.note(where + ": " + entry.first + " holds a non-finite length " + rectText(r));
        if (r.x < 0.0f || r.y < 0.0f || r.w < 0.0f || r.h < 0.0f)
            failures.note(where + ": " + entry.first + " is negative " + rectText(r));
        if (!isWhole(r.x) || !isWhole(r.y) || !isWhole(r.w) || !isWhole(r.h))
            failures.note(where + ": " + entry.first + " is not a whole number of units " +
                          rectText(r));
        if (!inside(r, screen))
            failures.note(where + ": " + entry.first + " leaves the display " + rectText(r));
    }
    float sum = 0.0f;
    for (const auto& entry : rects)
        sum += area(entry.second);
    for (std::size_t i = 0; i < rects.size(); ++i) {
        for (std::size_t j = i + 1; j < rects.size(); ++j) {
            if (overlaps(rects[i].second, rects[j].second))
                failures.note(where + ": " + rects[i].first + " overlaps " + rects[j].first);
        }
    }
    if (std::fabs(sum - area(screen)) > kTol)
        failures.note(where + ": the regions sum to " + number(sum) + " of " +
                      number(area(screen)) + " units");

    // No gap: every horizontal band is covered by exactly the spans that touch it,
    // in order, from 0 to the display's width.
    std::vector<float> edges;
    for (const auto& entry : rects) {
        if (entry.second.h <= 0.0f)
            continue;
        edges.push_back(entry.second.y);
        edges.push_back(entry.second.y + entry.second.h);
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
        const float y0 = edges[i];
        const float y1 = edges[i + 1];
        if (y1 - y0 <= kTol)
            continue;
        const std::vector<std::pair<float, float>> spans = bandSpans(rects, y0, y1);
        if (spans.empty()) {
            failures.note(where + ": the band " + number(y0) + ".." + number(y1) + " is empty");
            continue;
        }
        if (spans.front().first > kTol || spans.back().second < display.x - kTol)
            failures.note(where + ": the band " + number(y0) + ".." + number(y1) +
                          " does not reach both edges");
        for (std::size_t s = 1; s < spans.size(); ++s) {
            if (std::fabs(spans[s].first - spans[s - 1].second) > kTol)
                failures.note(where + ": the band " + number(y0) + ".." + number(y1) +
                              " has a gap at " + number(spans[s].first));
            if (spans[s].first < spans[s - 1].second - kTol)
                failures.note(where + ": the band " + number(y0) + ".." + number(y1) +
                              " has an overlap at " + number(spans[s].first));
        }
    }
}

/// The minimum-content promises (design §3.3). They only hold at and above the
/// documented floor - 40x15 cells, 640x480 px; below it the frame layer shows the
/// error page and the engine owes nothing but well-formed geometry.
void checkMinimumContent(const ui::ShellLayout& layout, const ui::LayoutPrefs& prefs,
                         const ui::Metrics& m, ImVec2 display, Failures& failures) {
    const std::string where = sizeText(display, m.textGrid) + (prefs.rawLogOpen ? " +drawer" : "");
    if (layout.thread.h < 3.0f * m.row - kTol)
        failures.note(where + ": the thread is " + number(layout.thread.h) +
                      " tall, below its three rows");
    if (layout.composer.h < std::min(m.composerMinH, display.y) - kTol)
        failures.note(where + ": the composer is " + number(layout.composer.h) +
                      " tall, below its empty height");
    if (std::fabs(layout.nowBar.h - std::min(m.nowBarH, display.y)) > kTol)
        failures.note(where + ": the now bar is " + number(layout.nowBar.h) + " tall");
    if (layout.loadEarlier.h <= 0.0f)
        failures.note(where + ": the Load earlier row has no height");
    if (!inside(layout.loadEarlier, layout.thread))
        failures.note(where + ": the Load earlier row is not inside the thread");
    if (layout.loadEarlier.y != layout.thread.y)
        failures.note(where + ": the Load earlier row is not the thread's first row");
    // The readable column (task 43): inside the region, the region's own height and first row,
    // and - in focus mode - capped at 90 gu and centred; with the mode off, exactly the region
    // (so a consumer that ignores the field draws what it always drew).
    if (!inside(layout.threadColumn, layout.thread))
        failures.note(where + ": the thread column leaves the thread " +
                      rectText(layout.threadColumn));
    if (layout.threadColumn.y != layout.thread.y || layout.threadColumn.h != layout.thread.h)
        failures.note(where + ": the thread column does not share the thread's rows " +
                      rectText(layout.threadColumn));
    if (prefs.focusMode) {
        const float cap = 90.0f * m.gu;
        if (layout.threadColumn.w > cap + kTol)
            failures.note(where + ": the focus column is " + number(layout.threadColumn.w) +
                          " wide, above its 90-gu cap " + number(cap));
        if (layout.threadColumn.w < std::min(cap, layout.thread.w) - 1.0f - kTol)
            failures.note(where + ": the focus column is " + number(layout.threadColumn.w) +
                          " wide, below the cap the region can afford");
        // Centred to within the half unit a whole-unit layout must round away.
        const float centre = layout.threadColumn.x + layout.threadColumn.w * 0.5f;
        const float want = layout.thread.x + layout.thread.w * 0.5f;
        if (std::fabs(centre - want) > 0.5f + kTol)
            failures.note(where + ": the focus column is not centred (" +
                          number(layout.threadColumn.x) + " in " + number(layout.thread.w) + ")");
        if (layout.topBar.h > 0.0f)
            failures.note(where + ": focus mode still draws the top bar");
        if (hasArea(layout.rail) || hasArea(layout.inspector))
            failures.note(where + ": focus mode still draws a side region");
        if (!hasArea(layout.composer) || !hasArea(layout.nowBar))
            failures.note(where + ": focus mode dropped the composer or the now bar");
    } else if (!rectsEqual(layout.threadColumn, layout.thread)) {
        failures.note(where + ": the thread column is not the thread with the mode off");
    }
    if (prefs.rawLogOpen && !m.textGrid) {
        // The drawer never covers the composer and stays inside the display.
        if (layout.rawLogOverlay.y + layout.rawLogOverlay.h > layout.composer.y + kTol)
            failures.note(where + ": the Raw Log drawer covers the composer");
        if (layout.rawLogOverlay.h <= 0.0f)
            failures.note(where + ": the Raw Log drawer has no height");
    }
    // Every open overlay is inside the display and at least its minimum size, where
    // "its minimum" means the design minimum capped by the band the surface lives in
    // (a minimum is a target, never a promise to overflow the display or to steal the
    // composer's and the thread's rows).
    const ui::Rect screen{0.0f, 0.0f, display.x, display.y, m.textGrid};
    const float panelBandH = std::max(0.0f, layout.nowBar.y - layout.topBar.h);
    for (const auto& overlay : overlaysOf(layout)) {
        const ui::Rect& r = *overlay.second;
        // A docked inspector is a tiled column, not an overlay: its rect was
        // checked with the regions above.
        const bool dockedInspector = overlay.first == ui::OverlayLayer::Inspector &&
                                     prefs.inspectorOpen && !layout.inspectorOverlay;
        if (dockedInspector)
            continue;
        if (!overlayIsUp(overlay.first, layout, prefs)) {
            if (hasArea(r))
                failures.note(where + ": the closed " + ui::overlayLayerName(overlay.first) +
                              " has a rectangle " + rectText(r));
            continue;
        }
        if (!inside(r, screen))
            failures.note(where + ": the " + ui::overlayLayerName(overlay.first) +
                          " overlay leaves the display " + rectText(r));
        // The band each surface lives in: the centred sheets get the display, the two
        // panels the band between the top bar and the now bar, and the drawer what is
        // left above the composer after the composer's and the thread's rows.
        float bandH = display.y;
        if (overlay.first == ui::OverlayLayer::Inspector)
            bandH = panelBandH;
        else if (overlay.first == ui::OverlayLayer::RawLog)
            bandH = m.textGrid ? display.y : layout.rawLogBand.h;
        const float minW = std::min(ui::overlayMinimumW(m, overlay.first), display.x);
        const float minH = std::min(ui::overlayMinimumH(m, overlay.first), bandH);
        if (r.w < minW - kTol || r.h < minH - kTol)
            failures.note(where + ": the " + ui::overlayLayerName(overlay.first) + " overlay is " +
                          rectText(r) + ", below its minimum " + number(minW) + "x" + number(minH) +
                          " (band " + number(bandH) + ")");
    }
}

/// Focus mode (task 43, FR-28) at the reference sizes, with both side regions requested:
/// the top bar, the rail and the inspector are gone, the composer and the now bar are the
/// rows they always were, and the transcript column is capped at 90 gu and centred.
void checkFocusMode() {
    const ui::Metrics grid = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::Metrics pixel = ui::metricsFor(false, 1.0f, 1.0f);

    {
        const ui::ShellLayout l = ui::computeShell(grid, ImVec2(120.0f, 36.0f), focusPrefs());
        expect(l.topBar.h, 0.0f, "focus 120x36: the top bar is hidden");
        expect(l.rail.w, 0.0f, "focus 120x36: the rail is hidden although it was asked for");
        expect(l.inspector.w, 0.0f, "focus 120x36: the inspector is hidden too");
        check(!l.railOverlay && !l.inspectorOverlay,
              "focus 120x36: neither side region claims the panel shape it does not have");
        expectRect(l.thread, ui::Rect{0, 0, 120, 31, true},
                   "focus 120x36: the thread takes the whole width and the bar's row");
        expectRect(l.threadColumn, ui::Rect{15, 0, 90, 31, true},
                   "focus 120x36: the transcript is a centred 90-cell column");
        expectRect(l.loadEarlier, ui::Rect{15, 0, 90, 1, true},
                   "focus 120x36: Load earlier is the column's first row");
        expectRect(l.composer, ui::Rect{0, 31, 120, 4, true},
                   "focus 120x36: the composer keeps its rows");
        expectRect(l.nowBar, ui::Rect{0, 35, 120, 1, true},
                   "focus 120x36: the now bar keeps the last row");
        check(l.top == ui::OverlayLayer::None,
              "focus 120x36: nothing is closeable (a hidden region is not a surface)");
    }
    {
        const ui::ShellLayout l = ui::computeShell(grid, ImVec2(60.0f, 20.0f), focusPrefs());
        expectRect(l.threadColumn, ui::Rect{0, 0, 60, 15, true},
                   "focus 60x20: a display narrower than the cap gets the whole width");
        check(!l.compact, "focus 60x20: 60 cells is still the narrow band, not compact");
        expectRect(l.nowBar, ui::Rect{0, 19, 60, 1, true}, "focus 60x20: the now bar is there");
    }
    {
        // Compact mode and focus mode are independent: at 40x15 the group window is still the
        // compact one, and the column still fits the display it is given.
        const ui::ShellLayout l = ui::computeShell(grid, ImVec2(40.0f, 15.0f), focusPrefs());
        check(l.compact, "focus 40x15: compact mode still applies");
        expect(static_cast<float>(l.compactGroups), 1.0f, "focus 40x15: one group");
        expectRect(l.threadColumn, ui::Rect{0, 0, 40, 10, true},
                   "focus 40x15: the column is the whole width");
        expectRect(l.composer, ui::Rect{0, 10, 40, 4, true}, "focus 40x15: the composer is there");
        expectRect(l.nowBar, ui::Rect{0, 14, 40, 1, true}, "focus 40x15: the now bar is there");
    }
    {
        const ui::ShellLayout l = ui::computeShell(pixel, ImVec2(1280.0f, 820.0f), focusPrefs());
        expectRect(l.threadColumn, ui::Rect{280, 0, 720, 742, false},
                   "focus 1280x820: the transcript is a centred 720-px column");
        expectRect(l.thread, ui::Rect{0, 0, 1280, 742, false},
                   "focus 1280x820: the thread region is the full width");
        expectRect(l.composer, ui::Rect{0, 742, 1280, 50, false},
                   "focus 1280x820: the composer is untouched");
        expectRect(l.nowBar, ui::Rect{0, 792, 1280, 28, false},
                   "focus 1280x820: the now bar is untouched");
        expect(l.topBar.h, 0.0f, "focus 1280x820: the top bar is hidden");
    }
    {
        const ui::ShellLayout l = ui::computeShell(pixel, ImVec2(640.0f, 480.0f), focusPrefs());
        expectRect(l.threadColumn, ui::Rect{0, 0, 640, 402, false},
                   "focus 640x480: the window floor is narrower than the cap, so it is used whole");
    }
    // The GUI drawer is still reserved above the composer in focus mode - the mode hides chrome,
    // it does not change the tiling rules.
    {
        ui::LayoutPrefs p = focusPrefs();
        p.rawLogOpen = true;
        const ui::ShellLayout l = ui::computeShell(pixel, ImVec2(1280.0f, 820.0f), p);
        check(hasArea(l.rawLogOverlay) &&
                  l.rawLogOverlay.y + l.rawLogOverlay.h <= l.composer.y + kTol,
              "focus 1280x820 + drawer: the drawer still sits above the composer");
        check(l.thread.h < 742.0f,
              "focus 1280x820 + drawer: it still takes its rows from the thread");
    }
}

void checkGridReferenceSizes() {
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::LayoutPrefs prefs = dockedPrefs();

    {
        const ui::ShellLayout l = ui::computeShell(m, ImVec2(120.0f, 36.0f), prefs);
        expectRect(l.topBar, ui::Rect{0, 0, 120, 1, true}, "120x36: the top bar is one row");
        expectRect(l.rail, ui::Rect{0, 1, 26, 30, true}, "120x36: the rail is 26 cells");
        expectRect(l.thread, ui::Rect{26, 1, 66, 30, true}, "120x36: the thread is the remainder");
        expectRect(l.inspector, ui::Rect{92, 1, 28, 30, true}, "120x36: the inspector is 28 cells");
        expectRect(l.composer, ui::Rect{0, 31, 120, 4, true},
                   "120x36: the composer is its empty height, above the now bar");
        expectRect(l.nowBar, ui::Rect{0, 35, 120, 1, true}, "120x36: the now bar is the last row");
        expectRect(l.loadEarlier, ui::Rect{26, 1, 66, 1, true},
                   "120x36: Load earlier is the thread's first row");
        check(!l.compact && !l.railOverlay && !l.inspectorOverlay,
              "120x36: every column is docked and nothing is compact");
        expect(static_cast<float>(l.compactGroups), 40.0f,
               "120x36: the grid's bounded window is 40 groups");
    }
    {
        const ui::ShellLayout l = ui::computeShell(m, ImVec2(100.0f, 30.0f), prefs);
        expectRect(l.thread, ui::Rect{26, 1, 46, 24, true}, "100x30: 26 + 46 + 28 columns");
        expectRect(l.inspector, ui::Rect{72, 1, 28, 24, true}, "100x30: the inspector is docked");
        check(!l.railOverlay && !l.inspectorOverlay,
              "100x30: the wide breakpoint is inclusive (100 cells docks both)");
    }
    {
        const ui::ShellLayout l = ui::computeShell(m, ImVec2(80.0f, 24.0f), prefs);
        expectRect(l.rail, ui::Rect{0, 1, 7, 18, true},
                   "80x24: the rail collapses to 7 cells on the grid");
        expectRect(l.thread, ui::Rect{7, 1, 73, 18, true}, "80x24: the thread takes the rest");
        check(l.inspectorOverlay, "80x24: the inspector is an overlay");
        expectRect(l.inspector, ui::Rect{8, 1, 72, 22, true},
                   "80x24: the inspector overlay is 90 % of the width, anchored right");
    }
    {
        const ui::ShellLayout l = ui::computeShell(m, ImVec2(60.0f, 20.0f), prefs);
        check(l.railOverlay, "60x20: the rail is an overlay (single column)");
        expectRect(l.thread, ui::Rect{0, 1, 60, 14, true}, "60x20: the thread is the full width");
        expectRect(l.rail, ui::Rect{0, 1, 26, 18, true},
                   "60x20: the rail panel spans the top bar's bottom to the now bar's top");
        expectRect(l.inspector, ui::Rect{6, 1, 54, 18, true},
                   "60x20: the inspector overlay is 90 % of the width");
        check(!l.compact, "60x20: 60 cells is still the narrow band, not compact");
    }
    {
        const ui::ShellLayout l = ui::computeShell(m, ImVec2(40.0f, 15.0f), prefs);
        check(l.compact, "40x15: compact mode");
        expect(l.topBar.h, 0.0f, "40x15: the top bar folds into the now bar");
        expectRect(l.thread, ui::Rect{0, 0, 40, 10, true}, "40x15: the thread keeps 10 rows");
        expect(static_cast<float>(l.compactGroups), 1.0f, "40x15: one group at 15 rows");
        expectRect(l.composer, ui::Rect{0, 10, 40, 4, true}, "40x15: the composer keeps its rows");
        expectRect(l.nowBar, ui::Rect{0, 14, 40, 1, true}, "40x15: the now bar is the last row");
        expectRect(l.loadEarlier, ui::Rect{0, 0, 40, 1, true},
                   "40x15: Load earlier is the thread's first row");
        expectRect(l.rail, ui::Rect{0, 0, 26, 14, true},
                   "40x15: the rail panel keeps its column width");
        expectRect(l.inspector, ui::Rect{4, 0, 36, 14, true},
                   "40x15: the inspector overlay still fits its 28-cell minimum");
    }
}

void checkPixelReferenceSizes() {
    const ui::Metrics m = ui::metricsFor(false, 1.0f, 1.0f);
    const ui::LayoutPrefs prefs = dockedPrefs();

    {
        const ui::ShellLayout l = ui::computeShell(m, ImVec2(1280.0f, 820.0f), prefs);
        expectRect(l.topBar, ui::Rect{0, 0, 1280, 48, false}, "1280x820: the top bar is 48 px");
        expectRect(l.rail, ui::Rect{0, 48, 208, 694, false}, "1280x820: the rail is 208 px");
        expectRect(l.thread, ui::Rect{208, 48, 848, 694, false}, "1280x820: the thread is 848 px");
        expectRect(l.inspector, ui::Rect{1056, 48, 224, 694, false},
                   "1280x820: the inspector is docked at 224 px");
        expectRect(l.composer, ui::Rect{0, 742, 1280, 50, false},
                   "1280x820: the composer is 50 px above the now bar");
        expectRect(l.nowBar, ui::Rect{0, 792, 1280, 28, false}, "1280x820: the now bar is 28 px");
        check(!l.compact && !l.railOverlay && !l.inspectorOverlay,
              "1280x820: everything is docked");
        expect(static_cast<float>(l.compactGroups), 30.0f,
               "1280x820: the pixel profile's window is 30 groups");
    }
    {
        const ui::ShellLayout l = ui::computeShell(m, ImVec2(1000.0f, 700.0f), prefs);
        expect(l.rail.w, 208.0f, "1000x700: the pixel rail keeps its 208 px down to 900 px");
        check(l.inspectorOverlay, "1000x700: the inspector is an overlay");
        expectRect(l.thread, ui::Rect{208, 48, 792, 574, false},
                   "1000x700: the thread is the full remainder");
        expectRect(l.inspector, ui::Rect{100, 48, 900, 624, false},
                   "1000x700: the inspector overlay is 90 % of the width");
    }
    {
        const ui::ShellLayout l = ui::computeShell(m, ImVec2(640.0f, 480.0f), prefs);
        expect(l.rail.w, 56.0f, "640x480: the pixel rail collapses to 56 px");
        expectRect(l.thread, ui::Rect{56, 48, 584, 354, false},
                   "640x480: the thread keeps the remainder");
        check(l.inspectorOverlay && !l.compact, "640x480: the inspector overlays, no compact");
        expectRect(l.inspector, ui::Rect{64, 48, 576, 404, false},
                   "640x480: the inspector overlay is 576 px");
    }
}

// ---------------------------------------------------------------------------
// 2. The composer, reserved before the thread
// ---------------------------------------------------------------------------

void checkComposerReservation() {
    const ui::Metrics grid = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::Metrics pixel = ui::metricsFor(false, 1.0f, 1.0f);

    ui::LayoutPrefs prefs = barePrefs();
    ui::ShellLayout l = ui::computeShell(grid, ImVec2(120.0f, 36.0f), prefs);
    expect(l.composer.h, 4.0f,
           "composer: an empty composer is 4 cells (2 text + meta + 2 borders)");
    expect(l.thread.h, 30.0f, "composer: the thread gets the rest");

    prefs.composerH = 11.0f;
    l = ui::computeShell(grid, ImVec2(120.0f, 36.0f), prefs);
    expect(l.composer.h, 11.0f, "composer: a full composer is 11 cells");
    expect(l.thread.h, 23.0f, "composer: the thread gives up exactly the grown rows");

    prefs.composerH = 1000.0f;
    l = ui::computeShell(grid, ImVec2(120.0f, 36.0f), prefs);
    expect(l.composer.h, 11.0f, "composer: an absurd height clamps to composerMaxH");

    // The three-row floor wins over a grown composer at the 40x15 floor: the
    // metrics' 11-cell maximum is exactly what is left there.
    prefs.composerH = 11.0f;
    l = ui::computeShell(grid, ImVec2(40.0f, 15.0f), prefs);
    expect(l.composer.h, 11.0f, "composer: at 40x15 the maximum height fits");
    expect(l.thread.h, 3.0f, "composer: and the thread keeps exactly its three rows");

    prefs.composerH = 40.0f;
    l = ui::computeShell(grid, ImVec2(40.0f, 15.0f), prefs);
    expect(l.composer.h, 11.0f, "composer: the 40-cell request clamps to the metric maximum");
    expect(l.thread.h, 3.0f, "composer: the thread still keeps three rows");

    prefs.composerH = 218.0f;
    l = ui::computeShell(pixel, ImVec2(1280.0f, 820.0f), prefs);
    expect(l.composer.h, 218.0f, "composer: the pixel maximum is 218 px");
    expect(l.thread.h, 526.0f, "composer: the pixel thread gives up the grown rows");

    // The thread's three-row floor wins over a grown composer. At the 640x480
    // floor with the Raw Log drawer open there are 260 px between the top bar and
    // the now bar, so a full 218-px composer is cut back to 188 px - exactly what
    // is left once the thread has its three 24-px rows.
    ui::LayoutPrefs drawerPrefs = barePrefs();
    drawerPrefs.rawLogOpen = true;
    drawerPrefs.composerH = 218.0f;
    l = ui::computeShell(pixel, ImVec2(640.0f, 480.0f), drawerPrefs);
    expect(l.composer.h, 188.0f, "composer: the thread's three rows cap a full composer");
    expect(l.thread.h, 72.0f, "composer: the thread keeps exactly its three rows");

    // The composer is reserved before the thread: its rect does not move when the
    // rail and the inspector come and go.
    const ui::ShellLayout bare = ui::computeShell(grid, ImVec2(120.0f, 36.0f), barePrefs());
    const ui::ShellLayout docked = ui::computeShell(grid, ImVec2(120.0f, 36.0f), dockedPrefs());
    expectRect(docked.composer, bare.composer, "composer: identical with and without the docks");
    expectRect(docked.nowBar, bare.nowBar, "now bar: identical with and without the docks");
    expectRect(docked.topBar, bare.topBar, "top bar: identical with and without the docks");
}

// ---------------------------------------------------------------------------
// 3. Breakpoints and compact mode
// ---------------------------------------------------------------------------

void checkBreakpoints() {
    const ui::Metrics grid = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::Metrics pixel = ui::metricsFor(false, 1.0f, 1.0f);
    const ui::Metrics pixel150 = ui::metricsFor(false, 1.5f, 1.0f);
    const ui::LayoutPrefs prefs = dockedPrefs();

    // Compact: strictly below the narrow boundary, on both sides of it.
    check(!ui::computeShell(grid, ImVec2(60.0f, 20.0f), prefs).compact,
          "compact: 60 cells is not compact");
    check(ui::computeShell(grid, ImVec2(59.0f, 20.0f), prefs).compact,
          "compact: 59 cells is compact");

    // The group window: 1 at <= 20 rows, 3 above it (D-BOUNDS).
    expect(static_cast<float>(ui::computeShell(grid, ImVec2(50.0f, 20.0f), prefs).compactGroups),
           1.0f, "compact groups: 1 at exactly 20 rows");
    expect(static_cast<float>(ui::computeShell(grid, ImVec2(50.0f, 21.0f), prefs).compactGroups),
           3.0f, "compact groups: 3 at 21 rows");
    expect(static_cast<float>(ui::computeShell(grid, ImVec2(50.0f, 40.0f), prefs).compactGroups),
           3.0f, "compact groups: 3 on a tall narrow terminal");
    expect(static_cast<float>(ui::computeShell(grid, ImVec2(120.0f, 36.0f), prefs).compactGroups),
           40.0f, "compact groups: the ordinary grid window is 40");
    expect(
        static_cast<float>(ui::computeShell(pixel, ImVec2(1280.0f, 820.0f), prefs).compactGroups),
        30.0f, "compact groups: the ordinary pixel window is 30");

    // Compact gives the top bar's row to the thread.
    const ui::ShellLayout compact = ui::computeShell(grid, ImVec2(40.0f, 15.0f), prefs);
    expect(compact.topBar.h, 0.0f, "compact: the top bar is folded");
    expect(compact.thread.h, 10.0f, "compact: 15 rows - now bar 1 - composer 4 = 10 thread rows");

    // The collapsed rail boundaries differ per profile, exactly as the design's
    // table says: the grid collapses at 100 cells, the pixel profile at 900 px.
    expect(ui::computeShell(grid, ImVec2(99.0f, 30.0f), prefs).rail.w, 7.0f,
           "rail: collapsed below 100 grid cells");
    expect(ui::computeShell(grid, ImVec2(100.0f, 30.0f), prefs).rail.w, 26.0f,
           "rail: full at 100 grid cells");
    expect(ui::computeShell(pixel, ImVec2(1000.0f, 700.0f), prefs).rail.w, 208.0f,
           "rail: full at 1000 px (the pixel profile collapses at 900)");
    expect(ui::computeShell(pixel, ImVec2(904.0f, 700.0f), prefs).rail.w, 208.0f,
           "rail: full at 904 px");
    expect(ui::computeShell(pixel, ImVec2(896.0f, 700.0f), prefs).rail.w, 56.0f,
           "rail: collapsed below 900 px");

    // The inspector's overlay boundary: 100 cells / 1200 px.
    check(!ui::computeShell(grid, ImVec2(100.0f, 30.0f), prefs).inspectorOverlay,
          "inspector: docked at 100 grid cells");
    check(ui::computeShell(grid, ImVec2(99.0f, 30.0f), prefs).inspectorOverlay,
          "inspector: overlay at 99 grid cells");
    check(!ui::computeShell(pixel, ImVec2(1200.0f, 820.0f), prefs).inspectorOverlay,
          "inspector: docked at 1200 px");
    check(ui::computeShell(pixel, ImVec2(1192.0f, 820.0f), prefs).inspectorOverlay,
          "inspector: overlay at 1192 px");

    // The same display size is two different layouts in the two profiles: the
    // engine reads units, never a magic number.
    const ui::ShellLayout asCells = ui::computeShell(grid, ImVec2(100.0f, 30.0f), prefs);
    const ui::ShellLayout asPixels = ui::computeShell(pixel, ImVec2(100.0f, 30.0f), prefs);
    check(!asCells.compact, "units: 100x30 cells is wide enough");
    check(!asPixels.compact, "units: 100x30 pixels is not compact (compact is a text-grid state)");

    // A 150 % font asks for twice the pixels: 1280 px is then 106 columns, i.e.
    // the collapsed rail and an inspector overlay - the same intent at another
    // scale rather than a silently different layout.
    const ui::ShellLayout scaled = ui::computeShell(pixel150, ImVec2(1280.0f, 820.0f), prefs);
    expect(scaled.rail.w, 84.0f, "scale: a 150 % rail is 56 px x 1.5 collapsed");
    check(scaled.inspectorOverlay, "scale: a 150 % window overlays the inspector");
    check(!scaled.compact, "scale: 106 columns is not compact");

    // The pixel profile never folds its top bar and never narrows the group window,
    // at any scale: 640x480 is the documented window minimum, and at 150 %/200 % it
    // is a small window with big text, not a "compact" layout.
    const ui::ShellLayout scaled640 = ui::computeShell(pixel150, ImVec2(640.0f, 480.0f), prefs);
    check(!scaled640.compact, "scale: 640 px at 150 % is a small window, not compact mode");
    check(!scaled640.railOverlay, "scale: the pixel rail stays a column at 150 %");
    expect(scaled640.rail.w, 84.0f, "scale: the collapsed pixel rail scales to 84 px");
    expect(scaled640.topBar.h, 72.0f, "scale: the pixel top bar keeps its rows at 150 %");
    expect(static_cast<float>(scaled640.compactGroups), 30.0f,
           "scale: the pixel group window stays 30 at 150 %");
    check(scaled640.inspectorOverlay, "scale: the inspector overlays at 150 %");

    const ui::Metrics pixel200 = ui::metricsFor(false, 2.0f, 1.0f);
    const ui::ShellLayout huge = ui::computeShell(pixel200, ImVec2(640.0f, 480.0f), prefs);
    check(!huge.compact, "scale: 640 px at 200 % is still not compact");
    expect(static_cast<float>(huge.compactGroups), 30.0f,
           "scale: the pixel group window stays 30 at 200 %");
    expect(huge.topBar.h, 96.0f, "scale: the pixel top bar keeps its rows at 200 %");
}

// ---------------------------------------------------------------------------
// 4. The Raw Log (the one overlay that changes the tiling)
// ---------------------------------------------------------------------------

void checkRawLog() {
    const ui::Metrics grid = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::Metrics pixel = ui::metricsFor(false, 1.0f, 1.0f);

    ui::LayoutPrefs prefs = dockedPrefs();
    prefs.rawLogOpen = true;

    // Grid: a full-screen overlay that does not take a single row from the thread.
    const ui::ShellLayout gridOpen = ui::computeShell(grid, ImVec2(120.0f, 36.0f), prefs);
    expectRect(gridOpen.rawLogOverlay, ui::Rect{0, 0, 120, 36, true},
               "raw log (grid): the overlay is the whole display");
    expect(gridOpen.thread.h, 30.0f, "raw log (grid): the thread keeps its rows");
    check(gridOpen.rawLogOpen, "raw log (grid): the flag is echoed");
    const ui::ShellLayout gridClosed = ui::computeShell(grid, ImVec2(120.0f, 36.0f), dockedPrefs());
    check(rectsEqual(gridClosed.thread, gridOpen.thread),
          "raw log (grid): opening it does not move the regions");
    check(!hasArea(gridClosed.rawLogOverlay), "raw log (grid): closed means no rectangle");

    // Pixel: a drawer above the composer, which shrinks the thread and never
    // covers the composer.
    const ui::ShellLayout pixelOpen = ui::computeShell(pixel, ImVec2(1280.0f, 820.0f), prefs);
    expectRect(pixelOpen.rawLogOverlay, ui::Rect{0, 496, 1280, 246, false},
               "raw log (px): the drawer is 30 % of 820 px, above the composer");
    expect(pixelOpen.rawLogOverlay.y + pixelOpen.rawLogOverlay.h, pixelOpen.composer.y,
           "raw log (px): the drawer ends exactly where the composer starts");
    expect(pixelOpen.thread.h, 448.0f, "raw log (px): the thread gives up the drawer's rows");
    const ui::ShellLayout pixelClosed =
        ui::computeShell(pixel, ImVec2(1280.0f, 820.0f), dockedPrefs());
    expect(pixelClosed.rawLogOverlay.h, 0.0f, "raw log (px): closed means no rectangle");
    expect(pixelClosed.thread.h - pixelOpen.thread.h, 246.0f,
           "raw log (px): the thread loses exactly the drawer's height");

    // The drawer's band: the room above the composer once the composer's empty
    // height and the thread's three rows are reserved (task 33 resizes inside it).
    expectRect(pixelOpen.rawLogBand, ui::Rect{0, 48, 1280, 622, false},
               "raw log (px): the band is 820 - 48 - 28 - 50 - 72 px");
    expectRect(gridOpen.rawLogBand, ui::Rect{0, 0, 120, 36, true},
               "raw log (grid): the band is the display (the overlay is full-screen)");

    // At a 200 % scale in the smallest window the band is smaller than the drawer's
    // four-row minimum, so the drawer takes the whole band instead of stealing the
    // thread's rows - the minimum is a target, and the band is the truth.
    const ui::Metrics pixel200 = ui::metricsFor(false, 2.0f, 1.0f);
    const ui::ShellLayout huge = ui::computeShell(pixel200, ImVec2(640.0f, 480.0f), prefs);
    expect(huge.rawLogBand.h, 84.0f,
           "raw log (px 200 %): the band is 328 - 100 - 144 px, below the four-row minimum");
    expect(huge.rawLogOverlay.h, 84.0f, "raw log (px 200 %): the drawer takes the whole band");
    expect(huge.thread.h, 144.0f, "raw log (px 200 %): the thread keeps its three rows");
    expect(huge.composer.h, 100.0f, "raw log (px 200 %): the composer keeps its empty height");
}

// ---------------------------------------------------------------------------
// 5. Overlay boxes
// ---------------------------------------------------------------------------

void checkOverlays() {
    const ui::Metrics grid = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::Metrics pixel = ui::metricsFor(false, 1.0f, 1.0f);

    ui::LayoutPrefs prefs;
    prefs.paletteOpen = true;
    expectRect(ui::computeShell(grid, ImVec2(80.0f, 24.0f), prefs).paletteOverlay,
               ui::Rect{8, 5, 64, 14, true}, "palette (grid 80x24): 80 % x 60 %, centred");
    expectRect(ui::computeShell(grid, ImVec2(40.0f, 15.0f), prefs).paletteOverlay,
               ui::Rect{4, 3, 32, 9, true}, "palette (grid 40x15): still inside the display");
    expectRect(ui::computeShell(pixel, ImVec2(1280.0f, 820.0f), prefs).paletteOverlay,
               ui::Rect{320, 0, 640, 820, false}, "palette (px 1280x820): 640 px, centred");
    expectRect(ui::computeShell(pixel, ImVec2(1000.0f, 700.0f), prefs).paletteOverlay,
               ui::Rect{180, 0, 640, 700, false}, "palette (px 1000x700): 640 px, centred");
    expectRect(ui::computeShell(pixel, ImVec2(640.0f, 480.0f), prefs).paletteOverlay,
               ui::Rect{96, 0, 448, 480, false}, "palette (px 640x480): 70 % of a narrow window");

    prefs = ui::LayoutPrefs{};
    prefs.settingsOpen = true;
    expectRect(ui::computeShell(grid, ImVec2(120.0f, 36.0f), prefs).settingsOverlay,
               ui::Rect{24, 9, 72, 18, true}, "settings (grid 120x36): 60 % x 50 %, centred");
    expectRect(ui::computeShell(pixel, ImVec2(1280.0f, 820.0f), prefs).settingsOverlay,
               ui::Rect{380, 110, 520, 600, false},
               "settings (px 1280x820): 520 x 600 px, centred");
    expectRect(ui::computeShell(pixel, ImVec2(640.0f, 480.0f), prefs).settingsOverlay,
               ui::Rect{64, 48, 512, 384, false}, "settings (px 640x480): 80 % of the window");

    // A closed overlay has no geometry, so a view cannot draw a stale box.
    const ui::ShellLayout closed = ui::computeShell(grid, ImVec2(120.0f, 36.0f), ui::LayoutPrefs{});
    check(!hasArea(closed.paletteOverlay) && !hasArea(closed.settingsOverlay) &&
              !hasArea(closed.rawLogOverlay),
          "overlays: closed surfaces have no rectangle");

    // The centred boxes really are centred (within the rounding of a half unit).
    const ui::ShellLayout centred = ui::computeShell(grid, ImVec2(101.0f, 37.0f), prefs);
    const ui::Rect& r = centred.settingsOverlay;
    check(std::fabs((101.0f - r.w) * 0.5f - r.x) <= 0.5f &&
              std::fabs((37.0f - r.h) * 0.5f - r.y) <= 0.5f,
          "overlays: the settings sheet is centred on an odd display (" + rectText(r) + ")");

    // The inspector overlay is anchored to the right edge and keeps the docked
    // width as its floor.
    const ui::LayoutPrefs inspector = dockedPrefs();
    const ui::ShellLayout l = ui::computeShell(grid, ImVec2(80.0f, 24.0f), inspector);
    expect(l.inspector.x + l.inspector.w, 80.0f, "inspector overlay: anchored to the right edge");
    expect(l.inspector.w, 72.0f, "inspector overlay: 90 % of the width");
    const ui::ShellLayout wide = ui::computeShell(grid, ImVec2(120.0f, 36.0f), inspector);
    expectRect(wide.inspector, ui::Rect{92, 1, 28, 30, true},
               "inspector: docked at the wide breakpoint (no overlay rect)");

    // The minimum is a target, and a font scale can make the natural size too small
    // for it: at 300 % in the smallest window the palette and the sheet grow to their
    // minimum (clamped by the display) instead of shrinking below it.
    ui::LayoutPrefs sheets;
    sheets.paletteOpen = true;
    sheets.settingsOpen = true;
    const ui::Metrics pixel300 = ui::metricsFor(false, 3.0f, 1.0f);
    const ui::ShellLayout huge = ui::computeShell(pixel300, ImVec2(640.0f, 480.0f), sheets);
    expectRect(huge.paletteOverlay, ui::Rect{80, 0, 480, 480, false},
               "palette (px 300 %): 70 % of the width is below its 480-px minimum");
    expectRect(huge.settingsOverlay, ui::Rect{64, 0, 512, 480, false},
               "settings (px 300 %): the sheet grows to its 504-px minimum (capped by the "
               "display)");
}

void checkZOrder() {
    const ui::Metrics grid = ui::metricsFor(true, 1.0f, 1.0f);
    const ImVec2 display(120.0f, 36.0f);

    const auto top = [&](const ui::LayoutPrefs& prefs) {
        return ui::computeShell(grid, display, prefs).top;
    };

    ui::LayoutPrefs p = allOpenPrefs();
    check(top(p) == ui::OverlayLayer::Palette, "z-order: the palette is on top");
    p.paletteOpen = false;
    check(top(p) == ui::OverlayLayer::Settings, "z-order: settings above the Raw Log");
    p.settingsOpen = false;
    check(top(p) == ui::OverlayLayer::RawLog, "z-order: the Raw Log above the inspector");
    p.rawLogOpen = false;
    check(top(p) == ui::OverlayLayer::None,
          "z-order: a docked inspector is a column, not an overlay");
    check(top(ui::LayoutPrefs{}) == ui::OverlayLayer::None,
          "z-order: nothing open, nothing on top");

    // The inspector, once it is an overlay, is the bottom of the stack.
    const ImVec2 narrowDisplay(80.0f, 24.0f);
    const auto narrowTop = [&](const ui::LayoutPrefs& prefs) {
        return ui::computeShell(grid, narrowDisplay, prefs).top;
    };
    ui::LayoutPrefs narrow;
    narrow.inspectorOpen = true;
    check(narrowTop(narrow) == ui::OverlayLayer::Inspector,
          "z-order: a narrow inspector overlay is closeable");
    narrow.rawLogOpen = true;
    check(narrowTop(narrow) == ui::OverlayLayer::RawLog,
          "z-order: the Raw Log covers the inspector");
    narrow.settingsOpen = true;
    check(narrowTop(narrow) == ui::OverlayLayer::Settings, "z-order: settings cover the Raw Log");

    // The rail panel is in the stack too: a narrow single-column layout turns the rail
    // into a panel, so the Esc chain must be able to close it - and it sits *below* the
    // inspector panel, which is the one that wins when both are up.
    ui::LayoutPrefs railOnly;
    railOnly.railOpen = true;
    const ui::ShellLayout railPanel = ui::computeShell(grid, ImVec2(65.0f, 22.0f), railOnly);
    check(railPanel.railOverlay, "z-order: 65 cells puts the rail in a panel");
    check(railPanel.top == ui::OverlayLayer::Rail,
          "z-order: the rail panel is the closeable overlay when it is alone");
    ui::LayoutPrefs both = railOnly;
    both.inspectorOpen = true;
    const ui::ShellLayout twoPanels = ui::computeShell(grid, ImVec2(65.0f, 22.0f), both);
    check(twoPanels.railOverlay && twoPanels.inspectorOverlay,
          "z-order: 65 cells puts both regions in panels");
    check(twoPanels.top == ui::OverlayLayer::Inspector,
          "z-order: the inspector panel is above the rail panel");
    ui::LayoutPrefs railRaw = both;
    railRaw.rawLogOpen = true;
    check(ui::computeShell(grid, ImVec2(65.0f, 22.0f), railRaw).top == ui::OverlayLayer::RawLog,
          "z-order: the Raw Log covers both panels");

    // A ``top`` that names a surface with no geometry would make the Esc chain close
    // nothing. Below the pixel floor the drawer is squeezed to zero height, so the
    // Raw Log cannot be the answer there.
    const ui::Metrics pixel = ui::metricsFor(false, 1.0f, 1.0f);
    ui::LayoutPrefs squeezed;
    squeezed.rawLogOpen = true;
    const ui::ShellLayout noRoom = ui::computeShell(pixel, ImVec2(320.0f, 120.0f), squeezed);
    check(!hasArea(noRoom.rawLogOverlay), "z-order: the drawer has no room at 320x120");
    check(noRoom.top == ui::OverlayLayer::None,
          "z-order: an overlay with no rectangle is not the topmost one");

    // The enum's numeric order is the documented order, so a later switch over it
    // cannot disagree with the list.
    check(static_cast<int>(ui::OverlayLayer::Palette) >
                  static_cast<int>(ui::OverlayLayer::Settings) &&
              static_cast<int>(ui::OverlayLayer::Settings) >
                  static_cast<int>(ui::OverlayLayer::RawLog) &&
              static_cast<int>(ui::OverlayLayer::RawLog) >
                  static_cast<int>(ui::OverlayLayer::Inspector) &&
              static_cast<int>(ui::OverlayLayer::Inspector) >
                  static_cast<int>(ui::OverlayLayer::Rail) &&
              static_cast<int>(ui::OverlayLayer::Rail) > static_cast<int>(ui::OverlayLayer::None),
          "z-order: palette > settings > Raw Log > inspector > rail > none");
}

// ---------------------------------------------------------------------------
// 7. Reference tiling and the sweep
// ---------------------------------------------------------------------------

struct RefCase {
    const char* name;
    float w, h;
};

void checkReferenceTiling() {
    const std::array<RefCase, 5> gridCases{
        RefCase{"120x36", 120.0f, 36.0f}, RefCase{"100x30", 100.0f, 30.0f},
        RefCase{"80x24", 80.0f, 24.0f}, RefCase{"60x20", 60.0f, 20.0f},
        RefCase{"40x15", 40.0f, 15.0f}};
    const std::array<RefCase, 3> pixelCases{RefCase{"1280x820", 1280.0f, 820.0f},
                                            RefCase{"1000x700", 1000.0f, 700.0f},
                                            RefCase{"640x480", 640.0f, 480.0f}};

    for (int profile = 0; profile < 2; ++profile) {
        const bool textGrid = profile == 0;
        const ui::Metrics m = ui::metricsFor(textGrid, 1.0f, 1.0f);
        // One case list per profile: the grid and the pixel profile have their own
        // reference sizes, and the index is bounded by the profile's own count.
        const int caseCount =
            textGrid ? static_cast<int>(gridCases.size()) : static_cast<int>(pixelCases.size());
        for (int rawLog = 0; rawLog < 2; ++rawLog) {
            for (int focus = 0; focus < 2; ++focus) {
                ui::LayoutPrefs prefs = dockedPrefs();
                prefs.rawLogOpen = rawLog != 0;
                // Task 43: the same invariants with the chrome hidden - the mode may not tile a
                // display worse than the ordinary layout does.
                prefs.focusMode = focus != 0;
                for (int index = 0; index < caseCount; ++index) {
                    const RefCase& c = textGrid ? gridCases[static_cast<std::size_t>(index)]
                                                : pixelCases[static_cast<std::size_t>(index)];
                    const ImVec2 display(c.w, c.h);
                    const ui::ShellLayout l = ui::computeShell(m, display, prefs);
                    Failures failures;
                    checkTilingCore(l, prefs, m, display, failures);
                    checkMinimumContent(l, prefs, m, display, failures);
                    failures.report(
                        std::string("tiling at ") + c.name + (focus ? " in focus mode" : "") +
                        (rawLog ? " with the Raw Log open" : " with the Raw Log closed"));
                }
            }
        }
    }
}

void checkSweep() {
    Failures coreFailures;    // every size, floor or not
    Failures contentFailures; // the minimum-content promises, supported sizes only
    Failures bareFailures;    // the same with every region closed
    int cases = 0;

    const auto sweepOne = [&](const ui::Metrics& m, ImVec2 display, const ui::LayoutPrefs& prefs,
                              bool supported) {
        ++cases;
        const ui::ShellLayout l = ui::computeShell(m, display, prefs);
        Failures core;
        checkTilingCore(l, prefs, m, display, core);
        coreFailures.merge(core);
        if (!supported)
            return;
        Failures content;
        checkMinimumContent(l, prefs, m, display, content);
        contentFailures.merge(content);
    };

    for (int rawLog = 0; rawLog < 2; ++rawLog) {
        ui::LayoutPrefs prefs = dockedPrefs();
        prefs.rawLogOpen = rawLog != 0;

        // Grid sweep: every row height and a wide range of widths.
        const ui::Metrics grid = ui::metricsFor(true, 1.0f, 1.0f);
        for (float w = 40.0f; w <= 200.0f; w += 3.0f) {
            for (float h = 15.0f; h <= 60.0f; h += 1.0f)
                sweepOne(grid, ImVec2(w, h), prefs, true);
        }
        // Pixel sweep at 100 %, 150 % and 200 %: the scale changes every length, and
        // the drawer must keep its promises at each - including the extremes, where a
        // row-based minimum has to yield to the band it lives in.
        for (int scalePass = 0; scalePass < 3; ++scalePass) {
            const float scale = scalePass == 0 ? 1.0f : (scalePass == 1 ? 1.5f : 2.0f);
            const ui::Metrics pixel = ui::metricsFor(false, scale, 1.0f);
            for (float w = 640.0f; w <= 1920.0f; w += 16.0f) {
                for (float h = 480.0f; h <= 1200.0f; h += 8.0f)
                    sweepOne(pixel, ImVec2(w, h), prefs, true);
            }
        }
    }

    // A closed-region sweep (once, not per Raw Log value): the layout must be total
    // with nothing docked.
    const ui::Metrics grid = ui::metricsFor(true, 1.0f, 1.0f);
    for (float w = 40.0f; w <= 200.0f; w += 7.0f) {
        for (float h = 15.0f; h <= 60.0f; h += 5.0f) {
            ++cases;
            const ImVec2 display(w, h);
            const ui::ShellLayout l = ui::computeShell(grid, display, barePrefs());
            Failures core;
            checkTilingCore(l, barePrefs(), grid, display, core);
            coreFailures.merge(core);
            Failures content;
            checkMinimumContent(l, barePrefs(), grid, display, content);
            bareFailures.merge(content);
        }
    }

    // Below the floor the engine still owes non-negative, whole, disjoint, exactly
    // covering geometry: the error page is the frame layer's business, but a poisoned
    // layout must not come out of here. Both profiles, because the drawer's clamp
    // (the one overlay that can be squeezed to nothing) only exists in the pixel one.
    for (float w = 1.0f; w < 40.0f; w += 1.0f) {
        for (float h = 1.0f; h < 15.0f; h += 1.0f)
            sweepOne(grid, ImVec2(w, h), dockedPrefs(), false);
    }
    const ui::Metrics pixel = ui::metricsFor(false, 1.0f, 1.0f);
    for (float w = 8.0f; w < 640.0f; w += 24.0f) {
        for (float h = 8.0f; h < 480.0f; h += 24.0f) {
            sweepOne(pixel, ImVec2(w, h), dockedPrefs(), false);
            ui::LayoutPrefs drawer = dockedPrefs();
            drawer.rawLogOpen = true;
            sweepOne(pixel, ImVec2(w, h), drawer, false);
        }
    }

    // Focus mode's own sweep (task 43): one branch of the vertical composition plus one column, so
    // a coarser step than the ordinary sweep is honest coverage rather than a second full pass -
    // the invariants it has to keep are the same ones.
    const ui::LayoutPrefs focused = focusPrefs();
    for (float w = 40.0f; w <= 200.0f; w += 9.0f) {
        for (float h = 15.0f; h <= 60.0f; h += 3.0f)
            sweepOne(grid, ImVec2(w, h), focused, true);
    }
    for (int scalePass = 0; scalePass < 3; ++scalePass) {
        const float scale = scalePass == 0 ? 1.0f : (scalePass == 1 ? 1.5f : 2.0f);
        const ui::Metrics scaled = ui::metricsFor(false, scale, 1.0f);
        for (float w = 640.0f; w <= 1920.0f; w += 64.0f) {
            for (float h = 480.0f; h <= 1200.0f; h += 48.0f)
                sweepOne(scaled, ImVec2(w, h), focused, true);
        }
    }

    if (g_verbose)
        std::fprintf(stderr, "ok   sweep: %d layout(s)\n", cases);
    coreFailures.report("sweep: every layout tiles its display exactly");
    contentFailures.report("sweep: the minimum-content invariants");
    bareFailures.report("sweep: the minimum-content invariants with nothing docked");
}

/// The same invariants with **every** surface open, swept: this is where the overlay
/// boxes and their minimums get their coverage (the reference cases pin a handful of
/// sizes; this walks the ranges).
void checkOverlaySweep() {
    Failures failures;
    int cases = 0;

    const auto sweepOne = [&](const ui::Metrics& m, ImVec2 display) {
        ++cases;
        const ui::LayoutPrefs prefs = allOpenPrefs();
        const ui::ShellLayout l = ui::computeShell(m, display, prefs);
        Failures all;
        checkTilingCore(l, prefs, m, display, all);
        checkMinimumContent(l, prefs, m, display, all);
        failures.merge(all);
    };

    const ui::Metrics grid = ui::metricsFor(true, 1.0f, 1.0f);
    for (float w = 40.0f; w <= 200.0f; w += 5.0f) {
        for (float h = 15.0f; h <= 60.0f; h += 3.0f)
            sweepOne(grid, ImVec2(w, h));
    }
    for (int scalePass = 0; scalePass < 3; ++scalePass) {
        const float scale = scalePass == 0 ? 1.0f : (scalePass == 1 ? 1.5f : 2.0f);
        const ui::Metrics pixel = ui::metricsFor(false, scale, 1.0f);
        for (float w = 640.0f; w <= 1920.0f; w += 32.0f) {
            for (float h = 480.0f; h <= 1200.0f; h += 24.0f)
                sweepOne(pixel, ImVec2(w, h));
        }
    }

    if (g_verbose)
        std::fprintf(stderr, "ok   overlay sweep: %d layout(s)\n", cases);
    failures.report("overlay sweep: every surface open still tiles, and every overlay is "
                    "inside the display and above the minimum its band affords");
}
// ---------------------------------------------------------------------------
// 8. Purity
// ---------------------------------------------------------------------------

void checkPurity() {
    // The engine reads no ImGui state at all: this harness never creates a context,
    // so any read would be an assert or a crash - and the answers below are still
    // complete.
    check(ImGui::GetCurrentContext() == nullptr,
          "purity: the layout engine runs without an ImGui context");
    const ui::Metrics grid = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::LayoutPrefs prefs = allOpenPrefs();
    const ui::ShellLayout first = ui::computeShell(grid, ImVec2(120.0f, 36.0f), prefs);
    const ui::ShellLayout second = ui::computeShell(grid, ImVec2(120.0f, 36.0f), prefs);
    check(layoutsEqual(first, second), "purity: the same arguments answer the same layout");

    // A poisoned display collapses the layout instead of producing NaN geometry.
    const ui::ShellLayout nan = ui::computeShell(grid, ImVec2(std::nanf(""), std::nanf("")), prefs);
    check(!hasArea(nan.thread) && !hasArea(nan.topBar) && !hasArea(nan.paletteOverlay),
          "purity: a NaN display lays out nothing");
    check(nan.thread.cells, "purity: a degenerate layout still states its unit");
    const ui::ShellLayout negative = ui::computeShell(grid, ImVec2(-100.0f, -50.0f), prefs);
    check(!hasArea(negative.thread) && !hasArea(negative.nowBar),
          "purity: a negative display lays out nothing");

    // A fractional display (a DPI-scaled `DisplaySize`) is floored once, so the whole
    // unit claim survives: the answer equals the one for the integer display below.
    const ui::ShellLayout fractional = ui::computeShell(grid, ImVec2(119.7f, 35.9f), prefs);
    check(layoutsEqual(fractional, ui::computeShell(grid, ImVec2(119.0f, 35.0f), prefs)),
          "purity: a fractional display is floored to whole units");
    check(isWhole(fractional.thread.w) && isWhole(fractional.composer.y),
          "purity: the floored display answers whole-length rects");
    // An unmetered Metrics (the struct's "no profile" state) answers nothing
    // rather than guessing a profile.
    const ui::ShellLayout unmetered = ui::computeShell(ui::Metrics{}, ImVec2(120.0f, 36.0f), prefs);
    check(!hasArea(unmetered.topBar) && !hasArea(unmetered.thread) &&
              unmetered.top == ui::OverlayLayer::None,
          "purity: an unmetered Metrics lays out nothing");
}

// ---------------------------------------------------------------------------
// 9. The unit stamp
// ---------------------------------------------------------------------------

void checkUnitStamp() {
    const ui::Metrics grid = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::Metrics pixel = ui::metricsFor(false, 1.0f, 1.0f);

    // A narrow grid size, so the inspector and the rail really are panels here and the
    // label below states what the rect is (at 120x36 the inspector is a docked column).
    const ui::ShellLayout gridLayout = ui::computeShell(grid, ImVec2(65.0f, 22.0f), allOpenPrefs());
    const ui::ShellLayout pixelLayout =
        ui::computeShell(pixel, ImVec2(1000.0f, 700.0f), allOpenPrefs());
    check(gridLayout.inspectorOverlay && gridLayout.railOverlay,
          "unit stamp: the grid layout has both regions as panels");
    check(pixelLayout.inspectorOverlay, "unit stamp: the pixel layout has a panel");
    for (const auto& entry : overlaysOf(gridLayout)) {
        if (!overlayIsUp(entry.first, gridLayout, allOpenPrefs()))
            continue;
        check(entry.second->cells, std::string("unit stamp: the grid ") +
                                       ui::overlayLayerName(entry.first) + " overlay claims cells");
    }
    for (const auto& entry : overlaysOf(pixelLayout)) {
        if (!overlayIsUp(entry.first, pixelLayout, allOpenPrefs()))
            continue;
        check(!entry.second->cells, std::string("unit stamp: the pixel ") +
                                        ui::overlayLayerName(entry.first) +
                                        " overlay claims pixels");
    }
    for (const ui::Rect* r : {&gridLayout.topBar, &gridLayout.rail, &gridLayout.thread,
                              &gridLayout.inspector, &gridLayout.composer, &gridLayout.nowBar,
                              &gridLayout.loadEarlier, &gridLayout.rawLogBand})
        check(r->cells, "unit stamp: a grid rect claims cells (" + rectText(*r) + ")");
    for (const ui::Rect* r : {&pixelLayout.topBar, &pixelLayout.rail, &pixelLayout.thread,
                              &pixelLayout.inspector, &pixelLayout.composer, &pixelLayout.nowBar,
                              &pixelLayout.loadEarlier, &pixelLayout.rawLogBand})
        check(!r->cells, "unit stamp: a pixel rect claims pixels (" + rectText(*r) + ")");

    // The docked inspector is a region, so it is checked as one.
    const ui::ShellLayout docked = ui::computeShell(grid, ImVec2(120.0f, 36.0f), allOpenPrefs());
    check(!docked.inspectorOverlay && docked.inspector.cells,
          "unit stamp: the docked inspector is a cells region, not an overlay");
}

// ---------------------------------------------------------------------------
// 10. No view reads the display size
// ---------------------------------------------------------------------------

void checkNoViewReadsTheDisplay() {
    const fs::path views{TEST_LAYOUT_VIEWS_DIR};
    std::error_code error;
    const bool exists = fs::exists(views, error) && fs::is_directory(views, error);
    check(exists, "views/: the views directory exists (" + views.string() + ")");
    if (!exists)
        return;

    std::vector<std::string> offenders;
    int files = 0;
    for (fs::directory_iterator it(views, error), end; it != end; it.increment(error)) {
        if (error || !it->is_regular_file())
            continue;
        const std::string extension = it->path().extension().string();
        if (extension != ".cpp" && extension != ".h")
            continue;
        ++files;
        std::ifstream in(it->path());
        // An unreadable view would look like a clean one; that is the failure mode
        // a layering check must not have.
        check(in.good(), "views/: " + it->path().string() + " is readable");
        std::string line;
        int lineNumber = 0;
        while (std::getline(in, line)) {
            ++lineNumber;
            // Two spellings, one rule: a view neither reads `ImGuiIO::DisplaySize`
            // nor asks the layout layer for it - it receives a `Rect`.
            if (line.find("DisplaySize") == std::string::npos &&
                line.find("displaySize(") == std::string::npos)
                continue;
            offenders.push_back(it->path().filename().string() + ":" + std::to_string(lineNumber));
        }
    }
    std::string found;
    for (const std::string& where : offenders)
        found += (found.empty() ? "" : ", ") + where;
    check(files > 0, "views/: the scan saw at least one file");
    check(offenders.empty(),
          "views/: no view reads the display size - the overlay geometry comes from "
          "computeShell" +
              (found.empty() ? std::string() : " (found: " + found + ")"));
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
    checkGridReferenceSizes();
    checkPixelReferenceSizes();
    checkComposerReservation();
    checkFocusMode();
    checkBreakpoints();
    checkRawLog();
    checkOverlays();
    checkZOrder();
    checkReferenceTiling();
    checkSweep();
    checkOverlaySweep();
    checkPurity();
    checkUnitStamp();
    checkNoViewReadsTheDisplay();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d layout check(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(
        stderr, "test_layout: all checks passed (tiling, breakpoints, overlays, z-order, sweep)\n");
    return 0;
}
