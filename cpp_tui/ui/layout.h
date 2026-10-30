/// The shell's geometry: one function owns it (D-METRICS, D-GEOM, D-OVERLAY,
/// design §3.3).
///
///     const ui::Metrics m = ui::metricsFor();
///     const ui::ShellLayout shell = ui::computeShell(m, display, prefs);
///     views::renderThread(model, shell.thread, m);
///
/// This is the *only* place that turns a display size into rectangles. A view
/// receives a `Rect` and draws inside it; it never reads `ImGui::GetIO()`, never
/// asks which profile is live, never positions itself and never converts a unit.
/// That is what makes "the composer is never covered", "the thread keeps three
/// rows" and "the overlays stay inside the display" properties of one function
/// instead of a coincidence of five renderers, and it is why `./test_layout` can
/// prove them over a sweep of sizes instead of screenshotting every window size.
///
/// Regions (tiled) - they partition the display exactly, with one exception: in
/// the pixel profile an open Raw Log is a drawer between the thread and the
/// composer, so the six regions *plus that drawer* partition the display (which is
/// what keeps the drawer from covering the composer while still accounting for
/// every row). On the grid the Raw Log is a full-screen overlay and the regions
/// keep their rows.
///
///     topBar     full width, one row (folds into the now bar in compact mode)
///     rail       left column (docked) or a left panel (overlay)
///     thread     what is left in the middle
///     inspector  right column (docked) or a right panel (overlay)
///     composer   full width, above the now bar
///     nowBar     full width, the last row(s)
///
/// Focus mode (task 43, FR-28) hides the top bar, the rail and the inspector - they keep
/// zero rectangles - and the thread takes the whole width. The transcript is then drawn
/// into `ShellLayout::threadColumn`: a centred column of at most 90 `gu` (90 cells, 720 px
/// at 100 %), so an ultrawide window does not stretch a line of prose. The tiling is
/// untouched by that cap: the column is *inside* the thread, never a region of its own.
///
/// Overlays (not tiled) - each has its own invariant: inside the display and, when
/// the display can afford it, at least `overlayMinimumW/H()` (a minimum is a design
/// target, never a promise to overflow the display; see `rawLogBand` and the
/// inspector's panel band). The z-order is palette > settings > Raw Log > inspector
/// > rail, and `ShellLayout::top` is that order resolved for this frame, which is
/// what the `Esc` chain closes first (D-ESC). Only an overlay that actually has a
/// rectangle can be on top, so `top` never names a surface with nothing to close.
/// An overlay may cover the composer (the palette does, and so do the two panels -
/// that is what keeps their minimum height reachable at the 640x480 floor); what may
/// never cover it is the Raw Log drawer, because that one is part of the tiling.
///
/// Breakpoints. The design states them in each profile's own unit, and a single
/// scale-free number is what makes the same intent survive a 150 % font: the
/// boundaries are counted in `gu` units (one cell on the grid, one 8-px column at
/// 100 % in the pixel profile), so `1200 px` is 150 units and `100 cells` is 100.
///
///     width in gu       grid (cells)   pixel (px @ 100 %)    layout
///     >= wide           >= 100         >= 1200               rail + thread + inspector
///     >= medium         >= 80          >= 900                rail (+collapsed on the
///                                                            grid), inspector overlay
///     >= narrow         >= 60          >= 640 (window floor) rail collapsed, inspector
///                                                            overlay, both as panels on
///                                                            the grid
///     < narrow          < 60           < 640 (window floor)  grid: compact (no top bar,
///                                                            K groups, Load earlier);
///                                                            pixel: the 640x480 window
///                                                            minimum, never compact
///     < 40 x 15 cells   -              -                    the error page (the frame
///                                                            layer's floor, not this
///                                                            function's)
///
/// Compact mode is a **text-grid** state (design §4.1: "compact (TUI < 60 cells)").
/// The pixel profile's window cannot go below 640x480, so it never folds its top bar
/// and never narrows the thread's group window - at any font scale. (The other
/// breakpoints *are* scale-free: a 150 % font asks for 1500 px before it docks the
/// inspector, which is the same intent in bigger pixels.)
///
/// Tiling and units. A rectangle is cells on the text grid and pixels in the GUI,
/// and `Rect.cells` says which. `computeShell` stamps every rectangle it returns
/// (including a degenerate one), and a consumer checks it with
/// `rectMatchesProfile()` before it draws - 28 cells read as 28 px is a
/// legal-looking number that lays out wrong. Sizes are in the active profile's
/// units throughout: `m.gu` horizontally, `m.row` vertically, the display is
/// floored to whole units once, and every result is whole (no fraction survives into
/// a `SetNextWindow*` call).
/// - it knows nothing about the frame, the model or the v4 renderer, so the
/// geometry can be tested headless, before the first frame exists.
#pragma once

