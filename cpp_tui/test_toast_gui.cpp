// test_toast_gui.cpp
//
// The feedback stack in the pixel profile (task 45, design §5.25 / FR-29), where a card is
// rectangles and colour rather than cells.
//
// What this pins:
//   - three items raise three cards, newest at the bottom, right-aligned, one `pad1` apart and
//     inside the area the shell allows (the display above the composer) - the geometry
//     `views::toastStackRects()` answered, read back from the captured frame;
//   - each card really is a `raised` surface at the rect the geometry answered: the stack's
//     position is not a second layout that could disagree with the one the frame drew;
//   - the severity's 4 px rule is the card's leading colour, per item (a warning is amber, an
//     error is the danger token, a success the ok token) - the one colour a card spends on a
//     surface;
//   - the gap between two cards is not a card: the stack is three surfaces, not one tall panel.
//
// The text grid's presentation is the now bar's one-line flash instead (`./test_nowbar`), so this
// harness exists for the pixel profile only.
//
// Needs a display; runs headless under xvfb:
//   xvfb-run -s "-screen 0 1280x820x24" ./test_toast_gui [--out DIR]
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = setup error (no display / no backend).

#include "tui.h"
#include "tui_backend.h"
#include "tui_settings.h"
#include "views/feedback.h"
#include "views/shell.h"

#include "imgui/imgui.h"
#include "model/toast.h"
#include "model/ui_state.h"
#include "ui/metrics.h"
#include "ui/tokens.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
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

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;

    bool near(const Rgb& other) const {
        return std::abs(r - other.r) <= 3 && std::abs(g - other.g) <= 3 &&
               std::abs(b - other.b) <= 3;
    }
    std::string text() const {
        return "(" + std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b) + ")";
    }
};

Rgb pixelAt(const Ppm& p, int x, int y) {
    Rgb out;
    if (x < 0 || y < 0 || x >= p.w || y >= p.h)
        return out;
    const std::size_t i = (static_cast<std::size_t>(y) * p.w + x) * 3;
    out.r = p.rgb[i];
    out.g = p.rgb[i + 1];
    out.b = p.rgb[i + 2];
    return out;
}

Rgb rgbOf(llmfun::tui::ui::ColorToken token) {
    const ImVec4 c = llmfun::tui::ui::color(token);
    Rgb out;
    out.r = static_cast<int>(std::lround(std::fmin(std::fmax(c.x, 0.0f), 1.0f) * 255.0f));
    out.g = static_cast<int>(std::lround(std::fmin(std::fmax(c.y, 0.0f), 1.0f) * 255.0f));
    out.b = static_cast<int>(std::lround(std::fmin(std::fmax(c.z, 0.0f), 1.0f) * 255.0f));
    return out;
}

} // namespace

