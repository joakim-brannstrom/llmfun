/// The now bar (`views/nowbar.cpp`): the one row that answers "what is happening right
/// now" (design §4.7, FR-11 / D-PHASE / D-FOLLOW).
///
///     ▌ ✓ Ready                        Ctrl+K commands  ctx 42k/128k  38.1 tok/s  qwen3-coder
///     Ready ▌ ◐ writing | 00:12   Esc stop | Ctrl+K commands  ctx 42k/128k  38.1 tok/s qwen3-coder
///     Busy ▌ ◐ writing | 00:03   Esc stop | Ctrl+K commands  ctx 42k  ◐          (60 cells: the
///     chips collapse)
///
/// Left: a state rule (1 cell on the text grid, 2 px in the pixel profile), the phase's
/// mark, the phase word, then the detail (the current tool or step) and the run's
/// clock. Right: the contextual key hints, the four canonical chips and - wherever the top
/// bar draws no row (compact mode, where it is folded into this row, and focus mode, where
/// it is hidden) - the `RAW` badge (`views/chrome.h`).
///
/// Three things this row must never do:
///
///   * **lose the state.** The phase word is the row's reason to exist, so it is
///     reserved first and is never passed through `ui::truncate()`; when the row is too
///     narrow for everything the hints go first, then the detail and the clock, then the
///     chips degrade (design §4.7: "truncate hints first, never the state"). At 60 cells -
///     the breakpoint's own edge - the four chips already cannot fit beside the state even
///     with no hints and no clock, so they take their collapsed form there too; the
///     design's "below 60" names where the collapse is *asked for*, not where it first
///     becomes necessary.
///   * **reformat a number.** The four chips arrive formatted (`model::StatusChips`,
///     task 6) and are drawn verbatim - the same strings the status sentence carries and
///     the composer's meter reads (§6.3: the chips are canonical for the numbers). The
///     clock is the one number this row formats (`views::elapsedLabel`, the top bar's
///     formatter, called once per frame - it is a value that changes every frame, and the
///     top bar's pill has always rendered it that way); every other string here is a join
///     of finished parts.
///   * **half-answer a question.** A hint is dropped whole, never cut mid-word, and an open
///     question is drawn in a form that still names the key that answers it - even at the
///     40x15 floor, where the metric chips give up their room for it. The one thing worse
///     than a missing number is a question nobody can act on.
///
/// Two vocabularies, one row: the phase word on the left is the *fine* state (seven of
/// them), the fourth chip on the right is the design's coarse `Ready|Busy` - so in the
/// idle state the two read the same word, which is honest (the coarse state of an idle run
/// *is* ready) and is the price of keeping the four chips canonical (§4.7).
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

// ---------------------------------------------------------------------------
// The row's vocabulary
// ---------------------------------------------------------------------------

/// The contextual hints (design §4.7), most contextual first: the row draws the longest
/// prefix that fits *whole*, so what survives a narrow bar is the hint that applies now -
/// and never a fragment of one (a hint cut mid-word advertises nothing, and a dangling
/// separator advertises less).
inline constexpr char kNowBarHintStop[] = "Esc stop";
inline constexpr char kNowBarHintPalette[] = "Ctrl+K commands";
/// Focus mode's exit (task 43), the hint between the stop hint and the palette one: the mode hides
/// the chrome, so the row that survives is the only place a user can read how to get it back.
inline constexpr char kNowBarHintLeaveFocus[] = "Esc leave focus";

/// The `ctx ` label the context chip carries - the design gives exactly one chip a
/// label (§4.7: `ctx 42k/128k`), and the label belongs to the view because the value
/// must not change with it.
inline constexpr char kNowBarContextLabel[] = "ctx ";

/// The key that answers a persistent flash (a warning or an error), drawn beside it: one press
/// clears the item, and a message with no visible way out is a message that stays forever. The
/// grid reads it as the typed character it is (`ui::takeCharacter('x')`, the same seam `y`/`s`/`?`
/// use), and the pixel profile's stack has the card's own `Dismiss` control as well.
inline constexpr char kNowBarFlashDismiss[] = "x dismiss";

/// The narrowest hint worth drawing, in `gu`. Below this the copy is a fragment of a
/// word ("Es…"), which advertises nothing; the row keeps its air instead.
inline constexpr int kNowBarMinHintGu = 8;

// ---------------------------------------------------------------------------
// The input
// ---------------------------------------------------------------------------

