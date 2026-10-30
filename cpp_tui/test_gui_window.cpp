// test_gui_window.cpp
//
// Pins task 9 (D-GEOM / the end of ImGui's stock chrome): a fresh GUI profile
// opens at 1280x820 with a 640x480 floor, a persisted size still wins, and the
// pixel profile runs on the chrome metrics while the text grid keeps its own
// cell metrics.
//
// The window size is read from the pixels the driver produced, not from a
// constant the test shares with the code under test: every case renders real
// frames through tuiRender and reads the framebuffer back with
// Backend::captureNextFramePpm() (the task-8 seam), so a window that came up at
// another size fails on the PPM header. The resize floor has no such pixel: glfw
// has no limits getter, so the backend reads WM_NORMAL_HINTS back and reports
// three things - whether it asked (limitsRequested), whether the window system
// holds the floor (limitsApplied) and whether this platform can answer at all
// (limitsObserved). The same floor is also checked behaviorally at startup: a
// persisted 320x240 opens at 640x480. An interactive drag stays a
// window-manager clamp and is recorded as a manual check in plan/baseline.md.
//
// Needs a display; runs headless under xvfb:
//   xvfb-run -s "-screen 0 1280x820x24" ./test_gui_window --out DIR
// DIR must exist. A content scale other than 1.0 (HiDPI) is refused as a setup
// error: both the geometry and the chrome metrics are asserted unscaled.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = setup error (no
// display, HiDPI display, unwritable output directory, GUI backend init failed,
// bad arguments).

#include "tui.h"
#include "tui_backend.h"
#include "tui_chrome.h"
#include "tui_settings.h"

#include "imtui/imtui.h" // imgui + ImTui_ImplText_* (imtui-impl-text.h)

#include "ui/metrics.h" // ui::metricsFor: the scale -> geometry endpoint
#include "ui/tokens.h"  // ui::scaledTypeSize: the base a run must draw at
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

/// The pixel profile's chrome, pinned as numbers (design §3.4 spacing and §4.15
/// radii/hairline). The implementation derives the same values from the token
/// module (`ui::space`, `ui::radius`, the profile's border), so this list is the
/// contract: a token change has to update these numbers in the same commit, and a
/// chrome value that drifts away from a token step fails here instead of shipping.
/// The pixel profile's chrome, pinned as numbers (design §3.4 spacing and §4.15
/// radii/hairline). The implementation derives the same values from the token
/// module (`ui::space`, `ui::radius`, the profile's border), so this list is the
/// contract: a token change has to update these numbers in the same commit, and a
/// chrome value that drifts away from a token step fails here instead of shipping.
///
/// Deliberately *not* written as `ui::space(...)` calls: a constant that reads the
/// same token as the implementation would move with it and pass the defect it
/// guards (the chrome check used to compare the style against exactly the
/// constants `applyGuiChromeMetrics()` read). The comments name the step each
/// number is the value of - `Space` is `{none 0, hair 2, tight 4, compact 6,
/// base 8, comfy 12, section 16, page 24}`.
constexpr float kWindowPaddingX = 16.0f;   // space.section
constexpr float kWindowPaddingY = 12.0f;   // space.comfy
constexpr float kItemSpacingX = 8.0f;      // space.base
constexpr float kItemSpacingY = 6.0f;      // space.compact
constexpr float kFramePaddingX = 10.0f;    // space.base + space.hair
constexpr float kFramePaddingY = 5.0f;     // ceil((row 24 - body 15) / 2)
constexpr float kItemInnerSpacing = 4.0f;  // space.tight
constexpr float kWindowRounding = 6.0f;    // radius.large
constexpr float kChildRounding = 6.0f;     // radius.large
constexpr float kPopupRounding = 6.0f;     // radius.large
constexpr float kFrameRounding = 4.0f;     // radius.small
constexpr float kGrabRounding = 4.0f;      // radius.small
constexpr float kScrollbarRounding = 6.0f; // radius.large
constexpr float kScrollbarSize = 12.0f;    // space.comfy
constexpr float kBorderSize = 1.0f;        // the hairline
/// A framed widget keeps no border: its `inset` fill against the surface is the
/// edge, and a border would draw the same boundary twice in two greys.
constexpr float kFrameBorderSize = 0.0f;
/// The base size this run must draw at: the type scale's body step, or the
/// operator's `LLMFUN_GUI_FONT_SIZE` when it is set and sane (the backend applies
/// the same rule through `ui::scaledTypeSize`). Reading it here rather than
/// hard-coding 15 keeps the harness usable for the escape hatch - running it with
/// the variable set proves the configured size reaches the style, which is the one
/// thing an `AddFont*` size cannot do in 1.92.
float expectedFontSizeBase() {
    const char* env = std::getenv("LLMFUN_GUI_FONT_SIZE");
    float configured = 0.0f;
    if (env != nullptr && env[0] != '\0') {
        char* end = nullptr;
        const float px = std::strtof(env, &end);
        if (end != env && *end == '\0' && px > 0.0f && px <= 512.0f)
            configured = px;
    }
    return llmfun::tui::ui::scaledTypeSize(llmfun::tui::ui::TypeStep::body, configured);
}