int main(int argc, char** argv) {
    using namespace llmfun::tui;
    namespace model = llmfun::tui::model;
    namespace views = llmfun::tui::views;

    std::string outDir = ".";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            outDir = argv[++i];
        else {
            std::fprintf(stderr, "usage: %s [--out DIR]\n", argv[0]);
            return 2;
        }
    }

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;

    TuiState state;
    SessionEntry entry;
    entry.id = "s1";
    entry.title = "feedback";
    entry.preview = "a copy and a save";
    entry.messageCount = 2;
    entry.isActive = true;
    state.sessionPanel.sessions.push_back(entry);
    state.sessionPanel.activeId = "s1";
    state.readyStatus = true;

    TuiSettings view;
    view.fontScaleMain = 1.0f;
    view.windowWidth = 1280;
    view.windowHeight = 820;
    const std::string ini = outDir + "/toast-gui.conf";
    saveTuiSettings(view, ini);

    std::string error;
    state.backend = makeGuiBackend(ini, state.settings);
    if (!state.backend->init(error)) {
        std::fprintf(stderr, "GUI backend init failed (no display?): %s\n", error.c_str());
        ImGui::DestroyContext();
        return 2;
    }
    initMarkdownGuiMode(state);
    applyUiTheme();

    const ImVec2 display(1280.0f, 820.0f);

    // Three items, raised the way a copy, a save and a failed save raise them.
    const std::int64_t nowMs = model::agentClockMs();
    model::raiseToast(state.ui, model::Toast{model::ToastKind::Info, "Copied USER", {}}, nowMs,
                      false);
    model::raiseToast(state.ui,
                      model::Toast{model::ToastKind::Success, "Saved to /tmp/llmfun-raw.log", {}},
                      nowMs, false);
    model::raiseToast(state.ui,
                      model::Toast{model::ToastKind::Error, "Save failed: EACCES",
                                   "the profile directory is read-only"},
                      nowMs, false);
    check(state.ui.toasts.size() == 3, "three items are up");

    /// One frame of the shell through the real backend.
    const auto frameOnce = [&](bool capture, const std::string& path) {
        if (capture && !state.backend->captureNextFramePpm(path)) {
            std::fprintf(stderr, "the GUI backend refused the capture request\n");
            std::exit(2);
        }
        state.backend->newFrame();
        tuiRender(state);
        state.backend->renderFrame();
    };

    frameOnce(false, "");
    frameOnce(false, "");
    const std::string path = outDir + "/toast-stack-1280x820.ppm";
    frameOnce(true, path);

    Ppm ppm;
    if (!readPpm(path, ppm)) {
        std::fprintf(stderr, "the capture is not a readable P6 PPM: %s\n", path.c_str());
        return 2;
    }
    if (ppm.w != 1280 || ppm.h != 820) {
        std::fprintf(stderr, "capture is %dx%d, expected 1280x820\n", ppm.w, ppm.h);
        return 2;
    }

    // The geometry the frame drew with: the same layout derivation `renderShell()` uses, and the
    // same area `renderOverlays()` hands the stack (the display down to the composer's top).
    const ui::Metrics m = ui::metricsFor();
    const ui::LayoutPrefs prefs = views::layoutPrefsFor(state.ui, state.palette.open);
    const ui::ShellLayout layout = views::shellLayoutFor(prefs, display);
    ui::Rect area;
    area.w = display.x;
    area.h = layout.composer.h > 0.0f ? layout.composer.y : display.y;
    area.cells = m.textGrid;
    check(!m.textGrid, "this harness runs the pixel profile");

    const std::vector<ui::Rect> rects = views::toastStackRects(state.ui, area, m);
    check(rects.size() == 3,
          "three items make three cards (got " + std::to_string(rects.size()) + ")");
    if (rects.size() != 3) {
        std::fprintf(stderr, "FAIL: %d check(s) failed\n", g_failures);
        return 1;
    }
    check(rects[0].y > rects[1].y && rects[1].y > rects[2].y, "the newest card is the lowest one");
    check(rects[0].bottom() <= area.bottom() && rects[2].y >= area.y,
          "the stack is inside the area the shell allows");
    check(rects[0].right() <= display.x && rects[0].x > display.x * 0.5f,
          "the stack hugs the right edge of the window");

    // The pixels: each card is a `raised` surface at the rect the geometry answered, and its rule
    // is the severity's own colour. The sample sits in the card's leading padding, right of the
    // 4 px rule - padding, so no glyph can be there.
    const float ruleW = 4.0f * m.border;
    const Rgb raised = rgbOf(ui::ColorToken::raised);
    const Rgb wantRule[3] = {rgbOf(ui::ColorToken::stateDanger), rgbOf(ui::ColorToken::stateOk),
                             rgbOf(ui::ColorToken::textMid)};
    const char* names[3] = {"error", "success", "info"};
    for (std::size_t i = 0; i < rects.size(); ++i) {
        const int cardX = static_cast<int>(rects[i].x + ruleW + 2.0f);
        const int cardY = static_cast<int>(rects[i].y + rects[i].h * 0.5f);
        const Rgb fill = pixelAt(ppm, cardX, cardY);
        check(fill.near(raised), std::string("the ") + names[i] + " card is a raised surface at " +
                                     std::to_string(cardX) + "," + std::to_string(cardY) +
                                     " (got " + fill.text() + ", want " + raised.text() + ")");
        const int ruleX = static_cast<int>(rects[i].x + 1.0f);
        const Rgb rule = pixelAt(ppm, ruleX, cardY);
        check(rule.near(wantRule[i]), std::string("the ") + names[i] +
                                          " card's rule is its severity's colour (got " +
                                          rule.text() + ", want " + wantRule[i].text() + ")");
    }

    // The gap is a gap: between the second and the third card there is no card surface.
    const int gapY = static_cast<int>(rects[2].bottom() + m.pad1 * 0.5f);
    const Rgb gap = pixelAt(ppm, static_cast<int>(rects[2].x + ruleW + 2.0f), gapY);
    check(!gap.near(raised),
          "the cards are separate surfaces, not one panel (gap is " + gap.text() + ")");

    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("PASS: feedback stack, %s written\n", path.c_str());
    return 0;
}
