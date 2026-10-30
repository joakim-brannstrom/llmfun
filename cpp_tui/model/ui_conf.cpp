/// `ui.conf`: parsing, atomic writing and the write debounce (model/settings.h
/// owns the contract, the value catalogue and the format).
///
/// The file is named `ui_conf.cpp` and not `settings.cpp` because the settings *sheet* is
/// `views/settings.cpp` (task 34) and every object of the library ends up flattened into one
/// archive by basename (`libllmfun_tui_all_lib.a`, CMakeLists.txt): two `settings.cpp.o` members
/// cannot be told apart, and `ar -x` silently keeps only the last one - every symbol of the other
/// "undefined". `model/rawlog_data.cpp` and `views/rawlog.cpp` are the same pair for the same
/// reason (task 33).
#include "settings.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace llmfun::tui::model {
namespace {

constexpr char kUiConfName[] = "ui.conf";
constexpr char kTempSuffix[] = ".tmp";

/// The three states of a preference that can also follow the layout.
constexpr int kAutoOpen = -1;
constexpr int kClosed = 0;
constexpr int kOpen = 1;

/// `Pin.<sessionId>` is the one key with a variable name.
constexpr std::string_view kPinPrefix = "Pin.";

bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view trimBlanks(std::string_view text) {
    std::size_t first = 0;
    while (first < text.size() && isBlank(text[first]))
        ++first;
    std::size_t last = text.size();
    while (last > first && isBlank(text[last - 1]))
        --last;
    return text.substr(first, last - first);
}

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

/// A whole value word, case-insensitively: a hand-edited `SendKey=ENTER` has no
/// case to get wrong, and the writer's canonical spelling is only a convention.
bool equalsWord(std::string_view text, std::string_view word) {
    if (text.size() != word.size())
        return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (lowerAscii(text[i]) != lowerAscii(word[i]))
            return false;
    }
    return true;
}

/// The one rule for a pin id, used by the parser and the writer: an empty id
/// writes `Pin.=1` (which reads back as no pin), and one carrying '=' or a line
/// break writes a line that reads back as a different key. Such an id is neither
/// stored nor written, so the set can never hold one the file cannot carry.
bool storablePinId(std::string_view id) {
    return !id.empty() && id.find_first_of("=\r\n") == std::string_view::npos;
}

/// A whole numeric value: no trailing garbage, no hex, no locale (a de_DE user's
/// `1,5` is not a number here either). A magnitude too big for a `long` is a
/// *clamp*, not a parse failure: `RailOpen=99999999999999999999` means the same
/// as `RailOpen=5`, and only text that is not a number at all is rejected.
bool parseWholeNumber(std::string_view text, long& out) {
    if (text.empty())
        return false;
    const char* first = text.data();
    const char* last = first + text.size();
    const std::from_chars_result parsed = std::from_chars(first, last, out);
    if (parsed.ptr != last)
        return false; // the whole value has to be the number
    if (parsed.ec == std::errc::result_out_of_range) {
        // from_chars leaves `out` untouched; the sign is still in the text.
        out = text.front() == '-' ? std::numeric_limits<long>::min()
                                  : std::numeric_limits<long>::max();
        return true;
    }
    return parsed.ec == std::errc();
}

/// One `Key=Value` line, split at the first '='. Number keys clamp into their
/// legal range; word keys take the closed set the format lists. Anything that
/// does not parse leaves the current value in place (I-09).
void applyLine(ShellPrefs& prefs, std::string_view line) {
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos)
        return;
    const std::string_view key = line.substr(0, eq);
    const std::string_view value = trimBlanks(line.substr(eq + 1));

    if (key == "SendKey") {
        if (equalsWord(value, "enter"))
            prefs.sendKey = SendKey::Enter;
        else if (equalsWord(value, "ctrlenter"))
            prefs.sendKey = SendKey::CtrlEnter;
        else if (equalsWord(value, "enternewline"))
            prefs.sendKey = SendKey::EnterNewline;
        return;
    }
    if (key == "UiLog") {
        if (equalsWord(value, "off"))
            prefs.rawLog = kRawLogOff;
        else if (equalsWord(value, "raw"))
            prefs.rawLog = kRawLogRaw;
        else if (equalsWord(value, "debug"))
            prefs.rawLog = kRawLogDebug;
        else if (equalsWord(value, "trace"))
            prefs.rawLog = kRawLogTrace;
        return;
    }
    if (key == "RailOpen" || key == "InspectorOpen") {
        int resolved = kAutoOpen;
        if (equalsWord(value, "auto")) {
            resolved = kAutoOpen;
        } else {
            long number = 0;
            if (!parseWholeNumber(value, number))
                return;
            if (number < kAutoOpen)
                resolved = kAutoOpen;
            else if (number > kOpen)
                resolved = kOpen;
            else
                resolved = static_cast<int>(number);
        }
        if (key == "RailOpen")
            prefs.railOpen = resolved;
        else
            prefs.inspectorOpen = resolved;
        return;
    }
    if (key == "InspectorTab") {
        if (equalsWord(value, "agents"))
            prefs.tab = InspectorTab::Agents;
        else if (equalsWord(value, "activity"))
            prefs.tab = InspectorTab::Activity;
        return;
    }
    if (key == "Version")
        return; // checked before anything is applied, so the order in the file is free
    if (key.compare(0, kPinPrefix.size(), kPinPrefix) == 0) {
        const std::string_view id = key.substr(kPinPrefix.size());
        long flag = 0;
        if (!storablePinId(id) || !parseWholeNumber(value, flag))
            return; // malformed keeps the pinned set as it is
        if (flag != 0)
            prefs.pinned.insert(std::string(id));
        else
            prefs.pinned.erase(std::string(id));
        return;
    }
    // Unknown key: skipped, so a newer build's file still loads what it can.
}