#include "imgui/imgui.h"

#include "ui/metrics.h"

namespace llmfun::tui::ui {

/// The width breakpoints, in `gu` units (see the table above). Whole numbers for
/// the grid (100/80/60 cells) and the pixel profile's 1200/900/640 px expressed in
/// 8-px columns (150/112.5/80), which is what keeps a scaled GUI on the same
/// intent instead of switching to compact the moment the font is enlarged.
struct LayoutBreaks {
    float wide = 0.0f;
    float medium = 0.0f;
    float narrow = 0.0f;
};

/// The breakpoints of a profile: `{100, 80, 60}` on the text grid and
/// `{150, 112.5, 80}` in the pixel profile.
LayoutBreaks layoutBreaks(const Metrics& m);

/// The live frame's display size, in the profile's own units (cells on the text
/// grid, pixels in the GUI): the one input `computeShell()` cannot derive.
///
/// This lives next to the engine that is allowed to turn a display into
/// rectangles, so the call site states *what* it wants instead of reaching into
/// `ImGuiIO`, and so the gate ledger keeps a single row for "display geometry is
/// read here" (`tools/gate_ratchet.txt`). The frame layer calls it once per frame;
/// a view never calls it at all, because a view receives a `Rect`.
ImVec2 displaySize();

/// Everything the layout is allowed to know about the shell's state.
///
/// The four members the design names - `railOpen`, `inspectorOpen`, `focusMode`
/// and the requested `railW` - plus the three overlay flags and the composer's
/// grown height. The additions are additive and defaulted, so a caller that only
/// fills the design's four still gets a complete layout (the overlays closed, the
/// composer at its empty height).
struct LayoutPrefs {
    bool railOpen = false;
    bool inspectorOpen = false;
    /// Focus mode (task 43, FR-28): the top bar, the rail and the inspector are
    /// hidden and the thread gets the room. The two side regions are *hidden*, not
    /// closed - `railOpen`/`inspectorOpen` are left alone, so leaving the mode puts
    /// the shell back exactly as the user had it - and the composer and the now bar
    /// stay where they are, because the run's state has to stay visible (NFR-16).
    /// The transcript is then drawn into `ShellLayout::threadColumn`, the centred
    /// column the mode is for.
    bool focusMode = false;
    /// The requested rail width, in the profile's units (cells / px). 0 means "the
    /// profile's own width" (`Metrics::railW`, or `railCollapsedW` while the width
    /// breakpoint collapses the rail); a request below the collapsed width is
    /// floored there, because a narrower column has no room for a row's glyph and
    /// title. A non-finite request is no request.
    float railW = 0.0f;