/// Everything the row draws, derived once per frame by `views/shell.cpp`.
///
/// `phase` is the frame's phase (D-PHASE: the structured status once task 39 sends
/// one, else the provisional table over the facts the frame owns), `detail` the
/// current tool or step ("" while the frame does not know one - the row then simply
/// has no middle segment), `chips` the four canonical values and `elapsedMs` the run's
/// clock.
///
/// Five flags shape the row without changing a value:
///   * `running` - a run is in flight, so the clock is drawn even while it reads
///     `00:00` (idle has no clock: a time beside `Ready` would claim a run);
///   * `stopAvailable` - the `Esc` cancel step is *open* (`cancelStepOpen`), so
///     `Esc stop` is drawn exactly while pressing it would stop the run. A stop that
///     is already on its way (phase `Waiting`) shows no hint, because the press would
///     fall through to the next `Esc` step;
///   * `following` - the thread follows the newest content (D-FOLLOW). While it is
///     false the hints are replaced by the "there is more below" mark (design §5.11):
///     the grid's only affordance for "the answer continued below the fold", since the
///     ncurses backend delivers no wheel events (task 13a);
///   * `rawLogOpen` and `compact` - the `RAW` badge is this row's business just where the
///     top bar is folded away, and `compact` is the layout's own answer, never the view's.
///     `prompt` is the fifth: an open question, below.
struct NowBarInput {
    model::Phase phase = model::Phase::Idle;
    std::string detail;
    /// The **run's** clock, in milliseconds (design §4.7: the row says what is happening in the
    /// run, and the top bar's pill counts the same one). The live answer's *own* clock is the
    /// streaming row's in the thread (task 38) - a different fact about a different thing, so the
    /// two rows do not have to print the same number and neither borrows the other's.
    std::int64_t elapsedMs = 0;
    model::StatusChips chips;
    bool rawLogOpen = false;
    /// Compact mode (`ShellLayout::compact`): the chips collapse to `ctx 42k` plus the state mark.
    bool compact = false;
    /// The top bar draws no row this frame, so the `RAW` badge is this row's business - in
    /// compact mode (the bar is folded into the now bar) and in focus mode (task 43, the bar is
    /// hidden). Separate from `compact` because hiding the bar is not a reason to collapse the
    /// chips: focus mode hides the bar and keeps the four numbers.
    bool topBarFolded = false;
    bool running = false;
    bool stopAvailable = false;
    /// Focus mode is on (task 43), so the next `Esc` leaves the mode: the row names that step
    /// instead of leaving the user to discover it, because the terminal's way *in* (the palette's
    /// row) is not on screen once the mode is up. `stopAvailable` is false while this is true -
    /// the derivation asks the resolver, so a hint never outlives its step.
    bool focusMode = false;
    bool following = true;
    /// An open question the row shows instead of its hints - task 11's quit confirmation
    /// today. One line, and it never displaces the state or its chip: a question is not a
    /// reason to stop saying what the run is doing (the v4 row had to hide the
    /// chips to make room, which is exactly what this row does not do). While it is up the
    /// step and the clock step aside instead, and the question is drawn in the warning ink,
    /// because it is waiting for the user.
    std::string prompt;
    /// The one-line flash (design §5.25, FR-29, task 45): the newest feedback item while it is
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

// ---------------------------------------------------------------------------
// The measured row
// ---------------------------------------------------------------------------

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
/// `compactChips` records which chip form was drawn: the full four, the collapsed
/// `ctx` + state mark, or none at all (a question took their room).
/// `chipCtxText`/`chipSpeedText`/`chipModelText`/`chipStateText` hold the chip labels; an
/// empty `chipStateText` beside a non-empty `chipState` means the part draws the state
/// *mark* instead of a word.
struct NowBarParts {
    NowBarPart rule;
    NowBarPart mark;
    NowBarPart state;
    NowBarPart tail;
    NowBarPart hints;
    NowBarPart badge;
    NowBarPart chipCtx;
    NowBarPart chipSpeed;
    NowBarPart chipModel;
    NowBarPart chipState;

    std::string stateText;
    std::string tailText;
    std::string hintsText;
    std::string chipCtxText;
    std::string chipSpeedText;
    std::string chipModelText;
    std::string chipStateText;

    bool hintsTruncated = false;
    bool compactChips = false;
    /// The `hints` part holds `NowBarInput::flash` rather than the key hints (task 45): the ink and
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
/// the `RAW` badge and the chips from the right (full form, else the compact one, else the
/// state mark alone), the detail and the clock, and last the hints - the longest prefix of
/// the hint list that fits *whole*, dropped entirely below `kNowBarMinHintGu`.
///
/// An open question takes the hint slot and the detail/clock room, and then the metric
/// chips' room too if that is what it takes to still name the key that answers it: a
/// question is the one thing on this row a user has to act on. The chips step aside whole
/// (never half-drawn), and the state - word, mark and ink - stays.
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