int g_failures = 0;
const char* g_phase = "startup";
/// Settings files of the case in flight, so an early exit leaves no scratch.
std::string g_scratchIni;

/// Setup problem: the remaining checks would only report the consequence.
[[noreturn]] void fail(const std::string& what) {
    std::fprintf(stderr, "SETUP [%s]: %s\n", g_phase, what.c_str());
    std::remove(g_scratchIni.c_str());
    std::remove(llmfun::tui::settingsPath(g_scratchIni).c_str());
    std::exit(2);
}

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

/// Everything a PPM needs: the header, the size and the pixel bytes. Same
// reader as test_thread_states_gui; task 19's golden harness unifies them.
struct Ppm {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgb;
};

bool readPpm(const std::string& path, Ppm& out) {
    FILE* in = std::fopen(path.c_str(), "rb");
    if (in == nullptr)
        return false;
    char magic[3] = {0, 0, 0};
    int width = 0;
    int height = 0;
    int maxValue = 0;
    const bool header = std::fscanf(in, "%2s %d %d %d", magic, &width, &height, &maxValue) == 4;
    // One whitespace byte separates the header from the raster.
    const int separator = header ? std::fgetc(in) : EOF;
    if (!header || std::strcmp(magic, "P6") != 0 || width <= 0 || height <= 0 || maxValue != 255 ||
        separator == EOF) {
        std::fclose(in);
        return false;
    }
    out.width = width;
    out.height = height;
    out.rgb.resize(static_cast<std::size_t>(width) * height * 3);
    const std::size_t read = std::fread(out.rgb.data(), 1, out.rgb.size(), in);
    std::fclose(in);
    return read == out.rgb.size();
}

/// Distinct 5-bit colours in a capture: a blank or single-colour frame is not a
/// rendered UI (same measure as test_thread_states_gui).
std::size_t distinctColours(const Ppm& ppm) {
    std::vector<bool> seen(1u << 15, false);
    std::size_t count = 0;
    for (std::size_t i = 0; i + 2 < ppm.rgb.size(); i += 3) {
        const unsigned key = (static_cast<unsigned>(ppm.rgb[i] >> 3) << 10) |
                             (static_cast<unsigned>(ppm.rgb[i + 1] >> 3) << 5) |
                             static_cast<unsigned>(ppm.rgb[i + 2] >> 3);
        if (!seen[key]) {
            seen[key] = true;
            ++count;
        }
    }
    return count;
}

/// A stock ImGui style, i.e. exactly what a new context starts with.
void resetStyleToDefaults() { ImGui::GetStyle() = ImGuiStyle(); }

std::string sizeText(int width, int height) {
    return std::to_string(width) + "x" + std::to_string(height);
}

/// A framebuffer corner of a capture, named so a failure says which one.
struct Corner {
    const char* name;
    int x;
    int y;
};

/// The framebuffer value of a style colour channel: ImGui rounds the float
/// colour to the nearest u8 on upload, so the comparison does the same.
unsigned char channel8(float value) {
    const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    return static_cast<unsigned char>(clamped * 255.0f + 0.5f);
}

