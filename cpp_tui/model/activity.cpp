/// The Activity projection (D-ACTIVITY, task 31). `model/activity.h` owns the contract.
#include "activity.h"

// The inbound record's definition: this is the one file that reads its fields.
#include "../tui_api.h"

#include "ui_state.h"
namespace llmfun::tui::model {

// The model's three severities are the C enum's values: a record crosses the ABI without a
// mapping table, and a renumbering of either side fails the build instead of quietly
// relabelling every row.
static_assert(kActivityLevelInfo == TuiLogLevel_Info, "Activity's info is TuiLogLevel_Info");
static_assert(kActivityLevelWarn == TuiLogLevel_Warn, "Activity's warn is TuiLogLevel_Warn");
static_assert(kActivityLevelError == TuiLogLevel_Error, "Activity's error is TuiLogLevel_Error");

bool activityShowsLevel(int level) {
    // The three severities Activity draws, and nothing else. Written as the positive
    // condition (rather than "not trace and not debug") so an unknown value off the wire is
    // *not* promoted by accident: an unrecognised level has no user-facing meaning to add.
    return level == kActivityLevelInfo || level == kActivityLevelWarn ||
           level == kActivityLevelError;
}

int activityLevel(int rawLevel) {
    if (rawLevel == kActivityLevelWarn)
        return kActivityLevelWarn;
    if (rawLevel == kActivityLevelError)
        return kActivityLevelError;
    // trace, debug, info, and every value the enum does not name: the tab's lowest step.
    return kActivityLevelInfo;
}

const char* activityLevelWord(int level) {
    if (level == kActivityLevelWarn)
        // `warn`, not `warning`: the docked body is 26 cells wide and the four chips spelled
        // with the long word measure 27 (`all` 5, `info` 6, `warning` 9, `error` 7) - the last
        // chip would silently fall off a filter the user cannot reach. Spelled short they
        // measure 24 and fit. The design's own glyph line uses the short word too
        // ("i (info) / ! (warn) / x (error)").
        return "warn";
    if (level == kActivityLevelError)
        return "error";
    return "info";
}

LogRecord projectForActivity(const TuiLogRecord& record) {
    LogRecord out;
    // The clock: the sender's, unless it had none (0 = "the UI stamps arrival", and the
    // arrival stamp is written by the ABI's record copy, so a projection taken from a
    // record that never crossed the ABI keeps 0 - the row then prints no time rather than
    // an invented one).
    out.timestampMs = record.timestampMs;
    out.level = activityLevel(record.level);
    // Only the two user-facing strings are read. `module`, `function`, `file`, `line`,
    // `reserved` and the raw `detail` of the argument stay where they are; `detail` is
    // carried because the *projection* decides what Activity may show, and the Raw Log
    // (task 33) reads the ring, not this struct.
    out.message =
        record.message.data ? std::string(record.message.data, record.message.len) : std::string();
    out.detail =
        record.detail.data ? std::string(record.detail.data, record.detail.len) : std::string();
    switch (record.action) {
    case TuiLogAction_Retry:
        out.action = LogAction::Retry;
        break;
    case TuiLogAction_Details:
        out.action = LogAction::Details;
        break;
    default:
        // None, and anything off the wire: a row with no action. An unknown id is not
        // guessed into a button that would then act on the wrong thing.
        out.action = LogAction::None;
        break;
    }
    out.actionPayload = record.actionPayload.data
                            ? std::string(record.actionPayload.data, record.actionPayload.len)
                            : std::string();
    return out;
}

namespace {

/// Lower-case both sides once per comparison: a search for `Read_File` finds `read_file`.
/// ASCII-only on purpose - the messages are the app's own English strings, and a full
/// Unicode case fold would need a table this layer does not have.
char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool containsFold(std::string_view haystack, std::string_view needle) {
    if (needle.empty())
        return true;
    if (needle.size() > haystack.size())
        return false;
    for (std::size_t at = 0; at + needle.size() <= haystack.size(); ++at) {
        std::size_t i = 0;
        while (i < needle.size() && lowerAscii(haystack[at + i]) == lowerAscii(needle[i]))
            ++i;
        if (i == needle.size())
            return true;
    }
    return false;
}

/// The query with its leading and trailing blanks removed - "   " is no search at all.
std::string_view trimmed(std::string_view text) {
    const auto blank = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    while (!text.empty() && blank(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && blank(text.back()))
        text.remove_suffix(1);
    return text;
}

} // namespace

bool activityMatches(const LogRecord& record, ActivityFilter filter, std::string_view query) {
    switch (filter) {
    case ActivityFilter::Info:
        if (record.level != kActivityLevelInfo)
            return false;
        break;
    case ActivityFilter::Warn:
        if (record.level != kActivityLevelWarn)
            return false;
        break;
    case ActivityFilter::Error:
        if (record.level != kActivityLevelError)
            return false;
        break;
    case ActivityFilter::All:
        break;
    }
    return containsFold(record.message, trimmed(query));
}

void appendActivity(UiState& ui, const LogRecord& projected, int rawLevel, bool visible) {
    // The promotion rule (§6.4), applied at the only place that can apply it: the *raw* level,
    // because `projected.level` has already been clamped to the three-step scale. A trace or
    // debug line stays in the Raw Log's ring and never becomes a user-facing row.
    if (!activityShowsLevel(rawLevel))
        return;
    ui.activity.push_back(projected);
    // FIFO at the design's 500: the oldest row leaves, and the unread count is *not*
    // decremented - it counts arrivals the user has not read, not rows the ring still has.
    while (ui.activity.size() > kActivityRecordsMax)
        ui.activity.pop_front();
    if (!visible)
        ++ui.unreadActivity;
}

void clearActivityUnread(UiState& ui) { ui.unreadActivity = 0; }

} // namespace llmfun::tui::model