template <typename Fn> void forEachLine(const std::string& text, Fn fn) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t stop = (newline == std::string::npos) ? text.size() : newline;
        // A CRLF file is a Windows hand-edit, not a different format.
        const std::size_t end = (stop > start && text[stop - 1] == '\r') ? stop - 1 : stop;
        fn(std::string_view(text).substr(start, end - start));
        if (newline == std::string::npos)
            break;
        start = newline + 1;
    }
}

/// A file written by a future build keeps the defaults instead of being applied
/// line by line: half a newer file would be a lie about what the user chose, and
/// this build cannot know which of its keys the newer one still reads.
bool hasFutureVersion(const std::string& contents) {
    bool future = false;
    forEachLine(contents, [&future](std::string_view line) {
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos || line.substr(0, eq) != "Version")
            return;
        long version = 0;
        if (parseWholeNumber(trimBlanks(line.substr(eq + 1)), version) && version > kUiConfVersion)
            future = true;
    });
    return future;
}

bool readWholeFile(const std::string& path, std::string& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    out.clear();
    std::array<char, 512> buf{};
    std::size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0)
        out.append(buf.data(), n);
    std::fclose(f);
    return true;
}

std::string temporaryPath(const std::string& path) { return path + kTempSuffix; }

/// A temporary is never read, so a left-over one (a killed writer) is litter:
/// removed before the next attempt and after a save that had nothing to write.
void discardTemporary(const std::string& path) { std::remove(temporaryPath(path).c_str()); }

/// Whether there is a file at `path` whose bytes a save would replace. A save
/// without a preceding load may only create a missing profile; anything else at
/// the path (a directory, say) is left for the write attempt to report.
bool isRegularFile(const std::string& path) {
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

/// What a replaced file keeps: a user who tightened `ui.conf` to 0600 does not get
/// 0644 back, and a group-shared 0664 stays shareable. A file that does not exist
/// yet has no mode to keep, so it is created 0644 *through the process umask* -
/// the user's own policy on who may read a file that carries session ids.
struct DestinationMode {
    bool exists = false;
    mode_t mode = 0644;
};

DestinationMode destinationModeOf(const std::string& path) {
    DestinationMode destination;
    struct stat info{};
    if (::stat(path.c_str(), &info) == 0) {
        destination.exists = true;
        destination.mode = info.st_mode & 07777;
    }
    return destination;
}

/// `ui.conf.tmp`, fsync, `rename()`. The rename is what makes the swap atomic:
/// until it runs the old file is the only `ui.conf`, and a failure removes the
/// temporary instead of leaving it for something to read.
///
/// The file is synced, the directory is not: what the design has to rule out is a
/// *truncated* settings file (R-16), and that is the rename's job. A power loss
/// between the rename and the filesystem's own journal replay can only bring the
/// previous complete file back, which is the state this build started from.
bool writeWholeFileAtomic(const std::string& path, const std::string& text, std::string& error) {
    const std::string tmp = temporaryPath(path);
    const DestinationMode destination = destinationModeOf(path);
    // A fixed name in a user-writable directory: take a left-over temporary with
    // us rather than writing through whatever is there (O_NOFOLLOW keeps a planted
    // symlink from redirecting the write), and O_EXCL proves the file is new.
    discardTemporary(path);
    const int fd =
        ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, destination.mode);
    if (fd < 0) {
        error = std::strerror(errno);
        return false;
    }
    // O_CREAT's mode is filtered by the umask, which is right for a brand-new file
    // but would narrow a preserved one; only a replaced file gets its mode back.
    if (destination.exists && ::fchmod(fd, destination.mode) != 0) {
        error = std::strerror(errno);
        ::close(fd);
        discardTemporary(path);
        return false;
    }
    std::size_t done = 0;
    errno = 0;
    while (done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n > 0) {
            done += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        // A short write of zero bytes is a failure with no errno of its own; never
        // report whatever an earlier call happened to leave behind.
        error = std::strerror(n == 0 ? EIO : errno);
        ::close(fd);
        discardTemporary(path);
        return false;
    }
    if (::fsync(fd) != 0) {
        error = std::strerror(errno);
        ::close(fd);
        discardTemporary(path);
        return false;
    }
    if (::close(fd) != 0) {
        error = std::strerror(errno);
        discardTemporary(path);
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        error = std::strerror(errno);
        discardTemporary(path);
        return false;
    }
    return true;
}

