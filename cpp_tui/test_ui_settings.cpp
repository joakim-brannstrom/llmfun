// test_ui_settings.cpp
//
// Standalone unit test for the workbench settings file (model/settings.h, task 22,
// design §5.3 / D-UI-CONF): `ui.conf` is the sibling of `imgui2.conf` that only the
// workbench reads and writes, so a legacy save can never clobber a preference it has
// no field for.
//
// Compiles model/ui_conf.cpp and tui_settings.cpp directly and links only the
// standard library (no GLFW, no ImGui, no display). No test framework: CHECK(cond)
// prints each check and sets a fail flag; exits 0 if every check passes.
//
// Covers:
//   - uiConfPath: the sibling of the derived settings file ("imgui2.ini" ->
//     "ui.conf", "a/b/imgui2.ini" -> "a/b/ui.conf", "" -> "" and no file touched)
//   - the exact on-disk bytes, defaults and all-non-default
//   - a full round-trip of every key and every value, pins included
//   - the Version gate: a file from a future build keeps the defaults, whatever the
//     order of its lines; a missing or malformed Version is applied
//   - unknown keys skipped, malformed values keep the current value, out-of-range
//     numbers clamp - the same expectations as ./test_settings
//   - pins: `Pin.<id>=1` adds, `=0` removes, an empty or unknown id is dropped
//   - the atomic write: a temporary left behind by a killed writer is never read and
//     the old file stays intact; a failed swap removes the temporary
//   - 100 changes in 1 s produce no write at all before the 2 s idle window and
//     exactly one after it; a failed write does not become a retry loop
//   - the read-only-location warning is one line per failed attempt, and the values
//     still apply in memory
//   - a legacy save (saveTuiSettings) rewrites imgui2.conf only: ui.conf stays
//     byte-identical and no MVP key appears in the legacy file
//   - an explicit save of identical bytes leaves the file (and its timestamp) alone
//   - a quit save carries only a change: a Version=2 file and a hand-edited file
//     keep their bytes, while a real choice replaces them
//   - a brand-new file is created subject to the process umask (umask 077 -> 0600);
//     a replaced file keeps the mode the user set on it
//
// Run headless from the build dir (scratch files land under
// ./test_ui_settings_scratch; the directory is left in place and is safe to delete):
//   ./test_ui_settings

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/time.h>

#include "model/settings.h"
#include "tui_settings.h"

using llmfun::tui::loadTuiSettings;
using llmfun::tui::saveTuiSettings;
using llmfun::tui::TuiSettings;
using llmfun::tui::model::flushUiConf;
using llmfun::tui::model::InspectorTab;
using llmfun::tui::model::kRawLogDebug;
using llmfun::tui::model::kRawLogOff;
using llmfun::tui::model::kRawLogRaw;
using llmfun::tui::model::kRawLogTrace;
using llmfun::tui::model::kUiConfIdleSeconds;
using llmfun::tui::model::loadUiConf;
using llmfun::tui::model::saveUiConf;
using llmfun::tui::model::SendKey;
using llmfun::tui::model::ShellPrefs;
using llmfun::tui::model::takeUiConfWarning;
using llmfun::tui::model::uiConfPath;
using llmfun::tui::model::uiConfText;

static int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        const bool _ok = (cond);                                                                   \
        std::printf("%s %s\n", _ok ? "ok  " : "FAIL", #cond);                                      \
        if (!_ok) {                                                                                \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

namespace {

const std::string kScratch = "test_ui_settings_scratch";

void section(const char* name) { std::printf("\n-- %s\n", name); }

/// Every case gets its own directory: `ui.conf` has a fixed name, so the directory
/// is what makes two cases two files - and a fresh debounce window.
std::string caseDir(const char* name) {
    const std::string dir = kScratch + "/" + name;
    ::mkdir(kScratch.c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
    return dir;
}

std::string iniOf(const std::string& dir) { return dir + "/imgui2.ini"; }

std::string confOf(const std::string& dir) { return dir + "/ui.conf"; }

void removeFile(const std::string& path) { std::remove(path.c_str()); }

void writeText(const std::string& path, const std::string& text) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr || std::fwrite(text.data(), 1, text.size(), f) != text.size()) {
        std::printf("FAIL cannot write scratch file %s\n", path.c_str());
        ++g_failures;
        if (f != nullptr)
            std::fclose(f);
        return;
    }
    std::fclose(f);
}

std::string readText(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return "<open failed>";
    std::string out;
    char buf[256];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, n);
    std::fclose(f);
    return out;
}

bool fileExists(const std::string& path) {
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool isDirectory(const std::string& path) {
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

/// Whole seconds of the file's mtime; -1 when it cannot be read.
long long writeTime(const std::string& path) {
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0)
        return -1;
    return static_cast<long long>(info.st_mtime);
}

bool setWriteTime(const std::string& path, long long seconds) {
    struct timeval times[2];
    times[0].tv_sec = static_cast<time_t>(seconds);
    times[0].tv_usec = 0;
    times[1] = times[0];
    return ::utimes(path.c_str(), times) == 0;
}

long long fileMode(const std::string& path) {
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0)
        return -1;
    return static_cast<long long>(info.st_mode & 07777);
}

bool setFileMode(const std::string& path, mode_t mode) { return ::chmod(path.c_str(), mode) == 0; }

std::set<std::string> setOf(const std::vector<std::string>& ids) {
    return std::set<std::string>(ids.begin(), ids.end());
}

bool samePrefs(const ShellPrefs& a, const ShellPrefs& b) {
    return a.sendKey == b.sendKey && a.rawLog == b.rawLog && a.railOpen == b.railOpen &&
           a.inspectorOpen == b.inspectorOpen && a.tab == b.tab && a.pinned == b.pinned;
}

} // namespace

