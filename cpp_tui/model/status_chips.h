/// Status chips: the four status facts as data, independent of any renderer.
///
/// The status area answers four questions — how much context, how fast, which
/// model, what state — and shows one chip per answer. Until D sends structured
/// status (task 39) the only source is the one line D formats in
/// `formatStatusText()` (`source/llm/app_agent/ui.d:421-425`), so this module
/// owns the documented split of that sentence into the four chips (design §6.3,
/// FR-12).
///
/// Layering: `model` depends on nothing app-specific — no ImGui, no `tui.h`, no
/// `ChatMessage` — so the splitter is testable without a renderer
/// (`test_status_chips` compiles this TU with no other project source).
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace llmfun::tui::model {

/// The UI's one phase vocabulary (D-PHASE, design §4.7/§5.11): seven states, one
/// word each, shared by the now bar, the status chips, the composer's phase line and
/// the rail's running mark. It lives here, beside `RuntimeStatus`, because
/// `RuntimeStatus::phase` *is* this vocabulary's ordinal - and because a view that
/// reads the bars must not have to include the whole state aggregate for it.
///
/// The ordinal is the ABI's (`TuiRuntimeStatus.phase`, task 39), and it is *mapped*,
/// never cast: an ordinal this build does not know answers `Idle` instead of turning
/// into whichever phase happens to sit next to it.
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

/// The phase an ABI ordinal names; anything outside `0..6` is `Idle` (task 39:
/// "unknown phases clamp to idle with a debug log, never a crash").
Phase phaseFromOrdinal(int ordinal);

/// The ordinal of `phase` - the inverse of `phaseFromOrdinal()`, for the one place a
/// phase has to travel back to D or into a log record. `constexpr`, so the ordinal table
/// can be written as a `switch` over named phases instead of a second copy of the numbers.
constexpr int phaseOrdinal(Phase phase) { return static_cast<int>(phase); }

/// The one word a phase is called by: the now bar's left side, the composer's phase
/// line, a log line. `Ready` for idle and lower case for the six others - the top
/// bar's pill vocabulary (`views::pillLabel`), so one state is never two words.
const char* phaseWord(Phase phase);

/// The coarse state for the fourth chip (`Ready|Busy`, design §4.7).
///
/// The design gives that chip two words, and a run is `Busy` in all four of its
/// phases - the fine-grained phase belongs to the now bar's left side, not to the
/// chip. The two phases the *sentence* cannot express (a failed run, an unreachable
/// model) keep a word of their own, because `Busy` would claim work that is not
/// happening. This is also what lets the structured and the split path agree: D
/// writes `Ready` for idle and `Busy` while a run is in flight, which is exactly this
/// mapping.
const char* phaseStateWord(Phase phase);

/// The structured status D will send (task 39 → `tuiSetRuntimeStatus`).
/// Numbers, not sentences (design §6.3): the view formats them once per value
/// change. `phase` is the ordinal of the UI's own phase vocabulary (see `Phase`
/// above); `structured == false` means the chips came from the sentence
/// splitter instead of this struct.
struct RuntimeStatus {
    int phase = 0;      ///< phase ordinal (task 28's `Phase`)
    std::string detail; ///< current tool or model step, one line
    long long contextUsed = 0;
    long long contextMax = 0;
    long long compactAt = 0;
    double tokensPerSecond = 0.0;
    long long elapsedMs = 0;
    int pendingQuestions = 0;
    bool structured = false; ///< false = derived from the status sentence
};

/// The four chips' names, in the row's order - the words a document, a golden or the help page
/// calls them. They are the names of `StatusChips`' four members, so a reader can match a chip to
/// the field that carries it (the now bar and the composer's meter read the same struct).
inline constexpr const char* kStatusChipNames[] = {"context", "speed", "model", "state"};
inline constexpr std::size_t kStatusChipNameCount =
    sizeof(kStatusChipNames) / sizeof(kStatusChipNames[0]);

/// The four chips — the values only. The `ctx ` label and the separators belong
/// to the view: it is the view that decides which chips fit on the row, and a
/// value must not change with that decision. Task 39's structured path has to
/// reproduce these strings byte-for-byte for identical numbers (design §6.3:
/// "both paths must produce identical chips for the same values").
struct StatusChips {
    std::string context; ///< "42k/128k" — used/max, abbreviated (never "ctx …")
    std::string speed;   ///< "38.1 tok/s"
    std::string model;   ///< "qwen3-coder", "none"
    std::string state;   ///< "Ready", "Busy", or an unmatched sentence verbatim

    /// The numbers `context` was formatted from, kept so a *view* that needs the
    /// fraction (the composer's mini meter, design §4.6) does not have to parse the
    /// chip back - a second grammar for the same sentence is a second answer waiting
    /// to disagree with the first. `contextMax == 0` means the sentence carried no
    /// usable pair (the chip is then `kMissingChipValue` or a partial pair), and a
    /// meter must not divide by it.
    long long contextUsed = 0;
    long long contextMax = 0;
};

/// Pins the struct's shape to the name list above: a fifth chip changes the size and fails here
/// instead of leaving the help page naming four chips that no longer match the row.
static_assert(sizeof(StatusChips) == 4 * sizeof(std::string) + 2 * sizeof(long long),
              "a new chip needs a name in kStatusChipNames (and its own prose)");

/// The chip value for a field the sentence does not carry — absent *or* present
/// but empty (`Context: 1/ tokens`). A missing value is never rendered as "0"
/// (design §6.3): zero is a number, this is its absence. The view may draw an
/// ASCII floor glyph for it in the text grid (U+2014 is East-Asian *Ambiguous*,
/// so a CJK terminal could render it two cells wide); the model value is always
/// this one.
inline constexpr const char* kMissingChipValue = "\u2014"; // em dash

/// The sentence the UI falls back to while D has not sent one — `tuiCreateState`
/// seeds it, and the splitter derives its chips from it, so the status row is
/// never empty and never shows a placeholder the app does not mean.
inline constexpr const char* kDefaultStatusSentence = "Context: 0/0 tokens | Model: none | Ready";

/// Splits the status sentence into the four chips.
///
/// Grammar: `Context: <used>/<max> tokens | <x> tok/s | Model: '<m>' | Ready|Busy`.
/// The colon after `Context` is optional (D writes the sentence without one),
/// the quotes around the model are optional, and fields may be missing, empty or
/// out of order. Fragments are split on the exact `" | "` separator; a quoted
/// model name may contain it, in which case the splitter keeps reading until the
/// closing quote rather than truncating the name. Numbers are decimal and
/// locale-independent (no `strtod`/`snprintf`, so `LC_NUMERIC` cannot turn `38.1`
/// into a rejected field or `42k` into `42,0k`). Every fragment that is not one
/// of the three metrics is part of the `state` chip, verbatim and in source
/// order, so nothing the sentence carries is dropped and nothing is invented. An
/// empty sentence yields four `kMissingChipValue` chips.
StatusChips splitStatusText(std::string_view sentence);

/// The chips as a value: two sets are equal when every field agrees. The
/// structured/split identity test (design §6.3) and the frame layer's "did the
/// chip source change" test both compare instead of re-deriving.
bool operator==(const StatusChips& a, const StatusChips& b);
bool operator!=(const StatusChips& a, const StatusChips& b);

/// The chips for a structured status (task 39's `TuiRuntimeStatus`, D-STATUS).
///
/// Formatted with the splitter's own formatters, so the two paths cannot produce
/// two different strings for the same numbers (`./test_nowbar` pins the identity):
/// `contextUsed/contextMax` → `42k/128k`, `tokensPerSecond` → `38.1 tok/s`, and the
/// fourth chip from the phase (`phaseStateWord`).
///
/// `model` is the name the ABI struct does not carry: `TuiRuntimeStatus` has no
/// model field (a model changes when the user changes it, not per status tick), so
/// the sentence's chip stays the source for that one value.
///
/// Absence is `kMissingChipValue`, never `0`: a `contextMax` of 0 is "no capacity
/// known" and a `tokensPerSecond` of 0 is "no rate yet" - the rule the splitter
/// applies to a field its sentence does not carry.
StatusChips chipsFromStatus(const RuntimeStatus& status, const std::string& model);

} // namespace llmfun::tui::model
