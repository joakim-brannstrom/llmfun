/// The inspector panel (`views/inspector.cpp`): the right-hand frame and its two tabs
/// (design §4.8, D-INSPECTOR, tasks 29/30).
///
///     ┌────────────────────────────┐
///     │ [5] Agents   [6] Activity  │  tab strip: one row, the active tab inverted
///     │ [1] not available yet      │  the reserved-digit notice (only while it is up)
///     │ ────────────────────────── │  hairline: the pixel profile only
///     │ planner ✓ -> coder ◐       │  the mission strip: the run in one row
///     │ ▸ coder | ◐ running        │  one row per lane, the focused one marked
///     │   planner | ✓ done         │  ...
///     │ Stop all                   │  the action row: the run's stop, two presses
///     └────────────────────────────┘
///
/// Two tabs, and two only: **Agents** (`Ctrl+5`) and **Activity** (`Ctrl+6`). `Ctrl+1..4`
/// are reserved by the design and stay reserved - a press of one of the digits that arrives
/// selects nothing and the strip says `not available yet` (D-16: no tabs 1-4 in the MVP).
///
/// The Agents tab replaces the mutually exclusive pipeline panel (design §9.2, FR-14): one
/// lane per pipeline agent - a glyph and a word for its state, its label, its clock, its
/// context count and its current tool, each dropped from the right when the 28-cell column
/// cannot hold it and the *state never dropped* (D-BLOCK: the row's meaning). Above the list
/// is the mission strip (the run in one row) and below it the action row, both fixed, because
/// a summary that scrolls away is not a summary and a `Stop all` that scrolls away is a
/// control the user cannot reach. Clicking a lane points the *thread* at that agent's
/// transcript (`UiState::focusedAgent`); the lane menu - right click, or `m` while no field
/// owns the keyboard - stops one lane (`Stop lane`) or arms the run's stop (`Stop every
/// agent...`, which is the same confirmation the action row draws).
///
/// Where the panel is. The layout engine owns that: `ShellLayout::inspector` is the docked
/// column at and above the wide breakpoint and the `90 %`-wide right-hand panel below it
/// (`ShellLayout::inspectorOverlay`), and a closed panel has no rectangle at all. The view
/// never asks which of the two it got beyond the `overlay` flag the frame hands it, which
/// changes two things only: the panel draws a hairline (a floating surface needs an edge; a
/// column beside the thread does not) and `Esc` closes it (D-ESC, `views/shell.h`).
///
/// The tabs are a *control*, not a decoration: each one is a real hit box, and either one
/// also opens a closed panel - a press that selects a tab and shows nothing would be a key
/// that does nothing. The TUI's key fallback for the pair is the character-level `[`/`]`
/// (plan/key_gate.md §10/§11 option (a)): `Ctrl+5`/`Ctrl+6` are swallowed by the terminal, so
/// the strip reads the queued characters itself - never a chord - while no text field owns
/// the keyboard (`ui::takeCharacter`), which is what keeps `[` typed into the composer as
/// typing.
///
/// Contract: a view is a geometry taker. It receives `ShellLayout::inspector`, a `Metrics` and
/// the frame's lane list (`UiState::agents`, passed like the rail's rows), reads no display
/// size, no profile flag and no aggregate, so it draws the same in both profiles and a harness
/// can render it into a cell grid. `inspectorMeasure()` answers every part once and
/// `renderInspector()` draws from those numbers, so the panel that is drawn and the panel that
/// was measured cannot differ.
///
/// Layering: `model/settings.h`, `model/ui_state.h` (the lane value type), `ui/` and imgui - no
/// `tui.h`, no v4 type. That is what lets `./test_inspector` and `./test_agents_tab` draw the
/// panel into a cell grid with no application state.
#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

// TODO: no relative include paths should be used. Fix cmake config instead.
// Relative, not `"model/settings.h"`: this header travels inside `tui.h`, and several test
// targets include `tui.h` with no `cpp_tui/` on the include path (the composer header's
// rule). A `"../"` include resolves next to *this* file first, so it works either way.
#include "../model/activity.h"
#include "../model/settings.h"

// Forward declarations, not includes (the palette header's rule): a translation unit that
// reaches `tui.h` must not need the `cpp_tui` include root just because a view was mentioned.
namespace llmfun::tui::ui {
struct Metrics;
struct Rect;
} // namespace llmfun::tui::ui

namespace llmfun::tui::model {
struct AgentLane;
} // namespace llmfun::tui::model