/// The shell is the window (D-GEOM): the workbench's six regions tile the *whole* display, so
/// the last framebuffer row belongs to the now bar and the root window's padding is covered
/// rather than shown as a gutter. That is the one shape `ui::computeShell()` promises, and it is
/// what a capture can see: the bottom row is painted, and the now bar's own row height (one row
/// of the metrics) is the last thing on screen.
///
/// The legacy renderer's "content area plus an empty gutter" chrome is gone with it (task 41);
/// this check is the pixel profile's half of the shell's edge-to-edge rule, measured against the
/// window background so no theme value is duplicated here.
void checkContentAreaBottom(const Ppm& ppm, const std::string& label) {
    const ImVec4 canvas = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const unsigned char r = channel8(canvas.x);
    const unsigned char g = channel8(canvas.y);
    const unsigned char b = channel8(canvas.z);
    const auto isCanvas = [&ppm, r, g, b](int x, int y) {
        const unsigned char* px =
            ppm.rgb.data() + (static_cast<std::size_t>(y) * ppm.width + x) * 3;
        return px[0] == r && px[1] == g && px[2] == b;
    };
    const int lastRow = ppm.height - 1;
    // x = width/2 is inside the composer above the now bar; the rail is the left column,
    // probed a few pixels in and one row up from the bottom (the now bar spans the full width).
    const int composerX = ppm.width / 2;
    const int railX = 30;
    check(!isCanvas(composerX, lastRow),
          label + ": the now bar paints the window's last row (" + std::to_string(lastRow) + ")");
    check(!isCanvas(railX, lastRow),
          label + ": ...across the whole width, the rail's column included");
    check(!isCanvas(composerX, 0), label + ": and the top bar paints the first row");
}

/// The chrome metrics, one line per group, with the value on failure.
void checkChromeMetricsApplied(const char* label) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const auto expect = [label](const char* what, float got, float want) {
        check(got == want, std::string(label) + ": " + what + " is " + std::to_string(got) +
                               ", expected " + std::to_string(want));
    };
    expect("WindowPadding.x", style.WindowPadding.x, kWindowPaddingX);
    expect("WindowPadding.y", style.WindowPadding.y, kWindowPaddingY);
    expect("ItemSpacing.x", style.ItemSpacing.x, kItemSpacingX);
    expect("ItemSpacing.y", style.ItemSpacing.y, kItemSpacingY);
    expect("FramePadding.x", style.FramePadding.x, kFramePaddingX);
    expect("FramePadding.y", style.FramePadding.y, kFramePaddingY);
    expect("WindowRounding", style.WindowRounding, kWindowRounding);
    expect("ChildRounding", style.ChildRounding, kChildRounding);
    expect("PopupRounding", style.PopupRounding, kPopupRounding);
    expect("FrameRounding", style.FrameRounding, kFrameRounding);
    expect("GrabRounding", style.GrabRounding, kGrabRounding);
    expect("ScrollbarRounding", style.ScrollbarRounding, kScrollbarRounding);
    expect("ScrollbarSize", style.ScrollbarSize, kScrollbarSize);
    expect("ItemInnerSpacing.x", style.ItemInnerSpacing.x, kItemInnerSpacing);
    // The hairline, stated rather than inherited: ImGui's own defaults already
    // happen to be 1, so a version bump that changed them would silently re-style
    // every panel boundary.
    expect("WindowBorderSize", style.WindowBorderSize, kBorderSize);
    expect("ChildBorderSize", style.ChildBorderSize, kBorderSize);
    expect("PopupBorderSize", style.PopupBorderSize, kBorderSize);
    expect("FrameBorderSize", style.FrameBorderSize, kFrameBorderSize);
}

/// One window case: the settings file it starts from (0 = none at all, i.e. a
/// fresh profile), the geometry it must produce and the capture it leaves.
struct WindowCase {
    const char* name;
    int seededWidth;
    int seededHeight;
    float seededFontScale; // 0 = keep the default 1.0
    int wantWidth;
    int wantHeight;
    const char* capture; // nullptr = settings round-trip only, no capture
};

// D-GEOM's numbers, spelled out rather than read from tui_chrome.h: the test
// pins the requirement, so a changed constant fails here instead of moving the
// expectation with it.
constexpr int kWantFreshWidth = 1280;
constexpr int kWantFreshHeight = 820;
constexpr int kWantMinWidth = 640;
constexpr int kWantMinHeight = 480;

