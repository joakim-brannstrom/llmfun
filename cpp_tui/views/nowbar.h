/// The now bar (`views/nowbar.cpp`): the one row that answers "what is happening right
/// now".
///
///     ▌ ✓ Ready                                Ctrl+K commands  ctx ████░░ 42k/128k  38.1 tok/s
///     qwen3-coder Busy ▌ ◐ writing | 00:12   Esc stop | Ctrl+K commands  ctx ████░░ 42k/128k  38.1
///     tok/s  qwen3-coder Busy ▌ ◐ writing | 00:03   Esc stop | Ctrl+K commands      ctx ████░░ 42k
///     (60 cells: the model chip yields)
///
/// Left: a state rule (1 cell on the text grid, 2 px in the pixel profile), the phase's
/// mark, the phase word, then the detail and the run's clock. Right: the contextual key
/// hints, the context meter (the meter that owns the numbers), the speed chip and the
/// model chip - and the `RAW` badge (`views/chrome.h`) wherever the top bar draws no row.
/// The coarse state word is not this row's business: the phase word is the one state.
///
/// Three things this row must never do:
///
///   * **lose the state.** The phase word is the row's reason to exist, so it is
///     reserved first and is never passed through `ui::truncate()`; when the row is too
///     narrow for everything the hints go first, then the detail, then the speed chip,
///     then the count compacts, then the model chip yields, then the meter's bar - the
///     `ctx` label and the count stand last; an open question is the one exception
///     that reaches wider.
///   * **reformat a number.** The chip values arrive formatted (`model::StatusChips`)
///     and are drawn verbatim; the meter's bar is drawn from the chips' own numbers,
///     never from a second copy of them. The clock is the one number this row formats
///     (`views::elapsedLabel`, once per frame); every other string is a join of
///     finished parts.
///   * **half-answer a question.** A hint is dropped whole, never cut mid-word, and an open
///     question is drawn in a form that still names the key that answers it - even at the
///     40x15 floor, where the right side gives up its room for it. The one thing worse
///     than a missing number is a question nobody can act on.
///
/// One vocabulary, one row: the phase word on the left is the *fine* state (seven of
/// them), and it is the only state the row says. The coarse `Ready|Busy` word lives in
/// the thread and the log, where it belongs - the idle row's phase word *is* ready.
///
/// Contract: a view is a geometry taker. It receives `ShellLayout::nowBar` and a
/// `Metrics`, reads no display size, no profile flag and no aggregate - `views/shell.cpp`
/// is where the eleven input fields are derived from the state. `nowBarMeasure()` answers
/// every part once and `renderNowBar()` draws from those numbers, so the bar that is
/// drawn and the bar that was measured cannot differ, and a harness can assert the
/// degradation ladder without reading a screen. `ShellFrame::nowBar` hands the exact input
/// the frame drew back to its caller, so a golden pins the geometry *and* the screen it
/// describes.
///
/// Layering: `model/status_chips.h` (the phase vocabulary and the chip strings),
/// `ui/` and imgui - no `tui.h`, no v4 type. That is what lets `./test_nowbar` draw the
/// row into a cell grid with no application state.
#pragma once

#include <cstdint>
#include <string>

#include "model/status_chips.h"
#include "model/toast.h"

// Forward declarations, not includes (the palette header's rule): a header that a
// translation unit reaches without the `cpp_tui` root must not drag `ui/metrics.h`
// in just because a geometry type was mentioned.
namespace llmfun::tui::ui {
struct Metrics;
struct Rect;
} // namespace llmfun::tui::ui