namespace llmfun::tui::views {

/// The two tabs. *The same type* as the persisted `ui.conf` word (`model::InspectorTab`):
/// one enum, so a saved tab cannot mean two things in two places - the composer's
/// `model::SendKey` is reachable the same way and for the same reason.
using InspectorTab = model::InspectorTab;

// ---------------------------------------------------------------------------
// The panel's vocabulary
// ---------------------------------------------------------------------------

/// The tab labels and the digits that name them (D-INSPECTOR). The digit is drawn, not
/// inferred: the strip is where a user learns the chord.
inline constexpr char kInspectorAgentsLabel[] = "Agents";
inline constexpr char kInspectorActivityLabel[] = "Activity";
inline constexpr int kInspectorAgentsDigit = 5;
inline constexpr int kInspectorActivityDigit = 6;

/// The two empty states (the task's own copy). A tab with nothing to show says so; a blank
/// panel reads as a broken one, and "no agents this run" is a fact, not a failure.
inline constexpr char kInspectorEmptyAgents[] = "No agents this run";
inline constexpr char kInspectorEmptyActivity[] = "Nothing to report yet";

/// The Activity tab's vocabulary (task 31, FR-15, D-ACTIVITY).
///
/// The severity *words* are the level's short form, not the design's long one: the docked
/// column is 28 cells and its body 26, where `all info warning error` measures 27 and would
/// silently drop a chip - a filter the user cannot reach, at exactly the width the panel was
/// designed for. Spelled short, `all info warn error` measures 24 and fits. `warn` is also the
/// word the design's own glyph line uses ("i (info) · ! (warn) · x (error)"), so the row and
/// its filter chip spell the level the same way.
inline constexpr char kActivityFilterAll[] = "all";
inline constexpr char kActivityFilterInfo[] = "info";
inline constexpr char kActivityFilterWarn[] = "warn";
inline constexpr char kActivityFilterError[] = "error";

/// The search field's placeholder: the box is over the *message* only, and saying so is
/// cheaper than a tooltip nobody hovers.
inline constexpr char kActivitySearchHint[] = "Search";
/// The second empty state: records exist, the filter or the search hides all of them. Saying
/// "nothing to report yet" there would be a lie the user can disprove by clearing the field.
inline constexpr char kActivityNoMatches[] = "No records match";

/// The action a record can carry, as the row prints it (D-ACTIVITY: the id is the wire's,
/// the label is the UI's). Unknown ids render no action at all.
inline constexpr char kActivityActionRetry[] = "Retry";
inline constexpr char kActivityActionDetails[] = "Details";
/// The brackets around an action label: one pair for every action, so the row's shape does
/// not change with the action's length.
inline constexpr char kActivityActionOpen[] = "[";
inline constexpr char kActivityActionClose[] = "]";

/// `Ctrl+1`/`Ctrl+2` (reserved by D-INSPECTOR, and the two the terminal swallows - so the
/// pixel profile is where they arrive): the press selected nothing and the strip says why. The
/// digits 3 and 4 are reserved by the design as well but are not bound, and not because nobody
/// measured them: byte 3 is `Ctrl+C` (`SIGINT`) and byte 4 is `Ctrl+\` (`SIGQUIT`), so with
/// `ISIG` on neither can reach the application at all (`plan/key_gate.md` §3b measures `Ctrl+I`
/// for the same reason). A row for either would be a dead key claiming an action.
inline constexpr char kInspectorReservedHint[] = "not available yet";

/// The badge's separator, in `gu`: one unit between the Activity label and its count.
inline constexpr int kInspectorBadgeGapGu = 1;

/// The Agents tab's controls (design §5.21, task 30). `Stop all` is a two-press action: the
/// first press arms the question, the second answers it - a run stop is not undoable, and the
/// design asks for the confirmation explicitly ("stop all (confirm)").
inline constexpr char kInspectorStopAllLabel[] = "Stop all";
inline constexpr char kInspectorStopAllQuestion[] = "Stop all?";
inline constexpr char kInspectorStopConfirmLabel[] = "Yes";
inline constexpr char kInspectorStopCancelLabel[] = "No";
/// The lane menu's three rows. `Stop lane` freezes one lane (the pipeline has no per-agent
/// cancel, so this is the TUI's own stop and the run keeps going); `Stop every agent...`
/// arms the confirmation; `Back to main` leaves a drill-down.
inline constexpr char kInspectorMenuStopLane[] = "Stop lane";
inline constexpr char kInspectorMenuStopAll[] = "Stop every agent...";
inline constexpr char kInspectorMenuBack[] = "Back to main";
/// The mission strip's join. ASCII in both profiles: the design's `→` (U+2192) is
/// East-Asian *Ambiguous* - one cell in imtui, two in a CJK terminal - and it has no row in
/// `ui/icons.h`, so a literal would be both a second glyph home and a width hazard.
inline constexpr char kInspectorMissionArrow[] = " -> ";

// ---------------------------------------------------------------------------
// The input
// ---------------------------------------------------------------------------

/// Everything the panel reads, plus the answers it hands back.
///
/// One writer (the frame thread). The first group is derived once per frame by
/// `views/shell.cpp` - whether the region is up (`ShellLayout::inspector` has area), whether
/// it is the floating shape, which tab the preference names, and the unread badge - the
/// second group is what the surface wrote and the frame applies in the same frame, and the
/// third is what a harness and a golden measure.
struct InspectorState {
    // --- derived by the frame ----------------------------------------------
    bool open = false;
    InspectorTab tab = InspectorTab::Activity;
    /// The layout handed the panel the floating shape (`ShellLayout::inspectorOverlay`):
    /// the surface paints an edge of its own and `Esc` closes it.
    bool overlay = false;
    /// Records that arrived while the Activity tab was not on screen (D-UNREAD's rule for
    /// the record stream, cleared the moment the tab is shown). 0 draws no badge.
    int unreadActivity = 0;