/// Runs one window case end to end and returns the capture's distinct colours.
std::size_t runWindowCase(const WindowCase& c, const std::string& outDir) {
    g_phase = c.name;
    const std::string label = c.name;
    resetStyleToDefaults();

    const std::string iniPath = outDir + "/" + c.name + "-imgui2.ini";
    const std::string confPath = llmfun::tui::settingsPath(iniPath);
    g_scratchIni = iniPath;
    std::remove(iniPath.c_str());
    std::remove(confPath.c_str());
    if (c.seededWidth > 0 || c.seededFontScale > 0.0f) {
        llmfun::tui::TuiSettings seeded;
        seeded.windowWidth = c.seededWidth;
        seeded.windowHeight = c.seededHeight;
        if (c.seededFontScale > 0.0f)
            seeded.fontScaleMain = c.seededFontScale;
        llmfun::tui::saveTuiSettings(seeded, iniPath);
    }

    llmfun::tui::TuiState state;
    std::string error;
    auto backend = llmfun::tui::makeGuiBackend(iniPath, state.settings);
    if (!backend->init(error))
        fail("GUI backend init failed (no display?): " + error);
    state.backend = std::move(backend);
    llmfun::tui::initMarkdownGuiMode(state);
    llmfun::tui::applyUiTheme();

    // The text the widgets draw at, measured inside a frame: `FontSizeBase` is what
    // the style says, `GetFontSize()` is what this frame actually uses (after the
    // two global scale factors). Both must be the type scale's body step - the check
    // that catches a backend that forgot to apply the scale, which is invisible in
    // the chrome metrics and in a capture's colours.
    float drawnFontSize = 0.0f;
    const auto frames = [&state, &drawnFontSize](int count) {
        for (int i = 0; i < count; ++i) {
            state.backend->newFrame();
            if (!llmfun::tui::tuiRender(state))
                fail("tuiRender asked to quit");
            if (drawnFontSize == 0.0f)
                drawnFontSize = ImGui::GetFontSize();
            state.backend->renderFrame();
        }
    };

    // Two settle frames: the first one lays the shell out at the new size.
    frames(2);

    // The metrics seam's input chain: the settings value must reach
    // ImGuiStyle::FontScaleMain inside a frame (`applyGuiFontScale()` in tui.cpp)
    // and `ui::metricsFor()` must then answer a scaled layout. Each end alone is
    // not enough - the settings file can be right while the style is stock (every
    // length silently 100 %), which is what happens if the writer ever moves out
    // of the frame code (tasks 18/25).
    {
        const float wantScale = c.seededFontScale > 0.0f ? c.seededFontScale : 1.0f;
        const float styleScale = ImGui::GetStyle().FontScaleMain;
        check(styleScale == wantScale,
              label + ": the settings font scale reaches the frame style (" +
                  std::to_string(styleScale) + ", expected " + std::to_string(wantScale) + ")");
        const llmfun::tui::ui::Metrics m = llmfun::tui::ui::metricsFor();
        check(!m.textGrid, label + ": the metrics answer the pixel profile");
        const float wantGu = llmfun::tui::ui::roundCells(8.0f * wantScale);
        check(m.gu == wantGu, label + ": the scaled metrics follow the style (gu " +
                                  std::to_string(m.gu) + ", expected " + std::to_string(wantGu) +
                                  ")");
        const float wantRow = llmfun::tui::ui::roundCells(24.0f * wantScale);
        check(m.row == wantRow, label + ": the scaled metrics follow the style (row " +
                                    std::to_string(m.row) + ", expected " +
                                    std::to_string(wantRow) + ")");
    }
    // A case without a capture (`capture == nullptr`) is a settings round-trip
    // only: it writes no artifact and asserts no pixel.
    const bool haveCapture = c.capture != nullptr;
    Ppm ppm;
    const std::string ppmPath = haveCapture ? outDir + "/" + c.capture : std::string{};
    if (haveCapture) {
        if (!state.backend->captureNextFramePpm(ppmPath))
            fail("the GUI backend refused the capture request");
        frames(1);
        if (!readPpm(ppmPath, ppm))
            fail("the capture is not a readable P6 PPM: " + ppmPath);
    }

    llmfun::tui::GuiWindowGeometry geometry;
    const bool hasGeometry = state.backend->guiWindowGeometry(geometry);
    check(hasGeometry, label + ": the GUI backend reports the geometry it applied");
    // The PPM is framebuffer pixels, the window size is screen coordinates;
    // they agree only at content scale 1.0. A scaled display would otherwise
    // fail below with a misleading "framebuffer is 2560x1640" (and the chrome
    // metrics are asserted unscaled too), so refuse the environment instead.
    if (haveCapture && hasGeometry && ppm.width != geometry.width)
        fail("content scale is not 1.0: the window is " +
             sizeText(geometry.width, geometry.height) + " but its framebuffer is " +
             sizeText(ppm.width, ppm.height) + "; run this harness on a 1.0 display (xvfb)");

    if (haveCapture) {
        check(ppm.width == c.wantWidth && ppm.height == c.wantHeight,
              label + ": the framebuffer is " + sizeText(ppm.width, ppm.height) + ", expected " +
                  sizeText(c.wantWidth, c.wantHeight));
    }

    if (hasGeometry) {
        check(geometry.width == c.wantWidth && geometry.height == c.wantHeight,
              label + ": the window is reported as " + sizeText(geometry.width, geometry.height) +
                  ", expected " + sizeText(c.wantWidth, c.wantHeight));
        check(geometry.limitsRequested && geometry.minWidth == kWantMinWidth &&
                  geometry.minHeight == kWantMinHeight,
              label + ": the requested resize floor is " +
                  (geometry.limitsRequested ? sizeText(geometry.minWidth, geometry.minHeight)
                                            : std::string{"(nothing was requested)"}) +
                  ", expected " + sizeText(kWantMinWidth, kWantMinHeight));
        // glfw has no limits getter, so the backend reads WM_NORMAL_HINTS back:
        // "the request was made" and "the window system holds it" are two
        // separate observations, and only the second one survives a deleted
        // glfwSetWindowSizeLimits() call. Both are asserted - an unobservable
        // platform must not pass for an applied floor.
        check(geometry.limitsObserved,
              label + ": the window system can report the resize floor (X11 WM_NORMAL_HINTS)");
        check(geometry.limitsApplied,
              label + ": the window system holds the resize floor " +
                  sizeText(kWantMinWidth, kWantMinHeight) +
                  " (a request that was never written leaves no hint behind)");
    }

    if (haveCapture) {
        // The root window must not inherit the panel radius: a rounded full-window
        // surface leaves four clear-colour (black) wedges inside a square frame.
        const auto cornerIsClear = [&ppm](int x, int y) {
            const unsigned char* px =
                ppm.rgb.data() + (static_cast<std::size_t>(y) * ppm.width + x) * 3;
            return px[0] == 0 && px[1] == 0 && px[2] == 0;
        };
        const Corner corners[4] = {{"top left (0,0)", 0, 0},
                                   {"top right (w-1,0)", ppm.width - 1, 0},
                                   {"bottom left (0,h-1)", 0, ppm.height - 1},
                                   {"bottom right (w-1,h-1)", ppm.width - 1, ppm.height - 1}};
        for (const Corner& corner : corners) {
            check(!cornerIsClear(corner.x, corner.y),
                  label + ": the window corner " + corner.name + " is painted, not clear colour");
        }
        checkContentAreaBottom(ppm, label);
    }
    checkChromeMetricsApplied(c.name);
    // The type scale, both halves: the style's base (before the global scale
    // factors - the thing a backend that forgot to apply the scale gets wrong) and
    // the size the frames above actually drew at, which is that base times
    // FontScaleMain/FontScaleDpi (rounded to whole pixels by ImGui).
    check(expectedFontSizeBase() == ImGui::GetStyle().FontSizeBase,
          label + ": the style's base font size is the body step (" +
              std::to_string(ImGui::GetStyle().FontSizeBase) + ", expected " +
              std::to_string(expectedFontSizeBase()) + ")");
    const float scaledBase = ImGui::GetStyle().FontSizeBase * ImGui::GetStyle().FontScaleMain *
                             ImGui::GetStyle().FontScaleDpi;
    check(std::fabs(drawnFontSize - scaledBase) <= 1.0f,
          label + ": the frames drew at the body step times the scale factors (" +
              std::to_string(drawnFontSize) + ", expected about " + std::to_string(scaledBase) +
              ")");
    // (including a floor correction), and FontScaleMain survives the save.
    // The application's teardown order: markdown textures go while the backend
    // is still alive, then the backend itself (which writes imgui2.conf), then
    // the context (once, in main).
    llmfun::tui::destroyMarkdown(state);
    state.backend->shutdown();
    state.backend.reset();
    llmfun::tui::TuiSettings persisted;
    llmfun::tui::loadTuiSettings(persisted, iniPath);
    check(persisted.windowWidth == c.wantWidth && persisted.windowHeight == c.wantHeight,
          label + ": shutdown persisted " +
              sizeText(persisted.windowWidth, persisted.windowHeight) + ", expected " +
              sizeText(c.wantWidth, c.wantHeight));
    // FontScaleMain is the other value in this file that has nothing to do with
    // the window size: a seeded non-default must survive load -> save unchanged,
    // otherwise the next start silently resets the user's text scale.
    const float wantFontScale = c.seededFontScale > 0.0f ? c.seededFontScale : 1.0f;
    check(persisted.fontScaleMain == wantFontScale,
          label + ": FontScaleMain round-trips (" + std::to_string(persisted.fontScaleMain) +
              ", expected " + std::to_string(wantFontScale) + ")");

    std::remove(iniPath.c_str());
    std::remove(confPath.c_str());
    if (!haveCapture)
        return 0;
    const std::size_t colours = distinctColours(ppm);
    std::printf("     %s: %dx%d, %zu bytes, %zu distinct colours\n", ppmPath.c_str(), ppm.width,
                ppm.height, ppm.rgb.size(), colours);
    return colours;
}

