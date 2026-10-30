/// The one source of every length the shell draws with (D-METRICS, design §3.4).
///
/// A view is a **geometry taker**: it receives a `Rect` and a `Metrics` and
/// never asks which profile is live, never reads `ImGui::GetIO().DisplaySize`
/// and never converts a unit. The profile decision happens once, here:
///
///     const ui::Metrics m = ui::metricsFor();
///     drawAt(m.pad2, m.row * 3, m.railCollapsedW);
///
/// Units. A length is whole *cells* on the text grid and whole *pixels* in the
/// GUI. `gu` is one horizontal unit and `row` one vertical one, so a column
/// constant is `m.gu * n` and a row budget is `m.row * n`; a view converts no unit
/// itself. Every field is already rounded by `roundCells()`, so a layout
/// never carries a fraction and the text grid stays cell-exact.
///
/// Sizes, from the design (§1.4 definitions, §3.3 region anatomy, §3.4 metrics,
/// §4.2-§4.8 components, §4.15 tokens). GUI values are at 100 % scale and come
/// from the size tokens where one exists (D-THEME: no distance outside the token
/// table):
///
///     field             text grid   GUI @100 %   source
///     gu                1 cell      8 px         §1.4, §3.4
///     row               1 cell      24 px        §1.4, §3.4 (line height)
///     pad1              1 cell      8 px         §4.15 space.base
///     pad2              2 cells     12 px        §4.15 space.comfy
///     pad3              3 cells     16 px        §4.15 space.section
///     radius            0           6 px         §4.6 composer radius / §4.15 radius.large
///     border            1 cell      1 px         a hairline (one cell on the grid)
///     topBarH           1 row       48 px        §4.1, §4.2
///     railW             26 cells    208 px       §4.3
///     railCollapsedW    7 cells     56 px        §4.3
///     inspectorW        28 cells    224 px       §4.8
///     nowBarH           1 row       28 px        §4.1, §4.7
///     composerMinH      4 cells     50 px        2 rows (text + meta) + 2 borders, §4.6
///     composerMaxH      11 cells    218 px       8 text rows + meta row + 2 borders, §4.6
///
/// The two composer heights are the one place where the *resolved* anatomy, not
/// the scaled 100 % total, is the answer: at 150 % the rows grow (36 px) but the
/// hairline does not (1 px), so 2 x 36 + 2 x 1 = 74 px - one pixel less than
/// 50 px x 1.5. Both values are therefore built from the resolved `row` and
/// `border`, which keeps "text rows + meta row + two hairlines" true at every
/// scale (grid: 2 x 1 + 2 x 1 = 4 and 9 x 1 + 2 x 1 = 11).
///
/// The GUI column is multiplied by `fontScale * dpiScale` and rounded once, so a
/// 150 % window gets a 12 px `gu` and a 36 px `row`. The text grid ignores both
/// factors: a cell is a cell, and the terminal owns how big it is. Each factor
/// goes through a sanity check (non-finite or non-positive is 100 %) and the
/// product is capped at 800 %, so no pair of hand-edited settings can overflow a
/// length into 0 (`inf` x 0.5 would otherwise be a layout that draws nothing).
///
/// Naming: `Metrics` keeps the field names of the design's own struct (§3.4), so
/// the `…Cells`/`…Px`/`…Gu` suffix rule of §3.9 does not apply to it - these
/// fields are unit-agnostic by design. A *local* that holds one of them for a
/// single profile still carries the suffix, and `Rect.cells` states the unit of a
/// rectangle.
///
/// Layering: this header depends on nothing (a POD plus two free functions), and
/// `metrics.cpp` depends on `ui/tokens.h` only, so `ui/` stays free of `tui.h`
/// and `views/`. The live-profile accessor `metricsFor()` is *declared* here and
/// *implemented* by the frame layer (`tui.cpp`), exactly like `applyUiTheme()`:
/// reading `tuiIsTextGrid()` is app knowledge.
#pragma once

namespace llmfun::tui::ui {

/// A region or overlay rectangle (design §3.3).
///
/// `cells` states which unit the numbers are in - true on the text grid, false
/// in pixels - and every consumer asserts it instead of guessing. Mixing the
/// two is the bug the flag exists to catch: a 28-cell width read as 28 px is a
/// legal-looking number that lays out wrong.
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    bool cells = false;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
};

