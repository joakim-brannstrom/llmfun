/// The session rail (`views/rail.cpp`): search, date groups, rows, footer
/// (design §4.3, FR-03, D-RAIL, D-UNREAD).
///
///     const model::RailModel list = model::buildRailRows(rows, nowMs, filter);
///     views::renderRail(ui.rail, list, layout.rail, m,
///                       views::railRowLines(m, display.y), nowMs, acceptKeys);
///
///     [ Search                                          ]   <- search row
///     TODAY                                                   <- group header
///     ▌● Alpha                                    now  •      <- active row
///     ▌▸ Beta                a preview line…      2h          <- two-line row
///     [+ New session]                                         <- footer
///
/// A view is a geometry taker: it receives `ShellLayout::rail` and a `Metrics`,
/// reads no display size, no profile flag and no `TuiState`, and draws only the
/// rows intersecting its viewport plus one row of overscan (D-RAIL's "scale"
/// rule) - so a 2000-session snapshot costs a scrollbar and ~30 rows, not 2000.
///
/// A rail narrower than a title (the 7-cell collapsed column the width breakpoint
/// hands it) draws the design's *strip* instead of the full anatomy: a `/` entry - and
/// the `/` key, which the strip reads itself - that asks for the search the strip cannot
/// hold (the frame answers with a *width* request: this shape is the width rule, so
/// nothing else can widen it), one mark per session, and a `+` row. No word is cut to
/// nothing, and every session stays reachable by a click.
///
/// State: the rail's cross-frame state is `model::RailState` (it travels inside
/// `tui.h`, so it may carry no `ui::` type). The view updates it while drawing -
/// the search text, the pointer's row, the keyboard's row, the open menu and the
/// rename box - and *answers* the rest (`selectId`, `newRequested`, `renameId`,
/// `deleteId`, `pinToggleId`, `focusSearch`), which the frame layer applies and
/// clears in the same frame. The rail never calls `queueSessionAction()` itself:
/// `views/*` may not name a v4 type (D-ADAPTER), and one bridge in the shell is
/// where the top bar's rename and the rail's rename become the same action.
///
/// Keys. `Ctrl+B` is the rail's chord (the keymap's row, measured in
/// plan/key_gate.md §4d). The row keys `m`/`r`/`d`/`p`/`/` are **characters**, not
/// chords: in the TUI a printable key never produces a key event (plan/key_gate.md
/// F2), so they are read and consumed from `io.InputQueueCharacters` - the shipped
/// shape of task 7's `m` - and never through `IsKeyPressed`.
///
/// The row keys act only on a row the user aimed at: the pointer's row or the one
/// holding the keyboard focus. There is no "the active session, else the first row"
/// fallback - a `d` typed while reading the thread must not ask to delete a session
/// nobody pointed at. `/` is the exception the design asks for ("focus search from
/// anywhere except the composer").
///
/// Layering: `ui/`, `model/` and imgui. No `tui.h`, no `ChatMessage`, no
/// `RenderGroup`: the rail is written against the model's rows, which is what
/// lets `./test_rail` draw it into a cell grid with no application state.
#pragma once

#include <cstddef>
#include <string>

#include "ui/tokens.h"

namespace llmfun::tui::ui {
struct Metrics;
struct Rect;
} // namespace llmfun::tui::ui

namespace llmfun::tui::model {
struct RailModel;
struct RailState;
} // namespace llmfun::tui::model

