// test_ui_settings.cpp
//
// Standalone unit test for the workbench settings file (model/settings.h).
// `ui.conf` is the ONE settings file: every persisted key - the view choices and
// the window size/text scale alike - lives in it, and `model/ui_conf.cpp` is its
// only writer.
//
// Compiles model/ui_conf.cpp directly and links only the
// standard library (no GLFW, no ImGui, no display). No test framework: CHECK(cond)
// prints each check and sets a fail flag; exits 0 if every check passes.
//
// Covers:
//   - uiConfPath: the file named "ui.conf" beside the ini file ("imgui2.ini" ->
//     "ui.conf", "a/b/imgui2.ini" -> "a/b/ui.conf", "" -> "" and no file touched)
//   - the exact on-disk bytes, defaults and all-non-default
//   - a full round-trip of every key and every value
//   - the Version gate: a file from a future build keeps the defaults, whatever the
//     order of its lines; a missing or malformed Version is applied
//   - unknown keys skipped, malformed values keep the current value, out-of-range
//     numbers clamp - the same expectations as ./test_settings
//   - legacy `Pin.*` keys from the agent's old pin storage are ignored, and every
//     other key in such a profile still loads
//   - the atomic write: a temporary left behind by a killed writer is never read and
//     the old file stays intact; a failed swap removes the temporary
//   - 100 changes in 1 s produce no write at all before the 2 s idle window and
//     exactly one after it; a failed write does not become a retry loop
//   - the read-only-location warning is one line per failed attempt, and the values
//     still apply in memory
//   - a stale `.gui.conf` beside `ui.conf` is dead to the build: never read,
//     never rewritten, never removed - no migration, no cleanup code
//   - an explicit save of identical bytes leaves the file (and its timestamp) alone
//   - a quit save carries only a change: a Version=3 file and a hand-edited file
//     keep their bytes, while a real choice replaces them
//   - a brand-new file is created subject to the process umask (umask 077 -> 0600);
//     a replaced file keeps the mode the user set on it
//
// Run headless from the build dir (scratch files land under
// ./test_ui_settings_scratch; the directory is left in place and is safe to delete):
//   ./test_ui_settings

#include <clocale>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/time.h>

#include "model/settings.h"

using llmfun::tui::model::flushUiConf;
using llmfun::tui::model::InspectorTab;
using llmfun::tui::model::kRawLogError;
using llmfun::tui::model::kRawLogInfo;
using llmfun::tui::model::kRawLogTrace;
using llmfun::tui::model::kRawLogWarn;
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
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool isDirectory(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

/// Whole seconds of the file's mtime; -1 when it cannot be read.
std::int64_t writeTime(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0)
        return -1;
    return static_cast<std::int64_t>(info.st_mtime);
}

bool setWriteTime(const std::string& path, std::int64_t seconds) {
    struct timeval times[2];
    times[0].tv_sec = static_cast<time_t>(seconds);
    times[0].tv_usec = 0;
    times[1] = times[0];
    return ::utimes(path.c_str(), times) == 0;
}

std::int64_t fileMode(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0)
        return -1;
    return static_cast<std::int64_t>(info.st_mode & 07777);
}

bool setFileMode(const std::string& path, mode_t mode) { return ::chmod(path.c_str(), mode) == 0; }

bool samePrefs(const ShellPrefs& a, const ShellPrefs& b) {
    return a.sendKey == b.sendKey && a.rawLog == b.rawLog && a.railOpen == b.railOpen &&
           a.inspectorOpen == b.inspectorOpen && a.tab == b.tab && a.windowWidth == b.windowWidth &&
           a.windowHeight == b.windowHeight && a.fontScaleMain == b.fontScaleMain;
}

} // namespace

