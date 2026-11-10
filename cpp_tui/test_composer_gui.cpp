// test_composer_gui.cpp
//
// The composer's chip row in the pixel profile (FR-10: both profiles render the
// row): the same view the text grid draws (./test_composer, task-4 half), driven
// through the real GLFW/OpenGL3 backend at the design's 1280x820 window. The
// grid half pins the row's labels, truncation and x clicks; this binary exists
// for the three things only the pixel profile can answer:
//
//   1. a frame of the shipped workbench with two injected chips draws, and the
//      row's pixels land inside the composer region, on the meta row only;
//   2. a hover on a chip explains itself with the FULL drop path: the label is
//      the workarea path once D reports one (or the processing text while it
//      waits), so the tooltip is where the original path still shows - the
//      grid's fallback is the Log tab, which already carries full paths;
//   3. an x click removes its chip at once and queues the id for D's cancel
//      poll - the same one-frame answer the grid test pins, through the real
//      backend's mouse queue.
//
// The screenshot (composer-gui-chips-1280x820.ppm) is the visual evidence: the
// same frame the assertions measured.
//
// Needs a display (run under xvfb-run), exit codes: 0 = pass, 1 = a check
// failed, 2 = the backend could not start.
//
//   xvfb-run -s "-screen 0 1280x820x24" ./test_composer_gui [--out DIR]

#include "views/composer.h"
#include "views/shell.h"

#include "tui.h"
#include "tui_backend.h"
#include "tui_settings.h"

#include "model/drop_chip.h"
#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/primitives.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ui = llmfun::tui::ui;
namespace views = llmfun::tui::views;

namespace {

int g_checks = 0;
int g_failures = 0;
bool g_verbose = false;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) {
        if (g_verbose)
            std::printf("ok   %s\n", what.c_str());
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

/// One capture. The GUI has no cell grid to read (the row's labels live in
/// ./test_composer), so a claim about what the row *looks like* has to be a
/// claim about the frame the backend produced - the same seam test_rail_gui
/// uses.
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
    const int separator = header ? std::fgetc(in) : EOF;
    if (!header || std::strcmp(magic, "P6") != 0 || width <= 0 || height <= 0 || maxValue != 255 ||
        separator == EOF) {
        std::fclose(in);
        return false;
    }
    out.width = width;
    out.height = height;
    out.rgb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    const std::size_t read = std::fread(out.rgb.data(), 1, out.rgb.size(), in);
    std::fclose(in);
    return read == out.rgb.size();
}

/// Where two captures disagree: the bounding box of the differing pixels and how
/// many there are. `count == 0` means the frames are identical.
struct DiffBox {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = -1;
    int count = 0;
};

DiffBox diffBox(const Ppm& a, const Ppm& b) {
    DiffBox box;
    if (a.width != b.width || a.height != b.height)
        return box;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * a.width + x) * 3;
            if (a.rgb[i] == b.rgb[i] && a.rgb[i + 1] == b.rgb[i + 1] &&
                a.rgb[i + 2] == b.rgb[i + 2])
                continue;
            if (box.count == 0) {
                box.x0 = box.x1 = x;
                box.y0 = box.y1 = y;
            } else {
                box.x0 = x < box.x0 ? x : box.x0;
                box.x1 = x > box.x1 ? x : box.x1;
                box.y0 = y < box.y0 ? y : box.y0;
                box.y1 = y > box.y1 ? y : box.y1;
            }
            ++box.count;
        }
    }
    return box;
}

/// The tooltip window this frame opened, or nullptr. A tooltip is an ordinary
/// window with the Tooltip flag (its name carries a per-frame override counter),
/// so "the view opened one" is "a Tooltip-flagged window was active this frame" -
/// reading the flag, not the name, keeps the check off the counter the view does
/// not own.
const ImGuiWindow* activeTooltip() {
    const ImGuiContext* g = ImGui::GetCurrentContext();
    for (const ImGuiWindow* window : g->Windows)
        if ((window->Flags & ImGuiWindowFlags_Tooltip) != 0 &&
            window->LastFrameActive == g->FrameCount)
            return window;
    return nullptr;
}

std::string trimNum(float v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f", double(v));
    return buffer;
}

} // namespace

