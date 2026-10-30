// test_inspector_gui.cpp
//
// The floating inspector in the pixel profile (task 29, D-INSPECTOR / D-OVERLAY).
// 900x600 is inside the pixel profile's overlay band (the wide breakpoint is 150
// units = 1200 px at 100 %), so this is the only binary that draws the floating
// shape and its own `##inspector_panel` child window through the real
// GLFW/OpenGL3 backend. The grid half lives in `./test_inspector`, which also pins
// the paint-order mechanism; this one exists for what only the pixel profile can
// answer:
//
//   1. the shape: at 900x600 the layout gives the panel 90 % of the width, anchored
//      right, spanning the top bar's bottom to the now bar's top - over the composer;
//   2. the overlay is contained: closing the panel changes no pixel outside its own
//      rectangle (the tiling underneath stays where it was), measured on the frames
//      rather than on the layout.
//
// The screenshot (`inspector-gui-900x600.ppm`) is the visual evidence: the same
// frame the assertions measured.
//
// Needs a display (run under xvfb-run), exit codes: 0 = pass, 1 = a check failed,
// 2 = the backend could not start.
//
//   xvfb-run -s "-screen 0 900x600x24" ./test_inspector_gui [--out DIR]

#include "model/ui_state.h"
#include "tui.h"
#include "tui_backend.h"
#include "tui_settings.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/tokens.h"
#include "views/inspector.h"
#include "views/shell.h"

#include "imgui/imgui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ui = llmfun::tui::ui;
namespace model = llmfun::tui::model;
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

/// One P6 capture: the GUI has no cell grid, so a claim about what a region looks
/// like has to be a claim about the frame the backend produced - the same seam
/// `test_rail_gui` uses.
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

/// Where two captures disagree: the bounding box of the differing pixels and how many there
/// are. `count == 0` means the frames are identical, and `y1 < y0` is the empty box.
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

