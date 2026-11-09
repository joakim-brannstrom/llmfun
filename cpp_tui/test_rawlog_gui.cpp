// test_rawlog_gui.cpp
//
// The Raw Log surface in the pixel profile: the same view the text grid draws,
// rendered through the real GLFW/OpenGL3 backend. The grid half is
// `./test_rawlog`; this binary answers the three things only pixels can:
//
//   1. the column plan is profile-blind: at the same cell width the pixel
//      profile places every segment on the grid's ladder (marker, clock, level,
//      location, message), and the rungs flip at the same widths;
//   2. the message and the location are cut with the ASCII `...` marker when
//      the content is wider than its column - measured as visible ink on the
//      frame, not copied out of the view;
//   3. a drawn row is where the plan says it is, and the geometry is stable
//      across a second frame (the follower's pin never moves a full window).
//
// The comparison instrument: the frame's own draw lists. Glyph vertices carry
// the atlas texture's glyph area in their uv; fills paint with the white pixel
// uv, so vertices with a different uv are text ink, and a vertex counts only
// when its own command's clip rect admits it - a segment whose text overflowed
// its box is scissored away at render time and must not bleed into the next
// column's measurement. The strings the rows are compared against are measured
// in their own frame, drawn in a full-display probe window; the geometry is
// verified stable across frames, so a probe's ink is comparable with a row's
// ink drawn later.
//
// The screenshot (`rawlog-gui-100x30.ppm`) is the visual evidence: a clean
// frame of the same surface the assertions measured. The asserted widths sit
// mid-band, so a rounding grain cannot flip a rung under them.
//
// Needs a display (run under xvfb-run), exit codes: 0 = pass, 1 = a check
// failed, 2 = the backend could not start.
//
//   xvfb-run -s "-screen 0 1280x820x24" ./test_rawlog_gui [--out DIR]

#include "views/rawlog.h"

#include "tui.h"
#include "tui_api.h"
#include "tui_backend.h"
#include "tui_settings.h"
#include "ui/metrics.h"
#include "ui/tokens.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace ui = llmfun::tui::ui;
namespace views = llmfun::tui::views;
namespace model = llmfun::tui::model;

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

/// The same fixture epoch and coordinates the grid harness pins, so a record's
/// clock and location read the same on both sides of the ladder.
inline constexpr std::int64_t kFixtureEpochMs = 1'782'000'000'000;

model::RawLogRecord record(int level, const std::string& message, int index = 0,
                           const std::string& detail = std::string()) {
    model::RawLogRecord out;
    out.timestampMs = kFixtureEpochMs + 1000LL * index;
    out.level = level;
    out.action = 0;
    out.line = 412;
    out.message = message;
    out.detail = detail;
    out.module = "llm.agent.pool";
    out.function = "processTurn";
    out.file = "source/llm/agent/pool.d";
    return out;
}

/// Five records that cover every asserted segment: a plain row, a row with
/// detail (its marker and its preview line), a warn row with a short message
/// (the geometry anchor), a message far wider than its column (the message
/// ellipsis), and a function name far wider than the location column (the
/// location's mid-ellipsis).
std::deque<model::RawLogRecord> fixtureRecords() {
    std::deque<model::RawLogRecord> records;
    records.push_back(record(model::kRawLogLevelInfo, "session opened", 0));
    records.push_back(record(model::kRawLogLevelTrace, "tool args resolved", 1,
                             "args: path=README.md\nargs: limit=200"));
    records.push_back(record(model::kRawLogLevelWarn, "ui.conf is read-only", 2));
    records.push_back(record(model::kRawLogLevelInfo, std::string(80, 'x'), 3));
    model::RawLogRecord longFunction = record(model::kRawLogLevelInfo, "warming up", 4);
    longFunction.function = "processTurnWithAVeryLongName";
    records.push_back(longFunction);
    return records;
}

/// The visible text ink of one draw list inside a band: the bounding box of the
/// vertices that do not paint with the texture's white pixel (fills do; glyph
/// quads do not), inside the y band and the x window, and inside their own
/// command's clip rect - the scissor a glyph was drawn under is what the
/// display actually shows.
struct Run {
    bool empty = true;
    float minX = 0.0f;
    float maxX = 0.0f;
    int vertices = 0;
};

/// Which vertices the frame's commands can actually show: a vertex belongs to
/// the command whose index range references it - the backend draws with global
/// indices and a zero VtxOffset on every command - and it counts only when
/// that command's clip rect admits it, widened by a pixel: the scissor a glyph
/// was drawn under is what the display actually shows.
std::vector<unsigned char> admittedVertices(const ImDrawList* list) {
    std::vector<unsigned char> admitted(static_cast<std::size_t>(list->VtxBuffer.Size), 0);
    const ImVector<ImDrawCmd>& cmds = list->CmdBuffer;
    for (int c = 0; c < cmds.size(); ++c) {
        const ImDrawCmd& cmd = cmds[c];
        for (int k = 0; k < cmd.ElemCount; ++k) {
            const unsigned int idx = list->IdxBuffer[static_cast<int>(cmd.IdxOffset) + k];
            const ImDrawVert& v = list->VtxBuffer[static_cast<int>(idx)];
            if (v.pos.x >= cmd.ClipRect.x - 1.0f && v.pos.x <= cmd.ClipRect.z + 1.0f &&
                v.pos.y >= cmd.ClipRect.y - 1.0f && v.pos.y <= cmd.ClipRect.w + 1.0f)
                admitted[static_cast<std::size_t>(idx)] = 1;
        }
    }
    return admitted;
}