namespace llmfun::tui::views {

// The row's vocabulary

/// The contextual hints, most contextual first: the row draws the longest
/// prefix that fits *whole*, so what survives a narrow bar is the hint that applies now -
/// and never a fragment of one (a hint cut mid-word advertises nothing, and a dangling
/// separator advertises less).
///
/// The palette hint lives on the top bar alone (one `Ctrl+K`, one home for it) - this
/// row's hints are the ones about *this* row's presses.
inline constexpr char kNowBarHintStop[] = "Esc stop";
/// Focus mode's exit: the mode hides the chrome, so the row that survives is the
/// only place a user can read how to get it back.
inline constexpr char kNowBarHintLeaveFocus[] = "Esc leave focus";

/// The `ctx` label the meter carries - the meter is the row's one labeled chip
/// (`ctx ████░░ 42k/128k`), and the label belongs to the view because the count
/// must not change with it.
inline constexpr char kNowBarContextLabel[] = "ctx";

/// The key that answers a persistent flash (a warning or an error), drawn beside it: one press
/// clears the item, and a message with no visible way out is a message that stays forever. The
/// grid reads it as the typed character it is (`ui::takeCharacter('x')`, the same seam `y`/`s`/`?`
/// use), and the pixel profile's stack has the card's own `Dismiss` control as well.
inline constexpr char kNowBarFlashDismiss[] = "x dismiss";

/// The narrowest hint worth drawing, in `gu`. Below this the copy is a fragment of a
/// word ("Es…"), which advertises nothing; the row keeps its air instead.
inline constexpr int kNowBarMinHintGu = 8;

/// The meter's segment count on the pixel profile: fewer than the shared
/// `kMeterSegmentsGui` ten, sized for a row that shares the width with a detail, a clock
/// and hints. The grid keeps the shared six (`kMeterSegmentsGrid`, the meter vocabulary's
/// own constant in `views/composer.h` - both bars read the same thresholds and the same
/// `contextFraction`).
inline constexpr int kNowBarMeterSegmentsGui = 8;

// The input

/// Everything the row draws, derived once per frame by `views/shell.cpp`.
///
/// `phase` is the frame's phase (the structured status when D sends
/// one, else the provisional table over the facts the frame owns), `detail` the
/// current tool or step ("" while the frame does not know one - the row then simply
/// has no middle segment), `chips` the canonical values and `elapsedMs` the run's
/// clock.
///
/// Five flags shape the row without changing a value:
///   * `running` - a run is in flight, so the clock is drawn even while it reads
///     `00:00` (idle has no clock: a time beside `Ready` would claim a run);
///   * `stopAvailable` - the `Esc` cancel step is *open* (`cancelStepOpen`), so
///     `Esc stop` is drawn exactly while pressing it would stop the run. A stop that
///     is already on its way (phase `Waiting`) shows no hint, because the press would
///     fall through to the next `Esc` step;
///   * `following` - the thread follows the newest content. While it is
///     false the hints are replaced by the "there is more below" mark:
///     the grid's only affordance for "the answer continued below the fold", since the
///     ncurses backend delivers no wheel events;
///   * `rawLogOpen` and `compact` - the `RAW` badge is this row's business just where the
///     top bar is folded away, and `compact` is the layout's own answer, never the view's.
///     `prompt` is the fifth: an open question, below.
struct NowBarInput {
    model::Phase phase = model::Phase::Idle;
    std::string detail;
    /// The **run's** clock, in milliseconds (the row says what is happening in the
    /// run). The live answer's *own* clock is the
    /// streaming row's in the thread - a different fact about a different thing, so the
    /// two rows do not have to print the same number and neither borrows the other's.
    std::int64_t elapsedMs = 0;
    /// The compaction threshold, in tokens, or 0 when it is not known (the sentence path
    /// carries no threshold until structured status arrives): the one-unit tick across the
    /// meter's bar is drawn only when the number exists. The meter reads the same numbers
    /// the count beside it is drawn from (`StatusChips::contextUsed/contextMax`), so the
    /// bar and its tick cannot disagree with the count.
    long long compactAt = 0;
    model::StatusChips chips;
    bool rawLogOpen = false;
    /// Compact mode (`ShellLayout::compact`): the meter draws its compact count - the pair's
    /// used half (`ctx ████░░ 42k`), never the full pair - in whatever room is left.
    bool compact = false;
    /// The top bar draws no row this frame, so the `RAW` badge is this row's business - in
    /// compact mode (the bar is folded into the now bar) and in focus mode (the bar is
    /// hidden). Separate from `compact` because hiding the bar is not a reason to collapse the
    /// chips: focus mode hides the bar and keeps the four numbers.
    bool topBarFolded = false;
    bool running = false;
    bool stopAvailable = false;
    /// Focus mode is on, so the next `Esc` leaves the mode: the row names that step
    /// instead of leaving the user to discover it, because the terminal's way *in* (the palette's
    /// row) is not on screen once the mode is up. `stopAvailable` is false while this is true -
    /// the derivation asks the resolver, so a hint never outlives its step.
    bool focusMode = false;
    bool following = true;
    /// An open question the row shows instead of its hints - the quit confirmation.
    /// One line, and it never displaces the state or its chip: a question is not a
    /// reason to stop saying what the run is doing (the v4 row had to hide the
    /// chips to make room, which is exactly what this row does not do). While it is up the
    /// step and the clock step aside instead, and the question is drawn in the warning ink,
    /// because it is waiting for the user.
    std::string prompt;
    /// The one-line flash: the newest feedback item while it is
    /// up - a copy's answer, a saved path, a failed write. It takes the hints' slot and is drawn
    /// in the severity's ink with the severity's mark in front of it, because an answer to a press
    /// is not a key the user may press (`flashDrawn` says which of the two the slot holds).
    ///
    /// **Filled on the text grid only**: the pixel profile's presentation is the toast stack
    /// (`views/feedback.h`'s `nowBarFlashItem` is the rule), so this row never shows an item twice
    /// or takes its hints away for a card that is already on screen.
    ///
    /// It is drawn whole or not at all: a cut path is a path the user cannot find, and a cut
    /// warning is a warning nobody can act on. A persistent item (warn/error) also names the key
    /// that answers it, so the row can never hold the user hostage to a message with no way out.
    /// At a width where neither form fits, the row falls back to its hints and the item waits in
    /// the queue - `x` still answers it, and the pixel profile's card shows it in full.
    std::string flash;
    /// The flash's severity: it decides the mark and the ink, and whether the item persists (the
    /// frame drops a transient one when its lifetime is up - the view never times anything).
    model::ToastKind flashKind = model::ToastKind::Info;
    /// The same question in the shortest form that still names the key that answers it, or
    /// "" when there is none - what a narrow row says instead.
    ///
    /// The view does not invent it: the words belong to the question (`kQuitConfirmHint`
    /// and `kQuitConfirmShort` are the frame layer's), and a view that cut the prompt's
    /// *tail* would cut exactly the instruction a user cannot guess. So the row picks the
    /// form that fits, and only cuts the prompt itself when neither fits.
    std::string promptShort;
};

// The measured row

/// One part of the row, in the row's own coordinates and in the metrics' units (cells
/// on the text grid, pixels in the pixel profile).
///
/// Four floats rather than a `ui::Rect`, for the reason `TopBarPart` gives: this
/// header may not drag `ui/metrics.h` in. `renderNowBar()` stamps the unit when it
/// turns a part into a rectangle.
struct NowBarPart {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    /// Nothing to draw: a part this row is too narrow for.
    bool empty() const { return w <= 0.0f || h <= 0.0f; }
};

/// The row's parts, in drawing order from the left. An empty part is one the row could
/// not afford (the ladder in `nowBarMeasure`), never a part with nothing to say.
///
/// The `*Text` strings are what is drawn in the matching part, so a golden, a test and the
/// screen can only agree: `stateText` is the phase word untouched, `tailText` the
/// `detail`/clock joins that fit, `hintsText` the hint list that fits *whole* - or the open
/// question, in the longest form that still names its key.
///
/// `hintsTruncated` says the right side is not the full list: a hint was dropped (a hint is
/// never drawn as a fragment) or a question had to be cut to the row's last cells. It is a
/// diagnostic for a harness and a golden, never a state the row acts on.
///
/// `compactChips` records which form the right side drew: the full meter or a
/// narrower one - the speed gone, the compact count, the model chip gone, the bar
/// gone, or nothing at all (a question took the room); a golden pins the rung
/// without re-deriving it. `meterLabelText`/`meterCountText`/`chipSpeedText`/
/// `chipModelText` hold the labels; `meterBar` is empty when the bar yielded.
struct NowBarParts {
    NowBarPart rule;
    NowBarPart mark;
    NowBarPart state;
    NowBarPart tail;
    NowBarPart hints;
    NowBarPart badge;
    NowBarPart meterLabel;
    NowBarPart meterBar;
    NowBarPart meterCount;
    NowBarPart chipSpeed;
    NowBarPart chipModel;