const char* inspectorTabWord(InspectorTab tab) {
    switch (tab) {
    case InspectorTab::Agents:
        return "agents";
    case InspectorTab::Activity:
        return "activity";
    }
    assert(false && "unhandled InspectorTab");
    return "activity";
}

const char* triStateWord(int value) {
    if (value <= kAutoOpen)
        return "auto";
    return value == kClosed ? "0" : "1";
}

/// The debounce is module state on purpose: the call signature the design fixes
/// (`flushUiConf(prefs, ini, now)`) has nowhere else to keep a clock, and there
/// is one writer - the frame thread (D-C: no UI state is touched from another
/// thread). Everything below is keyed on the derived path, so a second profile
/// starts a fresh window instead of inheriting the first one's.
struct Debounce {
    std::string path;
    /// The text the newest observation produced - the write candidate.
    std::string pendingText;
    /// The last settled text: what this session believes it has, written or
    /// accepted as the file's state. A failed write settles it too, so a read-only
    /// location cannot become a retry loop - which is why it is *not* "the text the
    /// file holds" and must never be used to repair the file.
    std::string baseline;
    double changedAt = 0.0;
    bool pending = false;
    /// A preference changed since the profile was loaded: the one thing a quit
    /// save carries. A session that changed nothing must not rewrite the file -
    /// it may be a newer build's, or hand-edited - so `saveUiConf()` is inert
    /// while this is false.
    bool dirty = false;
    /// `loadUiConf()` has applied the profile (a file, or no file). Until it has,
    /// the file belongs to the user and no flush touches it. A newer build's file
    /// is not applied, so it sets `futureFile` instead of this one.
    bool loaded = false;
    /// The file was written by a newer build: never rewrite it unattended.
    bool futureFile = false;
};

Debounce g_debounce;
/// One line, taken once by whoever posts it: the frame layer turns it into the
/// single Activity warning a read-only directory is allowed (design §5.3).
std::string g_warning;

/// The debounce state of one profile. A different path starts from scratch, so a
/// second profile cannot inherit the first one's window or its "loaded" claim.
Debounce& debounceFor(const std::string& path) {
    if (g_debounce.path != path)
        g_debounce = Debounce{};
    g_debounce.path = path;
    return g_debounce;
}

void writeAndReport(const std::string& path, const std::string& text) {
    std::string error;
    if (writeWholeFileAtomic(path, text, error))
        return;
    if (g_warning.empty()) {
        g_warning = path + ": cannot write the workbench settings (" + error +
                    "); they stay in memory for this session";
    }
}

} // namespace

// ---------------------------------------------------------------------------
// The settings sheet's catalogue (model/settings.h owns the contract)
// ---------------------------------------------------------------------------

/// No `default:` label on purpose: a new enumerator must add its own word here, because falling
/// into another one would persist a preference the user never chose. This TU is built with
/// `-Wswitch-enum -Werror=switch-enum` (CMakeLists.txt), so a missing case fails the build
/// instead of waiting for a warning nobody reads; the `assert` below is only the debug backstop.
const char* sendKeyWord(SendKey key) {
    switch (key) {
    case SendKey::Enter:
        return "enter";
    case SendKey::CtrlEnter:
        return "ctrlenter";
    case SendKey::EnterNewline:
        return "enterNewline";
    }
    assert(false && "unhandled SendKey");
    return "enterNewline";
}

