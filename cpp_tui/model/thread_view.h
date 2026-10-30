/// The thread area's non-thread states as data, independent of any renderer.
///
/// A thread with nothing in it has three things it can say instead of staying
/// blank (design 4.11, FR-19): a new session explains itself and offers
/// starter prompts, a session switch shows a skeleton, and an unreachable model
/// backend shows the error card with Retry. Which one is showing is one
/// derivation (`deriveThreadView`); the renderers are separate functions in the
/// frame code, so the decision and the picture can never disagree.
///
/// Layering: `model` depends on nothing app-specific: no ImGui, no `tui.h`, no
/// `ChatMessage`, so the derivation and the copy are testable without a
/// renderer (`test_thread_view` compiles this TU with no other project source).
#pragma once

#include <cstddef>

namespace llmfun::tui::model {

/// What the thread area shows. `Ready` is the ordinary thread (messages to
/// render); the other three replace a body that would otherwise be blank.
enum class ThreadView {
    Ready,
    Empty,
    Loading,
    Offline,
};

/// Chooses the thread area's body from the three facts the frame code has.
///
/// Precedence, highest first (each rule pinned by a case in test_thread_view):
///   1. `loadingSession` -> Loading. The thread is being replaced, so the rows
///      on screen belong to the session the user just left; the skeleton is the
///      honest body (design: "skeleton while a session loads, never a blank
///      screen").
///   2. `hasMessages` -> Ready. A transcript that exists is never hidden: an
///      outage is reported by the now bar, the disabled composer and the Retry
///      path, not by taking the conversation away.
///   3. `offline` -> Offline. Offline wins over Empty: an empty thread is
///      exactly where the actionable card belongs.
///   4. -> Empty: a new session: wordmark, one sentence, three starter actions.
ThreadView deriveThreadView(bool hasMessages, bool loadingSession, bool offline);

// ---------------------------------------------------------------------------
// Empty state (design 4.11)
// ---------------------------------------------------------------------------

/// The wordmark row.
inline constexpr char kEmptyWordmark[] = "llmfun";

/// The one sentence under the wordmark, verbatim from the design.
inline constexpr char kEmptySentence[] =
    "Ask, plan, or code \u2014 the agent can read your workarea, search the RAG, and run tools.";

/// The same sentence in the text grid's alphabet: U+2014 is East-Asian
/// *Ambiguous* (imtui counts one cell, a CJK terminal draws two) and may be
/// missing from the terminal's font, so the grid draws a hyphen. Task 15's
/// glyph table takes this substitution over; until then the two strings are
/// pinned to differ by exactly this (test_thread_view).
inline constexpr char kEmptySentenceAscii[] =
    "Ask, plan, or code - the agent can read your workarea, search the RAG, and run tools.";

/// The palette hint beside the starter actions. A label, not a binding: it is
/// the visible promise `Ctrl+K` keeps from task 10 on.
inline constexpr char kPaletteHint[] = "Ctrl+K";

/// One starter action: the button label and the text it *fills* the composer
/// with. Filling is the whole contract: a starter action never submits, so no
/// query leaves the UI without the user pressing Send (FR-19).
struct StarterAction {
    const char* label;
    const char* prompt;
};

/// The starter actions, in the design's order. A new action belongs in this
/// table, not in a renderer: both profiles and the tests read it from here.
inline constexpr StarterAction kStarterActions[] = {
    {"Summarize this workarea",
     "Summarize this workarea: what does it do, how is it laid out, and what should I know "
     "first?"},
    {"Plan a feature", "Plan a feature for this workarea: "},
    {"Find failing tests", "Find the failing tests and explain the root cause of each."},
};
inline constexpr std::size_t kStarterActionCount =
    sizeof(kStarterActions) / sizeof(kStarterActions[0]);

// ---------------------------------------------------------------------------
// Loading state (design 4.11)
// ---------------------------------------------------------------------------

/// Skeleton rows while a session loads: three, at every width.
inline constexpr int kSkeletonRows = 3;

/// One full shimmer cycle of the pixel profile. The text grid never animates:
/// a terminal capture has to stay byte-stable.
inline constexpr double kSkeletonShimmerSeconds = 1.2;

/// The skeleton glyph in the text grid. Three plain ASCII dots, not U+2026:
/// that codepoint is East-Asian *Ambiguous* (one cell in imtui, two in a CJK
/// terminal) and may be missing from the cell font. The pixel profile draws a
/// pulsing dim bar instead of a glyph, so this constant has exactly one user.
inline constexpr char kSkeletonGlyphAscii[] = "...";

// ---------------------------------------------------------------------------
// Offline state (design 4.11)
// ---------------------------------------------------------------------------

/// The error card's headline: `!` is the design's glyph for the offline phase.
inline constexpr char kOfflineHeadline[] = "! model server unreachable";

/// One sentence explaining what to do about it. The endpoint itself is the
/// model's business, not the UI's, so the card never names a host or a port.
inline constexpr char kOfflineDetail[] =
    "The model endpoint did not answer. Check that the server is running, then retry.";

/// The retry action's label. `Open settings` joins it when the settings sheet
/// exists (task 34); the MVP card carries the one action it can honour.
inline constexpr char kRetryLabel[] = "Retry";

/// Why Retry can be unavailable: the card is shown before any query ever went
/// out, so there is nothing to replay.
inline constexpr char kRetryNothingYet[] =
    "Nothing to retry yet - send a query once the server is back";

/// Why Retry can be unavailable while a run is still active: the composer's own
/// action is `Stop` then, and Retry must not become a second way in.
inline constexpr char kRetryRunActive[] = "Stop the running agent before retrying";

/// The tooltip on the composer while the backend is unreachable. The composer
/// is disabled rather than hidden: the user's draft stays visible, and this is
/// what the disabled field says on hover.
inline constexpr char kComposerOfflineTooltip[] =
    "The model server is unreachable - Retry from the thread when it answers again";

} // namespace llmfun::tui::model
