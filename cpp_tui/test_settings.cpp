// test_settings.cpp
//
// Standalone unit test for the merged settings file (model/ui_conf.cpp): the
// `ui.conf` path derivation, the exact on-disk v2 bytes, per-key load with
// strict parsing, clamping and fallbacks, and the save-side guards.
//
// Compiles model/ui_conf.cpp directly and links only the standard library (no
// GLFW, no ImGui, no display). No test framework: CHECK(cond) prints each check
// and sets a fail flag; exits 0 if every check passes.
//
// Covers:
//   - uiConfPath: "imgui2.ini" -> "ui.conf"; "a/b/imgui2.ini" -> "a/b/ui.conf";
//     "" -> "" and nothing touched
//   - missing file -> load leaves the struct at its values (defaults)
//   - empty iniFilename: load/save no-ops, no file touched
//   - roundtrip of all eight keys, the exact on-disk v2 byte block, and the
//     0-vs-omitted sentinel (the writer omits WindowWidth/WindowHeight 0)
//   - per-key parse: unknown keys and lines without '=' skipped; malformed
//     values keep the current value; word keys read case-insensitively, number
//     keys clamp into range, in-range font scales quantize to the 5 % grid
//   - save-side guards: empty path, unwritable path
//
// Run headless from the build dir (scratch files land under
// ./test_settings_scratch):
//   ./build/tui/test_settings

#include <array>
#include <cstdio>
#include <string>

#include <sys/stat.h>

#include "model/settings.h"

using llmfun::tui::model::InspectorTab;
using llmfun::tui::model::kRawLogOff;
using llmfun::tui::model::kRawLogTrace;
using llmfun::tui::model::loadUiConf;
using llmfun::tui::model::saveUiConf;
using llmfun::tui::model::SendKey;
using llmfun::tui::model::ShellPrefs;
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

const std::string kScratch = "test_settings_scratch";

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
    std::array<char, 256> buf{};
    size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0)
        out.append(buf.data(), n);
    std::fclose(f);
    return out;
}

bool fileExists(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    std::fclose(f);
    return true;
}

/// A case directory of its own: `ui.conf` has a fixed name, and the debounce
/// state is keyed by path - one directory per case is one clean file and one
/// clean debounce window (and rerunning the binary is idempotent).
std::string caseDir(const char* name) {
    const std::string dir = kScratch + "/" + name;
    ::mkdir(kScratch.c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
    std::remove((dir + "/ui.conf.tmp").c_str());
    removeFile(confOf(dir));
    return dir;
}

} // namespace