    std::string stateText;
    std::string tailText;
    std::string hintsText;
    std::string meterLabelText;
    std::string meterCountText;
    std::string chipSpeedText;
    std::string chipModelText;

    bool hintsTruncated = false;
    bool compactChips = false;
    /// The `hints` part holds `NowBarInput::flash` rather than the key hints: the ink and
    /// the mark differ, and a golden can tell the two apart without re-deriving the choice.
    bool flashDrawn = false;
};

/// Lay the row out inside `bar`: the one place the part geometry is computed.
///
/// Pure within the call (it reads no ImGui state except the font `CalcTextSize`
/// measures with, and moves no cursor). It pushes and pops the row's type step itself
/// (`TypeStep::meta`; a no-op on the grid), so a measure taken outside a draw answers
/// the numbers the drawn row uses - the same rule `topBarMeasure()` follows.
///
/// The claims on the row, in order: the state rule, the phase mark and word (never cut),
/// the `RAW` badge and the right side from the right (full meter + speed chip + model
/// chip, else full meter + model chip, else the compact count, else no model chip, else
/// no bar), the detail and the clock, and last the hints - the longest prefix that fits
/// *whole*, dropped entirely below `kNowBarMinHintGu`: a hint is worth less than a number.
///
/// An open question takes the hint slot and the detail/clock room, and then the right
/// side's room too if that is what it takes to still name the key that answers it: a
/// question is the one thing on this row a user has to act on. The right side steps aside
/// whole (never half-drawn), and the state - word, mark and ink - stays.
NowBarParts nowBarMeasure(const NowBarInput& input, const ui::Rect& bar, const ui::Metrics& m);

/// Draw the row inside `bar`.
///
/// A zero or mixed-unit rectangle paints nothing (the layout always gives the row one
/// row, and a view that guessed a profile would paint in the wrong unit). The row's
/// chrome is the pixel profile's: a `surface` fill and a top hairline; the grid keeps
/// the terminal's own background, because a one-cell bar *is* one cell. The one colour
/// that is not chrome is the state rule, drawn in the phase's own ink - the design's
/// "no colour background except a 2 px state rule at the far left".
void renderNowBar(const NowBarInput& input, const ui::Rect& bar, const ui::Metrics& m);

} // namespace llmfun::tui::views
