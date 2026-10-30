/// The Raw Log's model (model/rawlog_data.h owns the contract).
///
/// Everything here is pure or file-shaped: the row text, the filter, the save path and the one
/// write. No renderer, no aggregate, no colour - which is what lets `./test_rawlog` prove the
/// surface's content and its privacy rule with no screen and no application state.
#include "model/rawlog_data.h"

#include "model/settings.h" // kRawLogOff..kRawLogTrace: the `UiLog` preference rawLogFloor() maps

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace llmfun::tui::model {

const RawLogRecord* RawLogRing::at(std::size_t index) const {
    if (records == nullptr || index >= records->size())
        return nullptr;
    return &(*records)[index];
}

const char* rawLogLevelWord(int level) {
    switch (level) {
    case kRawLogLevelTrace:
        return "trace";
    case kRawLogLevelDebug:
        return "debug";
    case kRawLogLevelWarn:
        return "warn";
    case kRawLogLevelError:
        return "error";
    case kRawLogLevelInfo:
        break;
    }
    return "info";
}

std::string rawLogLevelField(int level) {
    std::string word = rawLogLevelWord(level);
    // `trace`, `debug` and `error` are five cells; `info` and `warn` take two blanks. One join
    // per row is cheaper than a formatter, and the field's width is the *longest* word rather
    // than a number, so a new level name cannot silently shift the column.
    while (word.size() < 5)
        word.push_back(' ');
    return word;
}

namespace {

/// `HH:MM:SS.mmm` in local time for a millisecond clock reading, or `--:--:--.---` for a record
/// that never got a stamp. Activity's clock is the same reading in the same zone, so the two
/// surfaces cannot disagree about *when* a line happened.
std::string clockText(long long timestampMs) {
    if (timestampMs <= 0)
        return "--:--:--.---";
    const std::time_t seconds = static_cast<std::time_t>(timestampMs / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min,
                  local.tm_sec, static_cast<int>(timestampMs % 1000));
    return buffer;
}

/// The lower-case form of `text`, ASCII only.
///
/// ASCII on purpose: the filter is a substring test over a developer string, and a full Unicode
/// case fold would need a locale table this build does not carry. A non-ASCII codepoint is
/// compared byte-exactly, which is what a developer searching a module name expects.
std::string asciiLower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c >= 'A' && c <= 'Z')
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        else
            out.push_back(c);
    }
    return out;
}

bool isBlank(std::string_view text) {
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            return false;
    }
    return true;
}

/// `mkdir -p` for the save directory, one component at a time. A component that already exists
/// is fine (that is what `-p` means); anything else answers false and `rawLogSave()` reports the
/// `errno` it left behind.
bool makeDirectories(const std::string& path) {
    if (path.empty())
        return false;
    std::string partial;
    partial.reserve(path.size());
    std::size_t at = 0;
    while (at <= path.size()) {
        const std::size_t slash = path.find('/', at);
        const std::size_t end = slash == std::string::npos ? path.size() : slash;
        partial.assign(path, 0, end);
        if (!partial.empty() && ::mkdir(partial.c_str(), 0700) != 0 && errno != EEXIST)
            return false;
        if (slash == std::string::npos)
            break;
        at = slash + 1;
    }
    return true;
}

/// `$NAME`, or "" when it is unset or empty.
std::string envValue(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

} // namespace

std::string rawLogRowText(const RawLogRecord& record) {
    std::string row = clockText(record.timestampMs);
    row += ' ';
    row += rawLogLevelField(record.level);

    // The developer coordinates, each whole or not at all: `module.function`, then
    // `file:line`. A record that carries neither is a plain message, not a row of gaps.
    std::string where;
    if (!record.module.empty() || !record.function.empty()) {
        where = record.module;
        if (!record.function.empty()) {
            if (!where.empty())
                where += '.';
            where += record.function;
        }
    }
    if (!record.file.empty()) {
        if (!where.empty())
            where += ' ';
        where += record.file;
        if (record.line > 0)
            where += ':' + std::to_string(record.line);
    }
    if (!where.empty()) {
        row += ' ';
        row += where;
    }
    if (!record.message.empty()) {
        row += ' ';
        row += record.message;
    }
    // The action pair last: `action 1 turn-7`. An id the UI has no label for still prints its
    // number - the Raw Log is the developer surface, and the number is the fact the wire carries.
    if (record.action != 0) {
        row += " action ";
        row += std::to_string(record.action);
        if (!record.actionPayload.empty()) {
            row += ' ';
            row += record.actionPayload;
        }
    }
    return row;
}

std::vector<std::string> rawLogDetailLines(const RawLogRecord& record) {
    std::vector<std::string> lines;
    if (!record.detail.empty()) {
        std::string current;
        for (const char c : record.detail) {
            if (c == '\r')
                continue;
            if (c == '\n') {
                lines.push_back("  | " + current);
                current.clear();
                continue;
            }
            current.push_back(c);
        }
        lines.push_back("  | " + current);
        // A trailing newline produces one empty continuation and nothing else: dropping it keeps
        // the row count a function of the content instead of of the producer's formatting.
        if (lines.size() > 1 && lines.back() == "  | ")
            lines.pop_back();
    }
    // The action is the row's last segment. When the row is too narrow to draw it whole, the
    // expansion is where the id and its payload stay reachable without copy or Save.
    if (record.action != 0) {
        std::string action = "  | action " + std::to_string(record.action);
        if (!record.actionPayload.empty()) {
            action += ' ';
            action += record.actionPayload;
        }
        lines.push_back(action);
    }
    return lines;
}

