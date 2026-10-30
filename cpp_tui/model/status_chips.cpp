/// Status sentence → four chips. See status_chips.h for the grammar.
#include "status_chips.h"

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace llmfun::tui::model {
namespace {

/// The separator D writes between fields.
constexpr std::string_view kFieldSeparator = " | ";
constexpr std::string_view kContextField = "Context";
constexpr std::string_view kTokensSuffix = " tokens";
constexpr std::string_view kSpeedSuffix = " tok/s";
constexpr std::string_view kModelField = "Model:";
constexpr char kModelQuote = '\'';

std::string_view trim(std::string_view text) {
    constexpr std::string_view blanks = " \t";
    const std::size_t first = text.find_first_not_of(blanks);
    if (first == std::string_view::npos)
        return {};
    const std::size_t last = text.find_last_not_of(blanks);
    return text.substr(first, last - first + 1);
}

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// A plain decimal number and nothing else: optional minus, digits, at most one
/// decimal point. A closed grammar on purpose — `strtod`'s wider one ("inf",
/// "nan", "0x1p3", "1e5", a leading "+") would put words on the row that the chip
/// vocabulary does not have. Also independent of the C locale: no `strtod`, no
/// `LC_NUMERIC` (a de_DE user would otherwise see `38.1` rejected).
bool isDecimalNumber(std::string_view text) {
    if (text.empty())
        return false;
    std::size_t i = 0;
    if (text[i] == '-')
        ++i;
    bool digits = false;
    bool point = false;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            digits = true;
            continue;
        }
        if (c == '.' && !point) {
            point = true;
            continue;
        }
        return false;
    }
    return digits;
}

/// Whole-string integer parse: "42000" yes, "42000 tokens" no, "" no.
///
/// The empty string is rejected on purpose: that is the shape a *absent* field
/// takes (`Context: 1/ tokens`), and an absence is "—", never 0.
bool parseCount(std::string_view text, long long& out) {
    if (text.empty() || !isDecimalNumber(text))
        return false;
    long long value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return false;
    out = value;
    return true;
}

/// Whole-string rate parse under the same closed grammar (and the same empty
/// rejection): "38.1" yes, "38.1x" no, "inf" no, "" no.
bool parseRate(std::string_view text, double& out) {
    if (!isDecimalNumber(text))
        return false;
    double value = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return false;
    out = value;
    return true;
}

/// One decimal, `.` separator, locale-independent.
///
/// `snprintf("%.1f")` honours `LC_NUMERIC`, and the UI adopts the user's locale
/// on init (`tui_api.cpp` `setlocale(LC_ALL, "")`): in a comma-decimal locale it
/// turned `42k` into `42,0k` and rejected `38.1` outright. `std::to_chars` does
/// not look at the locale. Returns 0 on a formatting error.
std::size_t formatFixed1(double value, char* buffer, std::size_t size) {
    const auto result = std::to_chars(buffer, buffer + size, value, std::chars_format::fixed, 1);
    if (result.ec != std::errc{})
        return 0;
    return static_cast<std::size_t>(result.ptr - buffer);
}

/// 0 → "0", 1234 → "1.2k", 42000 → "42k", 128000 → "128k".
///
/// This is `ui::formatCount`'s documented contract (task 14). That module does
/// not exist yet and the chips are canonical for the numbers, so the splitter
/// carries the single copy; when the chips move onto `ui/text.h` (task 25/28)
/// this function is deleted, not duplicated.
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

/// "Context: 42000/128000 tokens" and "Context 41/8192 tokens" both → "42k/128k".
///
/// The colon is optional on purpose: D formats the sentence without one
/// (`app_agent/ui.d:422`), the startup default carries one, and the splitter has
/// to read what either producer actually sends.
bool parseContextField(std::string_view fragment, std::string& out, long long& usedOut,
                       long long& maxOut) {
    if (!startsWith(fragment, kContextField))
        return false;
    std::string_view body = fragment.substr(kContextField.size());
    if (!body.empty() && body.front() == ':') {
        body = body.substr(1);
    } else if (!body.empty() && body.front() != ' ') {
        return false; // "Contextual: …" is not the context field
    }
    body = trim(body);
    if (endsWith(body, kTokensSuffix))
        body = trim(body.substr(0, body.size() - kTokensSuffix.size()));
    const std::size_t slash = body.find('/');
    if (slash == std::string_view::npos)
        return false;
    long long used = 0;
    long long max = 0;
    if (!parseCount(trim(body.substr(0, slash)), used) ||
        !parseCount(trim(body.substr(slash + 1)), max))
        return false;
    out = formatCount(used) + "/" + formatCount(max);
    usedOut = used;
    maxOut = max;
    return true;
}