    /// The Raw Log is the one overlay that changes the tiling: as a GUI bottom
    /// drawer it takes rows away from the thread, and it must never cover the
    /// composer. On the grid it is a full-screen overlay and the regions keep
    /// their rows.
    bool rawLogOpen = false;
    /// The drawer's height in the profile's units, or 0 for "the design's 30 %". The drawer is
    /// resizable (design §4.9), so this is the user's drag handed back through the layout: it is
    /// clamped into `[kRawLogMinRows, the band the layout can afford]`, exactly like the
    /// composer's grown height, which keeps "the drawer never covers the composer" and "the thread
    /// keeps its three rows" properties of this function rather than of a view.
    float rawLogH = 0.0f;
    /// The palette and the settings sheet do not change the tiling; the flag
    /// decides whether their rectangle exists (a closed overlay has none) and
    /// where they sit in `ShellLayout::top`.
    bool paletteOpen = false;
    bool settingsOpen = false;
    /// The help page (task 42): the same kind of overlay flag - its own rectangle, no tiling
    /// change, and the top of the stack while it is up.
    bool helpOpen = false;

    /// The composer's current height (auto-grow 1-8 text rows), or 0 for "as tall
    /// as it is when empty" (`Metrics::composerMinH`). The layout clamps it into
    /// `[composerMinH, composerMaxH]` *and* keeps the thread's three rows, so a
    /// view can hand its grown height in without being able to squeeze the
    /// thread out of existence.
    float composerH = 0.0f;
};

/// The overlay stack, bottom-first. The order is the design's z-order (D-OVERLAY:
/// palette > settings > Raw Log > inspector) with the rail panel below the
/// inspector, and the `Esc` chain closes the topmost open one.
///
/// The rail is in the stack because the width breakpoint turns it into a panel too
/// ("single column, both overlays", design §3.3): without an entry here the `Esc`
/// chain could never close it and its order against the inspector panel would be
/// undefined. `None` means "nothing is open".
///
/// The help page (task 42) is the top of the stack: it is the page *about* the interface, so it
/// is drawn above every surface it explains (`views/shell.cpp`'s "render the help overlay above
/// the others"), and `Esc` closes it before anything else.
enum class OverlayLayer {
    None = 0,
    Rail,
    Inspector,
    RawLog,
    Settings,
    Palette,
    Help,
};

/// Stable name, for tests and log lines ("rail", "inspector", "rawLog",
/// "settings", "palette", "none").
const char* overlayLayerName(OverlayLayer layer);

/// The shell's rectangles for one frame.
///
/// `rail` and `inspector` are the region's rectangle in *both* of its shapes: the
/// docked column, or the panel it becomes when `railOverlay`/`inspectorOverlay`
/// says the width breakpoint moved it out of the tiling. A panel spans the top bar's
/// bottom to the now bar's top (one anatomy for both), so the two panels can overlap
/// - and `top` states which one is above (`inspector` > `rail`). A closed region
/// (`railOpen`/`inspectorOpen` false) has a zero rectangle.
///
/// An overlay's rectangle is zero while the surface is down - a closed overlay has
/// no geometry, and the GUI Raw Log drawer must not occupy rows it is not using.
/// `top` names the open overlay that the `Esc` chain closes first, and only when its
/// rectangle has area (a surface with nothing on screen is not closeable).
struct ShellLayout {
    // Tiled regions (a partition of the display).
    Rect topBar, rail, thread, inspector, composer, nowBar;

    /// The readable column inside `thread` (task 43): the whole region in the ordinary
    /// layout, and - in focus mode - a centred column of at most 90 `gu` (90 cells on the
    /// grid, 720 px at 100 % in the pixel profile). Centred to the nearest whole unit, so an
    /// odd gutter is one unit wider on one side.
    ///
    /// It is a *second* rect rather than a narrower `thread` on purpose: the six regions
    /// keep tiling the display (that invariant is the engine's strongest claim), and what a
    /// reader's eye gets is this column. `views/shell.cpp` draws the transcript into it, and
    /// `loadEarlier` is its first row - the same relation `thread` has to its own.
    ///
    /// Equal to `thread` whenever focus mode is off, so a consumer that ignores the field
    /// draws exactly what it drew before.
    Rect threadColumn;