/// A workbench with sessions and a running-ready status: the shell draws a thread,
/// a rail and a composer, so the panel has something to cover.
void seedState(llmfun::tui::TuiState& state) {
    llmfun::tui::SessionEntry entry;
    entry.id = "s1";
    entry.title = "Inspector frame";
    entry.preview = "the panel over the composer";
    entry.messageCount = 3;
    entry.isActive = true;
    state.sessionPanel.sessions.push_back(entry);
    state.sessionPanel.activeId = "s1";
    state.readyStatus = true;
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

    TuiState state;
    state.ui.prefs.inspectorOpen = 1;
    state.ui.prefs.tab = model::InspectorTab::Agents;
    seedState(state);
    // A draft with words the field draws, so the composer has content of its own under the
    // panel; one line keeps the composer's grown height stable between the layout below and
    // the captured frame.
    state.userQuery.inputBuf = "a draft the composer must not paint over the panel";

    TuiSettings view;
    view.fontScaleMain = 1.0f;
    view.windowWidth = 900;
    view.windowHeight = 600;
    const std::string ini = outDir + "/inspector-gui.conf";
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

    // One settle frame: the font scale and the composer's grown height are frame state, so
    // the layout below has to be the one the captured frame uses - the pre-frame metrics can
    // still be the stock scale.
    state.backend->newFrame();
    tuiRender(state);
    state.backend->renderFrame();

    const ImVec2 display(900.0f, 600.0f);
    const ui::Metrics m = ui::metricsFor();
    check(!m.textGrid, "premise: the pixel profile is live");

    const ui::LayoutPrefs prefs = views::layoutPrefsFor(state.ui, state.palette.open);
    const ui::ShellLayout layout =
        views::shellLayoutFor(prefs, display, /*inspectorAuto=*/state.ui.prefs.inspectorOpen < 0);
    if (g_verbose)
        std::printf("geometry: inspector %.0f..%.0f, composer %.0f..%.0f, nowBar %.0f..%.0f, "
                    "row %.0f\n",
                    layout.inspector.y, layout.inspector.bottom(), layout.composer.y,
                    layout.composer.bottom(), layout.nowBar.y, layout.nowBar.bottom(), m.row);
    check(layout.inspectorOverlay, "900x600 is the inspector's overlay band");
    check(layout.inspector.w == 810.0f && layout.inspector.right() == 900.0f,
          "the panel is 90 % wide and anchored right (" + std::to_string(layout.inspector.w) +
              " px)");
    check(layout.inspector.y == layout.topBar.bottom() &&
              layout.inspector.bottom() == layout.nowBar.y,
          "and it spans the top bar to the now bar");
    check(layout.composer.y < layout.inspector.bottom(),
          "which is over the composer (composer y=" + std::to_string(layout.composer.y) + ")");
    check(layout.top == ui::OverlayLayer::Inspector, "the panel is the topmost overlay");

    // The frame the screenshot is taken from: the shipped shell, the shipped renderer.
    const std::string shot = outDir + "/inspector-gui-900x600.ppm";
    if (!state.backend->captureNextFramePpm(shot)) {
        std::fprintf(stderr, "the GUI backend refused the capture request\n");
        std::exit(2);
    }
    state.backend->newFrame();
    tuiRender(state);
    state.backend->renderFrame();

    check(state.inspector.open && state.inspector.overlay, "the frame drew the floating panel");
    check(state.inspector.tab == model::InspectorTab::Agents, "on the Agents tab");
    check(state.inspector.drawnRows == 1, "the empty Agents tab draws its one row (" +
                                              std::to_string(state.inspector.drawnRows) + ")");

    Ppm open;
    check(readPpm(shot, open), "the open-panel frame was captured");
    check(open.width == 900 && open.height == 600,
          "at 900x600 (" + std::to_string(open.width) + "x" + std::to_string(open.height) + ")");

    // Closing the panel changes nothing outside its own rectangle: D-OVERLAY's "regions
    // underneath unchanged", measured on the frame instead of the layout. The z-order
    // mechanism (the panel's own `##inspector_panel` window, drawn after the composer) is
    // pinned on the text grid in `./test_inspector`; in the pixel profile the opaque child
    // backgrounds hide it, so this binary pins the shape and this property instead.
    const std::string closedShot = outDir + "/inspector-gui-900x600-closed.ppm";
    state.ui.prefs.inspectorOpen = 0;
    state.backend->newFrame();
    tuiRender(state);
    state.backend->renderFrame();
    state.backend->newFrame();
    tuiRender(state);
    state.backend->renderFrame();
    check(!state.inspector.open, "premise: the panel is closed");
    if (!state.backend->captureNextFramePpm(closedShot)) {
        check(false, "the GUI backend refused the second capture request");
    } else {
        state.backend->newFrame();
        tuiRender(state);
        state.backend->renderFrame();
        Ppm closed;
        check(readPpm(closedShot, closed), "the closed-panel frame was captured");
        const DiffBox diff = diffBox(open, closed);
        check(diff.count > 0,
              "closing the panel changes the frame (" + std::to_string(diff.count) + " pixels)");
        check(diff.x0 >= static_cast<int>(layout.inspector.x) &&
                  diff.x1 < static_cast<int>(layout.inspector.right()) &&
                  diff.y0 >= static_cast<int>(layout.inspector.y) &&
                  diff.y1 < static_cast<int>(layout.inspector.bottom()),
              "and every changed pixel is inside the panel's rectangle (" +
                  std::to_string(diff.x0) + ".." + std::to_string(diff.x1) + ", " +
                  std::to_string(diff.y0) + ".." + std::to_string(diff.y1) + ")");
    }

    // ---- the lane rows are measured in the step they are drawn in (task 30) --------------
    //
    // The pixel profile sets a label at 13 px and a body at 15 px, so a row measured under the
    // strip's scope and painted under the body's is accepted ~15 % too wide and then clipped
    // mid-glyph by the draw path - and no cell golden can see it, because on the grid both
    // steps are one row tall. The docked column (224 px) is where the degradation bites, so
    // that is the width this asks about.
    state.backend->newFrame();
    {
        const ui::Metrics pm = ui::metricsFor();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(900.0f, 600.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("lane-budget", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        std::vector<model::AgentLane> lanes;
        model::AgentLane longLane;
        longLane.id = "reviewer-of-the-parser-with-a-name-long-enough-to-be-cut";
        longLane.label = longLane.id;
        longLane.role = "assistant";
        longLane.state = model::kAgentStateRunning;
        longLane.currentTool = "read_file_with_a_long_name";
        longLane.tokens = 123'456;
        longLane.elapsedMs = 3'723'000;
        longLane.active = true;
        lanes.push_back(longLane);

        views::InspectorState panel;
        panel.open = true;
        panel.tab = model::InspectorTab::Agents;
        const ui::Rect region{0.0f, 0.0f, pm.inspectorW, 400.0f, /*cells=*/false};
        const views::InspectorParts parts = views::inspectorMeasure(panel, lanes, region, pm);
        check(parts.laneRows.size() == 1, "premise: the panel measured one lane row");
        // The budget the measure used (its `rowBudget`: the content width minus the scrollbar
        // allowance), recomputed here so the assertion is about the contract, not the code.
        const float budget = std::max(0.0f, region.w - 2.0f * pm.pad1) - pm.pad1;
        float widest = 0.0f;
        bool fits = true;
        {
            // The step the row is *drawn* in (`drawPanelBody()`'s body scope).
            const ui::TypeScope bodyType(ui::TypeStep::body, /*textGrid=*/false);
            for (const views::InspectorParts::LaneRow& row : parts.laneRows) {
                const float drawn =
                    ImGui::CalcTextSize(row.text.c_str(), row.text.c_str() + row.text.size()).x;
                widest = std::max(widest, drawn);
                if (drawn > budget + 0.5f)
                    fits = false;
            }
        }
        if (g_verbose)
            std::printf("row='%s' budget=%.1f body=%.1f\n", parts.laneRows.front().text.c_str(),
                        budget, widest);
        check(fits, "a lane row fits the body step it is drawn in (" + std::to_string(widest) +
                        " of " + std::to_string(budget) + " px)");

        ImGui::End();
        ImGui::PopStyleVar();
    }
    state.backend->renderFrame();

    state.backend->shutdown();
    ImGui::DestroyContext();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %d check(s); wrote %s\n", g_checks, shot.c_str());
    return 0;
}