    // --- the Agents tab (design §5.21, task 30) -----------------------------
    /// The lane whose transcript the thread draws (`UiState::focusedAgent`): the tab marks
    /// its row with the `▸` rest mark, and `""` is the main transcript. The *thread* is what
    /// filters - a 28-cell column cannot show a transcript - so this is the one value the
    /// two regions share.
    std::string focusedLane;
    /// The lane whose row menu is up. The menu opens on a right click or the `m` character
    /// key, the two paths the rail's row menu has.
    std::string menuLaneId;
    /// Measured menu height (px): the flip above a row uses it, so a menu at the body's
    /// bottom edge cannot cover the row it acts on.
    float menuHeightPx = 0.0f;
    /// `Stop all` has been pressed once: the action row now asks the question and waits for
    /// `Yes`/`No`. The surface's own latch - a confirm step is not a preference, so no frame
    /// derives it and nothing persists it.
    bool stopAllArmed = false;

    // --- the Activity tab (design §4.8, task 31) ---------------------------
    /// Which severity the list shows (`All` by default). A single choice rather than a set
    /// of toggles: the tab answers "what went wrong" and "what happened", and one press that
    /// says `error` must not need three more to get back. Not persisted - a filter is a
    /// question about this run, not a preference.
    model::ActivityFilter activityFilter = model::ActivityFilter::All;
    /// The search box's text as ImGui's field holds it (never null, always terminated). One
    /// field, one id: nothing here clears the query programmatically, so the field never needs
    /// the rail's id-bump trick (task-31 review M3 removed the unused seq).
    std::array<char, 64> activitySearch = {};
    /// Scroll to the newest record on the next frame the list is drawn (a tab switch, an
    /// open, or the first frame of a harness). One-shot by construction: quiet-by-default
    /// means nothing pins the scroll while records arrive (D-FOLLOW's rule, applied to the
    /// log: the *content* moves, never the view).
    bool activityScrollToEnd = true;

    // --- written by the surface, applied by the frame -----------------------
    /// A reserved digit arrived: nothing was selected, and the strip says so. Cleared by the
    /// next tab selection or toggle.
    bool reservedPressed = false;
    /// The tab the surface asked for (a click, `[`/`]`, `Ctrl+5`/`Ctrl+6`). The frame layer
    /// writes it into `ShellPrefs::tab` in the same frame - the view never owns the
    /// preference, exactly like the rail's `selectId`.
    InspectorTab tabRequested = InspectorTab::Activity;
    bool tabRequestPending = false;
    /// Show this lane's transcript in the thread (a click on its row).
    std::string focusRequest;
    /// Leave the drill-down: the thread draws the main transcript again.
    bool backRequested = false;
    /// Freeze this lane (the lane menu). The run is not touched: D has no per-agent cancel,
    /// so stopping one lane is the TUI's own answer and the other lanes keep running.
    std::string stopLaneId;
    /// Stop every lane: the run's stop path (`tuiRequestCancel`, D-CANCEL), applied by the
    /// frame layer after the confirmation.
    bool stopAllRequested = false;

    // --- observations -------------------------------------------------------
    /// Body rows the surface drew this frame. Bounded by the viewport plus one row of
    /// overscan (the rail's rule) - the counter a fixture asserts on.
    std::size_t drawnRows = 0;