int main(int argc, char** argv) {
    using namespace llmfun::tui;

    std::string outDir = ".";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            outDir = argv[++i];
        else if (std::strcmp(argv[i], "-v") == 0)
            g_verbose = true;
        else {
            std::fprintf(stderr, "usage: %s [-v] [--out DIR]\n", argv[0]);
            return 2;
        }
    }

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;

    llmfun::tui::TuiState state;
    state.readyStatus = true;

    // The profile the workbench loads in `tuiInit`; this harness stands in for
    // it: seed the merged `ui.conf` through the model's writer and read it into
    // the one preferences struct the backend and the frame both share.
    model::ShellPrefs view;
    view.fontScaleMain = 1.0f;
    view.windowWidth = 1280;
    view.windowHeight = 820;
    const std::string ini = outDir + "/composer-ui.ini";
    std::remove(model::uiConfPath(ini).c_str()); // a stale profile from an earlier run
    model::saveUiConf(view, ini);
    model::loadUiConf(state.ui.prefs, ini);

    std::string error;
    state.backend = makeGuiBackend(state.ui.prefs);
    if (!state.backend->init(error)) {
        std::fprintf(stderr, "GUI backend init failed (no display?): %s\n", error.c_str());
        ImGui::DestroyContext();
        return 2;
    }
    initMarkdownGuiMode(state);
    applyUiTheme();
    // The hover answers on its first frame: a real tooltip's delay is a user
    // comfort, and a test that slept out the delay would be testing the clock.
    ImGui::GetStyle().HoverDelayNormal = 0.0f;

    const ImVec2 display(1280.0f, 820.0f);
    const ui::Metrics m = ui::metricsFor();
    check(!m.textGrid, "premise: the pixel profile is live");

    const auto frameOnce = [&state]() {
        state.backend->newFrame();
        tuiRender(state);
        state.backend->renderFrame();
    };

    // One settle frame: the composer reports the height the layout reserves, the
    // next frame draws in it. An empty draft's answer is the minimum, so the
    // region the frame really gives the view is the one the math below derives.
    frameOnce();
    const ui::Metrics settled = ui::metricsFor();
    check(state.composer.height == settled.composerMinH,
          "an empty draft reserves the composer's minimum (height=" +
              trimNum(state.composer.height) + ", min=" + trimNum(settled.composerMinH) + ")");

    const ui::LayoutPrefs prefs =
        views::layoutPrefsFor(state.ui, state.palette.open, state.composer.height);
    const ui::ShellLayout layout = views::shellLayoutFor(prefs, display);
    const ui::Rect region = layout.composer;
    check(region.h == settled.composerMinH,
          "the layout reserves the minimum (h=" + trimNum(region.h) + ")");
    check(region.h == 2.0f * (settled.border + settled.row),
          "the minimum is the anatomy's two rows and two borders");

    // Park the pointer: a hover changes what the next frames draw, and the
    // no-chips baseline below must not depend on where the mouse happens to be.
    ImGui::GetIO().AddMousePosEvent(1.0f, 1.0f);
    frameOnce();
    check(activeTooltip() == nullptr, "premise: the parked pointer opens no tooltip");

    // The empty meta row: the no-chips baseline the row's own pixels are the
    // diff against.
    const std::string noChipsShot = outDir + "/composer-gui-nochips.ppm";
    if (!state.backend->captureNextFramePpm(noChipsShot)) {
        std::fprintf(stderr, "the GUI backend refused the capture request\n");
        state.backend->shutdown();
        ImGui::DestroyContext();
        return 2;
    }
    frameOnce();
    Ppm noChips;
    check(readPpm(noChipsShot, noChips), "the no-chips frame was captured");

    // The two chips, seeded the way task 4's grid test seeds them: one waiting
    // on D and one attached, whose label is the workarea path D reported.
    check(llmfun::tui::model::chipCount(state.ui.drop) == 0, "premise: the row starts empty");
    const std::int64_t firstId = llmfun::tui::model::appendDroppedPath(state.ui.drop, "/tmp/a.md");
    const std::int64_t secondId =
        llmfun::tui::model::appendDroppedPath(state.ui.drop, "/tmp/deep/nested/b.md");
    llmfun::tui::model::setChipStatus(state.ui.drop, secondId,
                                      llmfun::tui::model::DropChipState::Done, "dropped/b.md",
                                      "dropped/b.md");
    const llmfun::tui::model::DropChip* waiting = llmfun::tui::model::chipAt(state.ui.drop, 0);
    const llmfun::tui::model::DropChip* attached = llmfun::tui::model::chipAt(state.ui.drop, 1);
    check(waiting != nullptr && attached != nullptr, "premise: two chips seeded");
    check(waiting != nullptr && waiting->state == llmfun::tui::model::DropChipState::Pending &&
              waiting->label == "processing a.md..." && waiting->fullPath == "/tmp/a.md",
          "the waiting chip's label is the processing text, the hover's answer is the full path");
    check(attached != nullptr && attached->label == "dropped/b.md" &&
              attached->fullPath == "/tmp/deep/nested/b.md",
          "the attached chip's label is the workarea path, the hover's answer is the full path");

    // The row's geometry, derived the way the view derives it: the empty draft's
    // one text row puts the meta row above the bottom border, the chips grow
    // right from the row's left edge, and the row stops where the action starts.
    const float border = settled.border;
    const float textY = region.y + border;
    const float metaY = textY + static_cast<float>(views::kComposerMinRows) * settled.row;
    const float metaH = settled.row;
    const float innerX = region.x + border + settled.pad1;
    const float innerW = std::max(0.0f, region.w - 2.0f * (border + settled.pad1));
    const float actionW =
        std::min(ui::measureButton(settled, ui::ButtonSpec{"Stopping", true, false, false}).w,
                 std::max(0.0f, innerW / 2.0f));
    const float chipsRight = innerX + innerW - actionW - settled.pad1;
    const char* xMark = ui::icon(ui::Icon::Error, ui::pixelGlyphSet());
    const ui::ButtonSpec xSpec{xMark, true, false, false};
    const float xW = ui::measureButton(settled, xSpec).w;
    const float minLabelW =
        ui::measureChip(settled, ui::icon(ui::Icon::Ellipsis, ui::pixelGlyphSet())).w;

    float chipX = innerX + settled.pad1;
    const float cap1 = chipsRight - chipX - xW;
    const float labelW1 = ui::measureChip(settled, "processing a.md...").w;
    const float cap2 = chipsRight - (chipX + labelW1 + xW) - xW;
    const float labelW2 = ui::measureChip(settled, "dropped/b.md").w;
    check(cap1 > labelW1 && cap2 > labelW2,
          "premise: the row holds both chips whole (no truncation in the frame)");
    const ui::Rect label1Rect{chipX, metaY, labelW1, metaH, false};
    const ui::Rect x1Rect{chipX + labelW1, metaY, xW, metaH, false};
    const float chip2X = chipX + labelW1 + xW;
    const ui::Rect label2Rect{chip2X, metaY, labelW2, metaH, false};
    const float rowRight = chip2X + labelW2 + xW;

    // The frame the screenshot is taken from: the shipped shell, the shipped
    // renderer, the pointer parked.
    const std::string chipsShot = outDir + "/composer-gui-chips-1280x820.ppm";
    if (!state.backend->captureNextFramePpm(chipsShot)) {
        std::fprintf(stderr, "the GUI backend refused the capture request\n");
        state.backend->shutdown();
        ImGui::DestroyContext();
        return 2;
    }
    frameOnce();
    Ppm chips;
    check(readPpm(chipsShot, chips), "the chips frame was captured");

    const DiffBox rowDiff = diffBox(noChips, chips);
    check(rowDiff.count > 0,
          "the chips change the frame (differing=" + std::to_string(rowDiff.count) + ")");
    check(rowDiff.x0 >= static_cast<int>(chipX) - 1 && rowDiff.x1 <= static_cast<int>(rowRight) + 1,
          "...spanning exactly the chip row (x=" + std::to_string(rowDiff.x0) + ".." +
              std::to_string(rowDiff.x1) + ")");
    check(rowDiff.y0 >= static_cast<int>(metaY) - 1 &&
              rowDiff.y1 <= static_cast<int>(metaY + metaH) + 1,
          "...on the meta row only (y=" + std::to_string(rowDiff.y0) + ".." +
              std::to_string(rowDiff.y1) + ")");
    check(rowDiff.x0 >= static_cast<int>(region.x) &&
              rowDiff.x1 < static_cast<int>(region.right()) &&
              rowDiff.y0 >= static_cast<int>(region.y) &&
              rowDiff.y1 < static_cast<int>(region.bottom()),
          "...and the row is inside the composer region");

    // --- A hover explains the chip with the FULL drop path ------------------
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(label1Rect.x + label1Rect.w * 0.5f, metaY + metaH * 0.5f);
        // A tooltip window is laid out on the hover frame but drawn on the next
        // one (ImGui hides a window it has not sized yet: on the event frame the
        // window carries only its padding), so the hover is two frames - the
        // event, then the appearance the assertions and the capture read.
        frameOnce();
        frameOnce();
        const ImGuiWindow* tip = activeTooltip();
        check(tip != nullptr, "hovering the waiting chip opens its tooltip");
        // The window auto-sizes to its text: the width is the path's measure plus
        // the theme's padding, and the label is *not* that text - the tooltip is
        // where the original path still shows.
        const float expect =
            ImGui::CalcTextSize("/tmp/a.md").x + 2.0f * ImGui::GetStyle().WindowPadding.x;
        check(tip != nullptr && std::fabs(tip->Size.x - expect) <= 2.0f,
              "...carrying the full drop path (w=" + trimNum(tip ? tip->Size.x : -1.0f) +
                  ", path=" + trimNum(expect) + ")");
        const float labelW =
            ImGui::CalcTextSize("processing a.md...").x + 2.0f * ImGui::GetStyle().WindowPadding.x;
        check(tip != nullptr && std::fabs(tip->Size.x - labelW) > 2.0f,
              "...and not the label the row already shows (label=" + trimNum(labelW) + ")");

        const std::string hoverShot = outDir + "/composer-gui-hover-chip1.ppm";
        check(state.backend->captureNextFramePpm(hoverShot), "the hover frame is capturable");
        frameOnce();
        Ppm hover;
        check(readPpm(hoverShot, hover), "the hover frame was captured");
        const DiffBox hoverDiff = diffBox(chips, hover);
        check(hoverDiff.count > 0,
              "the tooltip paints (differing=" + std::to_string(hoverDiff.count) + ")");
    }
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(label2Rect.x + label2Rect.w * 0.5f, metaY + metaH * 0.5f);
        // The same two frames: the event, then the sized-and-drawn appearance.
        frameOnce();
        frameOnce();
        const ImGuiWindow* tip = activeTooltip();
        check(tip != nullptr, "hovering the attached chip opens its tooltip too");
        const float expect = ImGui::CalcTextSize("/tmp/deep/nested/b.md").x +
                             2.0f * ImGui::GetStyle().WindowPadding.x;
        check(tip != nullptr && std::fabs(tip->Size.x - expect) <= 2.0f,
              "...carrying its full drop path, not its workarea label (w=" +
                  trimNum(tip ? tip->Size.x : -1.0f) + ", path=" + trimNum(expect) + ")");
    }

    // Parked again: the tooltip goes, and it takes its row-space answer with it.
    ImGui::GetIO().AddMousePosEvent(1.0f, 1.0f);
    frameOnce();
    check(activeTooltip() == nullptr, "the tooltip follows the pointer away");

    // --- The x click removes its chip and queues the cancel -----------------
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(x1Rect.x + x1Rect.w * 0.5f, metaY + metaH * 0.5f);
        frameOnce();
        io.AddMouseButtonEvent(0, true);
        frameOnce();
        io.AddMouseButtonEvent(0, false);
        frameOnce();
        check(llmfun::tui::model::chipCount(state.ui.drop) == 1,
              "the x click removes its chip at once");
        const llmfun::tui::model::DropChip* kept = llmfun::tui::model::chipAt(state.ui.drop, 0);
        check(kept != nullptr && kept->id == secondId,
              "the removal takes the clicked chip, its neighbour stays");
        check(llmfun::tui::model::popChipCancel(state.ui.drop) == firstId,
              "the removed id queues for D's cancel poll");
        check(llmfun::tui::model::popChipCancel(state.ui.drop) == 0, "one click queues one cancel");
    }

    // The row the click left behind: the pointer parked, the survivor redrawn at
    // the head of the row.
    ImGui::GetIO().AddMousePosEvent(1.0f, 1.0f);
    const std::string afterShot = outDir + "/composer-gui-after-x.ppm";
    check(state.backend->captureNextFramePpm(afterShot), "the after-click frame is capturable");
    frameOnce();
    Ppm after;
    check(readPpm(afterShot, after), "the after-click frame was captured");
    const DiffBox afterDiff = diffBox(chips, after);
    check(afterDiff.count > 0, "the row redraws without the removed chip (differing=" +
                                   std::to_string(afterDiff.count) + ")");

    state.backend->shutdown();
    ImGui::DestroyContext();

    std::printf("%s: %d check(s), %d failure(s); wrote %s\n", g_failures == 0 ? "PASS" : "FAIL",
                g_checks, g_failures, chipsShot.c_str());
    return g_failures == 0 ? 0 : 1;
}
