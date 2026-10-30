/// The live answer's clock, caret and cancel mark (design FR-08, §4.5; task 38).
///
/// Streaming is the one part of the thread that changes while nobody presses anything, so
/// it needs four facts the message store cannot answer, and they live here together
/// because they are all readings of one clock:
///
///   * when the answer started (`startedMs`) and how long it has run (`elapsedMs`, frozen
///     where the user stopped it) - the live row's header prints `AGENT · writing · 00:12`;
///   * how long the model reasoned (`thinkingElapsedMs`, 0 while it is *still* reasoning) -
///     the `Thinking · 3.2 s` row prints it once the reasoning has ended;
///   * the caret's blink phase (`caretOn`), derived from the clock and never from a frame
///     counter: a per-frame toggle would blink at whatever the frame rate happens to be,
///     which is exactly what "blinking only at 20-30 fps, never a per-frame state change"
///     forbids. The half-period is `kStreamCaretHalfPeriodMs`, so the phase changes twice a
///     second at any frame rate, and two frames inside one half-period answer the same;
///   * whether the user stopped it (`cancelled`): the caret stops, the row says
///     `cancelled`, and the *store* is untouched - the partial answer is the answer
///     (`/stop` keeps it, D-CANCEL).
///
/// Plain data and free functions, clock-injected: `refreshStreamClock()` is the one writer
/// and it takes `nowMs`, so `./test_streaming` pins the whole arithmetic with a fake clock
/// while the frame layer calls it once per frame with its own reading. Nothing here knows
/// a message, a v4 type or a view - `model/` is a value layer, so the adapter and a view
/// may both read it.
///
/// The three counters (`applied`, `coalesced`, `unchanged`) are the *store's* observations
/// of the update throttle (`tuiUpdateStreamChatMessage`, task 38): how many snapshots reached the
/// store, how many were parked because the window had not elapsed, and how many were
/// byte-identical to the text already in the store and therefore cost no repaint at all.
/// Like the adapter's `groupBuilds()`, they exist so a test can prove the throttle instead
/// of trusting it.
///
/// `applied` counts *writes*, not offers: a parked snapshot its frame flushed and the stop's
/// final write are counted there too, so the three do not partition the offers once a frame (or
/// a stop) has run - and a check that adds them up only holds inside one window.
#pragma once

#include <cstdint>

namespace llmfun::tui::model {

/// The stream update window: one snapshot per 33 ms is ~30 fps, and a faster sender is
/// coalesced rather than dropped (`tuiUpdateStreamChatMessage` parks the newest snapshot and the
/// frame after the window applies it, so the last text is never lost).
inline constexpr std::int64_t kStreamUpdateIntervalMs = 33;

/// The caret's blink half-period: 500 ms on, 500 ms off - two visible changes a second,
/// which is a blink rather than a flicker and is slower than any frame rate in play.
inline constexpr std::int64_t kStreamCaretHalfPeriodMs = 500;

/// The word the live answer's row prints when the user stopped the run.
///
/// The two *working* words are the phase vocabulary's (`phaseWord(Phase::Thinking)` and
/// `phaseWord(Phase::Writing)`), so the row and the now bar cannot drift apart. A stop is
/// not a phase there - `waiting` means the run is winding down, which is the *frame's*
/// answer - while this row is naming what became of the answer, so it says what happened.
inline constexpr char kStreamCancelledWord[] = "cancelled";

/// The live answer's clock and the caret's phase.
struct StreamState {
    /// The stream's first sighting, unix ms; 0 = no stream has been seen.
    std::int64_t startedMs = 0;
    /// The reasoning's first sighting, and the moment the answer's text began (0 = still
    /// reasoning, or no reasoning at all).
    std::int64_t thinkingStartedMs = 0;
    std::int64_t thinkingEndedMs = 0;
    /// Where the user's stop froze the clock; 0 = still running.
    std::int64_t cancelledAtMs = 0;
    /// The newest reading this stream has been refreshed at: the wall clock is not monotone
    /// (an NTP step can move it back), and an elapsed time that shrinks - or a caret that jumps
    /// phase - is exactly what a clock must never print. A frame "before" the last one is read
    /// as the same moment.
    std::int64_t lastSeenMs = 0;
    /// The update window's start: when the store last accepted a snapshot.
    std::int64_t lastAppliedMs = 0;
    /// The answer's text has begun arriving - the fact that ends the reasoning.
    bool sawText = false;

    // --- derived once per frame (refreshStreamClock) -------------------------
    /// The run's clock, frozen at the stop.
    std::int64_t elapsedMs = 0;
    /// The reasoning's duration, 0 while it is still reasoning.
    std::int64_t thinkingElapsedMs = 0;
    bool cancelled = false;
    /// The caret's phase for this frame: on for the first half-period, off for the second.
    bool caretOn = false;

    // --- observations (the store's throttle) --------------------------------
    std::uint64_t applied = 0;
    std::uint64_t coalesced = 0;
    std::uint64_t unchanged = 0;

    /// Whether a stream has been seen at all (the clock is meaningful).
    bool active() const { return startedMs != 0; }
};

/// Forget everything: `tuiStreamChatMessageClear`, `tuiClearOutput` and a session switch
/// come through here (the adapter's `clearStream()`/`reset()`), so the next stream starts
/// its own clock, uncancelled, with the caret's phase free to begin at the beginning.
void resetStreamClock(StreamState& stream);

/// Derive the clock, the caret's phase and the cancel mark from the facts the frame owns.
///
/// `hasStream` is whether a live stream exists at all (the store's flag, mirrored by the
/// caller so this header needs no store), `hasText`/`hasThinking` whether the streamed
/// message carries prose/reasoning *now*, `nowMs` the frame's clock and `cancelRequested`
/// whether the user's stop is latched. Idempotent, and monotone in `nowMs`: a second call
/// with the same reading answers the same numbers (that is what makes the caret a blink
/// and not a per-frame flip), and the reasoning's duration stays at the value it took when
/// the answer began.
void refreshStreamClock(StreamState& stream, bool hasStream, bool hasText, bool hasThinking,
                        std::int64_t nowMs, bool cancelRequested);

/// The word the live answer's row prints after its role - `writing`, still `thinking`, or
/// `cancelled`. A pointer into the phase vocabulary (or `kStreamCancelledWord`), never a
/// formatted string: the row joins it with the clock itself.
const char* streamPhaseWord(const StreamState& stream, bool hasText);

} // namespace llmfun::tui::model