/// The constants override every stock value they replace: with any of them
/// equal to ImGui's default the "no stock chrome" claim would be hollow.
void checkConstantsOverrideStock() {
    g_phase = "chrome-constants";
    const ImGuiStyle stock;
    check(kWindowPaddingX != stock.WindowPadding.x || kWindowPaddingY != stock.WindowPadding.y,
          "chrome window padding differs from ImGui's stock " +
              sizeText(static_cast<int>(stock.WindowPadding.x),
                       static_cast<int>(stock.WindowPadding.y)));
    check(
        kItemSpacingX != stock.ItemSpacing.x || kItemSpacingY != stock.ItemSpacing.y,
        "chrome item spacing differs from ImGui's stock " +
            sizeText(static_cast<int>(stock.ItemSpacing.x), static_cast<int>(stock.ItemSpacing.y)));
    check(kFramePaddingX != stock.FramePadding.x || kFramePaddingY != stock.FramePadding.y,
          "chrome frame padding differs from ImGui's stock " +
              sizeText(static_cast<int>(stock.FramePadding.x),
                       static_cast<int>(stock.FramePadding.y)));
    check(kWindowRounding != stock.WindowRounding,
          "chrome window rounding differs from ImGui's stock 0");
    check(kChildRounding != stock.ChildRounding,
          "chrome child rounding differs from ImGui's stock 0");
    check(kPopupRounding != stock.PopupRounding,
          "chrome popup rounding differs from ImGui's stock 0");
    check(kFrameRounding != stock.FrameRounding,
          "chrome frame rounding differs from ImGui's stock 0");
    check(kGrabRounding != stock.GrabRounding, "chrome grab rounding differs from ImGui's stock 0");
    check(kScrollbarRounding != stock.ScrollbarRounding,
          "chrome scrollbar rounding differs from ImGui's stock " +
              std::to_string(static_cast<int>(stock.ScrollbarRounding)));
    check(kScrollbarSize != stock.ScrollbarSize,
          "chrome scrollbar size differs from ImGui's stock " +
              std::to_string(static_cast<int>(stock.ScrollbarSize)));
}

