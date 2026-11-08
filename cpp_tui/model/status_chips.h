/// Status chips: the four status facts as data, independent of any renderer.
///
/// The status area answers four questions — how much context, how fast, which
/// model, what state — and shows one chip per answer. The answers travel as the
/// structured status D sends every tick (`tuiSetRuntimeStatus`), and this module
/// owns the one formatting of those numbers into the four chips.
///
/// Layering: `model` depends on nothing app-specific — no ImGui, no `tui.h`, no
/// `ChatMessage` — so the formatter is testable without a renderer
/// (`test_status_chips` compiles this TU with no other project source).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace llmfun::tui::model {

/// The UI's one phase vocabulary: seven states, one
/// word each, shared by the now bar, the status chips, the composer's phase line and
/// the rail's running mark. It lives here, beside `RuntimeStatus`, because
/// `RuntimeStatus::phase` *is* this vocabulary's ordinal - and because a view that
/// reads the bars must not have to include the whole state aggregate for it.
///
/// The numbers match D's `TuiRuntimeStatus.phase`, so a run status crosses the
/// surface without a translation table. A number this build has no member for
/// answers `Idle` (`phaseFromOrdinal()`).
// TODO: share this enum with D directly (via ImportC) instead of keeping the
// numbers in sync by hand.
enum class Phase : int {
    Idle = 0,
    Thinking = 1,
    Tooling = 2,
    Writing = 3,
    Waiting = 4,
    Error = 5,
    Offline = 6,
};

/// Seven, spelled out so a new phase trips the walk in `./test_nowbar`.
inline constexpr int kPhaseCount = 7;

/// The `Phase` the run status's phase number names. D reports the phase as a
/// plain int, and the number can name a phase this build has no member for.
/// Such a number answers `Idle`, so the UI shows "Ready" instead of a phase
/// it cannot name.
Phase phaseFromOrdinal(int ordinal);

/// The ordinal of `phase` - the inverse of `phaseFromOrdinal()`, for the one place a
/// phase has to travel back to D or into a log record. `constexpr`, so the ordinal table
/// can be written as a `switch` over named phases instead of a second copy of the numbers.
constexpr int phaseOrdinal(Phase phase) { return static_cast<int>(phase); }

/// The one word a phase is called by: the now bar's left side, a log line.
/// `Ready` for idle and lower case for the six others, so one state is never two words.
const char* phaseWord(Phase phase);

/// The coarse state for the fourth chip (`Ready|Busy`).
///
/// The design gives that chip two words, and a run is `Busy` in all four of its
/// phases - the fine-grained phase belongs to the now bar's left side, not to the
/// chip. The two phases the numbers cannot express as work (a failed run, an
/// unreachable model) keep a word of their own, because `Busy` would claim work
/// that is not happening.
const char* phaseStateWord(Phase phase);

/// The structured status D sends (`tuiSetRuntimeStatus`).
/// Numbers, not sentences: the view formats them once per value change.
/// `phase` is the ordinal of the UI's own phase vocabulary (see `Phase` above),
/// and the struct mirrors the ABI's `TuiRuntimeStatus` field for field.
struct RuntimeStatus {
    int phase = 0;      ///< phase ordinal (`Phase`)
    std::string detail; ///< current tool or model step, one line
    std::string model;  ///< model display name, as D reports it ("none" when none)
    std::int64_t contextUsed = 0;
    std::int64_t contextMax = 0;
    std::int64_t compactAt = 0;
    double tokensPerSecond = 0.0;
    std::int64_t elapsedMs = 0;
    /// Forward-compat: transported and round-trip-tested, but nothing sends it and
    /// nothing reads it yet (the run's pending question count will). Stays 0.
    int pendingQuestions = 0;
};

/// The four chips' names, in the row's order - the words a document, a golden or the help page
/// calls them. They are the names of `StatusChips`' four members, so a reader can match a chip to
/// the field that carries it (the now bar and the composer's meter read the same struct).
inline constexpr const char* kStatusChipNames[] = {"context", "speed", "model", "state"};
inline constexpr std::size_t kStatusChipNameCount =
    sizeof(kStatusChipNames) / sizeof(kStatusChipNames[0]);

/// The four chips — the values only. The `ctx ` label and the separators belong
/// to the view: it is the view that decides which chips fit on the row, and a
/// value must not change with that decision.
struct StatusChips {
    std::string context; ///< "42k/128k" — used/max, abbreviated (never "ctx …")
    std::string speed;   ///< "38.1 tok/s"
    std::string model;   ///< "qwen3-coder", "none"
    std::string state;   ///< "Ready" or "Busy"

    /// The numbers `context` was formatted from, kept so a *view* that needs the
    /// fraction (the composer's mini meter) does not have to parse the
    /// chip back - a second grammar for the same sentence is a second answer waiting
    /// to disagree with the first. `contextMax == 0` means the status carried no
    /// usable pair (the chip is then `kMissingChipValue`), and a meter must not
    /// divide by it.
    std::int64_t contextUsed = 0;
    std::int64_t contextMax = 0;
};

/// Pins the struct's shape to the name list above: a fifth chip changes the size and fails here
/// instead of leaving the help page naming four chips that no longer match the row.
static_assert(sizeof(StatusChips) == 4 * sizeof(std::string) + 2 * sizeof(std::int64_t),
              "a new chip needs a name in kStatusChipNames (and its own prose)");

/// The chip value for a field the status does not carry - a `contextMax` of 0
/// (no capacity known), a `tokensPerSecond` of 0 (no rate yet) or an empty
/// `model`. A missing value is never rendered as "0": zero is a number, this
/// is its absence. The view may draw an ASCII floor glyph instead (U+2014 is
/// East-Asian *Ambiguous*, so a CJK terminal could render it two cells wide).
inline constexpr const char* kMissingChipValue = "\u2014"; // em dash

/// The chips as a value: two sets are equal when every field agrees. The frame
/// layer's "did the chip source change" test compares instead of re-deriving.
bool operator==(const StatusChips& a, const StatusChips& b);
bool operator!=(const StatusChips& a, const StatusChips& b);

/// The chips for one structured status.
///
/// `contextUsed/contextMax` → `42k/128k`, `tokensPerSecond` → `38.1 tok/s`, the
/// fourth chip from the phase (`phaseStateWord`), and the model chip straight
/// from `status.model`.
///
/// Absence is `kMissingChipValue`, never `0`: a `contextMax` of 0 is "no capacity
/// known", a `tokensPerSecond` of 0 is "no rate yet", and an empty `model` is
/// "no name reported". Numbers are decimal and locale-independent (no
/// `strtod`/`snprintf`, so `LC_NUMERIC` cannot turn `38.1` into `38,1`).
StatusChips chipsFromStatus(const RuntimeStatus& status);

} // namespace llmfun::tui::model
