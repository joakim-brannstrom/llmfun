// test_settings.cpp
//
// Standalone unit test for the GUI view settings helpers (tui_settings.h):
// imgui2.conf path derivation, per-key load with strict parsing, clamping
// and fallbacks, and the save-side guards.
//
// Compiles the helper TU directly and links only the standard library (no
// GLFW, no ImGui, no display; tui_settings.cpp includes no imgui.h/tui.h).
// No test framework: CHECK(cond) prints each check and sets a fail flag;
// exits 0 if every check passes.
//
// Covers:
//   - settingsPath: "imgui2.ini" -> "imgui2.conf"; "imgui2" -> "imgui2.conf";
//     "" -> ""; ".ini" replaced only as a trailing suffix (exact case)
//   - missing file -> load leaves the struct at its values (defaults)
//   - empty iniFilename: load/save no-ops, no file touched
//   - roundtrip and the exact on-disk bytes
//   - save omits WindowWidth/WindowHeight when 0 ("no persisted size")
//   - per-key fallbacks: unknown keys and lines without '=' skipped;
//     malformed/out-of-range values keep the current value; clamping
//   - save-side guards: empty path, unwritable path
//   - drag-end-style save with the raw ini filename lands in "<name>.conf",
//     never "<name>.conf.conf"
//
// Run headless from the repo root (scratch files land in the CWD):
//   ./build/tui/test_settings

#include <cstdio>
#include <string>

#include "tui_settings.h"

using llmfun::tui::loadTuiSettings;
using llmfun::tui::saveTuiSettings;
using llmfun::tui::settingsPath;
using llmfun::tui::TuiSettings;

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

const char* kIniName = "test_settings.ini";
const char* kPath = "test_settings.conf";

void writeText(const std::string& text) {
    std::FILE* f = std::fopen(kPath, "wb");
    if (f == nullptr || std::fwrite(text.data(), 1, text.size(), f) != text.size()) {
        std::printf("FAIL cannot write scratch file %s\n", kPath);
        ++g_failures;
        return;
    }
    std::fclose(f);
}

std::string readText() {
    std::FILE* f = std::fopen(kPath, "rb");
    if (f == nullptr)
        return "<open failed>";
    std::string out;
    char buf[128];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    std::fclose(f);
    return out;
}

bool fileExists(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr)
        return false;
    std::fclose(f);
    return true;
}

} // namespace