/// "38.1 tok/s": one decimal and the suffix, exactly what `parseSpeedField` accepts
/// back. One formatter for the split path and the structured path, so the two cannot
/// disagree about rounding (design §6.3).
std::string formatRate(double value) {
    char text[40];
    const std::size_t length = formatFixed1(value, text, sizeof(text) - kSpeedSuffix.size());
    if (length == 0)
        return kMissingChipValue;
    std::string out(text, length);
    out += kSpeedSuffix;
    return out;
}

/// "38.1 tok/s" → "38.1 tok/s": one decimal, which is the format D writes — the
/// chip never shows a different number than the sentence carried.
bool parseSpeedField(std::string_view fragment, std::string& out) {
    if (!endsWith(fragment, kSpeedSuffix))
        return false;
    const std::string_view number = trim(fragment.substr(0, fragment.size() - kSpeedSuffix.size()));
    double value = 0.0;
    if (!parseRate(number, value))
        return false;
    out = formatRate(value);
    return out != kMissingChipValue;
}

/// Reads the model field that starts on fragment `index` and returns the index of
/// the last fragment it consumed (the caller continues after it).
///
/// A quoted name may contain the field separator (`Model: 'a | b'`), so an open
/// quote keeps consuming fragments instead of truncating the name at the first
/// `" | "`. The value ends at the closing quote: text after it is not part of the
/// name, so it is handed back through `trailing` and becomes a state fragment.
/// An unterminated quote keeps what the sentence actually carried.
std::size_t parseModelField(const std::vector<std::string_view>& fragments, std::size_t index,
                            std::string& out, std::string& trailing) {
    const std::string_view field = trim(fragments[index]);
    const std::string_view value = trim(field.substr(kModelField.size()));
    if (value.empty()) {
        out = kMissingChipValue; // "Model:" with no value carries no model
        return index;
    }
    if (value.front() != kModelQuote) {
        out = std::string(value); // unquoted, e.g. the default's "none"
        return index;
    }
    std::string name;
    std::string_view piece = value.substr(1);
    for (;;) {
        const std::size_t close = piece.find(kModelQuote);
        if (close != std::string_view::npos) {
            name += piece.substr(0, close);
            trailing = std::string(trim(piece.substr(close + 1)));
            break;
        }
        name += piece;
        if (index + 1 >= fragments.size())
            break; // unterminated quote: no closing fragment left
        name += kFieldSeparator;
        piece = trim(fragments[++index]);
    }
    out = name.empty() ? std::string(kMissingChipValue) : name;
    return index;
}

std::vector<std::string_view> splitFields(std::string_view sentence) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    for (;;) {
        const std::size_t separator = sentence.find(kFieldSeparator, start);
        if (separator == std::string_view::npos) {
            fields.push_back(sentence.substr(start));
            return fields;
        }
        fields.push_back(sentence.substr(start, separator - start));
        start = separator + kFieldSeparator.size();
    }
}

} // namespace

StatusChips splitStatusText(std::string_view sentence) {
    StatusChips chips;
    chips.context = kMissingChipValue;
    chips.speed = kMissingChipValue;
    chips.model = kMissingChipValue;

    // Everything that is not one of the three metrics is the state chip, in
    // source order: "Ready"/"Busy" land here together with a malformed field, so
    // a sentence the splitter does not fully recognise still says what it said.
    std::string state;
    bool haveContext = false;
    bool haveSpeed = false;
    bool haveModel = false;

    const auto appendToState = [&state](std::string_view fragment) {
        if (fragment.empty())
            return;
        if (!state.empty())
            state += kFieldSeparator;
        state += fragment;
    };

    const std::vector<std::string_view> fields = splitFields(sentence);
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const std::string_view field = trim(fields[i]);
        if (field.empty())
            continue;
        if (!haveContext &&
            parseContextField(field, chips.context, chips.contextUsed, chips.contextMax)) {
            haveContext = true;
            continue;
        }
        if (!haveSpeed && parseSpeedField(field, chips.speed)) {
            haveSpeed = true;
            continue;
        }
        if (!haveModel && startsWith(field, kModelField)) {
            std::string trailing;
            i = parseModelField(fields, i, chips.model, trailing);
            haveModel = true;
            appendToState(trailing);
            continue;
        }
        appendToState(field);
    }

    chips.state = state.empty() ? std::string(kMissingChipValue) : state;
    return chips;
}

// ---------------------------------------------------------------------------
// The phase vocabulary (D-PHASE) and the structured path (D-STATUS)
// ---------------------------------------------------------------------------

Phase phaseFromOrdinal(int ordinal) {
    // Mapped, never cast: the ordinal is D's (tui_api.h's TuiRuntimeStatus comment
    // lists the same seven), and a value this build does not know must not become a
    // neighbouring phase.
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

StatusChips chipsFromStatus(const RuntimeStatus& status, const std::string& model) {
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
    chips.model = model.empty() ? std::string(kMissingChipValue) : model;
    chips.state = phaseStateWord(phaseFromOrdinal(status.phase));
    return chips;
}

} // namespace llmfun::tui::model