int main() {
    // ---------------------------------------------------------------------------
    section("uiConfPath: the fixed ui.conf beside the ini file");
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
        prefs.rawLog = kRawLogError;
        saveUiConf(prefs, "");
        loadUiConf(prefs, "");
        CHECK(prefs.rawLog == kRawLogError);
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
        loadUiConf(prefs, iniOf(dir));
        CHECK(prefs.sendKey == SendKey::Enter && prefs.rawLog == kRawLogTrace &&
              prefs.railOpen == 1 && prefs.inspectorOpen == 0 && prefs.tab == InspectorTab::Agents);
        CHECK(!fileExists(confOf(dir))); // a load never creates the file
    }

    // ---------------------------------------------------------------------------
    section("the exact on-disk bytes");
    // ---------------------------------------------------------------------------
    {
        const std::string dir = caseDir("bytes");
        removeFile(confOf(dir));
        saveUiConf(ShellPrefs{}, iniOf(dir));
        CHECK(readText(confOf(dir)) == "Version=2\n"
                                       "SendKey=tab\n"
                                       "UiLog=info\n"
                                       "RailOpen=auto\n"
                                       "InspectorOpen=auto\n"
                                       "InspectorTab=activity\n"
                                       "FontScaleMain=1.00\n");
    }
    {
        const std::string dir = caseDir("bytes-full");
        removeFile(confOf(dir));
        ShellPrefs prefs;
        prefs.sendKey = SendKey::None;
        prefs.rawLog = kRawLogError;
        prefs.railOpen = 1;
        prefs.inspectorOpen = 0;
        prefs.tab = InspectorTab::Agents;
        saveUiConf(prefs, iniOf(dir));
        CHECK(readText(confOf(dir)) == "Version=2\n"
                                       "SendKey=none\n"
                                       "UiLog=error\n"
                                       "RailOpen=1\n"
                                       "InspectorOpen=0\n"
                                       "InspectorTab=agents\n"
                                       "FontScaleMain=1.00\n");
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
        const SendKey keys[] = {SendKey::None, SendKey::Enter, SendKey::Tab};
        const InspectorTab tabs[] = {InspectorTab::Agents, InspectorTab::Activity};
        const int levels[] = {kRawLogTrace, kRawLogInfo, kRawLogWarn, kRawLogError};
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
        // The writer never emits a word the parser rejects: a value off the ladder is
        // spelled as the default it reads as, and reads back as that.
        ShellPrefs wild;
        wild.rawLog = 99;
        wild.railOpen = 7;
        wild.inspectorOpen = -42;
        const std::string text = uiConfText(wild);
        CHECK(text.find("UiLog=info\n") != std::string::npos);
        CHECK(text.find("RailOpen=1\n") != std::string::npos);
        CHECK(text.find("InspectorOpen=auto\n") != std::string::npos);
        saveUiConf(wild, ini);
        ShellPrefs clamped;
        loadUiConf(clamped, ini);
        CHECK(clamped.rawLog == kRawLogInfo && clamped.railOpen == 1 &&
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
        writeText(conf, "Version=3\nSendKey=enter\nUiLog=trace\nPin.s-1=1\n");
        ShellPrefs future;
        loadUiConf(future, ini);
        CHECK(samePrefs(future, ShellPrefs{}));
        writeText(conf, "SendKey=enter\nUiLog=trace\nVersion=3\n");
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
                        "SendKey=banana\n"
                        "UiLog=verbose\n"
                        "RailOpen=banana\n"
                        "InspectorTab=main\n"
                        "InspectorOpen=7\n");
        ShellPrefs prefs;
        prefs.sendKey = SendKey::None;
        prefs.rawLog = kRawLogError;
        prefs.railOpen = 0;
        prefs.inspectorOpen = 0;
        prefs.tab = InspectorTab::Agents;
        loadUiConf(prefs, ini);
        CHECK(prefs.sendKey == SendKey::None);
        CHECK(prefs.rawLog == kRawLogError);
        CHECK(prefs.railOpen == 0);
        CHECK(prefs.tab == InspectorTab::Agents);
        CHECK(prefs.inspectorOpen == 1); // 7 clamps to the open end

        // Keys are matched case-sensitively and are not trimmed, exactly like
        // tui_settings: "sendkey" and "SendKey " are unknown keys.
        writeText(conf, "sendkey=enter\n SendKey=enter\nSendKey =enter\n");
        ShellPrefs keys;
        loadUiConf(keys, ini);
        CHECK(keys.sendKey == SendKey::Tab);

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

        // The ladder's words are the only ones that map: a stale word from an older format
        // (`off`, `raw`, `debug`) and any garbage read as the shipped default (`info`), exactly
        // like a missing line.
        bool staleReadsDefault = true;
        for (const char* stale : {"off", "raw", "debug", "verbose"}) {
            writeText(conf, std::string("UiLog=") + stale + "\n");
            ShellPrefs defaulted;
            loadUiConf(defaulted, ini);
            staleReadsDefault = staleReadsDefault && defaulted.rawLog == kRawLogInfo;
        }
        CHECK(staleReadsDefault);

        // No legacy vocabulary: a word outside the none/enter/tab set is simply
        // unknown. It leaves the stored value in place (I-09), so on the shipped
        // default an unparseable word falls back to `tab`.
        bool unknownIgnored = true;
        for (const char* unknown : {"quux", "newlinewithenter"}) {
            writeText(conf, std::string("SendKey=") + unknown + "\n");
            ShellPrefs kept;
            kept.sendKey = SendKey::Enter; // any value proves the ignore
            loadUiConf(kept, ini);
            unknownIgnored = unknownIgnored && kept.sendKey == SendKey::Enter;
        }
        CHECK(unknownIgnored);
        writeText(conf, "SendKey=quux\n");
        ShellPrefs fallback;
        loadUiConf(fallback, ini);
        CHECK(fallback.sendKey == SendKey::Tab);

        // ... and the round-trip: an unknown word loads as the default, and the
        // canonical text a later save would write is the default's (a save of an
        // unchanged load is a no-op - it never canonicalizes a file nobody
        // touched).
        CHECK(uiConfText(fallback) == "Version=2\n"
                                      "SendKey=tab\n"
                                      "UiLog=info\n"
                                      "RailOpen=auto\n"
                                      "InspectorOpen=auto\n"
                                      "InspectorTab=activity\n"
                                      "FontScaleMain=1.00\n");

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
        writeText(conf, "Version=1\r\nSendKey=enter\r\nUiLog=error");
        ShellPrefs crlf;
        loadUiConf(crlf, ini);
        CHECK(crlf.sendKey == SendKey::Enter && crlf.rawLog == kRawLogError);
    }

    // ---------------------------------------------------------------------------
    section("legacy pin keys: ignored, everything else loads");
    // ---------------------------------------------------------------------------
    {
        // Pins live in the agent's `state.json`, so the workbench parser
        // ignores `Pin.*` keys: an old profile still loads the keys it can,
        // and the next write of a real change drops the pin keys for good.
        const std::string dir = caseDir("legacy-pins");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);

        writeText(conf, "Version=1\nSendKey=enter\nPin.s-a=1\nPin.s-b=0\nRailOpen=1\n");
        ShellPrefs prefs;
        loadUiConf(prefs, ini);
        CHECK(prefs.sendKey == SendKey::Enter && prefs.railOpen == 1);

        // A change rewrites the profile in this build's canonical form.
        prefs.rawLog = kRawLogError;
        saveUiConf(prefs, ini);
        CHECK(readText(conf).find("Pin.") == std::string::npos);
        ShellPrefs reloaded;
        loadUiConf(reloaded, ini);
        CHECK(samePrefs(reloaded, prefs));
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
        writeText(conf + ".tmp", "Version=1\nSendKey=none\nRailOpen=0\nUiLog=trace\n");
        ShellPrefs prefs;
        loadUiConf(prefs, ini);
        CHECK(prefs.sendKey == SendKey::Enter); // the intact file, not the temporary
        CHECK(prefs.railOpen == -1 && prefs.rawLog == kRawLogInfo);
        CHECK(readText(conf) == before); // and loading does not "repair" the file
        CHECK(fileExists(conf + ".tmp"));

        // The next real save replaces the file and takes the temporary with it.
        prefs.sendKey = SendKey::None;
        saveUiConf(prefs, ini);
        CHECK(!fileExists(conf + ".tmp"));
        CHECK(readText(conf) == uiConfText(prefs));
        ShellPrefs reloaded;
        loadUiConf(reloaded, ini);
        CHECK(reloaded.sendKey == SendKey::None);
    }
    {
        // The swap itself can fail (here: the destination is a directory, so the
        // rename is rejected). The temporary is removed, the old thing stays, and the
        // failure is one warning instead of a crash.
        const std::string dir = caseDir("swapfail");
        removeFile(dir + "/ui.conf");
        CHECK(::mkdir((dir + "/ui.conf").c_str(), 0755) == 0);
        ShellPrefs prefs;
        prefs.rawLog = kRawLogError;
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
                                     "SendKey=none\n"
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
        const std::string newer = "Version=3\nSendKey=enter\nBrand=1\n";
        writeText(conf, newer);
        ShellPrefs prefs;
        loadUiConf(prefs, ini);
        CHECK(samePrefs(prefs, ShellPrefs{})); // defaults, not half-applied
        flushUiConf(prefs, ini, 10.0);
        prefs.rawLog = kRawLogError;
        flushUiConf(prefs, ini, 20.0);
        flushUiConf(prefs, ini, 30.0);
        CHECK(readText(conf) == newer);
        CHECK(!fileExists(conf + ".tmp"));
        // Starting and quitting this build changes nothing and writes nothing:
        // the backend hands the final window size to the settings struct on the
        // way out (tui_api.cpp's teardown), and that size-only difference is
        // the app's own doing - the session moved no window - so the newer file
        // keeps its bytes.
        ShellPrefs untouched;
        loadUiConf(untouched, ini);
        untouched.windowWidth = 1280;
        untouched.windowHeight = 820;
        saveUiConf(untouched, ini);
        CHECK(readText(conf) == newer);
        // Even a moved window must not spend the format guard: a file this
        // build cannot read is never rewritten for a size - only the real
        // preference change below does that.
        saveUiConf(untouched, ini, /*windowResized=*/true);
        CHECK(readText(conf) == newer);
        // A real choice is the user's own action and settles the profile: the
        // newer file is replaced by this build's Version=2 file.
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
        // The shutdown size capture is not a change of its own either: the
        // session moved no window, so the capture is the app's own doing, and
        // a clean exit still leaves the user's spelling and unknown keys alone.
        prefs.windowWidth = 1280;
        prefs.windowHeight = 820;
        saveUiConf(prefs, ini);
        CHECK(readText(conf) == handEdited);
        // A change is written in this build's canonical form.
        prefs.rawLog = kRawLogError;
        saveUiConf(prefs, ini);
        CHECK(readText(conf) == uiConfText(prefs));
    }

    // ---------------------------------------------------------------------------
    section("the merged file: window size and text scale (version 2)");
    // ---------------------------------------------------------------------------
    {
        // Byte-exact v2: all eight persisted keys, in the format's own order.
        const std::string dir = caseDir("v2-roundtrip");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);
        removeFile(conf);
        ShellPrefs prefs;
        prefs.windowWidth = 1280;
        prefs.windowHeight = 800;
        prefs.fontScaleMain = 1.25f;
        const std::string v2Bytes = "Version=2\n"
                                    "SendKey=tab\n"
                                    "UiLog=info\n"
                                    "RailOpen=auto\n"
                                    "InspectorOpen=auto\n"
                                    "InspectorTab=activity\n"
                                    "WindowWidth=1280\n"
                                    "WindowHeight=800\n"
                                    "FontScaleMain=1.25\n";
        saveUiConf(prefs, ini);
        CHECK(readText(conf) == v2Bytes);
        ShellPrefs loaded;
        loadUiConf(loaded, ini);
        CHECK(samePrefs(loaded, prefs));
        // Nothing in the writer may look at LC_NUMERIC (the app adopts the
        // user's locale, tui_api.cpp): a comma-decimal desktop must not write
        // `FontScaleMain=1,25`, which this file's own locale-free parser would
        // reject on the next start. The image only ships C/POSIX/C.utf8, so this
        // is a no-op there and a real check where the locale exists.
        if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr) {
            saveUiConf(prefs, ini);
            CHECK(readText(conf) == v2Bytes);
            std::setlocale(LC_NUMERIC, "C");
            std::printf("OK: the v2 writer is locale-independent (de_DE.UTF-8)\n");
        }
    }
    {
        // A resize the session made is the user's geometry moving: the backend
        // reports the fact (the captured size differs from the size the window
        // was granted at open), and the fact alone when nothing else changed.
        const std::string dir = caseDir("v2-resize");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);
        removeFile(conf);
        ShellPrefs prefs;
        prefs.windowWidth = 1280;
        prefs.windowHeight = 800;
        prefs.fontScaleMain = 1.25f;
        saveUiConf(prefs, ini);
        ShellPrefs loaded;
        loadUiConf(loaded, ini);
        CHECK(loaded.windowWidth == 1280); // the size the profile carried
        loaded.windowWidth = 1400;         // the backend's final glfwGetWindowSize
        loaded.windowHeight = 900;
        saveUiConf(loaded, ini, /*windowResized=*/true);
        CHECK(readText(conf) == uiConfText(loaded));
        CHECK(readText(conf).find("WindowWidth=1400\n") != std::string::npos);
        ShellPrefs reloaded;
        loadUiConf(reloaded, ini);
        CHECK(samePrefs(reloaded, loaded));
        // The fact, not the profile's shape, decides: the same size-only
        // difference without a moved window is the app's own doing - the floor
        // it enforces - and the file keeps the size the profile carries.
        loadUiConf(reloaded, ini);
        reloaded.windowWidth = 1500;
        reloaded.windowHeight = 950;
        saveUiConf(reloaded, ini);
        CHECK(readText(conf) == uiConfText(loaded));
    }
    {
        // A fresh profile owes nothing: the window the app chose at startup is
        // not the user's doing, so a clean exit that moved no window and
        // changed no preference writes no file at all - the size rides only a
        // real change.
        const std::string dir = caseDir("fresh-no-write");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);
        removeFile(conf);
        ShellPrefs prefs;
        loadUiConf(prefs, ini);   // no file: the defaults are the baseline
        prefs.windowWidth = 1280; // the backend's final glfwGetWindowSize
        prefs.windowHeight = 820;
        saveUiConf(prefs, ini);
        CHECK(!fileExists(conf));
        CHECK(!fileExists(conf + ".tmp"));
    }
    {
        // A fresh profile whose window the session moved is the user's
        // geometry: the backend's fact turns the captured size into a real
        // change, so the first move gives the profile its size - persistence
        // must not depend on the file already carrying one.
        const std::string dir = caseDir("fresh-resize");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);
        removeFile(conf);
        ShellPrefs prefs;
        loadUiConf(prefs, ini);   // no file: the defaults are the baseline
        prefs.windowWidth = 1100; // the backend's final glfwGetWindowSize
        prefs.windowHeight = 750;
        saveUiConf(prefs, ini, /*windowResized=*/true);
        CHECK(readText(conf) == uiConfText(prefs));
        ShellPrefs reloaded;
        loadUiConf(reloaded, ini);
        CHECK(samePrefs(reloaded, prefs));
    }
    {
        // The "not persisted" sentinel is the writer's omission, never a key
        // with a 0 value: a default struct writes no size keys at all.
        const std::string text = uiConfText(ShellPrefs{});
        CHECK(text.find("WindowWidth=") == std::string::npos);
        CHECK(text.find("WindowHeight=") == std::string::npos);
        const std::string scaleTail = "FontScaleMain=1.00\n";
        CHECK(text.size() >= scaleTail.size() &&
              text.compare(text.size() - scaleTail.size(), scaleTail.size(), scaleTail) == 0);
    }
    {
        // Hand-edited sizes clamp into the legal range: 0 (which the writer
        // never emits) is a size request, not the "not persisted" sentinel,
        // and negative or huge magnitudes land on the nearest end.
        const std::string dir = caseDir("v2-sizes");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);
        const char* texts[] = {"WindowWidth=0\n", "WindowWidth=-5\n",
                               "WindowWidth=99999999999999999999\n"};
        const int widths[] = {1, 1, 32767};
        bool sizesClamped = true;
        for (std::size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i) {
            writeText(conf, texts[i]);
            ShellPrefs prefs;
            loadUiConf(prefs, ini);
            sizesClamped =
                sizesClamped && prefs.windowWidth == widths[i] && prefs.windowHeight == 0;
        }
        CHECK(sizesClamped);
        // A malformed size keeps the current value.
        writeText(conf, "WindowWidth=12x8\n");
        ShellPrefs keep;
        keep.windowWidth = 800;
        loadUiConf(keep, ini);
        CHECK(keep.windowWidth == 800);
    }
    {
        // An in-range hand-edited scale loads snapped to the 5 % grid - a
        // silent value change, accepted by the format's design (the value the
        // UI can draw is what a later save writes back). Anything the model
        // cannot draw - malformed, NaN, out of range - keeps the current value.
        const std::string dir = caseDir("v2-scale");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);
        writeText(conf, "FontScaleMain=1.23\n");
        ShellPrefs snapped;
        loadUiConf(snapped, ini);
        CHECK(snapped.fontScaleMain == 1.25f);
        const char* rejected[] = {"FontScaleMain=abc\n", "FontScaleMain=9.0\n",
                                  "FontScaleMain=nan\n", "FontScaleMain=1e50\n",
                                  "FontScaleMain=0.25\n"};
        bool keptCurrent = true;
        for (const char* text : rejected) {
            writeText(conf, text);
            ShellPrefs prefs;
            prefs.fontScaleMain = 1.5f;
            loadUiConf(prefs, ini);
            keptCurrent = keptCurrent && prefs.fontScaleMain == 1.5f;
        }
        CHECK(keptCurrent);
    }
    {
        // A v1 file still loads: its six keys apply and the merged format's
        // new keys keep their defaults. No migration - and a quit that changed
        // nothing does not rewrite the old file into v2 bytes either.
        const std::string dir = caseDir("v1-still-loads");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);
        const std::string v1 = "Version=1\n"
                               "SendKey=enter\n"
                               "UiLog=debug\n"
                               "RailOpen=1\n"
                               "InspectorOpen=0\n"
                               "InspectorTab=agents\n";
        writeText(conf, v1);
        ShellPrefs prefs;
        loadUiConf(prefs, ini);
        // `debug` is not a word of this format's ladder, so the level reads as the shipped
        // default (`info`) - the other five keys still apply.
        CHECK(prefs.sendKey == SendKey::Enter && prefs.rawLog == kRawLogInfo &&
              prefs.railOpen == 1 && prefs.inspectorOpen == 0 && prefs.tab == InspectorTab::Agents);
        CHECK(prefs.windowWidth == 0 && prefs.windowHeight == 0 && prefs.fontScaleMain == 1.0f);
        saveUiConf(prefs, ini);
        CHECK(readText(conf) == v1);
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
            prefs.rawLog = (i % 2 == 0) ? kRawLogInfo : kRawLogError;
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
        CHECK(prefs.rawLog == kRawLogError);
        CHECK(readText(conf) == "Version=2\n"
                                "SendKey=tab\n"
                                "UiLog=error\n"
                                "RailOpen=auto\n"
                                "InspectorOpen=auto\n"
                                "InspectorTab=activity\n"
                                "FontScaleMain=1.00\n");

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
        prefs.rawLog = kRawLogError;
        saveUiConf(prefs, ini);
        std::string warning;
        CHECK(takeUiConfWarning(warning));
        CHECK(!warning.empty());
        CHECK(!takeUiConfWarning(warning));  // one line, not one per call
        CHECK(prefs.rawLog == kRawLogError); // the value still applies in memory

        // The per-frame path cannot become a retry loop: a failed write ends the
        // window, so the same bytes are never attempted again.
        double now = 10.0;
        flushUiConf(prefs, ini, now);
        prefs.rawLog = kRawLogInfo;
        flushUiConf(prefs, ini, now + 0.1);
        flushUiConf(prefs, ini, now + kUiConfIdleSeconds + 1.0); // one attempt
        CHECK(takeUiConfWarning(warning));
        flushUiConf(prefs, ini, now + 4.0);
        flushUiConf(prefs, ini, now + 30.0);
        CHECK(!takeUiConfWarning(warning));
        CHECK(prefs.rawLog == kRawLogInfo);

        // A new change is a new attempt, and is reported once.
        prefs.rawLog = kRawLogTrace;
        flushUiConf(prefs, ini, now + 40.0);
        flushUiConf(prefs, ini, now + 50.0);
        CHECK(takeUiConfWarning(warning));
        CHECK(!takeUiConfWarning(warning));
    }

    // ---------------------------------------------------------------------------
    section("a stale .gui.conf beside ui.conf is dead to the build");
    // ---------------------------------------------------------------------------
    {
        // A stale `<stem>.gui.conf` beside `ui.conf` is dead to the build: it is
        // never read, never rewritten and never removed, and no warning mentions
        // it - the user reconfigures rather than the build migrating anything.
        const std::string dir = caseDir("stale-gui");
        const std::string ini = iniOf(dir);
        const std::string stale = dir + "/imgui2.gui.conf";
        removeFile(confOf(dir));
        removeFile(stale);
        const std::string staleBytes = "WindowWidth=1024\nWindowHeight=768\nFontScaleMain=1.50\n";
        writeText(stale, staleBytes);

        // The load reads ui.conf only: the stale file contributes nothing.
        ShellPrefs prefs;
        loadUiConf(prefs, ini);
        CHECK(prefs.windowWidth == 0 && prefs.windowHeight == 0 && prefs.fontScaleMain == 1.0f);
        CHECK(prefs.sendKey == SendKey::Tab);

        // A save rewrites ui.conf and leaves the stale file byte-identical.
        prefs.sendKey = SendKey::Enter;
        prefs.rawLog = kRawLogTrace;
        saveUiConf(prefs, ini);
        CHECK(readText(confOf(dir)).find("SendKey=enter") != std::string::npos &&
              readText(confOf(dir)).find("Version=2") == 0);
        CHECK(readText(stale) == staleBytes);
        std::string warning;
        CHECK(!takeUiConfWarning(warning)); // the ignored file is silent, too

        // A reload applies ui.conf's values; the stale file is still not read,
        // and a later save still does not touch it.
        ShellPrefs reloaded;
        loadUiConf(reloaded, ini);
        CHECK(samePrefs(reloaded, prefs));
        CHECK(readText(stale) == staleBytes);
    }

    // ---------------------------------------------------------------------------
    section("an explicit save: identical bytes, the file's mode, the temporary");
    // ---------------------------------------------------------------------------
    {
        const std::string dir = caseDir("skip");
        const std::string conf = confOf(dir);
        removeFile(conf);
        ShellPrefs prefs;
        prefs.rawLog = kRawLogError;
        saveUiConf(prefs, iniOf(dir));
        CHECK(fileExists(conf));
        CHECK(setWriteTime(conf, 1000));
        CHECK(writeTime(conf) == 1000);
        saveUiConf(prefs, iniOf(dir)); // nothing changed
        CHECK(writeTime(conf) == 1000);
        prefs.rawLog = kRawLogTrace;
        saveUiConf(prefs, iniOf(dir)); // a change is written
        CHECK(writeTime(conf) != 1000);

        // The replaced file keeps its mode: the file holds the user's own
        // choices, so a user who tightened it does not get 0644 back.
        CHECK(setFileMode(conf, 0600));
        CHECK(fileMode(conf) == 0600);
        prefs.rawLog = kRawLogInfo;
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
        // The umask is the user's own policy on who may read the profile: a
        // first save must not bypass that. Each
        // mode gets a fresh profile, because a settled profile is not rewritten
        // (that is the quit-save rule above).
        const mode_t previous = ::umask(077);
        {
            const std::string dir = caseDir("umask-077");
            removeFile(confOf(dir));
            ShellPrefs prefs;
            prefs.rawLog = kRawLogError;
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
            prefs.rawLog = kRawLogError;
            ::umask(027);
            saveUiConf(prefs, iniOf(dir));
            CHECK(fileMode(confOf(dir)) == 0640);
        }
        {
            const std::string dir = caseDir("umask-022");
            removeFile(confOf(dir));
            ShellPrefs prefs;
            prefs.rawLog = kRawLogError;
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