Run runIn(const ImDrawList* list, const ImVec2& white, float y0, float y1, float x0, float x1) {
    Run run;
    run.minX = FLT_MAX;
    run.maxX = -FLT_MAX;
    if (list == nullptr)
        return run;
    // The backend draws with global indices and a zero VtxOffset on every
    // command, so a vertex belongs to the command whose index range references
    // it; a vertex counts only when that command's clip rect admits it - the
    // scissor a glyph was drawn under is what the display actually shows.
    const std::vector<unsigned char> admitted = admittedVertices(list);
    for (int i = 0; i < list->VtxBuffer.Size; ++i) {
        const ImDrawVert& v = list->VtxBuffer[i];
        if (!admitted[static_cast<std::size_t>(i)])
            continue;
        if (std::fabs(v.uv.x - white.x) < 1e-4f && std::fabs(v.uv.y - white.y) < 1e-4f)
            continue; // a fill, not a glyph
        if (v.pos.y < y0 || v.pos.y >= y1 || v.pos.x < x0 || v.pos.x > x1)
            continue;
        run.minX = std::min(run.minX, v.pos.x);
        run.maxX = std::max(run.maxX, v.pos.x);
        ++run.vertices;
    }
    run.empty = run.vertices == 0;
    return run;
}

/// The draw list of the window whose name carries `fragment`, the last one
/// begun this frame. The body is a real child window the view began; a probe
/// block is a window of the harness's own.
ImDrawList* windowList(const char* fragment) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImDrawList* out = nullptr;
    for (ImGuiWindow* window : g.Windows)
        if (window->Active && window->Name != nullptr &&
            std::strstr(window->Name, fragment) != nullptr)
            out = window->DrawList;
    return out;
}

constexpr ImGuiWindowFlags kSurfaceFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;

constexpr ImGuiWindowFlags kProbeFlags =
    kSurfaceFlags | ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_NoFocusOnAppearing;

/// The probe block's strings, in a fixed order; the named ones sit behind the
/// 80 head+ellipsis candidates.
std::vector<std::string> g_probes;
int g_probeFull = 0;
int g_probeWarnMsg = 0;
int g_probeClock12 = 0;
int g_probeClock8 = 0;
int g_probeLevel = 0;
int g_probeLocWarn = 0;
int g_probeLocPlain = 0;
int g_probeLocCut = 0;
int g_probeLocWhole = 0;
int g_probeLocCutWhole = 0;
int g_probeOpened = 0;

/// The probe runs, measured in their own frame: two columns of 33, with 24
/// pixels between rows, so one row's descenders never reach the next row's
/// measurement band and the columns never share a measurement window.
std::vector<Run> g_probeRuns;
constexpr int kProbeRowsPerColumn = 33;
constexpr float kProbeStrideX = 450.0f;
constexpr float kProbeStrideY = 24.0f;
float g_probeX = 0.0f;
float g_probeY = 0.0f;

void buildProbes(const model::RawLogRecord& plain, const model::RawLogRecord& warn,
                 const model::RawLogRecord& longFunction) {
    g_probes.clear();
    for (int k = 0; k < 56; ++k)
        g_probes.push_back(std::string(static_cast<std::size_t>(k), 'x') + "...");
    g_probeFull = static_cast<int>(g_probes.size());
    g_probes.push_back(std::string(80, 'x'));
    g_probeWarnMsg = static_cast<int>(g_probes.size());
    g_probes.push_back(warn.message);
    g_probeClock12 = static_cast<int>(g_probes.size());
    g_probes.push_back(model::rawLogClockText(warn.timestampMs));
    g_probeClock8 = static_cast<int>(g_probes.size());
    {
        std::string shortClock = g_probes[static_cast<std::size_t>(g_probeClock12)];
        shortClock.resize(8);
        g_probes.push_back(shortClock);
    }
    g_probeLevel = static_cast<int>(g_probes.size());
    g_probes.push_back(model::rawLogLevelField(warn.level));
    g_probeLocWarn = static_cast<int>(g_probes.size());
    g_probes.push_back(views::rawLogLocationText(warn, 36));
    g_probeLocPlain = static_cast<int>(g_probes.size());
    g_probes.push_back(views::rawLogLocationText(plain, 36));
    g_probeLocCut = static_cast<int>(g_probes.size());
    g_probes.push_back(views::rawLogLocationText(longFunction, 36));
    // The wide mode draws whole locations, so the comparison strings are the
    // whole forms (a cell budget far past both full lengths returns them
    // uncut).
    g_probeLocWhole = static_cast<int>(g_probes.size());
    g_probes.push_back(views::rawLogLocationText(plain, 80));
    g_probeLocCutWhole = static_cast<int>(g_probes.size());
    g_probes.push_back(views::rawLogLocationText(longFunction, 80));
    g_probeOpened = static_cast<int>(g_probes.size());
    g_probes.push_back(plain.message);
}

