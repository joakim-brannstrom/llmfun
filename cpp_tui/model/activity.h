/// The Activity tab's own model (design §4.8 / §6.4, D-ACTIVITY, task 31).
///
/// Activity is a *projection* of the log records the C ABI delivers, never a second log
/// store (R-08): the one raw ring lives on the state (`TuiState::logRecords`, task 32) and
/// `appendActivity()` projects each record into the user-facing history the tab draws. The
/// projected struct has no module, function, file, line or stack field *by construction* -
/// there is nothing to leak because there is nothing to hold it - which is what makes "no
/// developer data in Activity" (R-07) a structural fact rather than a filter somebody has
/// to remember.
///
/// What the tab shows: when it happened, how bad it was and what happened, in the user's
/// words. The raw detail text stays in the Raw Log (task 33); the module/function/file/line
/// never leave the raw ring at all.
///
/// Layering: `model/` - plain data, `std::string` and the inbound C POD record (the
/// projection's input, `tui_api.h`, forward-declared here). No renderer, no `ui/`, no
/// `views/`, and no `tui.h`: the aggregate is only forward-declared, so this header can be
/// included from `ui_state.h` and a harness can drive the whole projection with no application
/// state.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// The projection's input is the C ABI's record (`tui_api.h`). It is forward-declared rather
// than included: the *definition* is needed only where the projection reads it
// (`model/activity.cpp`), and dragging the C surface into every translation unit that includes
// `ui_state.h` would put a second global `TuiState` in scope for every test that says
// `TuiState` - `./test_inspector_gui` and `./test_topbar_gui` failed exactly that way.
struct TuiLogRecord;

