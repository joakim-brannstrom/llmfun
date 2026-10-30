// test_shell_frame.cpp
//
// The frame's layout contract (views/shell.h): the overlay layer draws on the tiling
// the shell *painted*. The keymap runs between `renderShell()` and
// `renderOverlays()`, so a chord can open an overlay - and, once the Raw Log gets a
// key, change a preference that re-tiles - after the frame's tiling is fixed. This
// binary pins the answer `overlayLayoutFor()` gives to that: the painted preferences
// plus the overlay flag, never the live state, so an overlay can never land inside a
// thread rect the screen does not show.
//
// Pure: the shell's layout functions are `computeShell()` plus one rule, so this runs
// headless - no ImGui context, no window, no terminal.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = usage error.
//
//   ./test_shell_frame [-v]

#include "views/shell.h"

#include "model/ui_state.h"
#include "ui/layout.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;
bool g_verbose = false;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    } else if (g_verbose) {
        std::printf("ok   %s\n", what.c_str());
    }
}

bool rectsEqual(const llmfun::tui::ui::Rect& a, const llmfun::tui::ui::Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h && a.cells == b.cells;
}

bool layoutsEqual(const llmfun::tui::ui::ShellLayout& a, const llmfun::tui::ui::ShellLayout& b) {
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

std::string rectText(const llmfun::tui::ui::Rect& r) {
    return "x=" + std::to_string(r.x) + " y=" + std::to_string(r.y) + " w=" + std::to_string(r.w) +
           " h=" + std::to_string(r.h);
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
    using llmfun::tui::model::UiState;
    using llmfun::tui::ui::computeShell;
    using llmfun::tui::ui::Metrics;
    using llmfun::tui::ui::metricsFor;
    using llmfun::tui::ui::ShellLayout;
    using llmfun::tui::views::closeRailSearch;
    using llmfun::tui::views::layoutPrefsFor;
    using llmfun::tui::views::openRailForSearch;
    using llmfun::tui::views::overlayLayoutFor;
    using llmfun::tui::views::ShellFrame;
    using llmfun::tui::views::shellLayoutFor;

    // A harness has no ImGui context, so the live profile is the pixel one
    // (`tuiIsTextGrid()` is false until a text backend initialises) and the display
    // is in pixels. 1280x820 is the wide breakpoint: rail and inspector docked.
    UiState ui;
    const ImVec2 display(1280.0f, 820.0f);

    ShellFrame frame;
    frame.display = display;
    frame.prefs = layoutPrefsFor(ui, /*paletteOpen=*/false);
    frame.layout = shellLayoutFor(frame.prefs, display);

    // Premises: a three-column frame with the palette down - the state in which the
    // overlay layer has to invent a box.
    check(frame.layout.thread.w > 0.0f && frame.layout.thread.h > 0.0f,
          "premise: the thread has an area (" + rectText(frame.layout.thread) + ")");
    check(frame.layout.rail.w > 0.0f && !frame.layout.railOverlay,
          "premise: the rail is docked at 1280x820");
    check(frame.layout.paletteOverlay.w <= 0.0f, "premise: a closed overlay has no box");

    const ShellLayout painted = frame.layout;
    // Between the shell and the overlay layer: a chord opens the palette *and*
    // toggles the Raw Log drawer - the tiling-changing preference the keymap must
    // not be able to apply behind the shell's back.
    ui.rawLogOpen = true;

    const ShellLayout overlay = overlayLayoutFor(frame, /*paletteOpen=*/true);
    check(overlay.paletteOverlay.w > 0.0f && overlay.paletteOverlay.h > 0.0f,
          "overlay: the palette that was just opened has a box (" +
              rectText(overlay.paletteOverlay) + ")");
    check(rectsEqual(overlay.thread, painted.thread),
          "overlay: ...on the thread rect the shell painted (" + rectText(overlay.thread) + " vs " +
              rectText(painted.thread) + ")");
    check(rectsEqual(overlay.composer, painted.composer) &&
              rectsEqual(overlay.nowBar, painted.nowBar),
          "overlay: ...and the rest of the tiling is the painted one too");
    check(!overlay.rawLogOpen, "overlay: a tiling change made after the shell is not applied here");

    // The guard has a job: the same re-layout from the *live* preferences would land
    // on the drawer's tiling, i.e. a thread rect the screen does not show.
    const ShellLayout live = shellLayoutFor(layoutPrefsFor(ui, /*paletteOpen=*/true), display);
    check(live.rawLogOpen, "premise: the live preferences now carry the drawer");
    check(live.thread.h < painted.thread.h, "premise: the drawer takes rows from the thread (" +
                                                rectText(live.thread) + " vs " +
                                                rectText(painted.thread) + ")");

    // ...and it is a one-frame deferral, not a state the overlay layer is stuck in:
    // the next frame's shell paints the drawer.
    const ShellLayout nextFrame =
        shellLayoutFor(layoutPrefsFor(ui, /*paletteOpen=*/false), display);
    check(rectsEqual(nextFrame.thread, live.thread), "next frame: the shell paints the drawer");

    // With nothing open, re-laying out for the overlay layer is the painted layout -
    // the re-layout cannot move a rectangle on its own.
    check(layoutsEqual(overlayLayoutFor(frame, /*paletteOpen=*/false), painted),
          "overlay: with no overlay open the painted layout comes back unchanged");

    // The inspector's auto rule is one rule for both calls: below the wide
    // breakpoint the inspector would be a panel, so it is closed instead of covering
    // the thread and the composer.
    UiState mediumUi;
    ShellFrame medium;
    medium.display = ImVec2(900.0f, 700.0f);
    medium.prefs = layoutPrefsFor(mediumUi, /*paletteOpen=*/false);
    medium.layout = shellLayoutFor(medium.prefs, medium.display);
    check(medium.prefs.inspectorOpen, "auto rule: the preference is auto (open) before the rule");
    check(medium.layout.inspector.w <= 0.0f,
          "auto rule: ...and the panel form is closed instead (" +
              rectText(medium.layout.inspector) + ")");
    const ShellLayout mediumOverlay = overlayLayoutFor(medium, /*paletteOpen=*/true);
    check(rectsEqual(mediumOverlay.inspector, medium.layout.inspector),
          "auto rule: the overlay re-layout applies the same rule");
    check(mediumOverlay.paletteOverlay.w > 0.0f, "auto rule: and the palette still gets its box");

    // The rail's collapse (`Ctrl+B`, task 23): the preference is the only switch, and
    // -1 auto is "open" wherever the display can hold the column (the first-run state,
    // which the settings sheet persists once the user chooses). The chord's own row is
    // pinned in `./test_keymap`; what the frame can answer here is what each value of
    // the preference means for the tiling.
    UiState railUi;
    check(railUi.prefs.railOpen == -1, "first run: the rail preference is auto");
    const ShellLayout autoRail =
        shellLayoutFor(layoutPrefsFor(railUi, /*paletteOpen=*/false), display);
    check(autoRail.rail.w > 0.0f,
          "auto: the rail has its column at 1280x820 (" + rectText(autoRail.rail) + ")");
    railUi.prefs.railOpen = 0;
    const ShellLayout closedRail =
        shellLayoutFor(layoutPrefsFor(railUi, /*paletteOpen=*/false), display);
    check(closedRail.rail.w <= 0.0f, "closed (Ctrl+B): the rail has no rectangle");
    check(closedRail.thread.w > autoRail.thread.w,
          "closed (Ctrl+B): the thread takes the freed column (" + rectText(closedRail.thread) +
              " vs " + rectText(autoRail.thread) + ")");
    railUi.prefs.railOpen = 1;
    const ShellLayout openRail =
        shellLayoutFor(layoutPrefsFor(railUi, /*paletteOpen=*/false), display);
    check(rectsEqual(openRail.rail, autoRail.rail), "open: the same column auto answers");
    // An explicit open keeps the rail's rectangle at a small display: the preference is 1
    // here, so the `-1 auto` rule does not run. (That rule's own evidence is the panel band
    // below and `./parity_core_loop`'s 60x20 premise.)
    const ShellLayout smallRail =
        shellLayoutFor(layoutPrefsFor(railUi, false), ImVec2(500.0f, 400.0f));
    check(smallRail.rail.w > 0.0f,
          "an explicit open keeps the rail at 500x400 (" + rectText(smallRail.rail) + ")");

    // "Show me the search" (`/`, task 23). The request has to be a *width*: the rail's
    // preference is a bool and the collapsed shape comes from the width breakpoint, so
    // nothing written to `railOpen` can widen a column. The band matters - between the
    // medium and the wide breakpoint the layout hands the rail the collapsed column,
    // which is the one shape with no search field of its own.
    //
    // `shellLayoutFor()` answers the *live* profile, which a headless harness cannot
    // switch, so both bands go through `computeShell()` with the profile's metrics as
    // data - the same call the shell makes once it has resolved the profile.
    const Metrics grid = metricsFor(true, 1.0f, 1.0f);
    const ImVec2 narrowGrid(90.0f, 30.0f);
    UiState searchUi;
    const ShellLayout beforeSearch =
        computeShell(grid, narrowGrid, layoutPrefsFor(searchUi, false));
    check(beforeSearch.rail.w == grid.railCollapsedW,
          "premise: 90x30 gives the rail the collapsed column (" + rectText(beforeSearch.rail) +
              ")");
    openRailForSearch(searchUi, grid.railW);
    const ShellLayout afterSearch = computeShell(grid, narrowGrid, layoutPrefsFor(searchUi, false));
    check(afterSearch.rail.w == grid.railW,
          "the search request widens the rail to the profile's own column (" +
              rectText(afterSearch.rail) + ")");
    check(afterSearch.thread.w >= 20.0f * grid.gu,
          "and the thread keeps its floor beside the widened rail (" +
              rectText(afterSearch.thread) + ")");
    // A closed rail opens for the same request: `/` means "the field", not "the shape I
    // was in", and `-1 auto` is resolved the same way at every size.
    UiState closedSearchUi;
    closedSearchUi.prefs.railOpen = 0;
    openRailForSearch(closedSearchUi, grid.railW);
    const ShellLayout closedAfter =
        computeShell(grid, narrowGrid, layoutPrefsFor(closedSearchUi, false));
    check(closedAfter.rail.w == grid.railW, "a closed rail opens on the same request");
    // The panel band (60x20 cells): the shape the narrow golden draws. `computeShell()` is the
    // engine's answer for a given profile; the `-1 auto` *resolution* above it is pinned by
    // `./parity_core_loop` (an automatic rail is closed there), so this pins the shape and the
    // search request from a closed rail - not the auto rule.
    const ImVec2 panelGrid(60.0f, 20.0f);
    UiState panelUi;
    panelUi.prefs.railOpen = 0;
    const ShellLayout panelClosed = computeShell(grid, panelGrid, layoutPrefsFor(panelUi, false));
    check(panelClosed.rail.w <= 0.0f && panelClosed.thread.w == panelGrid.x,
          "panel band: a closed rail leaves the thread the full width (" +
              rectText(panelClosed.thread) + ")");
    openRailForSearch(panelUi, grid.railW);
    const ShellLayout panelOpen = computeShell(grid, panelGrid, layoutPrefsFor(panelUi, false));
    check(panelOpen.railOverlay && panelOpen.rail.w == grid.railW,
          "panel band: the search request opens the panel at 60x20 (" + rectText(panelOpen.rail) +
              ")");
    check(panelOpen.thread.w == panelGrid.x,
          "panel band: the panel floats over the thread instead of taking its width");
    // The pixel profile reaches the same panel shape only at the top of the scale range: the
    // rail requests its *collapsed* column below the medium breakpoint, so the fixed 208 px
    // column is not the one that has to fit. At 300 % in a 640 px window even the collapsed
    // column leaves less than the thread's 20-gu floor.
    const Metrics pixelScaled = metricsFor(false, 3.0f, 1.0f);
    UiState pixelPanelUi;
    pixelPanelUi.prefs.railOpen = 1;
    const ShellLayout pixelPanel =
        computeShell(pixelScaled, ImVec2(640.0f, 480.0f), layoutPrefsFor(pixelPanelUi, false));
    check(pixelPanel.railOverlay && pixelPanel.rail.w == pixelScaled.railW,
          "pixel floor at 300 %: the collapsed column no longer fits, so the rail is a panel (" +
              rectText(pixelPanel.rail) + ")");
    // The pixel profile collapses its rail below 900 px as well, so the same rule has to hold
    // there - the GUI's design window is outside it, which is why this is pinned here.
    const Metrics px = metricsFor(false, 1.0f, 1.0f);
    const ImVec2 narrowPx(800.0f, 600.0f);
    UiState pxSearchUi;
    const ShellLayout pxBefore = computeShell(px, narrowPx, layoutPrefsFor(pxSearchUi, false));
    check(pxBefore.rail.w == px.railCollapsedW,
          "premise: 800 px gives the pixel rail its collapsed column (" + rectText(pxBefore.rail) +
              ")");
    openRailForSearch(pxSearchUi, px.railW);
    check(computeShell(px, narrowPx, layoutPrefsFor(pxSearchUi, false)).rail.w == px.railW,
          "the same request widens the pixel profile's rail");
    // A wider request the user made is kept: the search only ever widens.
    UiState wideUi;
    wideUi.railW = 40.0f;
    openRailForSearch(wideUi, grid.railW);
    check(wideUi.railW == 40.0f, "a wider rail the user already asked for is not narrowed");
    // The other end of the rule: the width borrowed for the search goes back, so the
    // display that chose the collapsed column gets it again.
    closeRailSearch(searchUi);
    const ShellLayout afterSearchEnd =
        computeShell(grid, narrowGrid, layoutPrefsFor(searchUi, false));
    check(afterSearchEnd.rail.w == grid.railCollapsedW,
          "ending the search gives the collapsed column back (" + rectText(afterSearchEnd.rail) +
              ")");
    closeRailSearch(wideUi);
    check(wideUi.railW == 40.0f, "...and restores a width the user had, not zero");

    // Focus mode (task 43): the one preference that hides regions instead of closing them. The
    // frame has to read it from the state - a mode nobody passes to `LayoutPrefs` would be a
    // preference that does nothing - and the side regions' own preferences must survive it, so
    // leaving the mode needs no restore step.
    UiState focusUi;
    check(focusUi.prefs.railOpen == -1 && focusUi.prefs.inspectorOpen == -1,
          "focus mode: both side regions start at auto (which resolves to open)");
    focusUi.focusMode = true;
    const ShellLayout focused =
        shellLayoutFor(layoutPrefsFor(focusUi, /*paletteOpen=*/false), display);
    check(focused.topBar.h <= 0.0f, "focus mode: the top bar is hidden at 1280x820");
    check(focused.rail.w <= 0.0f && focused.inspector.w <= 0.0f,
          "focus mode: the rail and the inspector are hidden (" + rectText(focused.rail) + ", " +
              rectText(focused.inspector) + ")");
    check(rectsEqual(focused.composer, painted.composer) &&
              rectsEqual(focused.nowBar, painted.nowBar),
          "focus mode: the composer and the now bar are exactly where they were");
    check(focused.threadColumn.w == 720.0f && focused.threadColumn.x == 280.0f,
          "focus mode: the transcript is the centred 720-px column (" +
              rectText(focused.threadColumn) + ")");
    check(focusUi.prefs.railOpen == -1 && focusUi.prefs.inspectorOpen == -1,
          "focus mode: the mode hides the regions, it does not close them");

    // The overlay layer's re-layout has to keep the mode: a palette opened over it draws on the
    // tiling the shell painted, not on the one the mode left behind.
    ShellFrame focusFrame;
    focusFrame.display = display;
    focusFrame.prefs = layoutPrefsFor(focusUi, /*paletteOpen=*/false);
    focusFrame.layout = focused;
    const ShellLayout focusOverlay = overlayLayoutFor(focusFrame, /*paletteOpen=*/true);
    check(focusOverlay.paletteOverlay.w > 0.0f && rectsEqual(focusOverlay.thread, focused.thread) &&
              focusOverlay.topBar.h <= 0.0f,
          "focus mode: an overlay opens over the focused tiling");

    if (g_failures != 0) {
        std::fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("OK: the overlay layer draws on the painted tiling (%d checks)\n", g_checks);
    return 0;
}