/// Clamped, so the writer (and the sheet) never names a level the parser would reject.
const char* uiLogWord(int level) {
    if (level <= kRawLogOff)
        return "off";
    if (level >= kRawLogTrace)
        return "trace";
    if (level == kRawLogDebug)
        return "debug";
    return "raw";
}

SendKey sendKeyChoice(int index) {
    // The table's order is the design's own list (`enter`, `ctrlenter`, `enterNewline`), so a
    // user coming from the design document finds the values where they left them.
    if (index <= 0)
        return SendKey::Enter;
    if (index >= kSendKeyChoiceCount - 1)
        return SendKey::EnterNewline;
    return SendKey::CtrlEnter;
}

int sendKeyIndex(SendKey key) {
    switch (key) {
    case SendKey::Enter:
        return 0;
    case SendKey::CtrlEnter:
        return 1;
    case SendKey::EnterNewline:
        return 2;
    }
    assert(false && "unhandled SendKey");
    return 2;
}

bool sendKeyProven(SendKey key) {
    switch (key) {
    case SendKey::Enter:
        // plan/key_gate.md §3: `Enter` arrives, but `Shift+Enter` is the same byte, so "Enter
        // sends, Shift+Enter breaks the line" cannot be honoured in the grid.
        return false;
    case SendKey::CtrlEnter:
        // §3: `Ctrl+Enter` is `NOT delivered` in both terminal backends, "not bound".
        return false;
    case SendKey::EnterNewline:
        // The shipping rule (R-01): Enter breaks the line, the Send button sends.
        return true;
    }
    assert(false && "unhandled SendKey");
    return false;
}

float clampFontScale(float scale) {
    // Non-finite first: `std::clamp` passes NaN through, and a NaN length would round a layout
    // to zero (the answer for a missing or invalid scale is the shipped default).
    if (!std::isfinite(scale) || !(scale > 0.0f))
        return 1.0f;
    const float bounded = std::min(std::max(scale, kFontScaleMin), kFontScaleMax);
    // Quantized to the step, in percent, so the sheet cannot print a
    // value that sits between two steps.
    const int percent = static_cast<int>(std::lround(bounded * 100.0f));
    const int snapped =
        ((percent + kFontScaleStepPercent / 2) / kFontScaleStepPercent) * kFontScaleStepPercent;
    const int low = static_cast<int>(std::lround(kFontScaleMin * 100.0f));
    const int high = static_cast<int>(std::lround(kFontScaleMax * 100.0f));
    return static_cast<float>(std::min(std::max(snapped, low), high)) / 100.0f;
}

float stepFontScale(float scale, int steps) {
    const int low = static_cast<int>(std::lround(kFontScaleMin * 100.0f));
    const int high = static_cast<int>(std::lround(kFontScaleMax * 100.0f));
    const int moved = fontScalePercent(clampFontScale(scale)) + steps * kFontScaleStepPercent;
    return fontScaleFromPercent(std::min(std::max(moved, low), high));
}

int fontScalePercent(float scale) {
    if (!std::isfinite(scale))
        return 100;
    return static_cast<int>(std::lround(scale * 100.0f));
}

float fontScaleFromPercent(int percent) { return static_cast<float>(percent) / 100.0f; }

std::string uiConfPath(const std::string& iniFilename) {
    if (iniFilename.empty())
        return std::string();
    // The same directory as the ini file, and therefore as the `imgui2.conf`
    // `tui_settings::settingsPath()` derives from it.
    const std::size_t slash = iniFilename.find_last_of('/');
    if (slash == std::string::npos)
        return std::string(kUiConfName);
    return iniFilename.substr(0, slash + 1) + kUiConfName;
}

std::string uiConfText(const ShellPrefs& prefs) {
    std::string text;
    text += "Version=" + std::to_string(kUiConfVersion) + "\n";
    text += "SendKey=" + std::string(sendKeyWord(prefs.sendKey)) + "\n";
    text += "UiLog=" + std::string(uiLogWord(prefs.rawLog)) + "\n";
    text += "RailOpen=" + std::string(triStateWord(prefs.railOpen)) + "\n";
    text += "InspectorOpen=" + std::string(triStateWord(prefs.inspectorOpen)) + "\n";
    text += "InspectorTab=" + std::string(inspectorTabWord(prefs.tab)) + "\n";
    for (const std::string& id : prefs.pinned) {
        if (!storablePinId(id))
            continue; // the parser drops it too, so writing it would store nothing
        text += "Pin." + id + "=1\n";
    }
    return text;
}

