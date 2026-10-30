/// The top bar (`views/topbar.cpp`): identity and the global controls (design §4.2, FR-02).
///
/// One row - one cell on the text grid, 48 px in the pixel profile - carrying the
/// wordmark, the inline-editable session title, the palette hint, the run pill, the `RAW`
/// badge and the settings entry:
///
///     [mark] llmfun | <title>                  Ctrl+K commands   (pill)   RAW   Settings
///
/// Five states (idle `Ready`, running, waiting, failed, raw-log open) and one
/// degradation: the fixed parts are reserved first, the title takes what is left and is
/// cut with the profile's ellipsis, and when even that has no room the palette hint, the
/// wordmark and the settings entry drop in that order. The mark, the run pill and the
/// `RAW` badge never drop - a bar that hides the safety signal to fit a hint is worse
/// than a bar with no hint.
///
/// Contract: a view is a geometry taker. The bar receives `ShellLayout::topBar` and a
/// `Metrics`; it reads no display size, no profile flag and no aggregate, so it renders
/// the same in both profiles and a harness can draw it into a cell grid. `topBarMeasure()`
/// answers every part once and `renderTopBar()` draws from those numbers, so a bar that
/// lays out differently from the one that was measured cannot exist.
///
/// The bar never acts on the application. A committed rename and a consumed `Escape` are
/// *answers* (`renameCommitted`/`renamedTitle`, `escapeConsumed`) that the frame layer
/// applies; the rename then goes through `queueSessionAction(SessionActionType::Rename,
/// ...)` - the very call the rail makes - so both entry points produce one
/// `SessionAction{Rename, id, title}` (design §6.2: "one registry entry").
///
/// Layering: `ui/` and imgui only - no `tui.h`, no `model/`, no v4 type. That is what lets
/// `./test_topbar` draw the bar into a 100x30 cell grid with no application state.
#pragma once

#include <array>
#include <string>

// Forward declarations, not includes (the palette header's rule): this header travels inside
// `TuiState`, and a translation unit that reaches `tui.h` must not need the `cpp_tui` include
// root just because a *view* was mentioned there.
namespace llmfun::tui::ui {
struct Metrics;
struct Rect;
} // namespace llmfun::tui::ui

namespace llmfun::tui::views {

/// The bar's five states (design §4.2).
///
/// The first four are the run's; the fifth is a *mode* of the bar rather than a run state -
/// the raw log opens while a run is in flight, and the pill has to keep reporting that
/// run. `TopBarState::run` therefore only ever holds the first four, and `barState()`
/// folds the flag in for the one value a golden, a log line or a test can name.
enum class RunState { Idle = 0, Running, Waiting, Failed, RawLogOpen };

/// Everything the bar reads, plus the answers it hands back.
///
/// One writer (the frame thread). The first group is derived from the aggregate once per
/// frame (`views/shell.cpp` owns that derivation: the run state comes from the busy flag
/// and the cancel latch until task 39 supplies `RuntimeStatus::phase`), the second group is
/// the bar's own edit state and survives across frames, and the third is consumed by the
/// frame layer in the same frame it is set.
struct TopBarState {
    // --- what the frame tells the bar, once per frame -----------------------
    /// The session the title belongs to; "" while D has sent no snapshot (or no active
    /// row). An empty id is also "nothing to rename": the title is not a hit target then.
    std::string sessionId;
    /// The active session's title, as the snapshot holds it - never the truncated form (the bar
    /// decides how much of it fits, and the window title mirrors the whole one). The bar draws this
    /// and nothing else: a committed rename is a request in flight, so the new name appears when
    /// D's next snapshot carries it (two frames after the commit, which is shorter than a blink).
    /// Keeping a local copy of the name until then would make the bar a second source of truth for
    /// it - and the copy has no answer for a rename D refuses.
    std::string sessionTitle;
    /// `Idle`, `Running`, `Waiting` or `Failed`; never `RawLogOpen` (see `barState()`).
    RunState run = RunState::Idle;
    /// The running pill's clock, in milliseconds. Ignored by the two states that do not
    /// count (`Ready`, `failed`), so a stale value cannot leak into their label.
    long long elapsedMs = 0;
    /// The Raw Log overlay is up: the `RAW` badge is drawn exactly while this holds.
    bool rawLogOpen = false;
    /// A rename may be queued (the rail's `canQueue`: no run is active). While it is
    /// false the title explains itself with a tooltip instead of opening a box D would
    /// refuse.
    bool canRename = true;
    /// Keys **and** clicks reach the bar: false while an overlay owns the keyboard (so `Esc`
    /// closes the overlay first and the rename box second, D-ESC: one step per press) and false
    /// on a frame whose key press a view-local handler already claimed (the frame's
    /// `escapeConsumed` latch), so the bar cannot take a press that has been spent. It gates
    /// the title's click as well as the box's `Esc`: an overlay that owns the keyboard must not
    /// lose it to a click on the bar underneath.
    bool acceptKeys = true;