    /// Ask for `tab`: the one answer this surface gives. The frame applies it.
    void requestTab(InspectorTab wanted) {
        tabRequested = wanted;
        tabRequestPending = true;
    }
};

/// Is the Activity tab the one the user is looking at? One predicate: the unread counter
/// (`views/shell.cpp`), the `End` catch-up (`tui.cpp`) and the fixtures must answer the same
/// question, or "the tab is on screen" means three different things.
inline bool activityVisible(const InspectorState& state) {
    return state.open && state.tab == InspectorTab::Activity;
}

/// The `End` press (`Action::ScrollEnd`) as the Activity list sees it: arm the one-shot latch
/// that shows the newest record on the next frame the list draws. The list has no follower -
/// quiet by default (task 31) - so the latch *is* the catch-up. False when that tab is not on
/// screen: another tab's list is not the user's target, and a closed panel has no list to
/// catch up.
bool scrollActivityToNewest(InspectorState& state);

// ---------------------------------------------------------------------------
// The measured panel
// ---------------------------------------------------------------------------

/// One part of the panel, in the panel's own coordinates and in the metrics' units (cells on
/// the text grid, pixels in the pixel profile). Four floats rather than a `ui::Rect`, for the
/// reason `NowBarPart` gives: this header may not drag `ui/metrics.h` in.
struct InspectorPart {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    /// Nothing to draw: a part this panel could not afford.
    bool empty() const { return w <= 0.0f || h <= 0.0f; }
};

/// The panel's parts, in drawing order.
///
/// `agents`/`activity` are the two hit boxes and cover their labels; `badge` sits after the
/// Activity label (and is *part of* that tab's meaning - a click on it selects Activity);
/// `notice` is the reserved-digit row; `body` is the content box the tab's rows live in.
///
/// The Agents tab adds three (task 30): `mission` is the run's one-row strip above the lane
/// list, and `stopAll`/`stopNo` are the action row below it (`Stop all` alone, or `Yes`/`No`
/// while the confirmation is armed). The lane rows themselves are not parts: they are the
/// scroller's content, so their rectangles are the renderer's (they move with the scroll).
///
/// The `*Text` strings are exactly what is drawn, so a golden, a test and the screen can only
/// agree. `badgeText` is the count form (`•3`) or its degradation, the bare mark (`•`), or
/// empty when there is nothing unread.
struct InspectorParts {
    InspectorPart agents;
    InspectorPart activity;
    InspectorPart badge;
    InspectorPart notice;
    InspectorPart body;
    InspectorPart mission;
    InspectorPart stopAll;
    InspectorPart stopNo;

    std::string agentsText;
    std::string activityText;
    std::string badgeText;
    std::string noticeText;
    /// The sentence the body draws while the tab has no rows (the tab's empty state).
    std::string emptyText;
    /// The body's row count this frame: the tab's rows, or 1 for the empty state.
    std::size_t bodyRows = 0;

    /// One lane row: what the tab draws and the lane it belongs to (`bodyRows` counts these
    /// when the Agents tab has lanes).
    ///
    /// `live` is an *observation* for harnesses and goldens - the row's own copy of
    /// `AgentLane::active`, so a fixture can assert "this row is the one with something to
    /// stop" without reading the model list again. The renderer takes the same answer from the
    /// model (`model::anyLiveAgentLane`), never from here.
    struct LaneRow {
        std::string id;
        std::string text;
        bool focused = false;
        bool live = false;
    };
    /// The mission strip's one row (`planner ✓ -> coder ◐`), empty when no lane exists.
    std::string missionText;
    /// The lane rows in lane order. The renderer draws the visible window of them; the whole
    /// list is built here because a row's text is a *measurement* (which fields fit in the
    /// column), not a drawing decision.
    std::vector<LaneRow> laneRows;
    /// `Stop all` or `Stop all?` (armed), and `Yes`/`No` while armed.
    std::string stopAllText;
    std::string stopNoText;

