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
        // The full 80-x probe is wider than a column: give it the display's
        // whole width so its measurement is not clipped by the column window.
        const float x1 = static_cast<int>(i) == g_probeFull ? 1270.0f : px + 440.0f;
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
    const views::RowPlan p0 = views::rawRowPlan(region100.w - 2.0f * m.pad2, m);
    check(p0.rung == 0, "100 cells sits on the widest rung");
    check(p0.msgW > 0.0f && p0.locW > 0.0f && p0.clockW > 0.0f,
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
        widthMatches(locRun, probeRun(g_probeLocWarn), "the location");
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

        // The 80-x message cannot fit its column: the drawn row must match one
        // head+ellipsis candidate, and never the whole message.
        const Run cutRun = windowRun(body, 4, x + p0.msgX, x + p0.msgX + p0.msgW);
        startsAt(cutRun, x + p0.msgX, "the truncated message");
        if (!cutRun.empty) {
            int matched = -1;
            int matchesCount = 0;
            for (int k = 0; k < 56; ++k) {
                const Run candidate = probeRun(k);
                if (candidate.empty)
                    continue;
                if (std::fabs(widthOf(cutRun) - widthOf(candidate)) <= kTol) {
                    ++matchesCount;
                    if (matched < 0)
                        matched = k;
                }
            }
            check(matchesCount == 1,
                  "the truncated message matches exactly one head+ellipsis candidate (found " +
                      std::to_string(matchesCount) + ")");
            check(matched >= 0 && matched < 56,
                  "the truncation keeps a head beside the ASCII ellipsis (kept " +
                      std::to_string(matched) + " of 80)");
            const Run fullRun = probeRun(g_probeFull);
            check(!fullRun.empty && std::fabs(widthOf(cutRun) - widthOf(fullRun)) > kTol,
                  "the message is really cut (the whole message is wider)");
        }

        // The long function name cannot fit the location column: the drawn run
        // is the mid-ellipsized form, and it starts with the ASCII marker.
        const Run locCutRun = windowRun(body, 5, x + p0.locX, x + p0.locX + p0.locW);
        startsAt(locCutRun, x + p0.locX, "the truncated location");
        widthMatches(locCutRun, probeRun(g_probeLocCut), "the truncated location");
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
    if (body != nullptr) {
        const float x = origin.x;
        const Run loc72 =
            windowRun(body, 0, x + p2.locX, std::min(x + p2.locX + p2.locW, x + p2.msgX - 4.0f));
        check(!loc72.empty && loc72.minX >= x + p2.locX - 1.0f && loc72.minX <= x + p2.locX + 3.0f,
              "the second rung still draws the location at its column");
        widthMatches(loc72, probeRun(g_probeLocPlain), "the second rung's location");
        const Run clock72 = windowRun(body, 0, x + p2.clockX, x + p2.clockX + p2.clockW);
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
    if (body != nullptr) {
        const float x = origin.x;
        const Run msg60 = windowRun(body, 0, x + p3.msgX, x + p3.msgX + p3.msgW);
        check(!msg60.empty && msg60.minX >= x + p3.msgX - 1.0f && msg60.minX <= x + p3.msgX + 3.0f,
              "the third rung's message starts at its column");
        widthMatches(msg60, probeRun(g_probeOpened), "the third rung's message");
        const Run right =
            windowRun(body, 0, x + p3.msgX + (msg60.empty ? 0.0f : widthOf(msg60)) + 2.0f, FLT_MAX);
        check(right.empty, "the third rung's row leaves the location's cells blank");
        const Run clock60 = windowRun(body, 0, x + p3.clockX, x + p3.clockX + p3.clockW);
        check(!clock60.empty, "the third rung still draws the short clock");
    }
    state.backend->renderFrame();

    state.backend->shutdown();
    ImGui::DestroyContext();

    std::printf("%s: %d check(s), %d failure(s); wrote %s\n", g_failures == 0 ? "PASS" : "FAIL",
                g_checks, g_failures, shot.c_str());
    return g_failures == 0 ? 0 : 1;
}