    // --- the inline rename box (the bar's own state) ------------------------
    bool renaming = false;
    /// The session the *open* box belongs to, captured when it opened: a session switch
    /// while it is up cannot rename the new session (the rail's `renameRowId` rule).
    std::string renameId;
    /// The draft title - the rail's 256 bytes, so a title of up to 255 bytes behaves the same
    /// from both entry points (it opens filled). A longer one opens empty rather than as a
    /// silently truncated string: the rail's rule, kept on purpose.
    std::array<char, 256> renameBuf = {};
    /// Focus the field on the next frame (the open frame cannot: the field does not exist
    /// until the frame after the click).
    bool renameFocus = false;
    /// The field held the keyboard at the previous frame boundary. ImGui clears a text field's
    /// active id *inside* NewFrame when the click lands elsewhere, so without this latch the
    /// box would keep a keyboard it no longer has - the rail's `renameHadFocus` rule, and what
    /// makes "click away" terminate the rename instead of leaving an unfocused field over the
    /// title.
    bool renameHadFocus = false;
    /// Bumped on every open, so a re-open re-reads the re-initialised buffer instead of
    /// ImGui's stale edit state for the same id.
    int renameSeq = 0;

    // --- what the bar answers, consumed by the frame layer ------------------
    /// Set for one frame when the user committed a rename (Enter, a non-empty title, a live
    /// target and `canRename`). The frame enqueues `SessionAction{Rename, renameId,
    /// renamedTitle}` and clears both.
    bool renameCommitted = false;
    std::string renamedTitle;
    /// Set for one frame when the box took the `Esc` press, so the frame-level `Esc` steps
    /// (close the overlay, cancel the run) stand down for that press.
    bool escapeConsumed = false;
    /// Set for one frame when the `Settings` entry was clicked (design §4.2: "gear opens
    /// Settings"). The frame opens the sheet with it - `views::openSettings()` - in the same frame,
    /// so the click that asked for the surface is the frame that draws it. One of the sheet's two
    /// entry points; the palette's `open settings` row (task 35) is the other.
    bool settingsRequested = false;
};

/// The state's name, for tests and log lines ("idle", "running", "waiting", "failed",
/// "rawLogOpen").
const char* runStateName(RunState state);

/// The bar's five-state view: `RunState::RawLogOpen` while the badge is up, else `run`.
/// The pill keeps the run state either way - the badge and the pill are two signals, and
/// the design's fifth state is the bar's mode, not a fifth kind of run.
RunState barState(const TopBarState& state);

/// The running pill's label: `Ready`, `running 00:12`, `waiting 00:12`, `failed`.
///
/// One word per state, always beside the state's glyph and in the state's colour
/// (NFR-16: a mark is never the only carrier of meaning), and the clock only in the two
/// states that are counting - `00:00` beside `Ready` would claim a run that is not
/// happening.
std::string pillLabel(RunState state, long long elapsedMs);

/// `ms` as `mm:ss`, or as `h:mm:ss` past the first hour. Negative, NaN-free by contract
/// (`long long`) and absurdly large values are all answered, because the clock is wall
/// time and a clock jump must not print a negative duration or crash a format.
std::string elapsedLabel(long long ms);

/// One part of the bar, in the bar's own coordinates and in the metrics' units (cells on the
/// text grid, pixels in the pixel profile).
///
/// Four floats rather than a `ui::Rect` on purpose: this header travels inside `TuiState`, and
/// a `Rect` member would drag `ui/metrics.h` into every translation unit that includes
/// `tui.h`. The unit is not repeated per part either - the `Metrics` the bar was measured with
/// answered it once, and `renderTopBar()` stamps it when it turns a part into a `Rect`.
struct TopBarPart {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    /// Nothing to draw: the part this bar is too narrow for.
    bool empty() const { return w <= 0.0f || h <= 0.0f; }
};

/// The bar's parts, in the bar's own coordinates: what `renderTopBar()` draws into.
///
/// An empty part is one this bar is too narrow to draw (`hint`, `wordmark`, `rule` and `gear`
/// are the ones that drop; `mark`, `pill` and `badge` only when the region itself is
/// degenerate). `titleText` is the title cut to `title` (`ui::truncate`, so the grid gets
/// `…`/`...` and never a half glyph), and `titleTruncated` says the cut happened - the
/// tooltip, the golden and the tests read it.
struct TopBarParts {
    TopBarPart mark;
    TopBarPart wordmark;
    TopBarPart rule;
    TopBarPart title;
    TopBarPart hint;
    TopBarPart pill;
    TopBarPart badge;
    TopBarPart gear;
    std::string titleText;
    bool titleTruncated = false;
};

/// Lay the bar out inside `bar`: the one place the part geometry is computed.
///
/// Side-effect free within the call: it reads no ImGui state except the font `CalcTextSize`
/// measures with, and it moves no cursor. It does push and pop the label type scope itself (the bar
/// draws its plain text at that step, see `renderTopBar`), so a caller does not have to - and a
/// measure taken inside a scope answers the same numbers, because a step is absolute (see
/// `ui::TypeScope`). The arithmetic is therefore testable without drawing, and `renderTopBar()`
/// draws from exactly this answer, which is what keeps the drawn bar and the measured one the same
/// bar.
TopBarParts topBarMeasure(const TopBarState& state, const ui::Rect& bar, const ui::Metrics& m);

/// Draw the bar inside `bar` and drive its inline rename.
///
/// The legend is `Surface > shell.cpp` (`ui::TypeStep::label`, which is a no-op on the
/// grid where a cell has one size). A zero or mixed-unit rectangle paints nothing: the
/// compact layout folds the top bar away (`topBar.h == 0`) and a view that guessed a
/// profile would draw in the wrong coordinate space.
void renderTopBar(TopBarState& state, const ui::Rect& bar, const ui::Metrics& m);

} // namespace llmfun::tui::views