int main() {
    // Scratch hygiene: a leftover ui.conf from an earlier run would make the
    // "missing file" cases read it instead, so every case starts clean.

    // The merged struct: one ShellPrefs carries the view keys with 0 = "no
    // persisted size" and scale 1.0 as the defaults.
    {
        const ShellPrefs prefs{};
        CHECK(prefs.windowWidth == 0);
        CHECK(prefs.windowHeight == 0);
        CHECK(prefs.fontScaleMain == 1.0f);
        CHECK(prefs.sendKey == SendKey::Tab);
        CHECK(prefs.railOpen == -1);
        CHECK(prefs.inspectorOpen == -1);
    }

    // uiConfPath: the file named "ui.conf" beside the ini file; no name, no path.
    CHECK(uiConfPath("imgui2.ini") == "ui.conf");
    CHECK(uiConfPath("a/b/imgui2.ini") == "a/b/ui.conf");
    CHECK(uiConfPath("imgui2") == "ui.conf");
    CHECK(uiConfPath("") == "");

    {
        const std::string dir = caseDir("missing");
        const std::string ini = iniOf(dir);
        // missing file: the struct keeps its values
        {
            ShellPrefs s; // defaults
            loadUiConf(s, ini);
            CHECK(s.fontScaleMain == 1.0f && s.windowWidth == 0 && s.windowHeight == 0);
        }
        {
            ShellPrefs s;
            s.fontScaleMain = 1.5f;
            s.windowWidth = 800;
            s.windowHeight = 600;
            loadUiConf(s, ini); // missing file is a no-op
            CHECK(s.fontScaleMain == 1.5f && s.windowWidth == 800 && s.windowHeight == 600);
        }
    }

    {
        const std::string dir = caseDir("emptyname");
        const std::string ini = iniOf(dir);
        writeText(confOf(dir), "Version=1\nSendKey=enter\n");
        // empty iniFilename: load/save no-ops, the file is not touched
        {
            ShellPrefs s;
            s.fontScaleMain = 2.0f;
            s.windowWidth = 640;
            s.windowHeight = 480;
            loadUiConf(s, ""); // nothing to read, values untouched
            CHECK(s.fontScaleMain == 2.0f && s.windowWidth == 640 && s.windowHeight == 480);
            saveUiConf(s, ""); // no name -> no path, nothing written
            CHECK(readText(confOf(dir)) == "Version=1\nSendKey=enter\n");
        }
    }

    // roundtrip: all eight keys and the exact on-disk v2 bytes
    {
        const std::string dir = caseDir("roundtrip");
        const std::string ini = iniOf(dir);
        const std::string conf = confOf(dir);
        ShellPrefs s;
        s.sendKey = SendKey::Enter;
        s.rawLog = kRawLogTrace;
        s.railOpen = 1;
        s.inspectorOpen = 0;
        s.tab = InspectorTab::Activity;
        s.windowWidth = 1024;
        s.windowHeight = 768;
        s.fontScaleMain = 1.25f;
        saveUiConf(s, ini);
        CHECK(fileExists(conf));
        CHECK(readText(conf) == "Version=2\n"
                                "SendKey=enter\n"
                                "UiLog=trace\n"
                                "RailOpen=1\n"
                                "InspectorOpen=0\n"
                                "InspectorTab=activity\n"
                                "WindowWidth=1024\n"
                                "WindowHeight=768\n"
                                "FontScaleMain=1.25\n");
        ShellPrefs loaded;
        loadUiConf(loaded, ini);
        CHECK(loaded.sendKey == SendKey::Enter && loaded.rawLog == kRawLogTrace &&
              loaded.railOpen == 1 && loaded.inspectorOpen == 0 &&
              loaded.tab == InspectorTab::Activity && loaded.windowWidth == 1024 &&
              loaded.windowHeight == 768 && loaded.fontScaleMain == 1.25f);
        CHECK(uiConfText(loaded) == uiConfText(s));
    }

    // the writer omits WindowWidth/WindowHeight 0 ("not persisted")
    {
        const std::string dir = caseDir("nosize");
        const std::string ini = iniOf(dir);
        ShellPrefs s; // 0 = "no persisted size"
        saveUiConf(s, ini);
        CHECK(readText(confOf(dir)) ==
              "Version=2\nSendKey=tab\nUiLog=off\nRailOpen=auto\nInspectorOpen=auto\n"
              "InspectorTab=activity\nFontScaleMain=1.00\n");
        // ... and the load keeps them at the sentinel (never a clamped 1)
        ShellPrefs loaded;
        loadUiConf(loaded, ini);
        CHECK(loaded.windowWidth == 0 && loaded.windowHeight == 0);
    }

    // per-key parse: unknown keys and non-"Key=Value" lines skipped
    {
        const std::string dir = caseDir("unknown");
        const std::string ini = iniOf(dir);
        writeText(confOf(dir), "WindowWidth=1024\nUnknownKey=5\ngarbage without an equals sign\n"
                               "WindowHeight=768\nFontScaleMain=1.5\n");
        ShellPrefs s; // defaults
        loadUiConf(s, ini);
        CHECK(s.windowWidth == 1024 && s.windowHeight == 768 && s.fontScaleMain == 1.5f);
    }

    // file without a trailing newline: the final line is still parsed
    {
        const std::string dir = caseDir("nonewline");
        const std::string ini = iniOf(dir);
        writeText(confOf(dir), "WindowWidth=5"); // no '\n'
        ShellPrefs s;
        loadUiConf(s, ini);
        CHECK(s.windowWidth == 5);
    }

    // malformed values keep the current value
    {
        const std::string dir = caseDir("malformed");
        const std::string ini = iniOf(dir);
        writeText(confOf(dir), "WindowWidth=abc\nWindowHeight=12x\nFontScaleMain=abc\n");
        ShellPrefs s;
        s.windowWidth = 640;
        s.windowHeight = 480;
        s.fontScaleMain = 1.5f;
        loadUiConf(s, ini);
        CHECK(s.windowWidth == 640 && s.windowHeight == 480 && s.fontScaleMain == 1.5f);
    }
    // Empty value and trailing garbage reject. A magnitude too big for `long`
    // is different: `parseWholeNumber` clamps it, and the value then lands on
    // the range's ceiling (clamping, not rejection).
    {
        const std::string dir = caseDir("malformed2");
        const std::string ini = iniOf(dir);
        writeText(confOf(dir),
                  "WindowWidth=\nWindowHeight=99999999999999999999\nFontScaleMain=1.5 nope\n");
        ShellPrefs s;
        s.windowWidth = 640;
        s.windowHeight = 480;
        s.fontScaleMain = 1.5f;
        loadUiConf(s, ini);
        CHECK(s.windowWidth == 640 && s.windowHeight == 32767 && s.fontScaleMain == 1.5f);
    }
    // Keys are matched case-sensitively, and key text is not trimmed: leading
    // whitespace and whitespace around '=' make the line an unknown key. Word
    // *values*, in contrast, have no case to get wrong.
    {
        const std::string dir = caseDir("keycase");
        const std::string ini = iniOf(dir);
        writeText(confOf(dir),
                  "windowwidth=999\n WindowWidth=999\nWindowWidth =999\nSendKey=ENTER\n");
        ShellPrefs s;
        loadUiConf(s, ini);
        CHECK(s.windowWidth == 0 && s.windowHeight == 0);
        CHECK(s.sendKey == SendKey::Enter);
    }

    // window-size clamping into [1, 32767]; a hand-edited 0 is a request, not
    // the writer's "not persisted" sentinel
    {
        const std::string dir = caseDir("clamp");
        const std::string ini = iniOf(dir);
        struct ClampCase {
            const char* text;
            int width;
            int height;
        };
        const std::array<ClampCase, 6> cases = {{
            {"WindowWidth=0\n", 1, 0},
            {"WindowWidth=-5\n", 1, 0},
            {"WindowHeight=0\n", 0, 1},
            {"WindowWidth=100000\n", 32767, 0},
            {"WindowHeight=100000\n", 0, 32767},
            {"WindowWidth=1\nWindowHeight=32767\n", 1, 32767},
        }};
        for (const ClampCase& c : cases) {
            writeText(confOf(dir), c.text);
            ShellPrefs s;
            loadUiConf(s, ini);
            const bool ok = s.windowWidth == c.width && s.windowHeight == c.height;
            CHECK(ok);
            if (!ok)
                std::printf("  (content \"%s\" -> %dx%d, want %dx%d)\n", c.text, s.windowWidth,
                            s.windowHeight, c.width, c.height);
        }
    }

    // font scale: in-range values quantize to the 5 % grid, the rest fall back
    {
        const std::string dir = caseDir("scale");
        const std::string ini = iniOf(dir);
        struct ScaleCase {
            const char* text;
            float want;
        };
        // 1.23 is off the grid on purpose: the load answers the value the UI
        // can actually draw (pinned here so it stays deliberate).
        const std::array<ScaleCase, 9> cases = {{
            {"FontScaleMain=0.5\n", 0.5f},
            {"FontScaleMain=1\n", 1.0f},
            {"FontScaleMain=1.25\n", 1.25f},
            {"FontScaleMain=2.0\n", 2.0f},
            {"FontScaleMain=3\n", 3.0f},
            {"FontScaleMain=1.23\n", 1.25f},
            {"FontScaleMain=nan\n", 1.0f},
            {"FontScaleMain=1e40\n", 1.0f},
            {"FontScaleMain=4\n", 1.0f},
        }};
        for (const ScaleCase& c : cases) {
            writeText(confOf(dir), c.text);
            ShellPrefs s;
            loadUiConf(s, ini);
            const bool ok = s.fontScaleMain == c.want;
            CHECK(ok);
            if (!ok)
                std::printf("  (content \"%s\" -> %.4f, want %.4f)\n", c.text, s.fontScaleMain,
                            c.want);
        }
    }
    // A value the parser rejects keeps the current one (0, 0.4 and -inf are
    // outside the range; "1.5x" is malformed).
    {
        const std::string dir = caseDir("scale-reject");
        const std::string ini = iniOf(dir);
        const std::array<const char*, 4> rejected = {"FontScaleMain=0\n", "FontScaleMain=0.4\n",
                                                     "FontScaleMain=-inf\n",
                                                     "FontScaleMain=1.5x\n"};
        for (const char* text : rejected) {
            writeText(confOf(dir), text);
            ShellPrefs s;
            s.fontScaleMain = 1.5f;
            loadUiConf(s, ini);
            const bool ok = s.fontScaleMain == 1.5f;
            CHECK(ok);
            if (!ok)
                std::printf("  (content \"%s\" -> %.4f, want 1.5000)\n", text, s.fontScaleMain);
        }
    }

    // Unwritable path: "blocked" is a file where a directory would be needed,
    // so confOf + "/ui.conf" cannot be created; the failure is reported through
    // the one warning channel, not silently.
    {
        const std::string dir = caseDir("unwritable");
        const std::string blocked = dir + "/blocked";
        writeText(blocked, "not a directory\n");
        ShellPrefs s;
        saveUiConf(s, blocked + "/imgui2.ini");
        std::string warning;
        CHECK(llmfun::tui::model::takeUiConfWarning(warning));
        CHECK(warning.find("cannot write") != std::string::npos);
    }

    if (g_failures == 0) {
        std::printf("all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
