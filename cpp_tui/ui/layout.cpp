/// The layout engine (design §3.3): tiling, breakpoints, overlays and the
/// minimum-content invariants, computed once per frame.
///
/// `ui/layout.h` states the contract and why one function owns the geometry; this
/// file holds the arithmetic. The composition order is the design's:
///
///   1. vertical, bottom-up: the now bar is one row, the composer is reserved
///      above it (auto-grow, clamped so the thread keeps three rows), the GUI Raw
///      Log drawer is reserved above the composer (it must never cover it), and
///      the thread takes what is left;
///   2. horizontal: the rail column, the thread and the inspector column share the
///      width; a column that the breakpoints or the width cannot fit degrades to a
///      collapsed column, then to a panel, never to a squeezed thread;
///   3. overlays: their own boxes, centred/anchored, clamped inside the display and
///      never smaller than their minimum *while the band they live in can afford
///      it*;
///   4. the z-order, resolved into `ShellLayout::top`.
///
/// Every stage rounds its own lengths with `roundCells()` and derives the next
/// rectangle from the previous one's edge, so the tiled regions add up exactly -
/// no gap, no overlap, no fraction left for a `SetNextWindow*` call to interpret.
#include "ui/layout.h"

#include <algorithm>
#include <cmath>

namespace llmfun::tui::ui {
namespace {

// ---------------------------------------------------------------------------
// Breakpoints (gu units; see the table in layout.h)
// ---------------------------------------------------------------------------

/// Text grid: the design's cell counts - rail + thread + inspector from 100
/// cells, the collapsed rail from 80, compact below 60.
constexpr float kGridWideUnits = 100.0f;
constexpr float kGridMediumUnits = 80.0f;
constexpr float kGridNarrowUnits = 60.0f;

/// Pixel profile: the design's 1200 / 900 / 640 px in 8-px columns, so a scaled
/// GUI keeps the same intent (a 150 % font doubles `gu` and therefore asks for
/// twice the pixels before it claims the same number of columns). `narrow` is the
/// window minimum: the pixel profile is never compact (`compact` is a text-grid
/// state), it only ever collapses the rail and overlays the inspector.
constexpr float kPixelWideUnits = 150.0f;   // 1200 px at 100 %
constexpr float kPixelMediumUnits = 112.5f; // 900 px at 100 %
constexpr float kPixelNarrowUnits = 80.0f;  // 640 px at 100 % = the window floor

/// A float comparison with a millimetre of slack: a breakpoint is a decision about
/// whole units, and `1200 / 8` must not fall on the wrong side because a scale
/// factor left a 1e-7 residue.
constexpr float kUnitEpsilon = 1e-3f;

// ---------------------------------------------------------------------------
// Minimum content (design §3.3: thread >= 3 rows, composer text >= 1 row, now
// bar = 1 row, Load earlier reachable)
// ---------------------------------------------------------------------------

/// The thread's content width floor: 20 cells on the grid, 160 px at 100 % in the
/// pixel profile. Below this a column is dropped (collapsed, then overlaid) rather
/// than shown as a two-character sliver.
constexpr float kMinThreadUnits = 20.0f;

/// The thread's row floor. The composer clamp below reserves it, so a grown
/// composer can never squeeze the thread out of existence.
constexpr float kMinThreadRows = 3.0f;

/// Compact mode's group window (D-BOUNDS): one group on a short display, three on
/// a tall one; and the ordinary bounded window when the display is not compact.
constexpr float kCompactRowLimit = 20.0f;
constexpr int kCompactGroupsShort = 1;
constexpr int kCompactGroupsTall = 3;
constexpr int kThreadWindowGrid = 40;
constexpr int kThreadWindowGui = 30;

/// Focus mode's readable column (task 43): 90 `gu`, which is 90 cells on the text grid and
/// 720 px at 100 % in the pixel profile - the design's two numbers in the one unit that keeps
/// them the same intent at any font scale (see the breakpoint table above).
constexpr float kFocusColumnUnits = 90.0f;

// ---------------------------------------------------------------------------
// Overlay boxes (design §4.9, §4.12, §4.10, §4.8)
// ---------------------------------------------------------------------------

/// Raw Log: 30 % of the display as a GUI bottom drawer, above the composer
/// (§4.9). The grid's Raw Log is a full-screen overlay, so this fraction is only
/// used by the pixel profile.
constexpr float kRawLogDrawerFraction = 0.3f;

/// Palette: 80 % x 60 % on the grid, 640 px wide (max 70 % of a narrow window) in
/// the pixel profile (§4.12).
constexpr float kPaletteWidthFraction = 0.8f;
constexpr float kPaletteHeightFraction = 0.6f;
constexpr float kPaletteMaxWidthPx = 640.0f;
constexpr float kPaletteNarrowFraction = 0.7f;

/// Settings: a sheet, not a full surface - 60 % x 50 % on the grid, 520 x 600 px at
/// most in the pixel profile (§4.10 "GUI modal / TUI overlay"; the sizes are this
/// engine's, so the sheet cannot be positioned by its view).
constexpr float kSettingsWidthFraction = 0.6f;
constexpr float kSettingsHeightFraction = 0.5f;
constexpr float kSettingsGuiWidthFraction = 0.8f;
constexpr float kSettingsGuiHeightFraction = 0.8f;
constexpr float kSettingsMaxWidthPx = 520.0f;
constexpr float kSettingsMaxHeightPx = 600.0f;

/// The inspector panel's and the rail panel's width, as a fraction of the display
/// (§4.8: "overlay from the right (90 % width)"; the rail panel keeps its column
/// width).
constexpr float kInspectorOverlayFraction = 0.9f;

/// The help page (task 42): a page, so the largest sheet - 80 % x 80 % of the display on the grid,
/// and in the pixel profile 80 % tall with a width of `min(760 px, the display)`: a page of prose
/// and a key table needs the room, and a 1280 px window would put a 1024 px line of text in front
/// of a reader.
constexpr float kHelpWidthFraction = 0.8f;
constexpr float kHelpHeightFraction = 0.8f;
constexpr float kHelpMaxWidthPx = 760.0f;

/// Overlay minimums, in `gu`/`row` units: a palette or a settings sheet with fewer
/// than three result rows is not usable, and a drawer shorter than four rows is a
/// status line, not a log (four is also what 30 % of the smallest window affords at
/// 150 %). The inspector's minimum width is its docked width. A minimum is a design
/// target: an overlay takes `max(its natural size, min(its minimum, its band))`, so
/// a display or a font scale that cannot afford it still gets the largest surface
/// that fits instead of one that overflows.
constexpr float kPaletteMinUnits = 20.0f;
constexpr float kPaletteMinRows = 7.0f;
constexpr float kSettingsMinUnits = 20.0f;
constexpr float kSettingsMinRows = 7.0f;
constexpr float kRawLogMinUnits = 20.0f;
constexpr float kRawLogMinRows = 4.0f;
constexpr float kInspectorMinRows = 10.0f;
/// The help page's minimum: a title row, a search row, a hint row and four lines of the page -
/// below that the page would show nothing it explains. Its width is the palette's, because a
/// key row is `chord` + gutter + sentence.
constexpr float kHelpMinUnits = 24.0f;
constexpr float kHelpMinRows = 7.0f;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

/// A length that can be laid out: finite and non-negative, else 0. A poisoned
/// display size or metric must collapse the layout, not propagate into a
/// `SetNextWindowPos` call.
float length(float v) { return std::isfinite(v) && v > 0.0f ? v : 0.0f; }

/// The caller's rail width request, or 0 when there is none. `isfinite` is tested
/// first on purpose: `length()` maps an infinity to 0, which would otherwise be
/// mistaken for "no request" only *after* the `> 0` sentinel had accepted it.
float railRequest(const LayoutPrefs& p) {
    return std::isfinite(p.railW) && p.railW > 0.0f ? p.railW : 0.0f;
}

/// A rectangle with a clean width/height. `cells` is stamped by `stamp()` once, so
/// no construction path can forget it.
Rect box(float x, float y, float w, float h) {
    Rect r;
    r.x = length(x);
    r.y = length(y);
    r.w = length(w);
    r.h = length(h);
    return r;
}

/// A box of `w` x `h` centred in a `W` x `H` display, clamped inside it. Both the
/// size and the position are whole units, so a centred overlay lands on the same
/// cell every frame instead of shivering by a fraction.
Rect centred(float w, float h, float W, float H) {
    const float cw = std::min(roundCells(w), W);
    const float ch = std::min(roundCells(h), H);
    const float x = roundCells((W - cw) * 0.5f);
    const float y = roundCells((H - ch) * 0.5f);
    return box(x, y, cw, ch);
}

/// A centred surface that never drops below its design minimum unless the display
/// itself is smaller: the "minimum is a target, not an overflow" rule.
Rect centredAtLeast(float w, float h, float minW, float minH, float W, float H) {
    return centred(std::max(w, std::min(minW, W)), std::max(h, std::min(minH, H)), W, H);
}

bool hasArea(const Rect& r) { return r.w > 0.0f && r.h > 0.0f; }

void stamp(Rect& r, bool cells) { r.cells = cells; }

void stamp(ShellLayout& layout, bool cells) {
    stamp(layout.topBar, cells);
    stamp(layout.rail, cells);
    stamp(layout.thread, cells);
    stamp(layout.threadColumn, cells);
    stamp(layout.inspector, cells);
    stamp(layout.composer, cells);
    stamp(layout.nowBar, cells);
    stamp(layout.rawLogOverlay, cells);
    stamp(layout.paletteOverlay, cells);
    stamp(layout.settingsOverlay, cells);
    stamp(layout.helpOverlay, cells);
    stamp(layout.rawLogBand, cells);
    stamp(layout.loadEarlier, cells);
}

} // namespace

ImVec2 displaySize() { return ImGui::GetIO().DisplaySize; }

LayoutBreaks layoutBreaks(const Metrics& m) {
    if (m.textGrid)
        return LayoutBreaks{kGridWideUnits, kGridMediumUnits, kGridNarrowUnits};
    return LayoutBreaks{kPixelWideUnits, kPixelMediumUnits, kPixelNarrowUnits};
}

const char* overlayLayerName(OverlayLayer layer) {
    switch (layer) {
    case OverlayLayer::Help:
        return "help";
    case OverlayLayer::Palette:
        return "palette";
    case OverlayLayer::Settings:
        return "settings";
    case OverlayLayer::RawLog:
        return "rawLog";
    case OverlayLayer::Inspector:
        return "inspector";
    case OverlayLayer::Rail:
        return "rail";
    case OverlayLayer::None:
        break;
    }
    return "none";
}

float overlayMinimumW(const Metrics& m, OverlayLayer layer) {
    switch (layer) {
    case OverlayLayer::Help:
        return kHelpMinUnits * m.gu;
    case OverlayLayer::Palette:
        return kPaletteMinUnits * m.gu;
    case OverlayLayer::Settings:
        return kSettingsMinUnits * m.gu;
    case OverlayLayer::RawLog:
        return kRawLogMinUnits * m.gu;
    case OverlayLayer::Inspector:
        // The inspector panel is the docked inspector widened, so its docked width
        // is the smallest it may be.
        return m.inspectorW;
    case OverlayLayer::Rail:
        // The rail panel is a session list, not a data surface: it keeps its column
        // width and has no minimum of its own.
        return 0.0f;
    case OverlayLayer::None:
        break;
    }
    return 0.0f;
}

float overlayMinimumH(const Metrics& m, OverlayLayer layer) {
    switch (layer) {
    case OverlayLayer::Help:
        return kHelpMinRows * m.row;
    case OverlayLayer::Palette:
        return kPaletteMinRows * m.row;
    case OverlayLayer::Settings:
        return kSettingsMinRows * m.row;
    case OverlayLayer::RawLog:
        return kRawLogMinRows * m.row;
    case OverlayLayer::Inspector:
        return kInspectorMinRows * m.row;
    case OverlayLayer::Rail:
    case OverlayLayer::None:
        break;
    }
    return 0.0f;
}

bool rectMatchesProfile(const Rect& r, const Metrics& m) {
    IM_ASSERT(r.cells == m.textGrid &&
              "Rect.cells must state the unit the Metrics were measured in");
    return r.cells == m.textGrid;
}

Rect inset(const Rect& r, float d) {
    const float band = length(d);
    Rect out;
    // The origin is clamped into the source rectangle as well as the size: a band
    // wider than the rect answers a zero-sized box *at* the far edge, never a point
    // outside the rect it came from.
    out.x = std::min(r.x + band, r.x + r.w);
    out.y = std::min(r.y + band, r.y + r.h);
    out.w = std::max(0.0f, r.w - 2.0f * band);
    out.h = std::max(0.0f, r.h - 2.0f * band);
    out.cells = r.cells;
    return out;
}

ShellLayout computeShell(const Metrics& m, ImVec2 display, const LayoutPrefs& p) {
    ShellLayout out;
    const bool textGrid = m.textGrid;

    // The display is floored to whole units once (a DPI-scaled display size can be
    // fractional), so every rectangle below is whole by construction.
    const float W = std::floor(length(display.x));
    const float H = std::floor(length(display.y));
    // An unmetered profile (the `Metrics` default) or a display with no area: lay
    // out nothing, but still state the unit, so a consumer's check fails on the
    // geometry rather than on the flag.
    const bool measurable = m.gu > 0.0f && m.row > 0.0f && W > 0.0f && H > 0.0f;
    if (!measurable) {
        stamp(out, textGrid);
        return out;
    }

    const LayoutBreaks breaks = layoutBreaks(m);
    const float unitsW = W / m.gu;
    const float unitsH = H / m.row;

    // -----------------------------------------------------------------------
    // 1. Vertical: now bar, composer, drawer, thread.
    // -----------------------------------------------------------------------

    // Compact mode is a *text grid* state (design §4.1: "compact (TUI < 60 cells)").
    // The pixel profile's window never goes below 640x480, so it never folds its top
    // bar and never narrows the thread's group window, at any font scale.
    const bool compact = textGrid && unitsW + kUnitEpsilon < breaks.narrow;
    // Focus mode (task 43): the chrome is hidden, so the top bar reserves no row at all. It
    // is the same fold compact mode performs - and for the same reason: the run's state has
    // to live somewhere, and the now bar is that somewhere.
    const bool focusMode = p.focusMode;

    const float nowBarH = std::min(length(m.nowBarH), H);
    const float topBarH =
        (compact || focusMode) ? 0.0f : std::min(length(m.topBarH), std::max(0.0f, H - nowBarH));
    const float availableH = std::max(0.0f, H - topBarH - nowBarH);
    const float minThreadH = std::min(kMinThreadRows * m.row, availableH);

    // The Raw Log is reserved before the composer is sized: it sits between the
    // thread and the composer, so it can never cover the composer (§4.9), and it may
    // not eat into the thread's three rows either. The band is the honest bound for
    // both the drawer's size and its minimum (task 33 resizes inside it).
    const float drawerBandH = std::max(0.0f, availableH - length(m.composerMinH) - minThreadH);
    out.rawLogBand = textGrid ? box(0.0f, 0.0f, W, H) : box(0.0f, topBarH, W, drawerBandH);
    // The drawer exists in the pixel profile only: on the grid the Raw Log is a
    // full-screen overlay (step 3) and takes no rows from the regions. Its height is the
    // design's 30 % until the user drags it (`LayoutPrefs::rawLogH`), and either way it is
    // clamped to the band: the drawer may not eat the composer or the thread's three rows.
    float drawerH = 0.0f;
    if (p.rawLogOpen && !textGrid) {
        const float drawerWant =
            p.rawLogH > 0.0f ? roundCells(p.rawLogH) : roundCells(H * kRawLogDrawerFraction);
        drawerH = std::min(std::max(drawerWant, std::min(kRawLogMinRows * m.row, drawerBandH)),
                           drawerBandH);
    }
    const float bodyBudgetH = std::max(0.0f, availableH - drawerH);

    // The composer auto-grows (1-8 text rows) and the caller hands the grown
    // height in; the layout clamps it into the metrics' range *and* to what is
    // left once the thread has its three rows. Below the supported floor there is
    // no room for either promise - the frame layer shows the error page there -
    // but the rectangles still have to stay inside the display.
    const float composerCeiling = std::max(
        length(m.composerMinH), std::min(length(m.composerMaxH), bodyBudgetH - minThreadH));
    const float composerWant =
        p.composerH > 0.0f ? roundCells(p.composerH) : length(m.composerMinH);
    const float nowBarY = std::max(0.0f, H - nowBarH);
    const float composerRoom = std::max(0.0f, nowBarY - topBarH);
    const float composerH = std::min(
        std::min(std::max(composerWant, length(m.composerMinH)), composerCeiling), composerRoom);

    const float composerY = std::max(topBarH, nowBarY - composerH);
    const float bodyBottom = std::max(topBarH, composerY - drawerH);
    const float bodyH = std::max(0.0f, bodyBottom - topBarH);

    out.topBar = box(0.0f, 0.0f, W, topBarH);
    out.nowBar = box(0.0f, nowBarY, W, nowBarH);
    out.composer = box(0.0f, composerY, W, composerH);
    if (drawerH > 0.0f)
        out.rawLogOverlay = box(0.0f, composerY - drawerH, W, drawerH);

    // -----------------------------------------------------------------------
    // 2. Horizontal: rail column, thread, inspector column.
    // -----------------------------------------------------------------------

    // The breakpoints first (design §3.3's table), then the width check: a column
    // that does not fit is collapsed and finally moved out of the tiling, so the
    // thread always keeps its content floor.
    //
    // The two profiles degrade differently, exactly as the table says: the grid
    // collapses its rail to 7 cells from 100 cells down and turns it into a panel
    // from 80; the pixel profile keeps the 208-px column until 900 px and shows the
    // 56-px column from there (its window floor is 640 px, so its rail is never a
    // panel). Both put the inspector into a panel from their "wide" boundary.
    const float minThreadW = std::min(kMinThreadUnits * m.gu, W);
    const bool railCollapsed = unitsW + kUnitEpsilon < (textGrid ? breaks.wide : breaks.medium);
    bool railOverlay = textGrid && unitsW + kUnitEpsilon < breaks.medium;
    bool inspectorOverlay = unitsW + kUnitEpsilon < breaks.wide;
    // Focus mode hides both side regions, in either shape: a column would eat the thread's
    // room and the panel shape would cover it, which is the opposite of what the mode asks
    // for. The two resolved flags say "this region is drawn as a panel", so they are false
    // here - a region with no rectangle has no panel, and `ShellLayout::top` must not name a
    // surface nothing can close.
    if (focusMode) {
        railOverlay = false;
        inspectorOverlay = false;
    }

    const float requestedRailW =
        railRequest(p) > 0.0f ? railRequest(p) : (railCollapsed ? m.railCollapsedW : m.railW);
    // One floor and one ceiling for both shapes: a request below the collapsed width
    // has no room for a row's glyph and title, and an overlay is a panel, so it keeps
    // the full rail width even where the docked column would have been collapsed.
    const float railFloor = std::min(length(m.railCollapsedW), W);
    float railColW = 0.0f;
    if (p.railOpen && !railOverlay && !focusMode) {
        const float inspectorColW =
            (p.inspectorOpen && !inspectorOverlay) ? length(m.inspectorW) : 0.0f;
        railColW = std::min(std::max(requestedRailW, railFloor), W);
        if (W - railColW - inspectorColW < minThreadW) {
            railColW = railFloor;
            if (W - railColW - inspectorColW < minThreadW) {
                railOverlay = true;
                railColW = 0.0f;
            }
        }
    }
    const float railPanelW =
        std::min(std::max(railRequest(p) > 0.0f ? railRequest(p) : length(m.railW), railFloor), W);

    float inspectorColW = 0.0f;
    if (p.inspectorOpen && !inspectorOverlay && !focusMode) {
        inspectorColW = std::min(length(m.inspectorW), W);
        if (W - railColW - inspectorColW < minThreadW) {
            inspectorOverlay = true;
            inspectorColW = 0.0f;
        }
    }

    const float threadW = std::max(0.0f, W - railColW - inspectorColW);
    out.thread = box(railColW, topBarH, threadW, bodyH);
    // The bounded window's first row (D-BOUNDS): the focusable `Load earlier` row
    // belongs to the thread, so it is the thread rect's first row, not a region - and in focus
    // mode it belongs to the *column* the transcript is drawn in, so a control never floats in
    // the margin beside the text it pages.
    const float columnW =
        focusMode ? std::min(out.thread.w, roundCells(kFocusColumnUnits * m.gu)) : out.thread.w;
    const float columnX = out.thread.x + roundCells((out.thread.w - columnW) * 0.5f);
    out.threadColumn = box(columnX, out.thread.y, columnW, out.thread.h);
    out.loadEarlier = box(out.threadColumn.x, out.threadColumn.y, out.threadColumn.w,
                          std::min(m.row, out.threadColumn.h));

    // Both panels span the top bar's bottom to the now bar's top: one anatomy, so a
    // consumer never has to ask which panel it holds. They may overlap, and `top`
    // says which one is above (the inspector).
    const float panelH = std::max(0.0f, nowBarY - topBarH);
    if (p.railOpen && !focusMode) {
        out.rail = railOverlay ? box(0.0f, topBarH, railPanelW, panelH)
                               : box(0.0f, topBarH, railColW, bodyH);
    }
    if (p.inspectorOpen && !focusMode) {
        if (inspectorOverlay) {
            const float overlayW = std::min(std::max(roundCells(W * kInspectorOverlayFraction),
                                                     std::min(length(m.inspectorW), W)),
                                            W);
            out.inspector = box(W - overlayW, topBarH, overlayW, panelH);
        } else {
            out.inspector = box(W - inspectorColW, topBarH, inspectorColW, bodyH);
        }
    }

    // -----------------------------------------------------------------------
    // 3. Overlays: their own invariant, never part of the tiling.
    // -----------------------------------------------------------------------

    if (p.paletteOpen) {
        const float w = textGrid ? W * kPaletteWidthFraction
                                 : std::min(kPaletteMaxWidthPx, W * kPaletteNarrowFraction);
        const float h = textGrid ? H * kPaletteHeightFraction : H;
        out.paletteOverlay =
            centredAtLeast(w, h, kPaletteMinUnits * m.gu, kPaletteMinRows * m.row, W, H);
    }
    if (p.settingsOpen) {
        const float w = textGrid ? W * kSettingsWidthFraction
                                 : std::min(kSettingsMaxWidthPx, W * kSettingsGuiWidthFraction);
        const float h = textGrid ? H * kSettingsHeightFraction
                                 : std::min(kSettingsMaxHeightPx, H * kSettingsGuiHeightFraction);
        out.settingsOverlay =
            centredAtLeast(w, h, kSettingsMinUnits * m.gu, kSettingsMinRows * m.row, W, H);
    }
    if (p.helpOpen) {
        // A page, not a sheet: it takes most of the display so a key row and its sentence fit on
        // one line (task 42). It is an overlay like the other two - no tiling changes.
        const float w = textGrid ? W * kHelpWidthFraction : std::min(kHelpMaxWidthPx, W);
        const float h = H * kHelpHeightFraction;
        out.helpOverlay = centredAtLeast(w, h, kHelpMinUnits * m.gu, kHelpMinRows * m.row, W, H);
    }
    if (p.rawLogOpen && textGrid) {
        // On the grid the Raw Log is a full-screen overlay and its rectangle does not
        // participate in the tiling; in the pixel profile it is the drawer reserved
        // above the composer (step 1), which does.
        out.rawLogOverlay = box(0.0f, 0.0f, W, H);
    }

    // -----------------------------------------------------------------------
    // 4. Flags.
    // -----------------------------------------------------------------------

    out.inspectorOverlay = inspectorOverlay;
    out.railOverlay = railOverlay;
    out.rawLogOpen = p.rawLogOpen;
    out.compact = compact;
    out.compactGroups = compact ? (unitsH <= kCompactRowLimit + kUnitEpsilon ? kCompactGroupsShort
                                                                             : kCompactGroupsTall)
                                : (textGrid ? kThreadWindowGrid : kThreadWindowGui);

    // The z-order is the design's (D-OVERLAY) with the rail panel at the bottom and the help page
    // on top of everything (task 42: the page explains the interface, so it covers it). An
    // overlay without a rectangle has nothing to close, so it cannot be `top` - the
    // Esc chain must never target an invisible surface.
    const bool railUp = p.railOpen && railOverlay && hasArea(out.rail);
    const bool inspectorUp = p.inspectorOpen && inspectorOverlay && hasArea(out.inspector);
    const bool rawLogUp = p.rawLogOpen && hasArea(out.rawLogOverlay);
    if (p.helpOpen && hasArea(out.helpOverlay))
        out.top = OverlayLayer::Help;
    else if (p.paletteOpen && hasArea(out.paletteOverlay))
        out.top = OverlayLayer::Palette;
    else if (p.settingsOpen && hasArea(out.settingsOverlay))
        out.top = OverlayLayer::Settings;
    else if (rawLogUp)
        out.top = OverlayLayer::RawLog;
    else if (inspectorUp)
        out.top = OverlayLayer::Inspector;
    else if (railUp)
        out.top = OverlayLayer::Rail;
    else
        out.top = OverlayLayer::None;

    stamp(out, textGrid);
    return out;
}

} // namespace llmfun::tui::ui