namespace llmfun::tui::model {

/// The aggregate the ring hangs on (`model/ui_state.h`). Forward-declared: this header is
/// included *by* that one, so it cannot include it back.
struct UiState;

/// The action a record offers (D-ACTIVITY: an id, never a display label - the UI owns the
/// word). A record without one draws no action at all.
enum class LogAction {
    None = 0,
    Retry = 1,
    Details = 2,
};

/// Activity's three severities, as the numbers that cross the ABI.
///
/// They are `TuiLogLevel_Info`/`_Warn`/`_Error` (the C enum's values), but Activity's model
/// names them itself and `model/activity.cpp` static-asserts the agreement: this header is
/// included by `ui_state.h`, and pulling the C surface in there would put a second global
/// `TuiState` in scope for every translation unit. A record therefore crosses the ABI without
/// a mapping table, and the tab never has to ask the C enum what it means.
inline constexpr int kActivityLevelInfo = 2;
inline constexpr int kActivityLevelWarn = 3;
inline constexpr int kActivityLevelError = 4;

/// One Activity row's whole content: when, how bad, what - plus what the row's action
/// would act on. Deliberately *not* a raw record: a field that is not here cannot be drawn
/// by accident, and `./test_activity` asserts the shape from the other side (a raw record
/// carrying `module`/`file`/`line` produces a projection that has none of them).
struct LogRecord {
    /// Milliseconds since the epoch, stamped at arrival when the sender had no clock
    /// (`timestampMs == 0` in the inbound record) - the row always has a time to print.
    long long timestampMs = 0;
    /// One of `kActivityLevelInfo`/`_Warn`/`_Error`: Activity's three-step severity. Trace and
    /// debug clamp to info (`activityLevel()`), so a row never claims a severity the tab
    /// cannot filter on.
    int level = kActivityLevelInfo;
    /// The plain, user-facing, one-line message. Never a formatted developer line.
    std::string message;
    /// Optional multi-line context. Carried, never rendered: it belongs to the Raw Log
    /// (task 33), which is why the Activity view has no path that prints it.
    std::string detail;
    /// The action id (`Retry`, `Details`) and what it would act on. `None` draws no action.
    LogAction action = LogAction::None;
    std::string actionPayload;
};

/// Activity keeps the last 500 records (design §5.1's store table), FIFO: the tab is the
/// recent operational history of the run, not a session-long audit trail - that is the Raw
/// Log's ring (5000 records / 2 MB, task 32).
inline constexpr std::size_t kActivityRecordsMax = 500;

/// The severity the Activity list shows (FR-15: "filter info / warning / error"). One
/// choice at a time, with `All` the default: the tab answers "what went wrong" and "what
/// happened", and a press that says `error` must not need three more to get back.
enum class ActivityFilter {
    All = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

/// Whether `record` passes `filter` and the search `query`.
///
/// The query matches the *message* only - never the detail (that is the Raw Log's text, and
/// Activity may not show it) and never a metadata field (this struct has none). An empty or
/// blanks-only query is "no search", the rail's rule. Pure, so the tab's filtering is
/// testable without a screen and the renderer and the row count can never disagree.
bool activityMatches(const LogRecord& record, ActivityFilter filter, std::string_view query);

/// Whether Activity may show a record of this level at all.
///
/// The promotion rule (design §6.4: "raw -> Activity only with a user-facing message and a
/// severity") is enforced here, in the projection's own module, and not by filtering the
/// source: the raw ring stays complete for the Raw Log. Trace and debug are developer
/// detail, so they are not promoted - Activity's severity scale has three steps
/// (info/warning/error) and a trace line dressed as `info` would be a user-facing claim
/// nobody made.
///
/// An *unknown* level is not promoted either, and the difference from `activityLevel()` is
/// deliberate: the projection folds an unrecognised number to info (something arrived and it is
/// not a warning), while the promotion gate drops it, because "a severity this binary has never
/// heard of" is not a claim Activity can make to the user. The raw ring keeps the record either
/// way, so nothing is lost - only the user-facing tab declines to interpret it.
bool activityShowsLevel(int level);

/// Activity's severity for a raw level: warning and error keep their word, everything else
/// (trace, debug, info and an unknown int off the wire) is `TuiLogLevel_Info` - the
/// three-step clamp the tab's filter is built on.
int activityLevel(int rawLevel);

/// The word a row prints for a severity (`info`, `warn`, `error`) - the half of the row's
/// meaning that does not depend on colour (D-BLOCK's rule, applied to Activity). `warn` and
/// not `warning`: see the implementation for the 28-cell arithmetic that decides it.
const char* activityLevelWord(int level);

/// Project one inbound record into what Activity may show: the timestamp, the clamped
/// severity, the message and the action id/payload.
///
/// Defined for *every* level (that is what makes it testable on its own); whether the
/// record is promoted at all is `activityShowsLevel()`, applied by `appendActivity()`.
/// The metadata fields of the argument are simply not read - the projection is the one
/// place that decides what Activity may show, and it decides by not having the fields.
LogRecord projectForActivity(const TuiLogRecord& record);

/// Append `projected` to Activity's ring (FIFO at `kActivityRecordsMax`) and count it as
/// unread while the tab is not on screen.
///
/// `rawLevel` is the *inbound* level, before the three-step clamp: this is the only place the
/// promotion rule can be applied, because `projected.level` has already folded trace and debug
/// into info (a gate on the projected value could never reject them). A level
/// `activityShowsLevel()` refuses never enters the ring and never moves the badge.
///
/// `visible` is the frame's answer (the panel is up *and* this tab is the one it shows) -
/// D-UNREAD's rule for the record stream: a record that arrives while the user is looking
/// at the tab is already read. The count is what the tab strip's badge draws and is cleared
/// the moment the tab is shown (`clearActivityUnread()`), so quiet-by-default costs one int
/// and never a copy of the ring.
void appendActivity(UiState& ui, const LogRecord& projected, int rawLevel, bool visible);

/// The tab was shown: the unread count is read, and reads to zero.
void clearActivityUnread(UiState& ui);

} // namespace llmfun::tui::model
