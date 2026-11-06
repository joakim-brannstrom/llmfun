/// Structured status → four chips. See status_chips.h for the contract.
#include "status_chips.h"

#include <charconv>
#include <cstddef>
#include <string>
#include <system_error>

namespace llmfun::tui::model {
namespace {

/// One decimal, `.` separator, locale-independent.
///
/// `snprintf("%.1f")` honours `LC_NUMERIC`, and the UI adopts the user's locale
/// on init (`tui_api.cpp` `setlocale(LC_ALL, "")`): in a comma-decimal locale it
/// turned `42k` into `42,0k`. `std::to_chars` does not look at the locale.
/// Returns 0 on a formatting error.
std::size_t formatFixed1(double value, char* buffer, std::size_t size) {
    const auto result = std::to_chars(buffer, buffer + size, value, std::chars_format::fixed, 1);
    if (result.ec != std::errc{})
        return 0;
    return static_cast<std::size_t>(result.ptr - buffer);
}

/// 0 → "0", 1234 → "1.2k", 42000 → "42k", 128000 → "128k".
///
/// The chips' own copy of the count format, so a number reads the same
/// wherever the UI shows it. The helper lives here, beside the values it
/// formats.
std::string formatCount(long long value) {
    if (value > -1000 && value < 1000)
        return std::to_string(value);
    char number[32];
    const std::size_t length =
        formatFixed1(static_cast<double>(value) / 1000.0, number, sizeof(number) - 1);
    if (length == 0)
        return std::to_string(value); // unreachable; a chip is never empty
    std::string text(number, length);
    if (text.size() > 2 && text.compare(text.size() - 2, 2, ".0") == 0)
        text.resize(text.size() - 2); // "42.0" → "42", so 42000 reads "42k"
    return text + "k";
}

/// "38.1 tok/s": one decimal and the suffix, one formatter so a chip can never
/// disagree with itself about rounding.
std::string formatRate(double value) {
    char text[40];
    const std::size_t length = formatFixed1(value, text, sizeof(text) - (sizeof(" tok/s") - 1));
    if (length == 0)
        return kMissingChipValue;
    std::string out(text, length);
    out += " tok/s";
    return out;
}

} // namespace

// The phase vocabulary

Phase phaseFromOrdinal(int ordinal) {
    // A number outside the enum would read as a neighbouring phase under a
    // cast. A phase this build cannot name is Idle.
    switch (ordinal) {
    case phaseOrdinal(Phase::Idle):
        return Phase::Idle;
    case phaseOrdinal(Phase::Thinking):
        return Phase::Thinking;
    case phaseOrdinal(Phase::Tooling):
        return Phase::Tooling;
    case phaseOrdinal(Phase::Writing):
        return Phase::Writing;
    case phaseOrdinal(Phase::Waiting):
        return Phase::Waiting;
    case phaseOrdinal(Phase::Error):
        return Phase::Error;
    case phaseOrdinal(Phase::Offline):
        return Phase::Offline;
    default:
        break;
    }
    return Phase::Idle;
}

const char* phaseWord(Phase phase) {
    switch (phase) {
    case Phase::Idle:
        return "Ready";
    case Phase::Thinking:
        return "thinking";
    case Phase::Tooling:
        return "tooling";
    case Phase::Writing:
        return "writing";
    case Phase::Waiting:
        return "waiting";
    case Phase::Error:
        return "error";
    case Phase::Offline:
        return "offline";
    }
    return "Ready";
}

const char* phaseStateWord(Phase phase) {
    switch (phase) {
    case Phase::Idle:
        return "Ready";
    case Phase::Error:
        return "Error";
    case Phase::Offline:
        return "Offline";
    case Phase::Thinking:
    case Phase::Tooling:
    case Phase::Writing:
    case Phase::Waiting:
        break;
    }
    return "Busy";
}

bool operator==(const StatusChips& a, const StatusChips& b) {
    return a.context == b.context && a.speed == b.speed && a.model == b.model &&
           a.state == b.state && a.contextUsed == b.contextUsed && a.contextMax == b.contextMax;
}

bool operator!=(const StatusChips& a, const StatusChips& b) { return !(a == b); }

StatusChips chipsFromStatus(const RuntimeStatus& status) {
    StatusChips chips;
    // A capacity of zero is not a capacity: the pair is an absence, and the absence
    // value is what the row shows (`kMissingChipValue`, never `0`).
    if (status.contextMax > 0) {
        chips.context = formatCount(status.contextUsed) + "/" + formatCount(status.contextMax);
        chips.contextUsed = status.contextUsed;
        chips.contextMax = status.contextMax;
    } else {
        chips.context = kMissingChipValue;
    }
    chips.speed = status.tokensPerSecond > 0.0 ? formatRate(status.tokensPerSecond)
                                               : std::string(kMissingChipValue);
    chips.model = status.model.empty() ? std::string(kMissingChipValue) : status.model;
    chips.state = phaseStateWord(phaseFromOrdinal(status.phase));
    return chips;
}

} // namespace llmfun::tui::model
