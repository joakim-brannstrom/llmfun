// test_settings_gui.cpp
//
// The settings sheet in the pixel profile, where the type scale is real. The text grid cannot see
// any of this: a cell has one size, so a sheet that draws its labels at the wrong font step, that
// measures its rows against the wrong height or that clips its value column still looks perfect in
// `./test_settings_sheet`.
//
// What this pins:
//   - the sheet fills the box `ui::computeShell()` answered for it (the layout owns the geometry,
//     the view is a geometry taker) and stays inside the display;
//   - the title and the value column are *drawn*, measured from the pixels rather than from the
//     boxes the view computed - a clip that ate the values would pass a box-only check;
//   - the text scale is the one setting whose effect the grid cannot show: changing it reaches the
//     next frame's `ImGuiStyle::FontScaleMain` and the next frame's metrics (`row` grows), and the
//     commit is written to `imgui2.conf` once - the legacy file, unchanged.
//
// The GUI half of "a golden screen in both backends" is this binary: the pixel sheet's type sizes,
// its search field's frame height and its placement only exist under the real GLFW/OpenGL3 backend
// (a text harness pins one cell to one cell). The evidence is the captured PPM plus the measured
// assertions rather than a stored fixture - the same shape `./test_topbar_gui` and
// `./test_inspector_gui` use, because a pixel-by-pixel fixture at five font scales is a second test
// of the font chain, not of the sheet. Task 40's parity checklist cites this binary and the two
// captures it writes (`settings-1280x820.ppm`, and `…-105.ppm` after the scale step).
//
// Needs a display; runs headless under xvfb:
//   xvfb-run -s "-screen 0 1280x820x24" ./test_settings_gui [--out DIR]
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = setup error (no display / no backend).

#include "tui.h"
#include "tui_backend.h"
#include "tui_settings.h"

#include "views/settings.h"
#include "views/shell.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "ui/layout.h"
#include "ui/metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace ui = llmfun::tui::ui;
namespace views = llmfun::tui::views;
namespace model = llmfun::tui::model;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok)
        return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

struct Ppm {
    int w = 0;
    int h = 0;
    std::vector<unsigned char> rgb;
};

bool readPpm(const std::string& path, Ppm& out) {
    FILE* in = std::fopen(path.c_str(), "rb");
    if (in == nullptr)
        return false;
    char magic[3] = {0, 0, 0};
    int w = 0, h = 0, maxValue = 0;
    const bool header = std::fscanf(in, "%2s %d %d %d", magic, &w, &h, &maxValue) == 4;
    const int separator = header ? std::fgetc(in) : EOF;
    if (!header || std::strcmp(magic, "P6") != 0 || w <= 0 || h <= 0 || maxValue != 255 ||
        separator == EOF) {
        std::fclose(in);
        return false;
    }
    out.w = w;
    out.h = h;
    out.rgb.resize(static_cast<std::size_t>(w) * h * 3);
    const std::size_t read = std::fread(out.rgb.data(), 1, out.rgb.size(), in);
    std::fclose(in);
    return read == out.rgb.size();
}

int luminance(const Ppm& p, int x, int y) {
    if (x < 0 || y < 0 || x >= p.w || y >= p.h)
        return 0;
    const std::size_t i = (static_cast<std::size_t>(y) * p.w + x) * 3;
    return (p.rgb[i] * 30 + p.rgb[i + 1] * 59 + p.rgb[i + 2] * 11) / 100;
}

/// The background luminance of a rectangle: its most common dark value, i.e. the surface the text
/// sits on. Taken as the *minimum-median* - the median of the darker half - so a box that is half
/// covered by glyphs still answers its surface, not its ink.
int surfaceLuminance(const Ppm& p, int x0, int y0, int x1, int y1) {
    std::vector<int> values;
    for (int y = std::max(0, y0); y < std::min(p.h, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(p.w, x1); ++x)
            values.push_back(luminance(p, x, y));
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 4];
}

