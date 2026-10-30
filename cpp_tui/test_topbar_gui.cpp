// test_topbar_gui.cpp
//
// The top bar in the pixel profile, where the type scale is real. The text grid cannot see any of
// this: a `ui::TypeScope` is a no-op there (one cell has one size), so a bar that measures itself
// at one pixel size and draws at another still looks perfect in `./test_topbar`.
//
// What this pins:
//   - a type step is absolute: a `label` scope inside a `label` scope is still 13 px, and a measure
//     taken inside the bar's own scope answers the numbers the draw uses. A scope that read
//     `Style.FontSizeBase` scaled a step by a step (13 * 13 / 15 = 11 px), so every plain-text box
//     came out 15 % narrower than the text drawn into it;
//   - the symptom, from the pixels: the wordmark and the palette hint fill their boxes, i.e. the
//     last glyph is drawn instead of clipped ("llmfun", not "llmfu"; "Ctrl+K Search", not "Ctrl+K
//     Sear"). A text-grid check cannot fail this way, which is why the mismatch shipped.
//
// Needs a display; runs headless under xvfb:
//   xvfb-run -s "-screen 0 1280x820x24" ./test_topbar_gui [--out DIR]
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = setup error (no display / no backend).

#include "tui.h"
#include "tui_backend.h"
#include "tui_settings.h"
#include "views/shell.h"
#include "views/topbar.h"

#include "imgui/imgui.h"
#include "ui/metrics.h"
#include "ui/tokens.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using llmfun::tui::views::TopBarPart;
using llmfun::tui::views::TopBarParts;

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

int luminance(const Ppm& p, int x, int y) {
    const std::size_t i = (static_cast<std::size_t>(y) * p.w + x) * 3;
    return (p.rgb[i] * 30 + p.rgb[i + 1] * 59 + p.rgb[i + 2] * 11) / 100;
}

/// How wide the ink inside `box` is, in pixels; 0 when the box holds none. Ink is any pixel
/// brighter than the bar's surface (`#12161c`; every text token clears 4.5:1 above it), so the
/// measurement does not depend on the token values.
int inkWidth(const Ppm& p, const TopBarPart& box) {
    const int x0 = std::max(0, static_cast<int>(std::floor(box.x)));
    const int x1 = std::min(p.w, static_cast<int>(std::ceil(box.x + box.w)));
    const int y0 = std::max(0, static_cast<int>(std::floor(box.y)));
    const int y1 = std::min(p.h, static_cast<int>(std::ceil(box.y + box.h)));
    int first = -1, last = -1;
    for (int x = x0; x < x1; ++x) {
        bool ink = false;
        for (int y = y0; y < y1 && !ink; ++y)
            ink = luminance(p, x, y) > 60;
        if (ink) {
            if (first < 0)
                first = x;
            last = x;
        }
    }
    return first < 0 ? 0 : last - first + 1;
}

} // namespace

int main(int argc, char** argv) {
    using namespace llmfun::tui;

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
    entry.title = "tui fixes";
    entry.preview = "hello";
    entry.messageCount = 3;
    entry.isActive = true;
    state.sessionPanel.sessions.push_back(entry);
    state.sessionPanel.activeId = "s1";
    state.readyStatus = true;

    TuiSettings view;
    view.fontScaleMain = 1.0f;
    view.windowWidth = 1280;
    view.windowHeight = 820;
    const std::string ini = outDir + "/topbar-gui.conf";
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
    TopBarParts parts;
    bool typeChecked = false;

    /// One frame of the shell through the real backend, with the type checks on the first one.
    const auto frameOnce = [&](bool capture, const std::string& path) {
        if (capture && !state.backend->captureNextFramePpm(path)) {
            std::fprintf(stderr, "the GUI backend refused the capture request\n");
            std::exit(2);
        }
        state.backend->newFrame();
        tuiRender(state);
        const ui::Metrics m = ui::metricsFor();
        // The layout `tuiRender` just drew: `renderShell` derives it from the same prefs and the
        // same display, so measuring here is measuring the bar that is on screen.
        const ui::LayoutPrefs prefs = views::layoutPrefsFor(state.ui, state.palette.open);
        const ui::ShellLayout layout = views::shellLayoutFor(prefs, display);
        parts = views::topBarMeasure(state.topBar, layout.topBar, m);
        if (!typeChecked) {
            typeChecked = true;
            // The bar draws its plain text at the label step, and `renderTopBar` measures inside
            // its own scope: both of these have to answer 13 px.
            const float outside = parts.wordmark.w;
            const ui::TypeScope outer(ui::TypeStep::label, /*textGrid=*/false);
            check(std::fabs(ImGui::GetFontSize() - 13.0f) < 0.5f,
                  "a label scope draws at 13 px (got " + std::to_string(ImGui::GetFontSize()) +
                      ")");
            const float labelWidth = ImGui::CalcTextSize("llmfun").x;
            const TopBarParts inside = views::topBarMeasure(state.topBar, layout.topBar, m);
            check(inside.wordmark.w == outside,
                  "a measure inside the bar's own scope answers the same wordmark width (" +
                      std::to_string(inside.wordmark.w) + " vs " + std::to_string(outside) + ")");
            check(std::fabs(inside.wordmark.w - labelWidth) < 0.5f,
                  "the wordmark box is the text's own width (" + std::to_string(inside.wordmark.w) +
                      " vs " + std::to_string(labelWidth) + ")");
            check(std::fabs(inside.hint.w - ImGui::CalcTextSize("Ctrl+K Search").x) < 0.5f,
                  "the hint box is the hint's own width (" + std::to_string(inside.hint.w) +
                      " vs " + std::to_string(ImGui::CalcTextSize("Ctrl+K Search").x) + ")");
            {
                const ui::TypeScope nested(ui::TypeStep::label, /*textGrid=*/false);
                check(std::fabs(ImGui::GetFontSize() - 13.0f) < 0.5f,
                      "a nested label scope keeps 13 px (got " +
                          std::to_string(ImGui::GetFontSize()) + ")");
                check(ImGui::CalcTextSize("llmfun").x == labelWidth,
                      "a nested scope measures the same text width");
            }
        }
        state.backend->renderFrame();
    };

    frameOnce(false, "");
    frameOnce(false, "");
    const std::string path = outDir + "/topbar-1280x820.ppm";
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

    // The pixel half of the check: a box that is the text's own width has to be filled by the text
    // drawn into it. A bar that measured 15 % narrow left the last glyph outside its clip rect.
    const int wordmarkInk = inkWidth(ppm, parts.wordmark);
    check(wordmarkInk >= static_cast<int>(parts.wordmark.w) - 2,
          "the wordmark is drawn in full (" + std::to_string(wordmarkInk) + " px of " +
              std::to_string(static_cast<int>(parts.wordmark.w)) + ")");
    const int hintInk = inkWidth(ppm, parts.hint);
    check(hintInk >= static_cast<int>(parts.hint.w) - 2,
          "the palette hint is drawn in full (" + std::to_string(hintInk) + " px of " +
              std::to_string(static_cast<int>(parts.hint.w)) + ")");

    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("PASS: top bar pixel profile, %s written\n", path.c_str());
    return 0;
}