    // --- the Activity tab (design §4.8, task 31) ---------------------------
    /// The severity filter's four chips, in drawing order, and the search field's rectangle.
    /// All of them are empty when the body was too short to afford its chrome: the list is
    /// what the tab exists for, so the chrome is given up before it (the Agents tab's rule
    /// for its mission strip and action row).
    InspectorPart filterAll;
    InspectorPart filterInfo;
    InspectorPart filterWarn;
    InspectorPart filterError;
    InspectorPart search;
    /// The chip labels exactly as drawn, so a golden, a test and the screen cannot differ.
    std::string filterAllText;
    std::string filterInfoText;
    std::string filterWarnText;
    std::string filterErrorText;
    /// The rows the Activity list draws, as indices into the record list the frame passed
    /// in: the filter and the search decide *which* rows, and they are cheap (one level
    /// compare and one substring per record). The row *text* is built by the renderer for
    /// the visible window only - §6.4 forbids a per-record string on the render path, and
    /// the ring holds 500 of them.
    std::vector<std::size_t> activityRows;
    /// True when the list has records but the filter or the search hides every one of them:
    /// the body then says so instead of claiming the run has nothing to report.
    bool activityFilteredEmpty = false;
};

/// The name of a tab, for diagnostics and goldens ("agents", "activity").
const char* inspectorTabName(InspectorTab tab);

/// The tab after/before `tab`, wrapping - what `]` and `[` select.
InspectorTab inspectorNextTab(InspectorTab tab);
InspectorTab inspectorPreviousTab(InspectorTab tab);

/// The sentence a tab with nothing to show draws (`kInspectorEmpty*`).
const char* inspectorEmptyText(InspectorTab tab);

/// Lay the panel out inside `region`: the one place the parts' geometry is computed.
///
/// Pure within the call (it reads no ImGui state beyond the font `CalcTextSize` measures
/// with, and moves no cursor). It pushes and pops the panel's own type steps, so a measure
/// taken outside a draw answers the numbers the drawn panel uses - the rule
/// `nowBarMeasure()` follows.
///
/// The strip's degradation: the two labels are reserved first (they are the panel's identity
/// and fit the docked 28-cell column), then the badge takes the count form while it fits and
/// the bare mark after that, and the notice is the one string that may be cut (it is a
/// notice, not a control).
InspectorParts inspectorMeasure(const InspectorState& state,
                                const std::vector<model::AgentLane>& lanes, const ui::Rect& region,
                                const ui::Metrics& m,
                                const std::deque<model::LogRecord>& activity = {});

/// Draw the panel inside `region`, and take the keys the surface owns.
///
/// A zero or mixed-unit rectangle paints nothing (the layout always gives an open panel a
/// real box, and a view that guessed a profile would paint in the wrong unit). The surface is
/// `surface` with the pixel profile's hairline while it floats; the grid blanks its cells so
/// the fill is a background rather than a ghost of the thread underneath.
///
/// `acceptKeys` is the shell's permission, the same one `renderRail()` and `renderThread()` take
/// (`!escStepClosesOverlay(state) && !state.escapeConsumed`): a surface that owns the `Esc`
/// chain's first step - the palette, or a panel that floats *over* this one - keeps the
/// character keys to itself, or `[`/`]` would switch a tab behind the surface the user is
/// looking at. `ui::takeCharacter()` only knows about a focused text field, so this flag is what
/// carries the rest of the rule.
///
/// `lanes` is the frame's own lane list (`UiState::agents`), passed in rather than read: the
/// panel is a geometry taker, exactly like `renderRail()` takes the rows `deriveRail()` built
/// (D-METRICS). The body draws the visible window of the tab's rows plus one row of overscan
/// (`inspectorWindow`) - the Agents tab's lane rows, or the tab's empty state on the same path,
/// because a second, throw-away renderer for "nothing here" is exactly the duplication the tab
/// bodies exist to avoid.
void renderInspector(InspectorState& state, const std::vector<model::AgentLane>& lanes,
                     const ui::Rect& region, const ui::Metrics& m, bool acceptKeys,
                     const std::deque<model::LogRecord>& activity = {});

/// The rows a tab body draws: everything the viewport touches, plus one row of overscan below
/// it (and one above it as well when `scrollY` is fractional, which the pixel profile's smooth
/// scrolling makes normal).
///
/// So the bound the caller and a fixture can rely on is `count <= ceil(viewportH / rowH) + 2`,
/// and exactly `+ 1` at an integer `scrollY` - the text grid's case, and the one the design's
/// "viewport + 1 row overscan" names. `views/rail.cpp`'s window has the same shape.
///
/// Pure, so the bounded-rendering claim can be tested without a screen - the same rule as
/// `views::railWindow()`. `scrollY` and `viewportH` are the child's own numbers, `rowH` the
/// profile's line height. A non-finite argument or a non-positive row height answers an empty
/// window rather than a guess.
struct InspectorWindow {
    std::size_t first = 0;
    std::size_t count = 0;
};
InspectorWindow inspectorWindow(std::size_t rows, float scrollY, float viewportH, float rowH);

} // namespace llmfun::tui::views