/// Pixels in the rectangle that differ from its surface by more than 20 levels: the ink of the
/// text drawn there (or of a rule), which a clipped box would leave empty.
int inkPixels(const Ppm& p, int x0, int y0, int x1, int y1) {
    const int surface = surfaceLuminance(p, x0, y0, x1, y1);
    int ink = 0;
    for (int y = std::max(0, y0); y < std::min(p.h, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(p.w, x1); ++x) {
            if (std::abs(luminance(p, x, y) - surface) > 20)
                ++ink;
        }
    return ink;
}

std::string readWhole(const std::string& path) {
    std::ifstream in(path);
    if (!in.good())
        return std::string();
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

int main(int argc, char** argv) {
    std::string outDir = ".";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc)
            outDir = argv[++i];
        else {
            std::fprintf(stderr, "usage: %s [--out DIR]\n", argv[0]);
            return 2;
        }
    }

    ::setenv("LLMFUN_ASCII", "0", 1);

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;

    llmfun::tui::TuiState state;
    state.readyStatus = true;
    llmfun::tui::SessionEntry entry;
    entry.id = "s1";
    entry.title = "settings";
    entry.preview = "hello";
    entry.messageCount = 3;
    entry.isActive = true;
    state.sessionPanel.sessions.push_back(entry);
    state.sessionPanel.activeId = "s1";

    // The profile the backend loads: a fixed window and 100 % text, so the capture is 1280x820.
    llmfun::tui::TuiSettings view;
    view.fontScaleMain = 1.0f;
    view.windowWidth = 1280;
    view.windowHeight = 820;
    const std::string ini = outDir + "/settings-gui.conf";
    llmfun::tui::saveTuiSettings(view, ini);
    std::remove(ini.c_str());

    std::string error;
    state.backend = llmfun::tui::makeGuiBackend(ini, state.settings);
    if (state.backend == nullptr || !state.backend->init(error)) {
        std::fprintf(stderr, "GUI backend init failed (no display?): %s\n", error.c_str());
        ImGui::DestroyContext();
        return 2;
    }
    // The profile name the *frame* layer uses for its own writes (`saveTuiSettings` on a commit,
    // `ui.conf` beside it): the backend knows its ini file, the state has to be told.
    state.iniFilename = ini;
    llmfun::tui::initMarkdownGuiMode(state);
    llmfun::tui::applyUiTheme();

    const ImVec2 display(1280.0f, 820.0f);
    views::openSettings(state.settingsSheet);

    /// One frame through the real backend; `capture` asks for this frame's PPM first, and `inject`
    /// queues input events before `newFrame()` consumes them (the backend's own pump is a no-op for
    /// a window nobody touches).
    const auto frameOnce = [&](bool capture, const std::string& path,
                               const std::function<void(ImGuiIO&)>& inject = nullptr) {
        if (capture && !state.backend->captureNextFramePpm(path)) {
            std::fprintf(stderr, "the GUI backend refused the capture request\n");
            std::exit(2);
        }
        if (inject)
            inject(ImGui::GetIO());
        state.backend->newFrame();
        llmfun::tui::tuiRender(state);
        state.backend->renderFrame();
    };

    frameOnce(false, "");
    frameOnce(false, "");
    const std::string path = outDir + "/settings-1280x820.ppm";
    frameOnce(true, path);

    // The layout the frame drew: `renderShell` derives it from the same preferences and the same
    // display, so measuring here is measuring the sheet that is on screen. The sheet re-lays out in
    // `renderOverlays` on the frame it opens, so the painted box is the overlay pass's answer.
    const ui::LayoutPrefs prefs =
        views::layoutPrefsFor(state.ui, /*paletteOpen=*/false, /*composerH=*/0.0f,
                              /*settingsOpen=*/state.settingsSheet.open);
    const ui::ShellLayout layout = views::shellLayoutFor(prefs, display);
    const ui::Rect sheet = layout.settingsOverlay;
    check(state.settingsSheet.open, "the sheet is open");
    check(sheet.w > 0.0f && sheet.h > 0.0f, "the sheet has a rectangle");
    check(layout.top == ui::OverlayLayer::Settings, "and it is the topmost overlay");
    check(sheet.x >= 0.0f && sheet.y >= 0.0f && sheet.right() <= display.x &&
              sheet.bottom() <= display.y,
          "the sheet is inside the display");

    Ppm ppm;
    if (!readPpm(path, ppm)) {
        std::fprintf(stderr, "the capture is not a readable P6 PPM: %s\n", path.c_str());
        return 2;
    }
    check(ppm.w == 1280 && ppm.h == 820, "the capture is the window's size");

    const int x0 = static_cast<int>(std::floor(sheet.x));
    const int y0 = static_cast<int>(std::floor(sheet.y));
    const int x1 = static_cast<int>(std::ceil(sheet.right()));
    const int y1 = static_cast<int>(std::ceil(sheet.bottom()));

    // The title row: its text is drawn (an empty box would be a surface with nothing on it).
    const int titleInk = inkPixels(ppm, x0 + 8, y0 + 8, x0 + 200, y0 + 40);
    check(titleInk > 40, "the title row is drawn (" + std::to_string(titleInk) + " ink pixels)");

    // The value column of the first body row: right-aligned values are what a clipped layout loses
    // first (the label takes the left half and the value is measured from the right edge).
    const int valueInk = inkPixels(ppm, x1 - 220, y0 + 150, x1 - 8, y0 + 200);
    check(valueInk > 20, "the value column is drawn (" + std::to_string(valueInk) + " ink pixels)");

    // ... and the sheet's bottom is drawn: the note and hint rows are the last two text lines, so a
    // footer that never made it into the window (or a sheet clipped by its own rectangle) would
    // show nothing there.
    const int footerInk = inkPixels(ppm, x0 + 8, y1 - 64, x0 + 300, y1 - 8);
    check(footerInk > 20,
          "the note and hint rows are drawn (" + std::to_string(footerInk) + " ink pixels)");

    // The text scale: the sheet's one setting the grid cannot show, and the only one whose commit
    // writes a *second* file. This drives the view's own path - a key press on the selected row -
    // rather than assigning the value: the row's `activate()` and the frame's `saveTuiSettings()`
    // are what "the font scale round-trips" actually has to hold for.
    const float rowBefore = ui::metricsFor().row;
    const std::string confPath = llmfun::tui::settingsPath(ini);
    std::remove(confPath.c_str());
    check(state.settingsSheet.selected == 1, "the cursor starts on the text-scale row");
    // The keyboard has to be in the list before `Enter` edits a row; `./test_settings_sheet` drives
    // that through a cursor key, so here the field is deactivated directly and this check is about
    // the commit, not about the key plumbing again.
    ImGui::ClearActiveID();
    state.settingsSheet.searchFocused = false;
    state.settingsSheet.searchFocus = false;
    frameOnce(false, ""); // the deactivation takes effect at this frame's NewFrame
    const float scaleBefore = state.settings.fontScaleMain;
    frameOnce(false, "", [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Enter, true); });
    frameOnce(false, "", [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Enter, false); });
    check(std::fabs(state.settings.fontScaleMain - (scaleBefore + 0.05f)) < 0.001f,
          "Enter steps the text scale by one 5 % step (got " +
              std::to_string(state.settings.fontScaleMain) + ")");
    const std::string conf = readWhole(confPath);
    check(!conf.empty(), "the commit wrote imgui2.conf");
    check(conf.find("FontScaleMain=1.05") != std::string::npos,
          "with the scale the sheet committed (got '" + conf + "')");
    check(!state.settingsSheet.fontScaleChanged, "and the answer was consumed once");

    // It applies on the *next* frame: `applyGuiFontScale()` runs at the top of `tuiRender`, before
    // the layout and the metrics are computed.
    frameOnce(false, "");
    check(std::fabs(ImGui::GetStyle().FontScaleMain - 1.05f) < 0.001f,
          "the scale reaches ImGuiStyle::FontScaleMain");
    check(ui::metricsFor().row > rowBefore, "and the metrics grow with it (" +
                                                std::to_string(ui::metricsFor().row) + " vs " +
                                                std::to_string(rowBefore) + ")");
    check(state.settingsSheet.open, "the sheet is still up after the scale change");
    frameOnce(true, outDir + "/settings-1280x820-105.ppm");

    state.backend->shutdown();
    ImGui::DestroyContext();

    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: settings sheet pixel profile, %d checks, %s written\n", g_checks,
                path.c_str());
    return 0;
}