/// Draw the probe block in its own full-display window. The rows draw under the
/// body type scope, so a probe's ink is comparable with a row's ink.
void drawProbes() {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(1280.0f, 820.0f));
    ImGui::Begin("rawlog_gui_probe", nullptr, kProbeFlags);
    const ImVec2 base = ImGui::GetCursorScreenPos();
    g_probeX = base.x + 8.0f;
    g_probeY = base.y + 8.0f;
    {
        const ui::TypeScope bodyType(ui::TypeStep::body, false);
        ImDrawList* list = ImGui::GetWindowDrawList();
        for (std::size_t i = 0; i < g_probes.size(); ++i) {
            const float px = g_probeX + kProbeStrideX * static_cast<float>(i / kProbeRowsPerColumn);
            const float py = g_probeY + kProbeStrideY * static_cast<float>(i % kProbeRowsPerColumn);
            list->AddText(ImVec2(px, py), IM_COL32_WHITE, g_probes[i].c_str());
        }
    }
    ImGui::End();
}

void measureProbes(const ImVec2& white) {
    const ImDrawList* probe = windowList("rawlog_gui_probe");
    g_probeRuns.assign(g_probes.size(), Run{});
    if (probe == nullptr)
        return;
    for (std::size_t i = 0; i < g_probes.size(); ++i) {
        const float px = g_probeX + kProbeStrideX * static_cast<float>(i / kProbeRowsPerColumn);
        const float py = g_probeY + kProbeStrideY * static_cast<float>(i % kProbeRowsPerColumn);
        // The full 80-x probe and the whole-location probes are wider than a
        // column: give them the display's whole width so their measurement is
        // not clipped by the column window.
        const float x1 =
            (static_cast<int>(i) == g_probeFull || static_cast<int>(i) == g_probeLocWhole ||
             static_cast<int>(i) == g_probeLocCutWhole)
                ? 1270.0f
                : px + 440.0f;
        g_probeRuns[i] = runIn(probe, white, py - 2.0f, py + 20.0f, px - 2.0f, x1);
    }
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
    const std::string ini = outDir + "/rawlog-ui.ini";
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

    const ui::Metrics m = ui::metricsFor();
    check(!m.textGrid, "premise: the pixel profile is live");
    check(m.gu > 0.0f && m.row > 0.0f, "premise: the pixel metrics resolve");

    std::deque<model::RawLogRecord> records = fixtureRecords();
    const model::RawLogRecord& plain = records[0];
    const model::RawLogRecord& warn = records[2];
    const model::RawLogRecord& longFunction = records[4];
    model::RawLogRing ring;
    ring.records = &records;
    ring.arrivals = records.size();

    views::RawLogState rl;
    rl.open = true;
    rl.overlayShape = true;
    rl.minLevel = model::kRawLogLevelTrace;

    const float gu = m.gu;
    const float row = m.row;
    // The overlay's parts: two padded rows (title, filter) open the body and
    // the footer closes it, so a 30-row surface leaves a 26-row body. The width
    // is exact: cells * gu, so the plan's cell count cannot wobble.
    const auto regionFor = [&](int cells) {
        return ui::Rect{0.0f, 0.0f, 2.0f * m.pad2 + cells * gu, 2.0f * m.pad1 + 30.0f * row, false};
    };
    const ImVec2 origin(m.pad2, m.pad1 + 2.0f * row); // the body child's top-left

    // 1. The plan is profile-blind
    // Every rung boundary plus the widest surface the tests use, measured on
    // both profiles: the same width in cells must answer the same rung and the
    // same segment geometry, unit for unit.
    for (int cells : {9, 10, 31, 32, 69, 70, 73, 74, 83, 84, 100}) {
        const ui::Metrics grid = ui::metricsFor(true, 1.0f, 1.0f);
        const views::RowPlan gridPlan = views::rawRowPlan(cells * grid.gu, grid);
        const views::RowPlan pixelPlan = views::rawRowPlan(cells * gu, m);
        const auto same = [&](float gridUnits, float pixelPx) {
            return std::fabs(gridUnits - pixelPx / gu) < 1e-3f;
        };
        check(gridPlan.rung == pixelPlan.rung, "the ladder agrees at " + std::to_string(cells) +
                                                   " cells (rung " +
                                                   std::to_string(pixelPlan.rung) + ")");
        check(same(gridPlan.markerX, pixelPlan.markerX) &&
                  same(gridPlan.clockX, pixelPlan.clockX) &&
                  same(gridPlan.levelX, pixelPlan.levelX) && same(gridPlan.locX, pixelPlan.locX) &&
                  same(gridPlan.msgX, pixelPlan.msgX),
              "the segment offsets agree at " + std::to_string(cells) + " cells");
        check(same(gridPlan.markerW, pixelPlan.markerW) &&
                  same(gridPlan.clockW, pixelPlan.clockW) &&
                  same(gridPlan.levelW, pixelPlan.levelW) && same(gridPlan.locW, pixelPlan.locW) &&
                  same(gridPlan.msgW, pixelPlan.msgW) && same(gridPlan.tagW, pixelPlan.tagW),
              "the segment widths agree at " + std::to_string(cells) + " cells");
    }

    // The frame drivers and the measuring instruments
    const auto drawSurface = [&](const ui::Rect& region) {
        ImGui::SetNextWindowPos(ImVec2(region.x, region.y));
        ImGui::SetNextWindowSize(ImVec2(region.w, region.h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("rawlog-frame", nullptr, kSurfaceFlags);
        views::renderRawLog(rl, region, m, ring);
        ImGui::End();
        ImGui::PopStyleVar();
    };

    // The plan the view actually drew: the frame's own decision, read back
    // from the state the just-drawn frame filled. The fixtures overflow the
    // narrow message column, so past this point the expected geometry is the
    // wide plan wherever the view says it engaged.
    const auto framePlan = [&](const ui::Rect& region) {
        const float rowW = region.w - 2.0f * m.pad2;
        const views::RowPlan narrowPlan = views::rawRowPlan(rowW, m);
        if (!rl.wideMode)
            return narrowPlan;
        return views::rawRowPlanWide(rowW, m, rl.maxLocationW,
                                     std::max(rl.maxMessageW, rl.maxDetailW));
    };
    // The body's row width in the profile's unit: what the plan answers to.
    const auto rowWOf = [&](const ui::Rect& region) { return region.w - 2.0f * m.pad2; };
    // The body child's window record, whose scroll state outlives the frame.
    const auto bodyWindow = [] {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        ImGuiWindow* out = nullptr;
        for (ImGuiWindow* window : g.Windows)
            if (window->Active && window->Name != nullptr &&
                std::strstr(window->Name, "##rawlog_body") != nullptr)
                out = window;
        return out;
    };

    buildProbes(plain, warn, longFunction);

    const float kTol = 2.0f;
    const auto whiteOf = [] { return ImGui::GetFontTexUvWhitePixel(); };
    const auto widthOf = [](const Run& r) { return r.maxX - r.minX; };
    const auto rowBandY = [&](int line, float& y0, float& y1) {
        y0 = origin.y + row * static_cast<float>(line) + 1.0f;
        y1 = origin.y + row * static_cast<float>(line + 1) - 1.0f;
    };
    const auto windowRun = [&](const ImDrawList* list, int line, float wx0, float wx1) {
        float y0 = 0.0f;
        float y1 = 0.0f;
        rowBandY(line, y0, y1);
        return runIn(list, whiteOf(), y0, y1, wx0 - 2.0f, wx1 + 2.0f);
    };
    const auto probeRun = [&](int index) {
        check(index >= 0 && index < static_cast<int>(g_probeRuns.size()),
              "the probe index is in range");
        return index >= 0 && index < static_cast<int>(g_probeRuns.size()) ? g_probeRuns[index]
                                                                          : Run{};
    };
    const auto startsAt = [&](const Run& r, float expected, const std::string& what) {
        check(!r.empty && r.minX >= expected - 2.0f && r.minX <= expected + 3.0f,
              what + " starts at its column (x " + std::to_string(r.minX) + ", expected " +
                  std::to_string(expected) + ")");
    };
    const auto widthMatches = [&](const Run& r, const Run& want, const std::string& what) {
        check(!r.empty && !want.empty && std::fabs(widthOf(r) - widthOf(want)) <= kTol,
              what + "'s ink matches its probe (drawn " + std::to_string(widthOf(r)) + ", probe " +
                  std::to_string(widthOf(want)) + ")");
    };
    const auto strays = [&](const ImDrawList* list, int line,
                            const std::vector<std::pair<float, float>>& windows) {
        int count = 0;
        float y0 = 0.0f;
        float y1 = 0.0f;
        rowBandY(line, y0, y1);
        if (list == nullptr)
            return -1;
        const ImVec2 white = whiteOf();
        // A vertex is visible ink only when the command whose index range
        // references it clips it in.
        const std::vector<unsigned char> admitted = admittedVertices(list);
        for (int i = 0; i < list->VtxBuffer.Size; ++i) {
            const ImDrawVert& v = list->VtxBuffer[i];
            if (!admitted[static_cast<std::size_t>(i)])
                continue;
            if (std::fabs(v.uv.x - white.x) < 1e-4f && std::fabs(v.uv.y - white.y) < 1e-4f)
                continue;
            if (v.pos.y < y0 || v.pos.y >= y1)
                continue;
            bool inside = false;
            // Each window loosens by two pixels so a glyph whose left edge sits
            // a fraction left of its column is not mistaken for a stray, while
            // another segment's overflow is many pixels away and still counts.
            for (const auto& w : windows)
                if (v.pos.x >= w.first - 2.0f && v.pos.x <= w.second + 2.0f) {
                    inside = true;
                    break;
                }
            if (!inside)
                ++count;
        }
        return count;
    };

    // 2. The probe frame: the comparison strings, measured once
    state.backend->newFrame();
    drawProbes();
    measureProbes(whiteOf());
    state.backend->renderFrame();
    if (g_verbose) {
        for (int index : {g_probeClock12, g_probeLevel, g_probeLocWarn, g_probeWarnMsg,
                          g_probeOpened, g_probeLocCut, 55})
            std::printf("  probe %d (%.20s): empty=%d w=%.2f\n", index,
                        g_probes[static_cast<std::size_t>(index)].c_str(),
                        g_probeRuns[static_cast<std::size_t>(index)].empty ? 1 : 0,
                        widthOf(g_probeRuns[static_cast<std::size_t>(index)]));
    }
    {
        int emptyProbes = 0;
        for (const Run& r : g_probeRuns)
            if (r.empty)
                ++emptyProbes;
        check(emptyProbes == 0, "every probe measured ink (found " + std::to_string(emptyProbes) +
                                    " empty of " + std::to_string(g_probeRuns.size()) + ")");
    }

    // 3. The widest rung draws the whole ladder
    const ui::Rect region100 = regionFor(100);
    // The narrow plan is the premise (the rung ladder the surface sits on);
    // the drawn plan below is what wide mode turned it into.
    const views::RowPlan narrow0 = views::rawRowPlan(region100.w - 2.0f * m.pad2, m);
    check(narrow0.rung == 0, "100 cells sits on the widest rung");
    check(narrow0.msgW > 0.0f && narrow0.locW > 0.0f && narrow0.clockW > 0.0f,
          "the widest rung reserves every column");

    // The clean frame the screenshot is taken from.
    const std::string shot = outDir + "/rawlog-gui-100x30.ppm";
    if (!state.backend->captureNextFramePpm(shot)) {
        std::fprintf(stderr, "the GUI backend refused the capture request\n");
        std::exit(2);
    }
    state.backend->newFrame();
    drawSurface(region100);
    state.backend->renderFrame();

    // The measured frame: the surface alone, compared against the probe runs.
    state.backend->newFrame();
    drawSurface(region100);
    ImDrawList* body = windowList("rawlog_body");
    check(body != nullptr, "the body child is findable");

    // The frame drew the wide plan: the fixtures' 80-x message overflows the
    // narrow message column, so wide mode engaged (a stretched location
    // column, whole messages, no tag slack). Everything below measures
    // against the plan the view actually picked.
    const views::RowPlan p0 = framePlan(region100);
    check(rl.wideMode, "the 80-x message engages wide mode at 100 cells");

    float firstMsgX = -1.0f;
    if (body != nullptr) {
        const float x = origin.x;
        // The warn row (body line 3): marker blank, then clock, level,
        // location, message - each at its planned column, each as wide as its
        // probe, and nothing anywhere else on the row.
        const Run clockRun = windowRun(body, 3, x + p0.clockX, x + p0.clockX + p0.clockW);
        startsAt(clockRun, x + p0.clockX, "the clock");
        widthMatches(clockRun, probeRun(g_probeClock12), "the clock");
        const Run levelRun = windowRun(body, 3, x + p0.levelX, x + p0.levelX + p0.levelW);
        startsAt(levelRun, x + p0.levelX, "the level");
        widthMatches(levelRun, probeRun(g_probeLevel), "the level");
        const Run locRun = windowRun(body, 3, x + p0.locX, x + p0.locX + p0.locW);
        startsAt(locRun, x + p0.locX, "the location");
        widthMatches(locRun, probeRun(g_probeLocWhole),
                     "the location draws whole in the wide column");
        const Run msgRun = windowRun(body, 3, x + p0.msgX, x + p0.msgX + p0.msgW);
        startsAt(msgRun, x + p0.msgX, "the message");
        widthMatches(msgRun, probeRun(g_probeWarnMsg), "the message");
        firstMsgX = msgRun.empty ? -1.0f : msgRun.minX;
        check(strays(body, 3,
                     {{x + p0.markerX, x + p0.markerX + p0.markerW},
                      {x + p0.clockX, x + p0.clockX + p0.clockW},
                      {x + p0.levelX, x + p0.levelX + p0.levelW},
                      {x + p0.locX, x + p0.locX + p0.locW},
                      {x + p0.msgX, x + p0.msgX + p0.msgW}}) == 0,
              "the warn row draws nothing outside the plan's segments");

        // The whole message of the plain row is not cut: it matches its probe.
        const Run openedRun = windowRun(body, 0, x + p0.msgX, x + p0.msgX + p0.msgW);
        widthMatches(openedRun, probeRun(g_probeOpened), "the whole message");

        // The row with detail reserves the marker column and uses it.
        const Run markerRun = windowRun(body, 1, x + p0.markerX, x + p0.markerX + p0.markerW);
        check(!markerRun.empty, "the marker cell carries the openable glyph");

        // The 80-x message cannot fit the narrow column, so wide mode drew it
        // whole: the row starts at the wide message column and runs into the
        // body's right edge, where the window - not an ellipsis - cuts it. An
        // ellipsized draw would stop an ellipsis room short of the edge.
        const Run cutRun = windowRun(body, 4, x + p0.msgX, x + p0.msgX + p0.msgW);
        startsAt(cutRun, x + p0.msgX, "the wide message");
        check(!cutRun.empty && cutRun.maxX >= x + rowWOf(region100) - 18.0f,
              "the wide message runs to the body's edge, uncut (no ellipsis room)");

        // The long function name fits the wide location column: the drawn run
        // is the whole form, not the mid-ellipsized one.
        const Run locCutRun = windowRun(body, 5, x + p0.locX, x + p0.locX + p0.locW);
        startsAt(locCutRun, x + p0.locX, "the wide location");
        widthMatches(locCutRun, probeRun(g_probeLocCutWhole),
                     "the long location draws whole in the wide column");
        check(g_probes[static_cast<std::size_t>(g_probeLocCut)].compare(0, 3, "...") == 0,
              "the location's mid-ellipsis is the ASCII form");
    }

    state.backend->renderFrame();

    // The geometry is stable: the same surface again, and the message column
    // sits where it sat (the follower's pin did not move the window).
    state.backend->newFrame();
    drawSurface(region100);
    body = windowList("rawlog_body");
    if (body != nullptr && firstMsgX > 0.0f) {
        const Run again = windowRun(body, 3, origin.x + p0.msgX, origin.x + p0.msgX + p0.msgW);
        check(!again.empty && std::fabs(again.minX - firstMsgX) <= 0.5f,
              "the message column is stable across frames (x " + std::to_string(again.minX) +
                  " then " + std::to_string(firstMsgX) + ")");
    }
    state.backend->renderFrame();

    // 4. The narrow rungs degrade the same way the grid's do
    // 72 cells: the clock shortens, the location stays.
    const ui::Rect region72 = regionFor(72);
    const views::RowPlan p2 = views::rawRowPlan(region72.w - 2.0f * m.pad2, m);
    check(p2.rung == 2, "72 cells sits on the second rung");
    check(p2.clockW == 8.0f * gu && p2.locW == 36.0f * gu,
          "the second rung keeps the location and shortens the clock");
    state.backend->newFrame();
    drawSurface(region72);
    body = windowList("rawlog_body");
    // The drawn plan: wide mode engaged (the fixtures overflow the narrow
    // column at 72 cells too), so the location column is the stretched one.
    const views::RowPlan draw2 = framePlan(region72);
    if (body != nullptr) {
        const float x = origin.x;
        const Run loc72 = windowRun(body, 0, x + draw2.locX,
                                    std::min(x + draw2.locX + draw2.locW, x + draw2.msgX - 4.0f));
        check(!loc72.empty && loc72.minX >= x + draw2.locX - 1.0f &&
                  loc72.minX <= x + draw2.locX + 3.0f,
              "the second rung still draws the location at its column");
        widthMatches(loc72, probeRun(g_probeLocWhole),
                     "the second rung's location draws whole in the wide column");
        const Run clock72 = windowRun(body, 0, x + draw2.clockX, x + draw2.clockX + draw2.clockW);
        widthMatches(clock72, probeRun(g_probeClock8), "the second rung's clock");
    }
    state.backend->renderFrame();

    // 60 cells: the location column is not drawn, and the row leaves the
    // location's cells blank.
    const ui::Rect region60 = regionFor(60);
    const views::RowPlan p3 = views::rawRowPlan(region60.w - 2.0f * m.pad2, m);
    check(p3.rung == 3, "60 cells sits on the third rung");
    check(p3.locW == 0.0f, "the third rung reserves no location");
    state.backend->newFrame();
    drawSurface(region60);
    body = windowList("rawlog_body");
    // The drawn plan: wide mode engaged, but the third rung has no location
    // column to stretch - the wide plan keeps the rung's message column and
    // only widens the message to the content's own width.
    const views::RowPlan draw3 = framePlan(region60);
    if (body != nullptr) {
        const float x = origin.x;
        const Run msg60 = windowRun(body, 0, x + draw3.msgX, x + draw3.msgX + draw3.msgW);
        check(!msg60.empty && msg60.minX >= x + draw3.msgX - 1.0f &&
                  msg60.minX <= x + draw3.msgX + 3.0f,
              "the third rung's message starts at its column");
        widthMatches(msg60, probeRun(g_probeOpened), "the third rung's message");
        const Run right = windowRun(
            body, 0, x + draw3.msgX + (msg60.empty ? 0.0f : widthOf(msg60)) + 2.0f, FLT_MAX);
        check(right.empty, "the third rung's row leaves the location's cells blank");
        const Run clock60 = windowRun(body, 0, x + draw3.clockX, x + draw3.clockX + draw3.clockW);
        check(!clock60.empty, "the third rung still draws the short clock");
    }
    state.backend->renderFrame();

    // 5. The wide mode's own behaviors
    // The tag row joins the fixtures - a short message with an action and a
    // payload, appended last so every body line the sections above pinned
    // keeps its index - and the tag's string joins the probe block, which is
    // re-measured whole so the earlier probes keep their ink.
    model::RawLogRecord tagged = record(model::kRawLogLevelInfo, "retry", 5);
    tagged.action = TuiLogAction_Retry;
    tagged.actionPayload = "turn-7";
    records.push_back(tagged);
    ring.arrivals = records.size();
    g_probes.push_back("action 1 turn-7");
    const int probeTag = static_cast<int>(g_probes.size()) - 1;
    state.backend->newFrame();
    drawProbes();
    measureProbes(whiteOf());
    state.backend->renderFrame();

    // 120 cells: wide mode is engaged (the 80-x message overflows the narrow
    // column's 56 cells), the content is wider than the body so the horizontal
    // scrollbar is live, and the wide plan reserves no tag slack - the tag's
    // cells stay blank.
    const ui::Rect region120 = regionFor(120);
    const views::RowPlan narrow120 = views::rawRowPlan(region120.w - 2.0f * m.pad2, m);
    check(narrow120.rung == 0, "120 cells sits on the widest rung");
    state.backend->newFrame();
    drawSurface(region120); // a first frame lays the wide content out
    state.backend->newFrame();
    drawSurface(region120);
    body = windowList("rawlog_body");
    {
        ImGuiWindow* bodyWin = bodyWindow();
        const float x = origin.x;
        const float rowW = rowWOf(region120);
        check(rl.wideMode, "the 80-x message engages wide mode at 120 cells");
        check(rl.contentW > rowW, "the wide content overflows the body");
        check(bodyWin != nullptr && bodyWin->ScrollMax.x > 0.0f,
              "the wide content engages the horizontal scrollbar");
        const Run tagRun = windowRun(body, 6, x + rowW - 15.0f * gu, x + rowW);
        check(tagRun.empty, "wide mode leaves the action tag's cells blank");
    }
    state.backend->renderFrame();

    // 150 cells: the message column fits the content again, so the exact narrow
    // layout is back - no wide mode, no horizontal scrollbar, and the
    // tag draws at the row's right edge, payload riding along.
    const ui::Rect region150 = regionFor(150);
    state.backend->newFrame();
    drawSurface(region150); // a first frame lays the narrow content out
    state.backend->newFrame();
    drawSurface(region150);
    body = windowList("rawlog_body");
    {
        ImGuiWindow* bodyWin = bodyWindow();
        const float x = origin.x;
        const float rowW = region150.w - 2.0f * m.pad2;
        check(!rl.wideMode, "the wide window keeps narrow mode");
        check(bodyWin != nullptr && bodyWin->ScrollMax.x <= 0.0f,
              "the narrow layout has no horizontal scrollbar");
        const Run tagRun = windowRun(body, 6, x + rowW - 20.0f * gu, x + rowW);
        widthMatches(tagRun, probeRun(probeTag),
                     "narrow mode draws the action tag at the row's right edge");
    }
    state.backend->renderFrame();

    // 6. The follower in a wide, overflowing body: the pin keeps the bottom
    // fold while content appends, a horizontal wheel notch leaves follow
    // alone, a vertical one takes it, and so does a press on the scrollbar's
    // corner square. A second surface keeps the first's geometry assertions
    // untouched.
    std::deque<model::RawLogRecord> records2;
    for (int i = 0; i < 40; ++i)
        records2.push_back(record(model::kRawLogLevelInfo, std::string(90, 'y'), i));
    model::RawLogRing ring2;
    ring2.records = &records2;
    ring2.arrivals = records2.size();
    views::RawLogState rl2;
    rl2.open = true;
    rl2.overlayShape = true;
    rl2.minLevel = model::kRawLogLevelTrace;
    const auto drawSurface2 = [&](const ui::Rect& region) {
        ImGui::SetNextWindowPos(ImVec2(region.x, region.y));
        ImGui::SetNextWindowSize(ImVec2(region.w, region.h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("rawlog-frame2", nullptr, kSurfaceFlags);
        views::renderRawLog(rl2, region, m, ring2);
        ImGui::End();
        ImGui::PopStyleVar();
    };

    const ui::Rect region60b = regionFor(60);
    for (int f = 0; f < 4; ++f) {
        state.backend->newFrame();
        drawSurface2(region60b);
        state.backend->renderFrame();
    }
    state.backend->newFrame();
    drawSurface2(region60b);
    {
        ImGuiWindow* bodyWin2 = bodyWindow();
        check(rl2.follow && !rl2.followCleared, "premise: the second surface follows");
        check(bodyWin2 != nullptr && std::fabs(bodyWin2->Scroll.y - bodyWin2->ScrollMax.y) <= 0.5f,
              "the follower pins to the bottom fold (scroll " +
                  std::to_string(bodyWin2 ? bodyWin2->Scroll.y : -1.0f) + " of " +
                  std::to_string(bodyWin2 ? bodyWin2->ScrollMax.y : -1.0f) + ")");
        check(rl2.followPins >= 1 && rl2.followPins <= 3,
              "the pin throttles to a handful of jumps (did " + std::to_string(rl2.followPins) +
                  ")");
    }
    state.backend->renderFrame();

    ImGuiIO& io = ImGui::GetIO();
    // A horizontal wheel notch scrolls the content sideways and leaves the
    // follow flag alone; a vertical one is the user taking the view.
    rl2.followCleared = false;
    io.AddMousePosEvent(origin.x + 8.0f, origin.y + 8.0f);
    io.AddMouseWheelEvent(6.0f, 0.0f);
    state.backend->newFrame();
    drawSurface2(region60b);
    check(!rl2.followCleared, "a horizontal wheel notch does not clear follow");
    state.backend->renderFrame();

    io.AddMouseWheelEvent(0.0f, 1.0f);
    state.backend->newFrame();
    drawSurface2(region60b);
    check(rl2.followCleared, "a vertical wheel notch clears follow");
    state.backend->renderFrame();

    // The scrollbar's corner square: the press lands in the last
    // ScrollbarSize columns of the body, which is the strip the surface calls
    // the scrollbar.
    rl2.follow = true;
    rl2.followCleared = false;
    {
        ImGuiWindow* bodyWin2 = bodyWindow();
        check(bodyWin2 != nullptr, "the second body is findable for the press");
        if (bodyWin2 != nullptr) {
            io.AddMousePosEvent(bodyWin2->Pos.x + bodyWin2->Size.x - 3.0f,
                                bodyWin2->Pos.y + bodyWin2->Size.y - 3.0f);
            io.AddMouseButtonEvent(0, true);
            state.backend->newFrame();
            drawSurface2(region60b);
            check(rl2.followCleared, "a press on the scrollbar's corner square clears follow");
            state.backend->renderFrame();
            io.AddMouseButtonEvent(0, false);
        }
    }

    // The ultra-narrow guard answers to the narrow plan even in wide mode:
    // below the leading segments' floor no record row draws at all.
    const ui::Rect region9 = regionFor(9);
    const views::RowPlan narrow9 = views::rawRowPlan(rowWOf(region9), m);
    check(narrow9.msgX > rowWOf(region9), "premise: the floor rung's segments exceed the body");
    state.backend->newFrame();
    drawSurface2(region9);
    check(rl2.drawnRows == 0, "the ultra-narrow body draws no record rows");
    state.backend->renderFrame();

    // The copy spans only the rows the user can see. Wide mode's horizontal
    // scrollbar occupies the body's last row, so the viewport a copy covers is
    // one row shorter than the body: the clipboard must not carry a top line
    // scrolled off the view, nor a bottom line hidden under the scrollbar.
    std::deque<model::RawLogRecord> records3;
    for (int i = 0; i < 40; ++i)
        records3.push_back(
            record(model::kRawLogLevelInfo, std::to_string(i) + std::string(80, 'x'), i));
    model::RawLogRing ring3;
    ring3.records = &records3;
    ring3.arrivals = records3.size();
    views::RawLogState rl3;
    rl3.open = true;
    rl3.overlayShape = true;
    rl3.minLevel = model::kRawLogLevelTrace;
    const auto drawSurface3 = [&](const ui::Rect& region) {
        ImGui::SetNextWindowPos(ImVec2(region.x, region.y));
        ImGui::SetNextWindowSize(ImVec2(region.w, region.h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("rawlog-frame3", nullptr, kSurfaceFlags);
        views::renderRawLog(rl3, region, m, ring3);
        ImGui::End();
        ImGui::PopStyleVar();
    };

    const ui::Rect region60c = regionFor(60);
    float bodyH = -1.0f;
    bool pinned = false;
    for (int f = 0; f < 4; ++f) {
        state.backend->newFrame();
        drawSurface3(region60c);
        if (f == 3) {
            ImGuiWindow* bodyWin3 = bodyWindow();
            if (bodyWin3 != nullptr) {
                bodyH = bodyWin3->Size.y;
                pinned = std::fabs(bodyWin3->Scroll.y - bodyWin3->ScrollMax.y) <= 0.5f;
            }
        }
        state.backend->renderFrame();
    }
    check(rl3.wideMode, "premise: the copy scene runs wide");
    check(pinned, "premise: the copy scene is pinned to the bottom fold");
    check(bodyH > 0.0f, "premise: the copy scene's body is measurable");
    const std::size_t fullRows = static_cast<std::size_t>(std::ceil(bodyH / row));
    const std::size_t visibleRows =
        static_cast<std::size_t>(std::ceil(std::max(0.0f, bodyH - row) / row));
    rl3.copiedNow = true;
    state.backend->newFrame();
    drawSurface3(region60c);
    state.backend->renderFrame();
    check(rl3.windowRows == visibleRows && visibleRows + 1 == fullRows,
          "the wide copy's viewport is one scrollbar row short of the body (" +
              std::to_string(rl3.windowRows) + " of " + std::to_string(fullRows) + ")");
    const std::size_t copiedLines =
        static_cast<std::size_t>(std::count(rl3.copiedRows.begin(), rl3.copiedRows.end(), '\n'));
    check(copiedLines == visibleRows, "the clipboard carries exactly the visible rows (carried " +
                                          std::to_string(copiedLines) + ", visible " +
                                          std::to_string(visibleRows) + ")");
    // Each record copies as one line whose message ends in the record's own
    // number, so the first carried line names the bottom-most record a whole
    // viewport still shows.
    const std::size_t firstKept = records3.size() - visibleRows;
    const std::string firstMessage = std::to_string(firstKept) + std::string(80, 'x');
    check(!rl3.copiedRows.empty() && rl3.copiedRows.find(firstMessage) != std::string::npos,
          "the clipboard's top line is the first record still visible (wanted " +
              std::to_string(firstKept) + ")");

    state.backend->shutdown();
    ImGui::DestroyContext();

    std::printf("%s: %d check(s), %d failure(s); wrote %s\n", g_failures == 0 ? "PASS" : "FAIL",
                g_checks, g_failures, shot.c_str());
    return g_failures == 0 ? 0 : 1;
}