void loadUiConf(ShellPrefs& prefs, const std::string& iniFilename,
                const std::set<std::string>* knownSessionIds) {
    const std::string path = uiConfPath(iniFilename);
    if (path.empty())
        return;
    Debounce& state = debounceFor(path);
    // Reading is not changing: the settled text covers whatever this load yields,
    // so a pruned pin or a hand-edited spelling never opens a write window of its
    // own. Only a later change to `prefs` does.
    state.loaded = false;
    state.futureFile = false;
    state.pending = false;
    state.changedAt = 0.0;
    state.dirty = false;

    std::string contents;
    if (!readWholeFile(path, contents)) {
        // No file yet: the defaults are the truth this session starts from.
        state.loaded = true;
        state.baseline = uiConfText(prefs);
        state.pendingText = state.baseline;
        return;
    }
    if (hasFutureVersion(contents)) {
        // A newer build owns this file: the struct keeps its values and neither the
        // flush nor an unchanged quit rewrites it. An explicit save of a change is
        // the user's own action. `pendingText` is what a change is measured
        // against, so "nothing changed" has an answer even though nothing applied.
        state.futureFile = true;
        state.baseline = contents;
        state.pendingText = uiConfText(prefs);
        return;
    }

    forEachLine(contents, [&prefs](std::string_view line) { applyLine(prefs, line); });

    if (knownSessionIds != nullptr) {
        for (auto it = prefs.pinned.begin(); it != prefs.pinned.end();) {
            if (knownSessionIds->count(*it) == 0)
                it = prefs.pinned.erase(it);
            else
                ++it;
        }
    }

    state.loaded = true;
    state.baseline = uiConfText(prefs);
    state.pendingText = state.baseline;
}

void saveUiConf(const ShellPrefs& prefs, const std::string& iniFilename) {
    const std::string path = uiConfPath(iniFilename);
    if (path.empty())
        return;
    Debounce& state = debounceFor(path);
    const std::string text = uiConfText(prefs);

    // The quit save carries a change this session made - it does not canonicalize a
    // file nobody touched. A newer build's file keeps its bytes, and a hand-edited
    // one keeps its comments and unknown keys, until the user really chooses
    // something. A profile that was read (applied, missing, or a newer build's) is
    // what a change is measured against; without any load the only thing a save may
    // do is create a profile that does not exist yet, because there is nothing
    // there to overwrite.
    if (state.loaded || state.futureFile) {
        if (text != state.pendingText)
            state.dirty = true;
    } else if (!isRegularFile(path)) {
        state.dirty = true;
    }
    if (!state.dirty) {
        discardTemporary(path); // a killed writer's litter, never a file to write
        return;
    }

    std::string onDisk;
    if (readWholeFile(path, onDisk) && onDisk == text) {
        // Nothing changed on disk: leave the file (and its timestamp) alone, but do
        // not leave a killed writer's temporary behind either.
        discardTemporary(path);
    } else {
        writeAndReport(path, text);
    }
    // An explicit save settles the profile whether it wrote or failed: the next
    // flush must not write these bytes a second time, and the file is now this
    // session's own even if it was a newer build's before.
    state.loaded = true;
    state.futureFile = false;
    state.pendingText = text;
    state.baseline = text;
    state.pending = false;
    state.dirty = false;
}

void flushUiConf(const ShellPrefs& prefs, const std::string& iniFilename, double nowSeconds) {
    const std::string path = uiConfPath(iniFilename);
    if (path.empty())
        return;
    // Nothing loaded for this profile, or the file belongs to a newer build: the
    // file is the user's, not ours to rewrite. (Starting a session from defaults
    // must never overwrite choices nobody has read yet.)
    if (g_debounce.path != path || !g_debounce.loaded || g_debounce.futureFile)
        return;

    const std::string text = uiConfText(prefs);
    if (text != g_debounce.pendingText) {
        g_debounce.pendingText = text;
        g_debounce.changedAt = nowSeconds;
        g_debounce.pending = true;
        g_debounce.dirty = true;
    }

    if (text == g_debounce.baseline) {
        g_debounce.pending = false; // back to the settled state
        g_debounce.dirty = false;   // nothing unsettled for the quit save to carry
        return;
    }
    if (!g_debounce.pending || nowSeconds - g_debounce.changedAt < kUiConfIdleSeconds)
        return; // one write when the user stops, never one per frame

    writeAndReport(path, text);
    g_debounce.baseline = text;
    g_debounce.pending = false;
    g_debounce.dirty = false; // written or given up on: the window is closed
}

bool takeUiConfWarning(std::string& message) {
    if (g_warning.empty())
        return false;
    message = g_warning;
    g_warning.clear();
    return true;
}

} // namespace llmfun::tui::model