/// Every length a view may draw with, in the active profile's units. Filled by
/// `metricsFor()`; the zero-initialised state lays out nothing rather than
/// guessing a profile, so a missing call collapses visibly instead of quietly
/// drawing in the wrong unit.
struct Metrics {
    bool textGrid = false;
    /// One horizontal unit: 1 cell (text grid) | 8 px * scale (GUI).
    float gu = 0.0f;
    /// One vertical unit: 1 cell | line height (24 px at 100 %).
    float row = 0.0f;
    /// Inner padding of a bordered region, gutter between regions, section gap.
    float pad1 = 0.0f;
    float pad2 = 0.0f;
    float pad3 = 0.0f;
    /// Corner radius (0 on the grid: a rounded corner needs a curve, and a cell
    /// has none) and hairline thickness.
    float radius = 0.0f;
    float border = 0.0f;
    /// The six tiled regions' fixed sides (design §4.1).
    float topBarH = 0.0f;
    float railW = 0.0f;
    float railCollapsedW = 0.0f;
    float inspectorW = 0.0f;
    float nowBarH = 0.0f;
    /// Composer height: one text row + one meta row when it is empty, eight text
    /// rows + one meta row when it is full (auto-grow 1-8), plus its two borders.
    /// Built from the resolved `row`/`border`, so that identity holds at any
    /// scale (see the note above: at 150 % it is 74/326 px, not 75/327).
    float composerMinH = 0.0f;
    float composerMaxH = 0.0f;
};

/// Every length in `Metrics`, in declaration order, through one visitor. The
/// header has no reflection, so this is the inventory a consumer walks instead of
/// copying the field list; `test_metrics` uses it for both the whole-profile
/// comparison and the "a default Metrics is inert" check.
template <class F> void visitLengths(const Metrics& m, F&& visit) {
    visit("gu", m.gu);
    visit("row", m.row);
    visit("pad1", m.pad1);
    visit("pad2", m.pad2);
    visit("pad3", m.pad3);
    visit("radius", m.radius);
    visit("border", m.border);
    visit("topBarH", m.topBarH);
    visit("railW", m.railW);
    visit("railCollapsedW", m.railCollapsedW);
    visit("inspectorW", m.inspectorW);
    visit("nowBarH", m.nowBarH);
    visit("composerMinH", m.composerMinH);
    visit("composerMaxH", m.composerMaxH);
}

/// 14 lengths + the profile flag: `bool` + 3 padding bytes + 14 floats = 60 bytes
/// on every platform this builds for (4-byte float). A new length trips this
/// instead of being silently skipped by `visitLengths()` and the test - the
/// failure message is the instruction: add it to the visitor (and the table).
static_assert(sizeof(Metrics) == 60, "a new Metrics length must be added to visitLengths() too");

/// The metrics of a given profile and scale. Pure: the same arguments always
/// answer the same numbers, which is what lets the test pin both profiles.
///
/// `fontScale` is the user's text scale and `dpiScale` the monitor's content
/// scale; both are ignored on the text grid. A non-finite or non-positive factor
/// is treated as 1.0, and the product of the two is capped at 8 (800 %): a pair of
/// individually plausible factors can still overflow, and an infinite length
/// would round to 0 - an empty layout - instead of failing loudly.
Metrics metricsFor(bool textGrid, float fontScale, float dpiScale);

/// The metrics of the LIVE profile - the accessor a view calls. Implemented by
/// the frame layer (`tui.cpp`) because the profile flag is app knowledge; the
/// one place in the code base where "which renderer is up" becomes geometry.
///
/// The GUI's `fontScale` comes from the live `ImGuiStyle::FontScaleMain`, which
/// the frame layer writes once per frame from `TuiState::settings`
/// (`applyGuiFontScale()` in `tui.cpp`, before the root window is begun), and its
/// `dpiScale` from `FontScaleDpi`, which the pixel backend sets from the monitor.
/// That writer is the seam's input: if it ever disappears, every GUI length
/// silently falls back to 100 % - `test_gui_window` asserts the chain
/// settings -> style -> metrics so that cannot happen unnoticed.
Metrics metricsFor();

/// Round a length to whole units, halves down: 0.5 -> 0, 1.5 -> 1, 2.5 -> 2.
///
/// The grid produces halves (a 3-cell budget split in two, a 150 % scale applied
/// to an odd pixel value) and a half cell does not exist. The half rounds *down*
/// - not the usual `floor(v + 0.5)`, which answers 1 for a half - so a 2.5-cell
/// budget cannot claim a third cell, and a length is never further than half a
/// unit from the value it rounds. An exact *negative* half rounds away from zero
/// (-0.5 -> -1, -1.5 -> -2), the mirror of the positive rule; no design length is
/// negative, so this only makes the contract total. Non-finite input answers 0 (a
/// layout length is never infinite, and a poisoned one must not propagate into a
/// SetNextWindow* call).
float roundCells(float v);

} // namespace llmfun::tui::ui