int main() {
    // ---------------------------------------------------------------------------
    section("uiConfPath: the sibling of imgui2.conf");
    // ---------------------------------------------------------------------------
    CHECK(uiConfPath("imgui2.ini") == "ui.conf");
    CHECK(uiConfPath("imgui2") == "ui.conf");
    CHECK(uiConfPath("") == "");
    CHECK(uiConfPath("a/b/imgui2.ini") == "a/b/ui.conf");
    CHECK(uiConfPath("/abs/dir/imgui2.ini") == "/abs/dir/ui.conf");
    // The name is fixed: the ini file's stem does not reach the settings file.
    CHECK(uiConfPath("some/other-name.ini") == "some/ui.conf");
    // Any trailing ".ini" is irrelevant: only the directory is carried over.
    CHECK(uiConfPath("dir/imgui2.INI") == "dir/ui.conf");

    // ---------------------------------------------------------------------------
    section("empty name and missing file: nothing is read or written");
    // ---------------------------------------------------------------------------
    {
        // An empty name has no path, so a save cannot create one and a load cannot
        // fail on it. (The file the legacy side would write stays untouched.)
        ShellPrefs prefs;
        prefs.rawLog = kRawLogDebug;
        saveUiConf(prefs, "");
        loadUiConf(prefs, "");
        CHECK(prefs.rawLog == kRawLogDebug);
    }
    {
        const std::string dir = caseDir("missing");
        removeFile(confOf(dir));
        ShellPrefs prefs;
        prefs.sendKey = SendKey::Enter;
        prefs.rawLog = kRawLogTrace;
        prefs.railOpen = 1;
        prefs.inspectorOpen = 0;
        prefs.tab = InspectorTab::Agents;
        prefs.pinned = setOf({"s-x"});
        loadUiConf(prefs, iniOf(dir));
        CHECK(prefs.sendKey == SendKey::Enter && prefs.rawLog == kRawLogTrace &&
              prefs.railOpen == 1 && prefs.inspectorOpen == 0 &&
              prefs.tab == InspectorTab::Agents && prefs.pinned == setOf({"s-x"}));
        CHECK(!fileExists(confOf(dir))); // a load never creates the file
    }

    // ---------------------------------------------------------------------------
    section("the exact on-disk bytes");
    // ---------------------------------------------------------------------------
    {
        const std::string dir = caseDir("bytes");
        removeFile(confOf(dir));
        saveUiConf(ShellPrefs{}, iniOf(dir));
        CHECK(readText(confOf(dir)) == "Version=1\n"
                                       "SendKey=enterNewline\n"
                                       "UiLog=off\n"
                                       "RailOpen=auto\n"
                                       "InspectorOpen=auto\n"
                                       "InspectorTab=activity\n");
    }
    {
        const std::string dir = caseDir("bytes-full");
        removeFile(confOf(dir));
        ShellPrefs prefs;
        prefs.sendKey = SendKey::CtrlEnter;
        prefs.rawLog = kRawLogDebug;
        prefs.railOpen = 1;
        prefs.inspectorOpen = 0;
        prefs.tab = InspectorTab::Agents;
        prefs.pinned = setOf({"s-b", "s-a"});
        saveUiConf(prefs, iniOf(dir));
        CHECK(readText(confOf(dir)) == "Version=1\n"
                                       "SendKey=ctrlenter\n"
                                       "UiLog=debug\n"
                                       "RailOpen=1\n"
                                       "InspectorOpen=0\n"
                                       "InspectorTab=agents\n"
                                       "Pin.s-a=1\n"
                                       "Pin.s-b=1\n");
        // Every key and value survives save -> load.
        ShellPrefs loaded;
        loadUiConf(loaded, iniOf(dir));
        CHECK(samePrefs(loaded, prefs));
        // No temporary survives a successful save.
        CHECK(!fileExists(confOf(dir) + ".tmp"));
    }

    // ---------------------------------------------------------------------------
    section("round-trip: every key and every value");
    // ---------------------------------------------------------------------------
    {
        const std::string dir = caseDir("roundtrip");
        const std::string ini = iniOf(dir);
        removeFile(confOf(dir)); // the first save creates the profile
        const SendKey keys[] = {SendKey::Enter, SendKey::CtrlEnter, SendKey::EnterNewline};
        const InspectorTab tabs[] = {InspectorTab::Agents, InspectorTab::Activity};
        const int levels[] = {kRawLogOff, kRawLogRaw, kRawLogDebug, kRawLogTrace};
        const int triStates[] = {-1, 0, 1};
        bool roundTripped = true;
        for (const SendKey key : keys) {
            for (const int level : levels) {
                for (const int rail : triStates) {
                    for (const int inspector : triStates) {
                        for (const InspectorTab tab : tabs) {
                            ShellPrefs prefs;
                            prefs.sendKey = key;
                            prefs.rawLog = level;
                            prefs.railOpen = rail;
                            prefs.inspectorOpen = inspector;
                            prefs.tab = tab;
                            saveUiConf(prefs, ini);
                            ShellPrefs loaded;
                            loadUiConf(loaded, ini);
                            roundTripped = roundTripped && samePrefs(loaded, prefs);
                        }
                    }
                }
            }
        }
        CHECK(roundTripped);
        // The writer never emits a word the parser rejects: an out-of-range value in
        // memory is clamped to its nearest legal word and reads back as that.
        ShellPrefs wild;
        wild.rawLog = 99;
        wild.railOpen = 7;
        wild.inspectorOpen = -42;
        const std::string text = uiConfText(wild);
        CHECK(text.find("UiLog=trace\n") != std::string::npos);
        CHECK(text.find("RailOpen=1\n") != std::string::npos);
        CHECK(text.find("InspectorOpen=auto\n") != std::string::npos);
        saveUiConf(wild, ini);
        ShellPrefs clamped;
        loadUiConf(clamped, ini);
        CHECK(clamped.rawLog == kRawLogTrace && clamped.railOpen == 1 &&
              clamped.inspectorOpen == -1);
    }

    // ---------------------------------------------------------------------------
    section("Version gate");
    // ---------------------------------------------------------------------------
    {
        const std::string dir = caseDir("version");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);

        // A file from a future build keeps the defaults, wherever its Version line is.
        writeText(conf, "Version=2\nSendKey=enter\nUiLog=trace\nPin.s-1=1\n");
        ShellPrefs future;
        loadUiConf(future, ini);
        CHECK(samePrefs(future, ShellPrefs{}));
        writeText(conf, "SendKey=enter\nUiLog=trace\nVersion=2\n");
        ShellPrefs futureLast;
        loadUiConf(futureLast, ini);
        CHECK(samePrefs(futureLast, ShellPrefs{}));

        // This version, an older one, a malformed one and a missing one all apply.
        const char* accepted[] = {"Version=1\nSendKey=enter\n", "Version=0\nSendKey=enter\n",
                                  "Version=abc\nSendKey=enter\n", "SendKey=enter\n"};
        bool allApplied = true;
        for (const char* text : accepted) {
            writeText(conf, text);
            ShellPrefs prefs;
            loadUiConf(prefs, ini);
            allApplied = allApplied && prefs.sendKey == SendKey::Enter;
        }
        CHECK(allApplied);
    }

    // ---------------------------------------------------------------------------
    section("unknown keys, malformed values, clamps");
    // ---------------------------------------------------------------------------
    {
        const std::string dir = caseDir("tolerant");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);

        // Unknown keys, a line without '=', and values outside a closed word set keep
        // the current value; one out-of-range number clamps.
        writeText(conf, "Frobnicate=9\n"
                        "garbage without an equals sign\n"
                        "SendKey=tab\n"
                        "UiLog=verbose\n"
                        "RailOpen=banana\n"
                        "InspectorTab=main\n"
                        "InspectorOpen=7\n");
        ShellPrefs prefs;
        prefs.sendKey = SendKey::CtrlEnter;
        prefs.rawLog = kRawLogDebug;
        prefs.railOpen = 0;
        prefs.inspectorOpen = 0;
        prefs.tab = InspectorTab::Agents;
        loadUiConf(prefs, ini);
        CHECK(prefs.sendKey == SendKey::CtrlEnter);
        CHECK(prefs.rawLog == kRawLogDebug);
        CHECK(prefs.railOpen == 0);
        CHECK(prefs.tab == InspectorTab::Agents);
        CHECK(prefs.inspectorOpen == 1); // 7 clamps to the open end

        // Keys are matched case-sensitively and are not trimmed, exactly like
        // tui_settings: "sendkey" and "SendKey " are unknown keys.
        writeText(conf, "sendkey=enter\n SendKey=enter\nSendKey =enter\n");
        ShellPrefs keys;
        loadUiConf(keys, ini);
        CHECK(keys.sendKey == SendKey::EnterNewline);

        // A value's *word* has no case to get wrong, and surrounding blanks are fine.
        writeText(conf, "SendKey= ENTER \nUiLog=Trace\nRailOpen=AUTO\nInspectorTab=AGENTS\n");
        ShellPrefs words;
        words.railOpen = 0;
        words.tab = InspectorTab::Activity;
        loadUiConf(words, ini);
        CHECK(words.sendKey == SendKey::Enter);
        CHECK(words.rawLog == kRawLogTrace);
        CHECK(words.railOpen == -1);
        CHECK(words.tab == InspectorTab::Agents);

        // Out-of-range tri-states clamp at both ends, and the numbers are read as
        // whole values (no trailing garbage, no hex, no locale).
        struct ClampCase {
            const char* text;
            int rail;
            int inspector;
        };
        const ClampCase clamps[] = {
            {"RailOpen=5\n", 1, -1},      {"RailOpen=-9\n", -1, -1},
            {"RailOpen=0\n", 0, -1},      {"RailOpen=1\n", 1, -1},
            {"RailOpen=auto\n", -1, -1},  {"InspectorOpen=-3\n", -1, -1},
            {"InspectorOpen=2\n", -1, 1}, {"InspectorOpen=0\n", -1, 0},
            {"RailOpen=0x1\n", -1, -1},   {"RailOpen=1x\n", -1, -1},
            {"RailOpen=\n", -1, -1},      {"RailOpen= 1 \n", 1, -1},
        };
        bool clamped = true;
        for (const ClampCase& one : clamps) {
            writeText(conf, one.text);
            ShellPrefs values;
            loadUiConf(values, ini);
            clamped =
                clamped && values.railOpen == one.rail && values.inspectorOpen == one.inspector;
            if (values.railOpen != one.rail || values.inspectorOpen != one.inspector) {
                std::printf("  (content \"%s\" -> rail %d inspector %d, want %d %d)\n", one.text,
                            values.railOpen, values.inspectorOpen, one.rail, one.inspector);
            }
        }
        CHECK(clamped);

        // A magnitude too big for a `long` is a clamp, not a parse failure (the two
        // ends are checked from a non-default state, or the check would prove nothing).
        writeText(conf, "RailOpen=99999999999999999999\nInspectorOpen=-99999999999999999999\n");
        ShellPrefs huge;
        huge.railOpen = 0;
        huge.inspectorOpen = 1;
        loadUiConf(huge, ini);
        CHECK(huge.railOpen == 1);
        CHECK(huge.inspectorOpen == -1);

        // CRLF is a Windows hand-edit, not a different format; a file without a
        // trailing newline still parses its last line.
        writeText(conf, "Version=1\r\nSendKey=ctrlenter\r\nUiLog=debug");
        ShellPrefs crlf;
        loadUiConf(crlf, ini);
        CHECK(crlf.sendKey == SendKey::CtrlEnter && crlf.rawLog == kRawLogDebug);
    }

    // ---------------------------------------------------------------------------
    section("pins");
    // ---------------------------------------------------------------------------
    {
        const std::string dir = caseDir("pins");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);

        writeText(conf, "Pin.s-a=1\nPin.s-b=1\nPin.s-c=0\nPin.s-d=x\nPin.=1\n");
        ShellPrefs prefs;
        loadUiConf(prefs, ini);
        CHECK(prefs.pinned == setOf({"s-a", "s-b"})); // =0, an unknown value and an empty id drop

        // A pin the session store does not know is pruned; one it does know stays.
        const std::set<std::string> known = setOf({"s-a", "s-z"});
        ShellPrefs pruned;
        loadUiConf(pruned, ini, &known);
        CHECK(pruned.pinned == setOf({"s-a"}));
        // Without a store there is nothing to prune against.
        ShellPrefs unpruned;
        loadUiConf(unpruned, ini);
        CHECK(unpruned.pinned == setOf({"s-a", "s-b"}));

        // `=0` removes a pin that is already in memory, so a hand-edit can unpin.
        writeText(conf, "Pin.s-a=0\n");
        ShellPrefs removed;
        removed.pinned = setOf({"s-a", "s-b"});
        loadUiConf(removed, ini);
        CHECK(removed.pinned == setOf({"s-b"}));

        // An empty id is never written (it would come back as no pin at all).
        ShellPrefs empty;
        empty.pinned = setOf({"", "s-a"});
        CHECK(uiConfText(empty).find("Pin.=1") == std::string::npos);
        CHECK(uiConfText(empty).find("Pin.s-a=1\n") != std::string::npos);

        // Neither is an id that cannot survive the round-trip: "a=b" would be read
        // back as the key "Pin.a" with a malformed value.
        ShellPrefs awkward;
        awkward.pinned = setOf({"a=b", "s-ok"});
        CHECK(uiConfText(awkward).find("Pin.a=b=1") == std::string::npos);
        CHECK(uiConfText(awkward).find("Pin.s-ok=1\n") != std::string::npos);
    }

    // ---------------------------------------------------------------------------
    section("atomic write: the temporary is never the source of truth");
    // ---------------------------------------------------------------------------
    {
        const std::string dir = caseDir("atomic");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);
        removeFile(conf + ".tmp");

        // The state a writer leaves behind when it dies between the write and the
        // rename: the new bytes are in ui.conf.tmp, the old file is still the file.
        writeText(conf, "Version=1\nSendKey=enter\n");
        const std::string before = readText(conf);
        writeText(conf + ".tmp", "Version=1\nSendKey=ctrlenter\nRailOpen=0\nUiLog=trace\n");
        ShellPrefs prefs;
        loadUiConf(prefs, ini);
        CHECK(prefs.sendKey == SendKey::Enter); // the intact file, not the temporary
        CHECK(prefs.railOpen == -1 && prefs.rawLog == kRawLogOff);
        CHECK(readText(conf) == before); // and loading does not "repair" the file
        CHECK(fileExists(conf + ".tmp"));

        // The next real save replaces the file and takes the temporary with it.
        prefs.sendKey = SendKey::CtrlEnter;
        saveUiConf(prefs, ini);
        CHECK(!fileExists(conf + ".tmp"));
        CHECK(readText(conf) == uiConfText(prefs));
        ShellPrefs reloaded;
        loadUiConf(reloaded, ini);
        CHECK(reloaded.sendKey == SendKey::CtrlEnter);
    }
    {
        // The swap itself can fail (here: the destination is a directory, so the
        // rename is rejected). The temporary is removed, the old thing stays, and the
        // failure is one warning instead of a crash.
        const std::string dir = caseDir("swapfail");
        removeFile(dir + "/ui.conf");
        CHECK(::mkdir((dir + "/ui.conf").c_str(), 0755) == 0);
        ShellPrefs prefs;
        prefs.rawLog = kRawLogDebug;
        saveUiConf(prefs, iniOf(dir));
        std::string warning;
        CHECK(takeUiConfWarning(warning));
        CHECK(!warning.empty() && warning.find("ui.conf") != std::string::npos);
        CHECK(!takeUiConfWarning(warning));
        CHECK(!fileExists(dir + "/ui.conf.tmp"));
        CHECK(isDirectory(dir + "/ui.conf"));
    }

    // ---------------------------------------------------------------------------
    section("a flush or quit save is inert until the profile has been loaded");
    // ---------------------------------------------------------------------------
    {
        // The file belongs to the user until this session has read it: starting
        // from defaults must never write them over choices nobody looked at.
        const std::string dir = caseDir("unloaded");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);
        const std::string existing = "Version=1\n"
                                     "SendKey=ctrlenter\n"
                                     "UiLog=trace\n"
                                     "RailOpen=0\n"
                                     "InspectorOpen=0\n"
                                     "InspectorTab=agents\n"
                                     "Pin.keep=1\n";
        writeText(conf, existing);
        ShellPrefs defaults; // never loaded
        flushUiConf(defaults, ini, 10.0);
        flushUiConf(defaults, ini, 20.0);
        CHECK(readText(conf) == existing);
        defaults.rawLog = kRawLogTrace; // even a "change" writes nothing
        flushUiConf(defaults, ini, 30.0);
        flushUiConf(defaults, ini, 40.0);
        CHECK(readText(conf) == existing);
        // The quit save is the same rule: a session that read nothing does not
        // overwrite a file it never saw.
        ShellPrefs quitDefaults;
        quitDefaults.rawLog = kRawLogTrace;
        saveUiConf(quitDefaults, ini);
        CHECK(readText(conf) == existing);
    }
    {
        // A file from a newer build keeps its bytes: the load refuses to apply it,
        // the flush refuses to rewrite it, and a quit that changed nothing does not
        // replace it either.
        const std::string dir = caseDir("future");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);
        const std::string newer = "Version=2\nSendKey=enter\nBrand=1\n";
        writeText(conf, newer);
        ShellPrefs prefs;
        loadUiConf(prefs, ini);
        CHECK(samePrefs(prefs, ShellPrefs{})); // defaults, not half-applied
        flushUiConf(prefs, ini, 10.0);
        prefs.rawLog = kRawLogDebug;
        flushUiConf(prefs, ini, 20.0);
        flushUiConf(prefs, ini, 30.0);
        CHECK(readText(conf) == newer);
        CHECK(!fileExists(conf + ".tmp"));
        // Starting and quitting this build changes nothing and writes nothing.
        ShellPrefs untouched;
        loadUiConf(untouched, ini);
        saveUiConf(untouched, ini);
        CHECK(readText(conf) == newer);
        // A real choice is the user's own action and settles the profile: the
        // newer file is replaced by this build's Version=1 file.
        saveUiConf(prefs, ini);
        CHECK(readText(conf) == uiConfText(prefs));
        ShellPrefs reloaded;
        loadUiConf(reloaded, ini);
        CHECK(samePrefs(reloaded, prefs));
    }
    {
        // A hand-edited file is not canonicalized by merely opening and quitting:
        // the quit save carries a change, not a rewrite of the user's formatting.
        const std::string dir = caseDir("handedit");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);
        const std::string handEdited = "# my settings\r\nSendKey=ENTER\r\nFrobnicate=9\r\n";
        writeText(conf, handEdited);
        ShellPrefs prefs;
        loadUiConf(prefs, ini);
        CHECK(prefs.sendKey == SendKey::Enter); // the value applies, the spelling stays
        saveUiConf(prefs, ini);
        CHECK(readText(conf) == handEdited);
        // A change is written in this build's canonical form.
        prefs.rawLog = kRawLogDebug;
        saveUiConf(prefs, ini);
        CHECK(readText(conf) == uiConfText(prefs));
    }
    {
        // A prune is a read, not an edit: the pins the store did not list are gone
        // from memory but still on disk until the user changes something.
        const std::string dir = caseDir("prunekeep");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);
        const std::string twoPins = "Version=1\nPin.s-a=1\nPin.s-b=1\n";
        writeText(conf, twoPins);
        const std::set<std::string> partial = setOf({"s-a"}); // a filtered/paged store
        ShellPrefs prefs;
        loadUiConf(prefs, ini, &partial);
        CHECK(prefs.pinned == setOf({"s-a"}));
        flushUiConf(prefs, ini, 10.0);
        flushUiConf(prefs, ini, 20.0);
        CHECK(readText(conf) == twoPins); // the user's pin is still on disk
    }

    // ---------------------------------------------------------------------------
    section("debounce: 100 changes in 1 s, one write after the idle window");
    // ---------------------------------------------------------------------------
    {
        const std::string dir = caseDir("flush");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);
        removeFile(conf);
        removeFile(conf + ".tmp");

        ShellPrefs prefs;
        double now = 1000.0;
        loadUiConf(prefs, ini);       // no file yet: the defaults are the truth
        flushUiConf(prefs, ini, now); // nothing changed, nothing written
        CHECK(!fileExists(conf));

        bool wroteDuringBurst = false;
        for (int i = 0; i < 100; ++i) {
            prefs.rawLog = (i % 2 == 0) ? kRawLogRaw : kRawLogDebug;
            now += 0.01; // 100 changes in 1 s
            flushUiConf(prefs, ini, now);
            if (fileExists(conf))
                wroteDuringBurst = true;
        }
        CHECK(!wroteDuringBurst); // never per frame, not even one

        flushUiConf(prefs, ini, now + 1.0); // still inside the idle window
        CHECK(!fileExists(conf));
        flushUiConf(prefs, ini, now + kUiConfIdleSeconds + 0.5); // the user stopped
        CHECK(fileExists(conf));
        // A literal expectation, not uiConfText()'s own answer.
        CHECK(prefs.rawLog == kRawLogDebug);
        CHECK(readText(conf) == "Version=1\n"
                                "SendKey=enterNewline\n"
                                "UiLog=debug\n"
                                "RailOpen=auto\n"
                                "InspectorOpen=auto\n"
                                "InspectorTab=activity\n");

        // Exactly one write: while nothing changes, the file is left alone (even if
        // something else changed it, the debounce compares prefs, not the file).
        writeText(conf, "SENTINEL\n");
        flushUiConf(prefs, ini, now + 100.0);
        CHECK(readText(conf) == "SENTINEL\n");

        // A new change opens a new window.
        prefs.railOpen = 1;
        flushUiConf(prefs, ini, now + 101.0);
        CHECK(readText(conf) == "SENTINEL\n"); // not yet
        flushUiConf(prefs, ini, now + 104.0);
        CHECK(readText(conf) == uiConfText(prefs));

        // Close/quit writes at once and ends the window, so the next frames write
        // nothing even though the file on disk is not the last one we wrote.
        prefs.tab = InspectorTab::Agents;
        flushUiConf(prefs, ini, now + 200.0); // opens a window
        saveUiConf(prefs, ini);
        CHECK(readText(conf) == uiConfText(prefs));
        writeText(conf, "SENTINEL\n");
        flushUiConf(prefs, ini, now + 300.0);
        CHECK(readText(conf) == "SENTINEL\n");

        // A change that goes back to the settled state writes nothing.
        prefs.tab = InspectorTab::Agents;
        flushUiConf(prefs, ini, now + 400.0);
        prefs.tab = InspectorTab::Activity;
        flushUiConf(prefs, ini, now + 401.0);
        prefs.tab = InspectorTab::Agents;
        flushUiConf(prefs, ini, now + 402.0);
        flushUiConf(prefs, ini, now + 410.0);
        CHECK(readText(conf) == "SENTINEL\n");
    }

    // ---------------------------------------------------------------------------
    section("unwritable location: one warning, values stay in memory");
    // ---------------------------------------------------------------------------
    {
        // A read-only *directory* cannot be created in a container that runs as root
        // (the permission bits are bypassed), so the location is made unwritable the
        // way the writer feels it: the parent is a regular file, so creating
        // ui.conf.tmp fails with ENOTDIR for every user.
        const std::string dir = caseDir("readonly");
        writeText(dir + "/blocked", "not a directory\n");
        const std::string ini = dir + "/blocked/imgui2.ini";
        CHECK(uiConfPath(ini) == dir + "/blocked/ui.conf");

        ShellPrefs prefs;
        prefs.rawLog = kRawLogDebug;
        saveUiConf(prefs, ini);
        std::string warning;
        CHECK(takeUiConfWarning(warning));
        CHECK(!warning.empty());
        CHECK(!takeUiConfWarning(warning));  // one line, not one per call
        CHECK(prefs.rawLog == kRawLogDebug); // the value still applies in memory

        // The per-frame path cannot become a retry loop: a failed write ends the
        // window, so the same bytes are never attempted again.
        double now = 10.0;
        flushUiConf(prefs, ini, now);
        prefs.rawLog = kRawLogRaw;
        flushUiConf(prefs, ini, now + 0.1);
        flushUiConf(prefs, ini, now + kUiConfIdleSeconds + 1.0); // one attempt
        CHECK(takeUiConfWarning(warning));
        flushUiConf(prefs, ini, now + 4.0);
        flushUiConf(prefs, ini, now + 30.0);
        CHECK(!takeUiConfWarning(warning));
        CHECK(prefs.rawLog == kRawLogRaw);

        // A new change is a new attempt, and is reported once.
        prefs.rawLog = kRawLogTrace;
        flushUiConf(prefs, ini, now + 40.0);
        flushUiConf(prefs, ini, now + 50.0);
        CHECK(takeUiConfWarning(warning));
        CHECK(!takeUiConfWarning(warning));
    }

    // ---------------------------------------------------------------------------
    section("a legacy save cannot clobber ui.conf");
    // ---------------------------------------------------------------------------
    {
        const std::string dir = caseDir("legacy");
        removeFile(confOf(dir));
        removeFile(dir + "/imgui2.conf");

        ShellPrefs prefs;
        prefs.sendKey = SendKey::CtrlEnter;
        prefs.rawLog = kRawLogTrace;
        prefs.railOpen = 1;
        prefs.pinned = setOf({"s-1"});
        saveUiConf(prefs, iniOf(dir));
        const std::string uiBefore = readText(confOf(dir));
        CHECK(!uiBefore.empty() && uiBefore != "<open failed>");

        TuiSettings legacy;
        legacy.windowWidth = 1024;
        legacy.windowHeight = 768;
        legacy.fontScaleMain = 1.5f;
        saveTuiSettings(legacy, iniOf(dir));
        CHECK(readText(confOf(dir)) == uiBefore); // byte-identical

        const std::string legacyText = readText(dir + "/imgui2.conf");
        CHECK(legacyText == "WindowWidth=1024\nWindowHeight=768\nFontScaleMain=1.50\n");
        CHECK(legacyText.find("SendKey") == std::string::npos);
        CHECK(legacyText.find("UiLog") == std::string::npos);
        CHECK(legacyText.find("RailOpen") == std::string::npos);
        CHECK(legacyText.find("InspectorOpen") == std::string::npos);
        CHECK(legacyText.find("InspectorTab") == std::string::npos);
        CHECK(legacyText.find("Pin.") == std::string::npos);
        CHECK(legacyText.find("Version") == std::string::npos);

        // The other direction too: the workbench reads only ui.conf, so the legacy
        // file's values are not its business (window size stays there, D-03).
        TuiSettings legacyLoaded;
        loadTuiSettings(legacyLoaded, iniOf(dir));
        CHECK(legacyLoaded.windowWidth == 1024 && legacyLoaded.windowHeight == 768);
        ShellPrefs reloaded;
        loadUiConf(reloaded, iniOf(dir));
        CHECK(samePrefs(reloaded, prefs));
    }

    // ---------------------------------------------------------------------------
    section("an explicit save: identical bytes, the file's mode, the temporary");
    // ---------------------------------------------------------------------------
    {
        const std::string dir = caseDir("skip");
        const std::string conf = confOf(dir);
        removeFile(conf);
        ShellPrefs prefs;
        prefs.rawLog = kRawLogDebug;
        saveUiConf(prefs, iniOf(dir));
        CHECK(fileExists(conf));
        CHECK(setWriteTime(conf, 1000));
        CHECK(writeTime(conf) == 1000);
        saveUiConf(prefs, iniOf(dir)); // nothing changed
        CHECK(writeTime(conf) == 1000);
        prefs.rawLog = kRawLogTrace;
        saveUiConf(prefs, iniOf(dir)); // a change is written
        CHECK(writeTime(conf) != 1000);

        // The replaced file keeps its mode: `Pin.*` carries session ids, so a user
        // who tightened it does not get 0644 back.
        CHECK(setFileMode(conf, 0600));
        CHECK(fileMode(conf) == 0600);
        prefs.rawLog = kRawLogRaw;
        saveUiConf(prefs, iniOf(dir));
        CHECK(fileMode(conf) == 0600);

        // A temporary left behind by a killed writer is removed even when the save
        // itself has nothing to write.
        writeText(conf + ".tmp", "Version=1\n");
        saveUiConf(prefs, iniOf(dir)); // identical bytes: skipped
        CHECK(!fileExists(conf + ".tmp"));
        CHECK(fileMode(conf) == 0600);
    }

    // ---------------------------------------------------------------------------
    section("a brand-new file goes through the umask; a replaced one keeps its mode");
    // ---------------------------------------------------------------------------
    {
        // `ui.conf` carries session ids in `Pin.*`: the umask is the user's own
        // policy on who may read it, and a first save must not bypass that. Each
        // mode gets a fresh profile, because a settled profile is not rewritten
        // (that is the quit-save rule above).
        const mode_t previous = ::umask(077);
        {
            const std::string dir = caseDir("umask-077");
            removeFile(confOf(dir));
            ShellPrefs prefs;
            prefs.rawLog = kRawLogDebug;
            saveUiConf(prefs, iniOf(dir));
            CHECK(fileMode(confOf(dir)) == 0600);
            // A mode the user set on an existing file still wins over the umask.
            CHECK(setFileMode(confOf(dir), 0660));
            prefs.rawLog = kRawLogTrace;
            saveUiConf(prefs, iniOf(dir));
            CHECK(fileMode(confOf(dir)) == 0660);
        }
        {
            const std::string dir = caseDir("umask-027");
            removeFile(confOf(dir));
            ShellPrefs prefs;
            prefs.rawLog = kRawLogDebug;
            ::umask(027);
            saveUiConf(prefs, iniOf(dir));
            CHECK(fileMode(confOf(dir)) == 0640);
        }
        {
            const std::string dir = caseDir("umask-022");
            removeFile(confOf(dir));
            ShellPrefs prefs;
            prefs.rawLog = kRawLogDebug;
            ::umask(022);
            saveUiConf(prefs, iniOf(dir));
            CHECK(fileMode(confOf(dir)) == 0644);
        }
        ::umask(previous);
    }

    std::printf("\nscratch files: ./%s (left in place, safe to delete)\n", kScratch.c_str());
    if (g_failures == 0) {
        std::printf("all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
