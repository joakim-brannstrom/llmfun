/// Short-lived feedback for actions whose outcome is not in the thread (design §5.25, FR-29,
/// task 45): a *toast* in the pixel profile's stack, a one-line *flash* in the text grid's now
/// bar.
///
///     model::raiseToast(ui, model::Toast{model::ToastKind::Success, "Copied ANSWER", {}},
///                       model::agentClockMs(), m.textGrid);
///
/// One queue, two presentations. The item is raised where the action happened (the block's copy
/// path, the Raw Log's save), and the *frame* owns what happens next: it expires the transients
/// once per frame (`expireToasts`, the injected clock) and the presentation reads the newest item
/// (`topToast`). The two surfaces therefore cannot disagree about what the user was told, which is
/// the same rule the status chips follow for the numbers (§6.3).
///
/// What is *not* a piece of feedback, by design: anything that belongs in the thread. An answer, a
/// tool result, a failed tool call and the run's progress are the transcript's, and a toast for
/// one of them would be a second, disappearing copy of a fact the user has to be able to re-read.
/// The rule is the design's own ("never use a toast for something that belongs in the thread") and
/// it is why this module has no producer for a message: every call site is a *control's* answer.
///
/// Lifetimes (task 45's table, which is the design's §5.25 with the numbers the task fixes):
///
///     profile | info  | success | warn / error
///     pixel   | 4 s   | 8 s     | persist until acknowledged
///     grid    | 2 s   | 2 s     | persist until acknowledged
///
/// A persistent item carries `expiresAtMs == 0`, so no clock can retire it: the user has to answer
/// it (the grid's `x`, the pixel profile's dismiss control). That is the whole difference between
/// the two kinds - no timer, no severity that changes what the item *is*.
///
/// Layering: plain data and pure functions, no renderer, no `ui::`, no `tui.h` - `model/ui_state.h`
/// includes this header for the queue and a harness drives it with no ImGui context at all.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace llmfun::tui::model {

struct UiState;

/// How bad the news is. Four values rather than three because the design's §5.25 table separates
/// a warning (a thing that is not broken but needs attention: a read-only profile, a save that
/// could not write) from an error (a thing that failed) - and the two draw in different inks.
enum class ToastKind { Info = 0, Success, Warn, Error };

/// A feedback item: one line, an optional second line, and how long it lives.
///
/// The strings are *finished* text: the producer wrote the words it wants shown (a path, a block's
/// label, the reason a write failed), and no presentation reformats them. A view clips; it never
/// rebuilds a sentence, for the same reason the status chips arrive formatted.
struct Toast {
    ToastKind kind = ToastKind::Info;
    std::string title;
    /// The detail line, drawn under the title. Empty is the ordinary case: a copy's answer is one
    /// word and the label it copied.
    std::string detail;
    /// The frame clock reading after which a *transient* item is dropped; 0 means "persist until
    /// acknowledged" and is the only value a warn/error ever carries. `raiseToast()` sets it, so a
    /// producer never computes a lifetime of its own.
    std::int64_t expiresAtMs = 0;
};

/// Whether the item has to be answered before it goes away (`warn` and `error`).
constexpr bool toastKindPersists(ToastKind kind) {
    return kind == ToastKind::Warn || kind == ToastKind::Error;
}

/// How long a transient item lives, in milliseconds; 0 for the two that persist.
///
/// `textGrid` selects the profile's column of the table above. It is a parameter rather than a
/// lookup because `model/` may not ask which renderer is up (`ui/metrics.h` is the only module that
/// answers that) - the caller knows its profile, and the two numbers are the whole difference.
std::int64_t toastLifetimeMs(ToastKind kind, bool textGrid);

/// How many items a profile keeps (`kToastMaxGui` / `kToastMaxGrid`).
std::size_t toastCapacity(bool textGrid);

/// The pixel profile's stack depth (design §5.25: "max 3 stacked").
inline constexpr std::size_t kToastMaxGui = 3;
/// The text grid's depth: one line in the now bar (design §5.25: "a single-line flash").
inline constexpr std::size_t kToastMaxGrid = 1;

/// Raise one item: stamp its lifetime, drop a duplicate it repeats, and keep the queue inside the
/// profile's depth.
///
/// `nowMs` is the caller's clock (`model::agentClockMs()` on the frame thread, a fixture in a
/// harness), so a lifetime is a number a test can read instead of a duration it has to wait for.
///
/// The three rules, in the order they are applied:
///
///   * **a repeat is not a second item.** Raising the same `(kind, title, detail)` as the newest
///     item refreshes that one's lifetime instead of pushing a copy: two `y` presses on one block
///     are one answer on screen, not two that would stack (the design's "one action, one item").
///   * **the queue is bounded.** One item over the cap evicts the oldest item that *may* go: a
///     transient first, and a persistent one only when every item is persistent. So an
///     unacknowledged warning is never pushed out by a success that arrived after it.
///   * **a transient never displaces a persistent item.** A grid row holds one line and a warning
///     that has not been answered owns it: the incoming transient is dropped rather than shown in
///     a queue nobody can see. (A warning raised over a warning evicts the older one - both are
///     persistent, and the newer is the one the user just earned.)
void raiseToast(UiState& ui, Toast toast, std::int64_t nowMs, bool textGrid);

/// Drop the items whose time is up (`nowMs >= expiresAtMs`, and only for a non-zero expiry).
///
/// Called once per frame by the frame layer. Idempotent, so a harness may call it with any clock
/// reading it likes - including a reading before the raise, which drops nothing.
void expireToasts(UiState& ui, std::int64_t nowMs);

/// Acknowledge the item the presentations show - the newest one, `topToast()` - and answer whether
/// there was one.
///
/// This is the key's answer (`x`, plan/key_gate.md §4i) and it is the *newest* item on purpose: the
/// grid's flash and its `x dismiss` hint are derived from `topToast()`, so a press that removed a
/// different item would answer something the user was not looking at. On the grid the queue holds
/// one item, so the two readings coincide; in the pixel profile the card's own `Dismiss` (or its
/// cross) answers *that* card instead (`dismissToastAt`), and `x` answers the card nearest the
/// corner - the newest, which is the one the stack puts at the bottom.
bool dismissShownToast(UiState& ui);

/// Acknowledge one item by position in the queue (`[0]` is the oldest). Out of range answers
/// false and changes nothing - a stale index from a previous frame cannot drop the wrong item.
///
/// The pixel profile's per-card controls use this: a card knows which index it was drawn for, and
/// dropping that index in the frame of the press is what keeps "the card I clicked" and "the card
/// that went away" the same one.
bool dismissToastAt(UiState& ui, std::size_t index);

/// The item a presentation shows first: the newest one, or nullptr when the queue is empty.
///
/// One accessor for the grid's flash and the pixel profile's stack (which draws the newest at the
/// bottom): "the newest item" is the same answer for both, and a presentation that indexed the
/// deque itself would be a second definition of it.
const Toast* topToast(const UiState& ui);

} // namespace llmfun::tui::model