int main() {
    // settingsPath: trailing ".ini" (exact case) -> ".conf", else append
    CHECK(settingsPath("imgui2.ini") == "imgui2.conf");
    CHECK(settingsPath("imgui2") == "imgui2.conf");
    CHECK(settingsPath("") == "");
    CHECK(settingsPath("a/b/imgui2.ini") == "a/b/imgui2.conf");
    // ".ini" only counts at the end: a name ending elsewhere keeps it and
    // appends; the suffix match is case-sensitive.
    CHECK(settingsPath("imgui2.ini.old") == "imgui2.ini.old.conf");
    CHECK(settingsPath("imgui2.INI") == "imgui2.INI.conf");

    // missing file: the struct keeps its values
    std::remove(kPath);
    {
        TuiSettings s; // defaults: 1.0 / 0 / 0
        loadTuiSettings(s, kIniName);
        CHECK(s.fontScaleMain == 1.0f && s.windowWidth == 0 && s.windowHeight == 0);
    }
    {
        TuiSettings s;
        s.fontScaleMain = 1.5f;
        s.windowWidth = 800;
        s.windowHeight = 600;
        loadTuiSettings(s, kIniName); // missing file is a no-op
        CHECK(s.fontScaleMain == 1.5f && s.windowWidth == 800 && s.windowHeight == 600);
    }

    // empty iniFilename: load/save no-ops, no file created
    {
        TuiSettings s;
        s.fontScaleMain = 2.0f;
        s.windowWidth = 640;
        s.windowHeight = 480;
        loadTuiSettings(s, ""); // nothing to read, values untouched
        CHECK(s.fontScaleMain == 2.0f && s.windowWidth == 640 && s.windowHeight == 480);
    }
    // A save with an empty name must not clobber the previously stored file.
    writeText("WindowWidth=1024\n");
    {
        TuiSettings s; // 0/0/1.0: an unconditional save would rewrite the file
        saveTuiSettings(s, "");
        CHECK(readText() == "WindowWidth=1024\n");
    }

    // roundtrip and the exact on-disk bytes
    std::remove(kPath);
    {
        TuiSettings s;
        s.windowWidth = 1024;
        s.windowHeight = 768;
        s.fontScaleMain = 1.25f;
        saveTuiSettings(s, kIniName);
        CHECK(fileExists(kPath));
        CHECK(readText() == "WindowWidth=1024\nWindowHeight=768\nFontScaleMain=1.25\n");
        TuiSettings loaded;
        loadTuiSettings(loaded, kIniName);
        CHECK(loaded.windowWidth == 1024 && loaded.windowHeight == 768 &&
              loaded.fontScaleMain == 1.25f);
    }

    // save writes WindowWidth/WindowHeight only when > 0
    std::remove(kPath);
    {
        TuiSettings s; // 0 = "no persisted size"
        saveTuiSettings(s, kIniName);
        CHECK(readText() == "FontScaleMain=1.00\n");
    }

    // per-key parse: unknown keys and non-"Key=Value" lines skipped
    writeText("WindowWidth=1024\nUnknownKey=5\ngarbage without an equals sign\n"
              "WindowHeight=768\nFontScaleMain=1.5\n");
    {
        TuiSettings s; // defaults
        loadTuiSettings(s, kIniName);
        CHECK(s.windowWidth == 1024 && s.windowHeight == 768 && s.fontScaleMain == 1.5f);
    }

    // file without a trailing newline: the final line is still parsed
    writeText("WindowWidth=5"); // no '\n'
    {
        TuiSettings s;
        loadTuiSettings(s, kIniName);
        CHECK(s.windowWidth == 5);
    }

    // malformed values keep the current value
    writeText("WindowWidth=abc\nWindowHeight=12x\nFontScaleMain=abc\n");
    {
        TuiSettings s;
        s.windowWidth = 640;
        s.windowHeight = 480;
        s.fontScaleMain = 1.5f;
        loadTuiSettings(s, kIniName);
        CHECK(s.windowWidth == 640 && s.windowHeight == 480 && s.fontScaleMain == 1.5f);
    }
    // Empty value, ERANGE and trailing garbage all reject.
    writeText("WindowWidth=\nWindowHeight=99999999999999999999\n"
              "FontScaleMain=1.5 nope\n");
    {
        TuiSettings s;
        s.windowWidth = 640;
        s.windowHeight = 480;
        s.fontScaleMain = 1.5f;
        loadTuiSettings(s, kIniName);
        CHECK(s.windowWidth == 640 && s.windowHeight == 480 && s.fontScaleMain == 1.5f);
    }
    // Keys are matched case-sensitively, and key text is not trimmed: leading
    // whitespace and whitespace around '=' make the line an unknown key.
    writeText("windowwidth=999\n WindowWidth=999\nWindowWidth =999\nwindowscalemain=2.0\n");
    {
        TuiSettings s;
        loadTuiSettings(s, kIniName);
        CHECK(s.windowWidth == 0 && s.fontScaleMain == 1.0f);
    }

    // window-size clamping into [1, 32767]
    {
        struct ClampCase {
            const char* text;
            int width;
            int height;
        };
        const ClampCase cases[] = {
            {"WindowWidth=0\n", 1, 0},           {"WindowWidth=-5\n", 1, 0},
            {"WindowHeight=0\n", 0, 1},          {"WindowWidth=100000\n", 32767, 0},
            {"WindowHeight=100000\n", 0, 32767}, {"WindowWidth=1\nWindowHeight=32767\n", 1, 32767},
        };
        for (const ClampCase& c : cases) {
            writeText(c.text);
            TuiSettings s;
            loadTuiSettings(s, kIniName);
            const bool ok = s.windowWidth == c.width && s.windowHeight == c.height;
            CHECK(ok);
            if (!ok)
                std::printf("  (content \"%s\" -> %dx%d, want %dx%d)\n", c.text, s.windowWidth,
                            s.windowHeight, c.width, c.height);
        }
    }

    // font scale: in-range values accepted, the rest fall back
    {
        const float accepted[] = {0.5f, 1.0f, 1.25f, 2.0f, 3.0f};
        for (float want : accepted) {
            char text[64];
            std::snprintf(text, sizeof(text), "FontScaleMain=%.4f\n", want);
            writeText(text);
            TuiSettings s;
            loadTuiSettings(s, kIniName);
            const bool ok = s.fontScaleMain == want;
            CHECK(ok);
            if (!ok)
                std::printf("  (content \"%s\" -> %.4f)\n", text, s.fontScaleMain);
        }
    }
    {
        // NaN, inf, 1e40 (overflow to inf), 0, 0.4 and 4.0 all fall back to
        // the current value.
        const char* rejected[] = {"FontScaleMain=nan\n",  "FontScaleMain=inf\n",
                                  "FontScaleMain=-inf\n", "FontScaleMain=1e40\n",
                                  "FontScaleMain=0\n",    "FontScaleMain=0.4\n",
                                  "FontScaleMain=4\n"};
        for (const char* text : rejected) {
            writeText(text);
            TuiSettings s;
            s.fontScaleMain = 1.5f;
            loadTuiSettings(s, kIniName);
            const bool ok = s.fontScaleMain == 1.5f;
            CHECK(ok);
            if (!ok)
                std::printf("  (content \"%s\" -> %.4f, want 1.5000)\n", text, s.fontScaleMain);
        }
    }

    // save-side guards
    CHECK(settingsPath("").empty()); // no name -> no path, nothing written
    // Unwritable path: kPath is a file, so kPath + "/x" cannot be created;
    // the failure is silent (void return, nothing logs).
    saveTuiSettings(TuiSettings{}, std::string(kPath) + "/x");

    // drag-end-style save: the raw ini filename is passed.
    // The save derives the .conf path itself (pre-deriving it wrote a stray
    // "<name>.conf.conf").
    std::remove(kPath);
    std::remove((std::string(kPath) + ".conf").c_str());
    {
        TuiSettings s;
        s.fontScaleMain = 2.0f;
        saveTuiSettings(s, kIniName); // raw name, as renderOptionsWindow passes it
        CHECK(fileExists(kPath));
        CHECK(readText() == "FontScaleMain=2.00\n");
        CHECK(!fileExists((std::string(kPath) + ".conf").c_str()));
    }

    std::remove(kPath);
    if (g_failures == 0) {
        std::printf("all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