    // Overlays (not tiled; inside the display, at least their minimum size while the
    // band they live in can afford it).
    Rect rawLogOverlay, paletteOverlay, settingsOverlay, helpOverlay;

    /// The band the Raw Log drawer may occupy in the pixel profile: the room left
    /// after the composer's empty height and the thread's three rows. The drawer is
    /// `min(max(30 % of the display, its minimum), this band)`, so
    /// `rawLogBand.h` is also the honest bound for that minimum - and task 33 resizes
    /// the drawer inside it. On the grid the Raw Log is a full-screen overlay and the
    /// band is the display.
    Rect rawLogBand;

    /// The thread's first row: the focusable `Load earlier` row of the bounded
    /// window (D-BOUNDS). Reachable with `PgUp`/`Ctrl+U` and a palette action, and
    /// it is the row the thread draws above its visible groups.
    Rect loadEarlier;

    bool inspectorOverlay = false;
    bool railOverlay = false;
    /// Echo of `LayoutPrefs::rawLogOpen`, so a consumer does not need both
    /// structs to know whether the Raw Log is up.
    bool rawLogOpen = false;
    /// Compact mode - a text-grid state (the pixel profile's window never goes below
    /// 640x480): the top bar is folded into the now bar (`topBar.h == 0`) and the
    /// thread shows `compactGroups` groups.
    bool compact = false;
    /// Groups in the thread's bounded window: 1 when compact and the display is
    /// at most 20 rows tall, 3 when compact and taller, and the ordinary window
    /// (40 on the grid, 30 in the pixel profile) otherwise.
    int compactGroups = 0;
    /// The topmost open overlay with a rectangle; `OverlayLayer::None` when nothing
    /// with geometry is open.
    OverlayLayer top = OverlayLayer::None;
};

/// The shell's geometry for `display` (cells on the grid, pixels in the GUI).
///
/// Pure: the same arguments always answer the same rectangles, no ImGui state is
/// read and no cursor is moved - which is what lets the property test sweep
/// thousands of sizes, and what lets a view be a geometry taker. The display is
/// floored to whole units once, so a fractional `DisplaySize` cannot make the
/// layout fractional.
///
/// An unmetered `Metrics` (a zero `gu`/`row`, which is the struct's "no profile"
/// state) or a non-positive display lays out nothing rather than guessing.
ShellLayout computeShell(const Metrics& m, ImVec2 display, const LayoutPrefs& p);

/// Does the rectangle speak the unit the metrics measured in?
///
/// Asserts in a debug build and returns the comparison in every build (the
/// shipped build is `-DNDEBUG`, where an `assert` is gone - a consumer that
/// ignores the answer would otherwise draw a plausible rectangle in the wrong
/// unit). Callers fail closed: paint nothing rather than guess.
bool rectMatchesProfile(const Rect& r, const Metrics& m);

/// The rectangle `d` inside `r`, anchored at the top-left, never negative and never
/// outside `r`.
///
/// The one inset a consumer needs for the `border` band: a hairline is drawn
/// *inside* the region rect (D-OVERLAY/N-9), so `inset(region, m.border)` is the
/// content box and the band between the two rects is the hairline's.
Rect inset(const Rect& r, float d);
/// The rectangle `d` inside `r`, anchored at the top-left and never negative.
///
/// The one inset a consumer needs for the `border` band: a hairline is drawn
/// *inside* the region rect (D-OVERLAY/N-9), so `inset(region, m.border)` is the
/// content box and the band between the two rects is the hairline's.
Rect inset(const Rect& r, float d);

/// The smallest an overlay may be and still be usable, in the profile's units.
/// `computeShell` never returns a smaller open overlay unless the display itself
/// is smaller.
float overlayMinimumW(const Metrics& m, OverlayLayer layer);
float overlayMinimumH(const Metrics& m, OverlayLayer layer);

} // namespace llmfun::tui::ui