/// The text grid is the profile the chrome metrics must not reach: one style
/// unit is one terminal cell there, so pixel spacing would break every
/// cell-exact layout. ImTui_ImplText_Init() pins the cell metrics itself, and
/// the theme application - which both profiles run - leaves them alone.
void checkTextGridKeepsCellMetrics() {
    g_phase = "text-grid";
    resetStyleToDefaults();
    ImTui_ImplText_Init();
    ImGui::GetIO().DisplaySize = ImVec2(80.0f, 24.0f);
    llmfun::tui::applyUiTheme();
    const ImGuiStyle& style = ImGui::GetStyle();
    // The message names what went wrong, not a difference: if the guard fires,
    // the grid picked the pixel value up.
    const auto absent = [](const char* what, float got, float chrome) {
        check(got != chrome, std::string("the text grid did not pick up the pixel ") + what +
                                 " (it is " + std::to_string(got) + ", not the chrome " +
                                 std::to_string(chrome) + ")");
    };
    absent("WindowPadding.x", style.WindowPadding.x, kWindowPaddingX);
    absent("WindowPadding.y", style.WindowPadding.y, kWindowPaddingY);
    absent("ItemSpacing.x", style.ItemSpacing.x, kItemSpacingX);
    absent("ItemSpacing.y", style.ItemSpacing.y, kItemSpacingY);
    absent("FramePadding.x", style.FramePadding.x, kFramePaddingX);
    absent("FramePadding.y", style.FramePadding.y, kFramePaddingY);
    absent("WindowRounding", style.WindowRounding, kWindowRounding);
    absent("ChildRounding", style.ChildRounding, kChildRounding);
    absent("PopupRounding", style.PopupRounding, kPopupRounding);
    absent("FrameRounding", style.FrameRounding, kFrameRounding);
    absent("GrabRounding", style.GrabRounding, kGrabRounding);
    absent("ScrollbarRounding", style.ScrollbarRounding, kScrollbarRounding);
    absent("ScrollbarSize", style.ScrollbarSize, kScrollbarSize);
    // The negative half only proves "not the pixel value"; these pin what the
    // grid must have instead, so a grid that lost imtui's own 0.5/1 cell metrics
    // (or gained a third party's) fails too. Values are imtui-impl-text.cpp's.
    const auto expect = [](const char* what, float got, float want) {
        check(got == want, std::string("the text grid keeps imtui's ") + what + " (" +
                               std::to_string(got) + ", expected " + std::to_string(want) + ")");
    };
    expect("window padding x", style.WindowPadding.x, 0.5f);
    expect("window padding y", style.WindowPadding.y, 0.0f);
    expect("item spacing x", style.ItemSpacing.x, 1.0f);
    expect("item spacing y", style.ItemSpacing.y, 0.0f);
    expect("frame padding x", style.FramePadding.x, 1.0f);
    expect("frame padding y", style.FramePadding.y, 0.0f);
    expect("scrollbar size", style.ScrollbarSize, 1.0f);
    ImTui_ImplText_Shutdown();
}

} // namespace