namespace llmfun::tui::views {

/// The states a drawn row can be in (design §4.3).
///
/// A row that is *not* drawn has no state to ink: "filtered out" is the absence
/// of a row, and the empty list is a message, not a row (`renderRail` draws it
/// with the same dim ink the thread's empty body uses).
enum class RailRowState {
    /// Nothing special about it: the rail's surface, the rest marker.
    Rest = 0,
    /// The pointer is over it (or the row wants attention): one surface step up.
    Hover,
    /// The active session: the `selection` fill and an accent rule at the left.
    Active,
    /// A run is in flight for it: the spinner replaces the time.
    ///
    /// Reserved for the multi-session run model (task 38): today D sets the running
    /// flag on the *active* row only, and an active row draws `Active` - the spinner
    /// is driven by `SessionRow::running` independently, so it is on screen anyway.
    /// The state keeps its inks so the table is complete for the task that can reach
    /// it, and so `./test_theme_contrast` measures it.
    Running,
    /// An update the user has not seen (D-UNREAD): a dot beside the time.
    Unread,
    /// A switch to it is queued or in flight - the busy queue or a select D has not
    /// acknowledged yet: the input well and the `stateWarn` rule, and the marker says
    /// `○` even when the row is the one currently active, because the row is waiting
    /// rather than "you are here".
    Pending,
    /// The inline rename box is open over it: the field's own well.
    Rename,
    Count
};

/// The name of a state, for tests and failure messages ("rest", "active", ...).
const char* railRowStateName(RailRowState state);

/// The inks one row state draws with. `fill` is the row's own fill (a rest row's
/// fill is the rail's surface, so every row has one and a label is always
/// measured against something), `rule` the left rule's colour - drawn only for
/// `Active` and `Pending`, and never the only carrier of the state (`D-BLOCK`:
/// the marker glyph and the fill say it too).
struct RailRowInks {
    ui::ColorToken fill;
    ui::ColorToken title;
    ui::ColorToken preview;
    ui::ColorToken rule;
};

/// The inks of one state: one table, so the contrast test measures exactly what
/// the view draws (there is no second copy of these choices).
///
/// `title` is `textHi` and `preview` `textMid` in *every* state on purpose: a row
/// tells its state with the fill, the rule and the marker, and never by making its
/// label dimmer - `textDim` is barred from carrying a glyph outright (NFR-09), and
/// the v4 rail's dimmed active label was the least legible text in the app
/// (1.18:1, the pair this task replaces).
RailRowInks railRowInks(RailRowState state);

/// Lines per row: 2 at a display of at least 30 terminal rows / 600 px, 1 below
/// (D-RAIL; automatic, no setting).
///
/// The threshold is stated in each profile's own unit - cells on the grid, pixels
/// in the GUI - exactly as the design words it, so a 24-row terminal gets the
/// compact row and a 1280x820 window gets the preview line.
int railRowLines(const ui::Metrics& m, float displayHeight);

/// A row's relative time as the rail shows it: `now`, `5m`, `3h`, `2d`, or `3w`
/// past a month. Empty for an unknown stamp (0) - the rail has no date to show
/// and does not invent one.
///
/// Pure and clock-injected, like the grouping it sits under: the same pair always
/// answers the same label, and a future stamp (clock skew) is `now` rather than a
/// negative duration.
std::string railTimeLabel(long long updatedAtMs, long long nowMs);

/// The window of rows a viewport shows: `first` is the index of the first item
/// that intersects `[scrollY, scrollY + viewportH)` within a sub-list that starts
/// at content offset `offset`, and `count` is how many are drawn - that window
/// plus one item of overscan, clamped to `slots`.
///
/// Pure, and the *only* windowing arithmetic the rail uses: group headers and
/// rows go through it, so a 2000-row snapshot draws a viewport's worth of items
/// and the bound is a property of one function instead of a loop condition.
struct RailWindow {
    std::size_t first = 0;
    std::size_t count = 0;
};
RailWindow railWindow(std::size_t slots, float offset, float slotHeight, float viewportH,
                      float scrollY);

/// Draw the rail inside `region` and drive its search, rows, menu and rename box.
///
/// `rowLines` is the layout answer (`railRowLines`), never a constant this view
/// picks; `nowMs` is the frame's clock, the same value the list was built with, so
/// a row's group and its time label cannot disagree. `acceptKeys` is the frame's
/// answer to "may this region take this frame's keys" - false while an overlay owns
/// the keyboard or when a view-local handler has already spent the press (D-ESC:
/// one step per press), which is why the rail's own `Esc` handler is gated on it
/// and reports a press it took through `RailState::escapeConsumed`. A zero or
/// mixed-unit rectangle paints nothing: the rail is a column *or* a panel and a view
/// that guessed a profile would draw in the wrong coordinate space.
///
/// The panel shape paints into the caller's window (the root list), not a window of its
/// own: below the medium breakpoint (80 cells) the layout makes the rail a panel that spans
/// the composer, and the composer's field is a child window, so its draw list runs after this
/// one and paints over the panel's bottom rows. The floating inspector avoids that with its
/// own late child window (`views/inspector.cpp`); the rail's panel needs the same treatment.
void renderRail(model::RailState& rail, const model::RailModel& list, const ui::Rect& region,
                const ui::Metrics& m, int rowLines, long long nowMs, bool acceptKeys);

} // namespace llmfun::tui::views