int rawLogFloor(int uiLogPref) {
    // The two constant families are deliberately separate and read alike: `prefs.rawLog` is the
    // `UiLog` word's number (settings.h: off/raw/debug/trace = 0..3) and `kRawLogLevel*` is a
    // *severity* (the C ABI's 0..4). The names differ only by the `Level` infix; a merge of the
    // two would make `UiLog=debug` mean "severity 2 = info" and silently widen nothing.
    switch (uiLogPref) {
    case kRawLogDebug:
        return kRawLogLevelDebug;
    case kRawLogTrace:
        return kRawLogLevelTrace;
    case kRawLogOff:
    case kRawLogRaw:
        break;
    }
    // `off` and an unknown value share the ordinary floor: a preference this build cannot read
    // must not widen what the developer surface shows.
    return kRawLogLevelInfo;
}

bool rawLogShowsLevel(int level, int minLevel) {
    // The levels are ordered trace < debug < info < warn < error (the C ABI's order, pinned by
    // the static assert in `tui_api.cpp`), so "at least as severe as the floor" is one compare.
    return level >= minLevel;
}

bool rawLogMatches(const RawLogRecord& record, std::string_view filter) {
    if (isBlank(filter))
        return true;
    const std::string needle = asciiLower(filter);
    const std::string haystack = asciiLower(rawLogRowText(record));
    if (haystack.find(needle) != std::string::npos)
        return true;
    return asciiLower(record.detail).find(needle) != std::string::npos;
}

std::string rawLogSaveDir() {
    const std::string dataHome = envValue("XDG_DATA_HOME");
    if (!dataHome.empty())
        return dataHome + "/llmfun/logs";
    const std::string home = envValue("HOME");
    if (!home.empty())
        return home + "/.local/share/llmfun/logs";
    return std::string();
}

std::string rawLogSaveName(std::int64_t nowMs, int attempt) {
    const std::time_t seconds = static_cast<std::time_t>(nowMs / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    const std::string suffix = attempt > 0 ? "-" + std::to_string(attempt) : std::string();
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "ui-%04d%02d%02d-%02d%02d%02d%s.log", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec,
                  suffix.c_str());
    return buffer;
}

bool rawLogSave(const RawLogRing& ring, std::string_view filter, std::int64_t nowMs,
                std::string& outPath, std::string& error) {
    outPath.clear();
    error.clear();
    const std::string dir = rawLogSaveDir();
    if (dir.empty()) {
        error = "no data directory (HOME and XDG_DATA_HOME are unset)";
        return false;
    }
    // An absolute path only. A relative `XDG_DATA_HOME`/`HOME` would put the directory - and the
    // log - under the process' working directory, which is exactly the workarea the privacy rule
    // forbids (a RAG indexes it). Refusing is the honest answer: the caller draws the reason.
    if (dir.front() != '/') {
        error = "the data directory is not an absolute path: " + dir;
        return false;
    }
    if (!makeDirectories(dir)) {
        error = "cannot create " + dir + ": " + std::strerror(errno);
        return false;
    }

    // The descriptor, not a `FILE*`: `O_EXCL` is the "never overwrite" rule, and a stream would
    // have to be reopened on every name collision anyway.
    int fd = -1;
    std::string path;
    for (int attempt = 0; attempt < 100 && fd < 0; ++attempt) {
        path = dir + "/" + rawLogSaveName(nowMs, attempt);
        fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0 && errno != EEXIST) {
            error = "cannot create " + path + ": " + std::strerror(errno);
            return false;
        }
    }
    if (fd < 0) {
        error = "cannot create a new file in " + dir + " (a hundred same-second saves?)";
        return false;
    }

    std::string payload;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const RawLogRecord* record = ring.at(i);
        if (record == nullptr || !rawLogMatches(*record, filter))
            continue;
        payload += rawLogRowText(*record);
        payload += '\n';
        for (const std::string& line : rawLogDetailLines(*record)) {
            payload += line;
            payload += '\n';
        }
    }

    // A failed save leaves no file behind: the next `Save` would see the name taken and take the
    // `-1` variant, so a run of failures would fill the directory with half-written `ui-*.log`
    // files nobody can tell from a good one. `EINTR` is a retry, not a failure (a signal during a
    // large write is not the disk's fault).
    const auto abandon = [&path](std::string& message, const std::string& why) {
        message = why;
        ::unlink(path.c_str());
    };
    std::size_t written = 0;
    while (written < payload.size()) {
        const ssize_t n = ::write(fd, payload.data() + written, payload.size() - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            const std::string why = "cannot write " + path + ": " + std::strerror(errno);
            ::close(fd);
            abandon(error, why);
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0) {
        abandon(error, "cannot close " + path + ": " + std::strerror(errno));
        return false;
    }
    outPath = path;
    return true;
}

} // namespace llmfun::tui::model