int main(int argc, char** argv) {
    std::string outDir;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) {
            outDir = argv[++i];
            continue;
        }
        std::fprintf(stderr, "usage: %s --out DIR (an existing directory)\n", argv[0]);
        return 2;
    }
    if (outDir.empty()) {
        std::fprintf(stderr, "usage: %s --out DIR (an existing directory)\n", argv[0]);
        return 2;
    }
    // The captures are written straight to DIR; say so here instead of failing
    // later with "the capture is not a readable P6 PPM".
    g_phase = "arguments";
    const std::string probePath = outDir + "/.write-probe";
    std::FILE* probe = std::fopen(probePath.c_str(), "wb");
    if (probe == nullptr)
        fail("the output directory is missing or not writable: " + outDir);
    std::fclose(probe);
    std::remove(probePath.c_str());

    std::setlocale(LC_ALL, "");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;

    const WindowCase cases[] = {
        // A fresh profile: no settings file at all, so the built-in default is
        // the only size there is.
        {"fresh-profile", 0, 0, 0.0f, kWantFreshWidth, kWantFreshHeight, "gui-window-1280x820.ppm"},
        // A persisted size wins over the default, and over nothing else.
        {"persisted-size", 1000, 700, 0.0f, 1000, 700, "gui-window-1000x700-persisted.ppm"},
        // A persisted size below the floor opens at the floor.
        {"below-floor", 320, 240, 0.0f, kWantMinWidth, kWantMinHeight,
         "gui-window-640x480-floor.ppm"},
        // FontScaleMain is not a window value and must survive untouched. Seeded
        // non-default on purpose: a case that only ever sees 1.0 cannot tell a
        // round-trip from a reset. No capture - the scale changes the text, and
        // this case is about the settings file.
        {"persisted-font-scale", 1000, 700, 1.25f, 1000, 700, nullptr},
    };
    std::size_t freshColours = 0;
    for (const WindowCase& c : cases) {
        const std::size_t colours = runWindowCase(c, outDir);
        if (std::strcmp(c.name, "fresh-profile") == 0)
            freshColours = colours;
    }

    // A blank window would satisfy "the size is right" while showing nothing.
    g_phase = "fresh-profile";
    check(freshColours >= 8, "the 1280x820 capture shows a rendered UI (" +
                                 std::to_string(freshColours) + " distinct colours)");

    checkConstantsOverrideStock();
    checkTextGridKeepsCellMetrics();

    ImGui::DestroyContext();
    if (g_failures == 0) {
        std::printf("all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
