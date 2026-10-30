// test_agents_tab.cpp
//
// The inspector's Agents tab (views/inspector.h, design §5.21 / FR-14 / task 30): the lanes,
// the mission strip, the lane menu, stop one / stop all (with its confirmation), the
// transcript drill-down with `Back to main`, and the rail and the inspector standing at the
// same time.
//
// What this binary proves, in order:
//
//   1. the source. `views/inspector.cpp` still writes no colour literal, takes the profile
//      from the metrics, reads no display size and names no v4 type; the mirror is the C
//      ABI's three pipeline calls and nothing else;
//   2. the mirror. `tuiPipelineAgent*` copies what a lane needs into `UiState::agents` - the
//      state word, the role, the context count and the tool name as the fields D sent, and the
//      agent's stream text - and a stopped lane is left alone. Nothing is parsed out of the
//      stream's text (task 39a);
//   3. the settling. A lane still `running` when the run ends is `failed`, a lane still
//      `running` behind a stop is `stopped`, and a settled lane keeps its answer;
//   4. the tab. The mission strip, one row per lane, the action row and the empty state -
//      with the state word never dropped, however narrow the column;
//   5. the interaction. A click drills into a lane, `m`/right-click opens the lane menu, the
//      menu's `Stop lane` freezes one lane, and `Stop all` asks before it stops the run
//      through the existing stop path (`tuiRequestCancel` -> `/stop`);
//   6. the drill-down. The thread shows only that agent's transcript with `Back to main`, and
//      the main transcript comes back byte-identical;
//   7. the layout. The rail and the inspector are both up at 100x30 and at 1280x820 - the old
//      mutual exclusion is gone - and the pipeline no longer owns the left slot;
//   8. the goldens: lanes running, a failed lane, and the transcript drill-down.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = usage error.
//
//   ./test_agents_tab [-v] [--update] [--print]
//
// `--update` rewrites the fixtures; do that only for a deliberate rendering change, and commit
// the diff with it. They are captured with `LLMFUN_ASCII=0`, so a CJK locale or `TERM=linux`
// cannot change a golden.

#include "views/inspector.h"
#include "views/shell.h"

#include "tui.h"

#include "model/settings.h"
#include "model/ui_state.h"
#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/text.h"

#include "imtui/imtui.h"

#include "imgui/imgui.h"

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
namespace tui = llmfun::tui;

namespace {

int g_checks = 0;
int g_failures = 0;
bool g_verbose = false;
bool g_print = false;

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

[[noreturn]] void usage(const char* argv0, const char* problem) {
    std::fprintf(stderr, "usage: %s [-v] [--update] [--print]   (%s)\n", argv0, problem);
    std::exit(2);
}

std::string num(float v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.3f", double(v));
    std::string text = buffer;
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text;
}

std::string trimTrailingSpaces(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
    return text;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path);
    if (!in.good())
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::string readWhole(const std::string& path) {
    std::string out;
    if (!readFile(path, out))
        return std::string();
    return out;
}

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    if (!out.good())
        return false;
    out << text;
    return out.good();
}

// ---------------------------------------------------------------------------
// The cell grid the text backend writes (the `TScreen` pattern)
// ---------------------------------------------------------------------------

ImTui::TScreen g_screen;
int g_w = 100;
int g_h = 30;

std::string utf8Of(uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

struct Grid {
    int nx = 0;
    int ny = 0;
    std::vector<uint32_t> ch;
    std::vector<uint8_t> fg;
    std::vector<uint8_t> bg;

    uint32_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return ch[static_cast<std::size_t>(y) * nx + x];
    }
    uint8_t bgAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return bg[static_cast<std::size_t>(y) * nx + x];
    }
    uint8_t fgAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return fg[static_cast<std::size_t>(y) * nx + x];
    }

    std::string rowText(int y) const {
        std::string text;
        for (int x = 0; x < nx; ++x) {
            const uint32_t cp = at(x, y);
            text += cp == 0 ? std::string(" ") : utf8Of(cp);
        }
        return text;
    }

    std::string allText() const {
        std::string text;
        for (int y = 0; y < ny; ++y) {
            text += rowText(y);
            text += '\n';
        }
        return text;
    }

    bool hasText(const std::string& needle) const {
        return allText().find(needle) != std::string::npos;
    }

    int rowOf(const std::string& needle) const {
        for (int y = 0; y < ny; ++y) {
            if (rowText(y).find(needle) != std::string::npos)
                return y;
        }
        return -1;
    }

    int colOf(int y, const std::string& needle) const {
        if (y < 0 || y >= ny)
            return -1;
        const std::string row = rowText(y);
        const std::size_t at_ = row.find(needle);
        return at_ == std::string::npos ? -1 : static_cast<int>(at_);
    }

    bool find(const std::string& needle, int& x, int& y) const {
        for (int row = 0; row < ny; ++row) {
            const int col = colOf(row, needle);
            if (col >= 0) {
                x = col;
                y = row;
                return true;
            }
        }
        return false;
    }
};

Grid capture() {
    Grid grid;
    grid.nx = g_screen.nx;
    grid.ny = g_screen.ny;
    const std::size_t cells = static_cast<std::size_t>(grid.nx) * grid.ny;
    grid.ch.resize(cells);
    grid.fg.resize(cells);
    grid.bg.resize(cells);
    for (std::size_t i = 0; i < cells; ++i) {
        grid.ch[i] = g_screen.data[i].ch;
        grid.fg[i] = g_screen.data[i].fg;
        grid.bg[i] = g_screen.data[i].bg;
    }
    return grid;
}

/// One frame of the text grid at `g_w x g_h`: a decoration-free root window, the body drawing
/// inside it, then the frame through the real backend. `inject` runs *before* `NewFrame`, which
/// is when ImGui consumes its input queue.
Grid frame(const std::function<void(const ui::Metrics&)>& body,
           const std::function<void(ImGuiIO&)>& inject = nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_w), static_cast<float>(g_h));
    io.DeltaTime = 1.0f / 60.0f;
    if (inject)
        inject(io);
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("agents-frame", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    body(ui::metricsFor(true, 1.0f, 1.0f));
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    if (g_print) {
        const Grid grid = capture();
        std::printf("--- frame %dx%d\n", g_w, g_h);
        for (int y = 0; y < grid.ny; ++y)
            std::printf("%2d |%s|\n", y, grid.rowText(y).c_str());
    }
    return capture();
}

void settle(const std::function<void(const ui::Metrics&)>& body, int frames = 2) {
    for (int i = 0; i < frames; ++i)
        frame(body);
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

const ui::Metrics& gridMetrics() {
    static const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    return m;
}

/// One lane, the way the mirror leaves it.
model::AgentLane lane(const std::string& id, const std::string& state, bool active,
                      const std::string& tool = std::string(), long long tokens = 0,
                      long long elapsedMs = 0, const std::string& content = std::string()) {
    model::AgentLane out;
    out.id = id;
    out.label = id;
    out.role = "assistant";
    out.state = state;
    out.currentTool = tool;
    out.tokens = tokens;
    out.elapsedMs = elapsedMs;
    out.content = content;
    out.active = active;
    return out;
}

/// A workbench state with a run in flight and the three lanes of a `/plan` pipeline.
///
/// `readyStatus` is false on purpose: the frame settles a lane that still claims `running`
/// beside a finished run into `failed`, so a scene that wants running lanes has to be a scene
/// where a run really is in flight.
tui::TuiState pipelineState() {
    tui::TuiState state;
    state.readyStatus = false;
    state.ui.prefs.tab = model::InspectorTab::Agents;
    state.ui.agents.push_back(lane("planner", model::kAgentStateDone, false, std::string(), 1200,
                                   12'000, "planner: the work is a three-step plan"));
    state.ui.agents.push_back(lane("coder", model::kAgentStateRunning, true, "read_file", 4200,
                                   31'000, "coder: reading the parser"));
    state.ui.agents.push_back(
        lane("reviewer", model::kAgentStateQueued, false, std::string(), 0, 0));
    return state;
}

/// The shell's whole frame, plus the cell grid it painted.
views::ShellFrame shellFrame(tui::TuiState& state, int w, int h) {
    views::ShellFrame out;
    g_w = w;
    g_h = h;
    frame([&](const ui::Metrics&) {
        out = views::renderShell(state, ImVec2(static_cast<float>(w), static_cast<float>(h)));
    });
    return out;
}

views::ShellFrame shellFrameGrid(tui::TuiState& state, int w, int h, Grid& grid,
                                 const std::function<void(ImGuiIO&)>& inject = nullptr) {
    views::ShellFrame out;
    g_w = w;
    g_h = h;
    frame(
        [&](const ui::Metrics&) {
            out = views::renderShell(state, ImVec2(static_cast<float>(w), static_cast<float>(h)));
        },
        inject);
    grid = capture();
    return out;
}

void clickAt(tui::TuiState& state, int w, int h, float x, float y) {
    // A click is a sequence: the pointer move, the press and the release each take their own
    // frame (ImGui trickles one event per frame), and the release must be delivered or the
    // held button latches `ActiveId` and blocks every later hover.
    Grid scratch;
    shellFrameGrid(state, w, h, scratch, [&](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
    shellFrame(state, w, h);
    shellFrameGrid(state, w, h, scratch, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
    });
    shellFrameGrid(state, w, h, scratch, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, false);
    });
    shellFrame(state, w, h);
    shellFrame(state, w, h);
}

void rightClickAt(tui::TuiState& state, int w, int h, float x, float y) {
    // The same sequence as `clickAt()`, with the secondary button.
    Grid scratch;
    shellFrameGrid(state, w, h, scratch, [&](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
    shellFrame(state, w, h);
    shellFrameGrid(state, w, h, scratch, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(1, true);
    });
    shellFrameGrid(state, w, h, scratch, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(1, false);
    });
    shellFrame(state, w, h);
    shellFrame(state, w, h);
}

/// Park the pointer off the layout, so a later capture cannot inherit a hover the previous
/// click left behind.
void parkPointer(tui::TuiState& state, int w, int h) {
    Grid scratch;
    shellFrameGrid(state, w, h, scratch,
                   [](ImGuiIO& io) { io.AddMousePosEvent(-100.0f, -100.0f); });
    shellFrame(state, w, h);
}

/// The panel's rectangle at `w x h`, through the same `shellLayoutFor()` the frame calls.
ui::Rect inspectorRect(int w, int h, const model::ShellPrefs& prefs) {
    ui::LayoutPrefs layoutPrefs;
    layoutPrefs.inspectorOpen = prefs.inspectorOpen != 0;
    const ui::ShellLayout layout =
        views::shellLayoutFor(layoutPrefs, ImVec2(static_cast<float>(w), static_cast<float>(h)),
                              /*inspectorAuto=*/prefs.inspectorOpen < 0);
    return layout.inspector;
}

// ---------------------------------------------------------------------------
// 1. The source
// ---------------------------------------------------------------------------

void checkSource() {
    const std::string panel = readWhole(TEST_AGENTS_INSPECTOR_SOURCE);
    check(!panel.empty(), "views/inspector.cpp is readable");
    check(panel.find("tuiIsTextGrid") == std::string::npos,
          "the tab takes the profile from the metrics, never from tuiIsTextGrid()");
    check(panel.find("ImVec4(") == std::string::npos, "the tab writes no colour literal (D-THEME)");
    check(panel.find("DisplaySize") == std::string::npos,
          "the tab reads no display size (a view is a geometry taker)");
    check(panel.find("ChatMessage") == std::string::npos &&
              panel.find("AgentStream") == std::string::npos &&
              panel.find("RenderGroup") == std::string::npos,
          "the tab names no v4 type: the lanes are `model::AgentLane` values (D-ADAPTER)");
    check(panel.find("#include \"tui.h\"") == std::string::npos,
          "the panel does not include tui.h");
    {
        const std::size_t pin = panel.find("SetScrollHereY");
        check(pin == std::string::npos ||
                  panel.find("activityScrollToEnd", pin) != std::string::npos,
              "the body is not pinned to the bottom every frame (D-FOLLOW)");
    }
    check(panel.find("tuiRequestCancel") == std::string::npos,
          "the tab asks (an answer the frame applies), it never cancels the run itself");

    // The mirror is the C ABI's, and it is the *only* writer of the lane list: the three
    // pipeline calls copy into it and nothing else does.
    const std::string api = readWhole(TEST_AGENTS_API_SOURCE);
    check(!api.empty(), "tui_api.cpp is readable");
    check(api.find("mirrorLaneUpdate(*state->inner, id, role_, c, tool, ctxUsed);") !=
              std::string::npos,
          "tuiPipelineAgentUpdate mirrors into the lane list (both its branches)");
    const std::size_t updates = api.find("void tuiPipelineAgentUpdate");
    const std::size_t done = api.find("void tuiPipelineAgentDone");
    const std::size_t clear = api.find("void tuiPipelineClear");
    check(updates != std::string::npos && done != std::string::npos && clear != std::string::npos,
          "premise: the three pipeline calls are in tui_api.cpp");
    check(api.find("finishAgentLane", done) < clear, "tuiPipelineAgentDone finishes the lane");
    check(api.find("clearAgentLanes", clear) != std::string::npos,
          "tuiPipelineClear forgets the lanes (and the drill-down with them)");
    check(api.find("msg.currentTool") > updates && api.find("msg.currentTool") < done,
          "the update copies the tool name off the struct (no parse of the stream text)");

    // The model parses nothing: task 39a deleted both readers of the stream's own text, so a
    // formatting change on the D side can no longer silently zero or mislabel a row.
    const std::string model = readWhole(TEST_AGENTS_MODEL_SOURCE);
    check(!model.empty(), "model/ui_state.cpp is readable");
    check(model.find("statusTokens") == std::string::npos &&
              model.find("newestToolName") == std::string::npos,
          "neither stream parser is left in the model (task 39a)");
    check(model.find("--- Tool ---") == std::string::npos,
          "the `--- Tool ---` marker has no reader in the model any more");
}

// ---------------------------------------------------------------------------
// 2. The mirror
// ---------------------------------------------------------------------------

void checkMirror() {
    model::UiState ui;
    const long long t0 = 1'000'000;
    model::applyAgentUpdate(ui, "coder", "assistant", "hello", "", 1200, t0);
    check(ui.agents.size() == 1, "the first update registers a lane");
    const model::AgentLane& first = ui.agents.front();
    check(first.id == "coder" && first.label == "coder", "the lane is named after the agent id");
    check(first.state == model::kAgentStateRunning && first.active,
          "a lane D is streaming is running (and live)");
    check(first.role == "assistant", "the role D sent is kept");
    check(first.content == "hello",
          "the agent's stream text is copied (what the drill-down draws)");
    check(first.tokens == 1200,
          "the context count is the number D sent (" + std::to_string(first.tokens) + ")");
    check(first.elapsedMs == 0, "a fresh lane has no duration yet");

    model::applyAgentUpdate(ui, "coder", "assistant", "hello world", "read_file", 1500, t0 + 4000);
    check(ui.agents.size() == 1, "the next update updates the same lane");
    check(ui.agents.front().tokens == 1500, "...and its count moves with D's field");
    check(ui.agents.front().elapsedMs == 4000,
          "the duration is measured from the first sighting (" +
              std::to_string(ui.agents.front().elapsedMs) + " ms)");
    check(ui.agents.front().currentTool == "read_file",
          "the current tool is the field D sent, not a parse of the stream text");

    model::applyAgentUpdate(ui, "reviewer", "assistant", "", "", 0, t0 + 5000);
    check(ui.agents.size() == 2, "a second agent gets its own lane, in arrival order");
    check(ui.agents[1].state == model::kAgentStateRunning && ui.agents[1].tokens == 0,
          "and a 0 count leaves the number undrawn rather than zero-confident");

    model::finishAgentLane(ui, "coder");
    check(ui.agents[0].state == model::kAgentStateDone && !ui.agents[0].active,
          "tuiPipelineAgentDone finishes the lane");
    check(ui.agents[0].elapsedMs == 4000, "a finished lane keeps the duration it took");
    model::refreshAgentLanes(ui, t0 + 90'000);
    check(ui.agents[0].elapsedMs == 4000, "...and does not keep counting after the agent is done");
    check(ui.agents[1].elapsedMs == 85'000, "a live lane's clock does keep counting");

    // Stop one lane: it freezes, and later updates for it are ignored - the run keeps going
    // for the lane beside it.
    model::stopAgentLane(ui, "reviewer");
    check(ui.agents[1].state == model::kAgentStateStopped && !ui.agents[1].active,
          "a lane the user stopped says so, and is no longer live");
    model::applyAgentUpdate(ui, "reviewer", "assistant", "more output", "exec", 9, t0 + 120'000);
    check(ui.agents[1].state == model::kAgentStateStopped,
          "an update cannot un-stop a lane (the user's answer outranks the runtime's)");
    check(ui.agents[1].content.empty(), "...and it does not fold the late text in either");
    model::applyAgentUpdate(ui, "coder", "assistant", "late", "", 0, t0 + 130'000);
    check(ui.agents[0].state == model::kAgentStateRunning && ui.agents[0].active,
          "an update after `done` is a second turn: the lane runs again");
    model::finishAgentLane(ui, "coder");
    check(ui.agents[0].state == model::kAgentStateDone, "...and it can finish again");

    // A lane a harness built by hand has no start stamp: the frame's clock refresh must leave
    // the duration it carries alone instead of printing the whole unix epoch.
    model::UiState hand;
    hand.agents.push_back(
        lane("harness", model::kAgentStateRunning, true, std::string(), 0, 31'000));
    model::refreshAgentLanes(hand, 1'700'000'000'000LL);
    check(hand.agents[0].elapsedMs == 31'000, "a lane with no start stamp keeps its duration");

    // The lane stores what D sent, and nothing is latched: a reading replaces the previous one, and
    // a 0 is a 0 rather than "keep the last number". `contextMax` (the producer's zero-vs-none bit)
    // stops at the ABI seam, so a 0 here covers both zeros - the row draws no number either way.
    model::UiState counted;
    model::applyAgentUpdate(counted, "coder", "assistant", "x", "", 1500, 0);
    check(counted.agents[0].tokens == 1500, "a reading is stored as sent");
    model::applyAgentUpdate(counted, "coder", "assistant", "still writing", "", 0, 500);
    check(counted.agents[0].tokens == 0,
          "a 0 replaces the previous count - no latch keeps a number D stopped reporting (" +
              std::to_string(counted.agents[0].tokens) + ")");
    model::applyAgentUpdate(counted, "coder", "assistant", "again", "", 1600, 1200);
    check(counted.agents[0].tokens == 1600, "...and the next reading replaces it");

    // The lane list has the pipeline's capacity and its policy (design §"capacity:
    // existing MaxAgents = 16, FIFO (unchanged)"), so the tab can never list an agent the
    // v4 store has already dropped.
    model::UiState burst;
    model::applyAgentUpdate(burst, "oldest", "assistant", "x", "", 0, 1);
    model::focusAgentLane(burst, "oldest");
    const int crowded = static_cast<int>(model::kAgentLaneMax) + 4;
    for (int i = 0; i < crowded; ++i) {
        model::applyAgentUpdate(burst, "a" + std::to_string(i), "assistant", "x", "", 0, 1000 + i);
    }
    check(burst.agents.size() == model::kAgentLaneMax,
          "a lane burst is capped at MaxAgents (" + std::to_string(burst.agents.size()) + ")");
    check(model::findAgentLane(burst, "oldest") == nullptr,
          "the oldest lane is the one evicted (LRU by last update)");
    check(model::findAgentLane(burst, "a" + std::to_string(crowded - 1)) != nullptr,
          "and the newest lane is there");
    check(burst.focusedAgent.empty(),
          "an evicted lane takes its transcript with it (the focus is dropped)");

    // The focus names a lane, and only a lane that exists.
    model::focusAgentLane(ui, "coder");
    check(ui.focusedAgent == "coder", "a lane can be focused");
    model::focusAgentLane(ui, "nobody");
    check(ui.focusedAgent.empty(), "a lane that is not there is the main transcript");

    // Stop-and-clear: the pipeline clearing forgets the lanes *and* the drill-down.
    model::focusAgentLane(ui, "coder");
    model::clearAgentLanes(ui);
    check(ui.agents.empty() && ui.focusedAgent.empty(),
          "clearing the pipeline clears the lanes and the focus (a transcript of nothing)");

    // A session switch is the same rule (the lanes describe the run of the session left).
    model::UiState other;
    model::applyAgentUpdate(other, "coder", "assistant", "x", "", 0, t0);
    model::focusAgentLane(other, "coder");
    model::resetForSessionSwitch(other);
    check(other.agents.empty() && other.focusedAgent.empty(),
          "a session switch drops the lanes with the thread");
}

// ---------------------------------------------------------------------------
// 3. Text independence: the lane reads fields, never the stream's text
// ---------------------------------------------------------------------------

void checkTextIndependence() {
    // Two updates whose text differs wildly but whose fields agree: the row they draw is the
    // same. This is what task 39a bought - a status sentence full of numbers, and one carrying
    // none, move no lane value, because there is no parser left to be fooled by a wording change
    // (D-STATUS: numbers are the contract, the sentence is D-10's transition aid).
    model::UiState fromSentence;
    model::applyAgentUpdate(fromSentence, "coder", "assistant",
                            "hello\n--- Tool ---\nread_file\n```json\n{}\n```\n", "", 1234, 10);

    model::UiState fromPlainText;
    model::applyAgentUpdate(fromPlainText, "coder", "assistant", "hello", "", 1234, 10);

    check(fromSentence.agents[0].tokens == 1234 && fromPlainText.agents[0].tokens == 1234,
          "the count is the field, not the biggest number in the sentence");
    check(fromSentence.agents[0].currentTool.empty() && fromPlainText.agents[0].currentTool.empty(),
          "no `--- Tool ---` block is read: the tool name is the field as well");
    check(fromSentence.agents[0].tokens == fromPlainText.agents[0].tokens &&
              fromSentence.agents[0].currentTool == fromPlainText.agents[0].currentTool,
          "two updates that differ only in text draw the same row");

    // The tool name travels as sent and survives a text that carries no marker at all: the
    // drill-down's transcript and the row's `tool:` tail come from different sources, so a
    // marker-less answer cannot hide a call the agent made.
    model::UiState toolOnly;
    model::applyAgentUpdate(toolOnly, "coder", "assistant", "no marker here", "write_file", 0, 1);
    check(toolOnly.agents[0].currentTool == "write_file",
          "the tool name does not need a marker in the content to reach the row");

    // ...and a field that is empty clears it. A new response starts with no tool, and a name kept
    // from the previous one would claim the agent is still using it.
    model::applyAgentUpdate(toolOnly, "coder", "assistant", "next response", "", 0, 2);
    check(toolOnly.agents[0].currentTool.empty(),
          "an empty field clears the tool name (no stale name survives into the next response)");

    // A negative count is a sender bug, not a reading: it is stored as 0, never as a number the
    // row would have to draw.
    model::applyAgentUpdate(toolOnly, "coder", "assistant", "x", "", -5, 3);
    check(toolOnly.agents[0].tokens == 0, "a negative contextUsed clamps to 0");
}

// ---------------------------------------------------------------------------
// 4. Settling against the run
// ---------------------------------------------------------------------------

void checkSettle() {
    // A run that ended without a `done`: the lane was abandoned, so it failed.
    model::UiState ended;
    model::applyAgentUpdate(ended, "coder", "assistant", "text", "", 0, 0);
    model::settleAgentLanes(ended, /*runActive=*/false, /*cancelRequested=*/false);
    check(ended.agents[0].state == model::kAgentStateFailed && !ended.agents[0].active,
          "a lane still running when the run ended is failed");

    // A run the user stopped: every lane stopped with it, and a stop is not a failure.
    model::UiState cancelled;
    model::applyAgentUpdate(cancelled, "coder", "assistant", "text", "", 0, 0);
    model::settleAgentLanes(cancelled, /*runActive=*/false, /*cancelRequested=*/true);
    check(cancelled.agents[0].state == model::kAgentStateStopped,
          "a lane behind a stop is stopped, not failed (the user asked for it)");

    // A run still going: nothing settles.
    model::UiState live;
    model::applyAgentUpdate(live, "coder", "assistant", "text", "", 0, 0);
    model::settleAgentLanes(live, /*runActive=*/true, /*cancelRequested=*/false);
    check(live.agents[0].state == model::kAgentStateRunning, "a live lane keeps running");

    // A settled lane keeps its answer through the next run.
    model::applyAgentUpdate(ended, "planner", "assistant", "text", "", 0, 0);
    model::settleAgentLanes(ended, /*runActive=*/true, /*cancelRequested=*/false);
    check(ended.agents[0].state == model::kAgentStateFailed,
          "a failed lane is never rewritten by the next run");
}

// ---------------------------------------------------------------------------
// 5. The tab's rows
// ---------------------------------------------------------------------------

views::InspectorState openAgents() {
    views::InspectorState state;
    state.open = true;
    state.tab = model::InspectorTab::Agents;
    return state;
}

void checkTabRows() {
    const ui::Metrics& m = gridMetrics();
    model::ShellPrefs prefs;
    const ui::Rect rect = inspectorRect(100, 30, prefs);
    check(rect.w == m.inspectorW, "premise: the docked 28-cell column");

    std::vector<model::AgentLane> lanes;
    lanes.push_back(lane("planner", model::kAgentStateDone, false, std::string(), 1200, 12'000));
    lanes.push_back(lane("coder", model::kAgentStateRunning, true, "read_file", 4200, 31'000));
    lanes.push_back(lane("reviewer", model::kAgentStateQueued, false));

    g_w = 100;
    g_h = 30;
    views::InspectorParts parts;
    frame([&](const ui::Metrics& metrics) {
        views::InspectorState state = openAgents();
        parts = views::inspectorMeasure(state, lanes, rect, metrics);
    });
    check(parts.bodyRows == lanes.size(),
          "the body scrolls one row per lane (" + std::to_string(parts.bodyRows) + ")");
    check(parts.laneRows.size() == lanes.size(), "the measure builds one row per lane");
    check(!parts.mission.empty(), "the mission strip has its own row above the list");
    check(parts.missionText.find("planner") != std::string::npos &&
              parts.missionText.find("coder") != std::string::npos,
          "the strip names the lanes in order (" + parts.missionText + ")");
    check(parts.missionText.find("->") != std::string::npos,
          "and joins them with the ASCII arrow the cell renderer can draw");
    check(parts.mission.y + parts.mission.h <= parts.body.y,
          "the strip sits above the lane list, not inside it");

    // The row anatomy: the state is a glyph and a word, and the word survives the column.
    const std::string& running = parts.laneRows[1].text;
    check(running.find("running") != std::string::npos,
          "a lane row carries its state word (" + running + ")");
    check(!parts.laneRows[1].focused && parts.laneRows[1].live,
          "no lane is focused, and the running one is live");
    check(parts.laneRows[0].text.find("done") != std::string::npos, "a done lane says done");
    check(parts.laneRows[2].text.find("queued") != std::string::npos, "a queued lane says queued");

    // The action row exists while a lane is live, with `Stop all` on it.
    check(!parts.stopAll.empty() && parts.stopAllText == std::string(views::kInspectorStopAllLabel),
          "a live lane puts `Stop all` on the action row");
    check(parts.stopNo.empty(), "and the confirmation's `No` is not there until it is asked");
    check(parts.stopAll.y >= parts.body.bottom(),
          "the action row sits below the list, so it cannot scroll out of reach");

    // The confirmation is a second press on the same button, and it brings its `No`.
    views::InspectorState armed = openAgents();
    armed.stopAllArmed = true;
    frame([&](const ui::Metrics& metrics) {
        parts = views::inspectorMeasure(armed, lanes, rect, metrics);
    });
    check(parts.stopAllText == std::string(views::kInspectorStopAllQuestion),
          "an armed stop asks the question (" + parts.stopAllText + ")");
    check(!parts.stopNo.empty() &&
              parts.stopNoText == std::string(views::kInspectorStopCancelLabel),
          "and offers the way out beside it");

    // A wide region shows the whole row; the 28-cell column drops the tail, never the state.
    const ui::Rect wide{0.0f, 0.0f, 80.0f, 20.0f, true};
    frame([&](const ui::Metrics& metrics) {
        views::InspectorState state = openAgents();
        parts = views::inspectorMeasure(state, lanes, wide, metrics);
    });
    const std::string& wideRow = parts.laneRows[1].text;
    check(wideRow.find("00:31") != std::string::npos,
          "a wide row carries the clock (" + wideRow + ")");
    check(wideRow.find("4.2k tok") != std::string::npos, "...the token count");
    check(wideRow.find("tool: read_file") != std::string::npos, "...and the current tool");

    g_w = 100;
    g_h = 30;
    frame([&](const ui::Metrics& metrics) {
        views::InspectorState state = openAgents();
        parts = views::inspectorMeasure(state, lanes, rect, metrics);
    });
    check(parts.laneRows[1].text.find("tool: read_file") == std::string::npos,
          "the 28-cell row gives the tool up rather than the state");

    // The focus mark: the lane whose transcript the thread shows is the one that says so.
    views::InspectorState focused = openAgents();
    focused.focusedLane = "coder";
    frame([&](const ui::Metrics& metrics) {
        parts = views::inspectorMeasure(focused, lanes, rect, metrics);
    });
    check(parts.laneRows[1].focused && !parts.laneRows[0].focused,
          "the focused lane is the marked one");
    check(parts.laneRows[1].text.rfind(std::string(ui::icon(ui::Icon::Rest)), 0) == 0,
          "the mark is the table's rest mark, in the row's first cell (" + parts.laneRows[1].text +
              ")");

    // No lanes: the empty state, one row, no strip and no action row.
    frame([&](const ui::Metrics& metrics) {
        views::InspectorState state = openAgents();
        parts = views::inspectorMeasure(state, {}, rect, metrics);
    });
    check(parts.bodyRows == 1 && parts.laneRows.empty(),
          "the empty Agents tab is the tab's one sentence, as it was before");
    check(parts.mission.empty() && parts.stopAll.empty(),
          "no lanes: no mission strip and nothing to stop");
    check(parts.emptyText == std::string("No agents this run"), "and the sentence is the task's");

    // Nothing on the Activity tab's body comes from the lanes.
    views::InspectorState activity;
    activity.open = true;
    activity.tab = model::InspectorTab::Activity;
    frame([&](const ui::Metrics& metrics) {
        parts = views::inspectorMeasure(activity, lanes, rect, metrics);
    });
    check(parts.bodyRows == 1 && parts.laneRows.empty() && parts.mission.empty(),
          "the Activity tab draws no lane rows (its own tab is task 31's)");
}

// ---------------------------------------------------------------------------
// 6. Bounded rendering: viewport + one row, however long the lane list
// ---------------------------------------------------------------------------

void checkBounded() {
    tui::TuiState state;
    state.readyStatus = false;
    state.ui.prefs.inspectorOpen = 1;
    state.ui.prefs.tab = model::InspectorTab::Agents;
    for (int i = 0; i < 16; ++i) {
        state.ui.agents.push_back(lane("agent" + std::to_string(i), model::kAgentStateRunning, true,
                                       std::string(), 100 * (i + 1), 1000 * (i + 1)));
    }
    Grid grid;
    const views::ShellFrame out = shellFrameGrid(state, 100, 30, grid);
    const float viewportRows = out.layout.inspector.h - 2.0f;
    check(state.ui.agents.size() == 16, "premise: the fixture has 16 lanes");
    check(static_cast<float>(state.inspector.drawnRows) <= viewportRows + 1.0f,
          "the body draws the viewport plus one row of overscan, not 16 lanes (" +
              std::to_string(state.inspector.drawnRows) + " rows)");
    check(state.inspector.drawnRows >= 1, "and it does draw the lanes it has");
    check(grid.hasText("agent0"), "the list starts with the first lane");
}

// ---------------------------------------------------------------------------
// 7. The interaction: drill down, lane menu, stop one, stop all
// ---------------------------------------------------------------------------

void checkInteraction() {
    const int w = 100;
    const int h = 30;

    // --- a click on a lane drills into it ----------------------------------
    tui::TuiState state = pipelineState();
    state.ui.prefs.inspectorOpen = 1;
    Grid grid;
    shellFrameGrid(state, w, h, grid);
    check(state.inspector.tab == model::InspectorTab::Agents, "premise: the Agents tab is up");
    int rowX = -1;
    int rowY = -1;
    check(grid.find("coder |", rowX, rowY), "premise: the coder's row is on screen");
    check(rowY > 0, "premise: the row is not the strip's row");
    clickAt(state, w, h, static_cast<float>(rowX), static_cast<float>(rowY));
    check(state.ui.focusedAgent == "coder",
          "a click on a lane focuses it (" + state.ui.focusedAgent + ")");
    shellFrameGrid(state, w, h, grid);
    check(state.inspector.focusedLane == "coder", "the panel marks the lane it focused");

    // --- the lane menu: right click on the row, and its `Stop lane` --------
    tui::TuiState menu = pipelineState();
    menu.ui.prefs.inspectorOpen = 1;
    shellFrameGrid(menu, w, h, grid);
    check(grid.find("coder |", rowX, rowY), "premise: the running lane's row is on screen");
    rightClickAt(menu, w, h, static_cast<float>(rowX), static_cast<float>(rowY));
    shellFrameGrid(menu, w, h, grid);
    check(menu.inspector.menuLaneId == "coder",
          "a right click opens the row menu on that row (" + menu.inspector.menuLaneId + ")");
    int menuX = -1;
    int menuY = -1;
    check(grid.find(std::string(views::kInspectorMenuStopLane), menuX, menuY),
          "the menu offers `Stop lane`");
    check(grid.hasText(views::kInspectorMenuStopAll), "...and `Stop every agent...`");
    clickAt(menu, w, h, static_cast<float>(menuX), static_cast<float>(menuY));
    // The answer is applied in the frame that produced it (`applyInspectorAnswers`), so the
    // lane is already stopped by the time the click returns - the effect is the assertion.
    const model::AgentLane* stoppedLane = model::findAgentLane(menu.ui, "coder");
    check(stoppedLane != nullptr && stoppedLane->state == model::kAgentStateStopped,
          "the lane stops");
    check(model::findAgentLane(menu.ui, "planner")->state == model::kAgentStateDone,
          "and the lanes beside it are untouched (a done lane stays done)");

    // A lane that is not live has nothing to stop: its menu row is disabled (`MenuItem`'s
    // fourth argument), so a press there cannot claim it stopped something.
    tui::TuiState queued = pipelineState();
    queued.ui.prefs.inspectorOpen = 1;
    shellFrameGrid(queued, w, h, grid);
    check(grid.find("reviewer |", rowX, rowY), "premise: the queued lane's row is on screen");
    rightClickAt(queued, w, h, static_cast<float>(rowX), static_cast<float>(rowY));
    shellFrameGrid(queued, w, h, grid);
    check(queued.inspector.menuLaneId == "reviewer", "premise: its menu is open");
    int stopLaneX = -1;
    int stopLaneY = -1;
    check(grid.find(std::string(views::kInspectorMenuStopLane), stopLaneX, stopLaneY),
          "premise: the menu's `Stop lane` is on screen");
    clickAt(queued, w, h, static_cast<float>(stopLaneX), static_cast<float>(stopLaneY));
    shellFrame(queued, w, h);
    const model::AgentLane* queuedLane = model::findAgentLane(queued.ui, "reviewer");
    check(queuedLane != nullptr && queuedLane->state == model::kAgentStateQueued,
          "a queued lane has nothing to stop: its menu row does nothing");
    check(!queued.cancelRequested, "...and nothing was stopped on its behalf");

    // --- the `m` key: the panel's own key while no field owns the keyboard --
    //
    // The full shell cannot test this path: the composer's field holds the keyboard, and
    // `ui::takeCharacter()` deliberately refuses then (D-KEYMAP's plain-key rule - an `m`
    // typed into the composer must stay typing). So the key is taken where the field is not:
    // the panel drawn alone, which is also how `./test_inspector` exercises its `[`/`]` pair.
    {
        tui::TuiState panelState;
        panelState.ui.prefs.tab = model::InspectorTab::Agents;
        const ui::Rect panel = inspectorRect(w, h, panelState.ui.prefs);
        panelState.ui.agents = pipelineState().ui.agents;
        panelState.inspector = openAgents();
        g_w = w;
        g_h = h;
        Grid panelGrid;
        const auto body = [&](const ui::Metrics& metrics) {
            views::renderInspector(panelState.inspector, panelState.ui.agents, panel, metrics,
                                   /*acceptKeys=*/true);
        };
        panelGrid = frame(body);
        int laneX = -1;
        int laneY = -1;
        check(panelGrid.find("planner |", laneX, laneY), "premise: the lane row is on screen");
        frame(body, [&](ImGuiIO& io) { io.AddMousePosEvent(laneX + 0.5f, laneY + 0.5f); });
        frame(body, [&](ImGuiIO& io) {
            io.AddMousePosEvent(laneX + 0.5f, laneY + 0.5f);
            io.AddInputCharactersUTF8("m");
        });
        check(panelState.inspector.menuLaneId == "planner",
              "`m` opens the menu on the row under the pointer (" +
                  panelState.inspector.menuLaneId + ")");
    }

    // ...and in the full shell it stays the field's, which is the rule that keeps typing
    // typing (the reason the lane menu also has a right click).
    {
        tui::TuiState typed = pipelineState();
        typed.ui.prefs.inspectorOpen = 1;
        shellFrame(typed, w, h);
        shellFrame(typed, w, h);
        check(typed.composer.focused, "premise: the composer's field holds the keyboard");
        {
            Grid typedGrid;
            shellFrameGrid(typed, w, h, typedGrid,
                           [](ImGuiIO& io) { io.AddInputCharactersUTF8("m"); });
        }
        check(typed.inspector.menuLaneId.empty(),
              "an `m` typed while the field is focused does not open a lane menu");
    }

    // --- the arming does not outlive the question it asks -----------------
    //
    // The action row (the question and its `No`) exists only while a lane is live, so an
    // arming left over from the previous run would make the *next* run's action row be born
    // asking, turning a two-press destructive action into one press nobody asked for.
    tui::TuiState stale = pipelineState();
    stale.ui.prefs.inspectorOpen = 1;
    shellFrame(stale, w, h);
    stale.inspector.stopAllArmed = true; // what the first press leaves behind
    stale.readyStatus = true;            // the run finishes by itself
    shellFrame(stale, w, h);
    check(!stale.inspector.stopAllArmed, "an arming with no question left on screen is dropped");

    stale.readyStatus = false; // the next run starts
    stale.ui.agents.push_back(lane("second", model::kAgentStateRunning, true));
    shellFrameGrid(stale, w, h, grid);
    int nextX = -1;
    int nextY = -1;
    check(grid.find(views::kInspectorStopAllLabel, nextX, nextY),
          "the next run's action row says `Stop all` again");
    check(!grid.hasText(views::kInspectorStopAllQuestion), "...not the question nobody pressed");
    clickAt(stale, w, h, static_cast<float>(nextX), static_cast<float>(nextY));
    check(stale.inspector.stopAllArmed && !stale.cancelRequested,
          "so its first press only arms it (two presses are still required)");

    // --- Stop all: the confirmation, then the run's own stop path ---------
    tui::TuiState stopAll = pipelineState();
    stopAll.ui.prefs.inspectorOpen = 1;
    shellFrameGrid(stopAll, w, h, grid);
    int stopX = -1;
    int stopY = -1;
    check(grid.find(views::kInspectorStopAllLabel, stopX, stopY),
          "premise: `Stop all` is on screen");
    clickAt(stopAll, w, h, static_cast<float>(stopX), static_cast<float>(stopY));
    check(stopAll.inspector.stopAllArmed, "the first press only arms the question");
    check(!stopAll.cancelRequested, "...and the run is still going");
    check(tui::cancelStepOpen(stopAll), "premise: a run is in flight");
    shellFrameGrid(stopAll, w, h, grid);
    check(grid.hasText(views::kInspectorStopAllQuestion),
          "the question is drawn where the button was");
    check(grid.find(views::kInspectorStopAllQuestion, stopX, stopY), "premise: on the same row");
    check(grid.hasText(views::kInspectorStopCancelLabel), "and the `No` beside it");
    clickAt(stopAll, w, h, static_cast<float>(stopX), static_cast<float>(stopY));
    check(stopAll.cancelRequested,
          "the second press stops the run through the existing stop path (tuiRequestCancel)");
    check(!stopAll.inspector.stopAllArmed, "and the question is answered");
    shellFrame(stopAll, w, h);
    check(model::findAgentLane(stopAll.ui, "coder")->state == model::kAgentStateStopped,
          "every live lane stops with the run");
    check(tui::tuiTakeCancel(stopAll), "the request is the one D's frame loop consumes (D-CANCEL)");

    // --- No: the question is dropped, and nothing stops -------------------
    tui::TuiState abort = pipelineState();
    abort.ui.prefs.inspectorOpen = 1;
    shellFrameGrid(abort, w, h, grid);
    check(grid.find(views::kInspectorStopAllLabel, stopX, stopY),
          "premise: `Stop all` is on screen");
    clickAt(abort, w, h, static_cast<float>(stopX), static_cast<float>(stopY));
    shellFrameGrid(abort, w, h, grid);
    check(grid.find(views::kInspectorStopAllQuestion, stopX, stopY),
          "premise: the question is on the stop button's row");
    const int noX = grid.colOf(stopY, views::kInspectorStopCancelLabel);
    const int noY = stopY;
    check(noX >= 0, "premise: `No` is on the confirmation row");
    clickAt(abort, w, h, static_cast<float>(noX), static_cast<float>(noY));
    check(!abort.inspector.stopAllArmed && !abort.cancelRequested,
          "`No` drops the question and leaves the run alone");

    // --- the menu arms the same question ----------------------------------
    tui::TuiState fromMenu = pipelineState();
    fromMenu.ui.prefs.inspectorOpen = 1;
    shellFrameGrid(fromMenu, w, h, grid);
    check(grid.find("coder |", rowX, rowY), "premise: the coder's row is on screen");
    rightClickAt(fromMenu, w, h, static_cast<float>(rowX), static_cast<float>(rowY));
    shellFrameGrid(fromMenu, w, h, grid);
    int armX = -1;
    int armY = -1;
    check(grid.find(views::kInspectorMenuStopAll, armX, armY),
          "premise: the menu offers `Stop every agent...`");
    clickAt(fromMenu, w, h, static_cast<float>(armX), static_cast<float>(armY));
    check(fromMenu.inspector.stopAllArmed, "the menu row arms the same confirmation");
    check(!fromMenu.cancelRequested, "...and stops nothing by itself");
}

// ---------------------------------------------------------------------------
// 8. The drill-down: the thread is that agent's transcript, and back is back
// ---------------------------------------------------------------------------

/// A state with one stored message (the main transcript) and one lane with its own text.
tui::TuiState drillState() {
    tui::TuiState state;
    state.readyStatus = false;
    state.ui.prefs.inspectorOpen = 1;
    state.ui.prefs.tab = model::InspectorTab::Agents;
    tui::ChatMessage msg;
    msg.id = 1;
    msg.type = tui::ChatMessageType::Assistant;
    msg.summary = "[1] the main thread's own words [2026-06-18]";
    msg.text = "the main thread's own words";
    state.chat.outputLines.push_back(msg);
    state.ui.view = model::ThreadView::Ready;
    state.ui.agents.push_back(lane("coder", model::kAgentStateRunning, true, "read_file", 4200,
                                   31'000, "the coder's private notes"));
    return state;
}

void checkDrilldown() {
    const int w = 100;
    const int h = 30;
    tui::TuiState state = drillState();
    Grid mainGrid;
    shellFrameGrid(state, w, h, mainGrid);
    check(mainGrid.hasText("the main thread's own words"),
          "premise: the thread draws the main transcript");
    check(!mainGrid.hasText("Back to main"), "premise: no drill-down is up");
    const std::string mainText = mainGrid.allText();

    // Drill in: the thread shows only that agent's transcript, and says how to get back.
    model::focusAgentLane(state.ui, "coder");
    Grid agentGrid;
    shellFrameGrid(state, w, h, agentGrid);
    check(agentGrid.hasText("Back to main"), "the drill-down offers `Back to main`");
    // ...and it says which agent it is filtered to, on the same row (a header, not a control):
    // the row that carries the way back is the one that names the lane.
    const int backRow = agentGrid.rowOf("Back to main");
    check(backRow >= 0 && agentGrid.colOf(backRow, "coder") > agentGrid.colOf(backRow, "Back"),
          "the row back also names the lane (" + (backRow >= 0 ? agentGrid.rowText(backRow) : "") +
              ")");
    check(agentGrid.hasText("the coder's private notes"), "and draws that agent's transcript");
    check(!agentGrid.hasText("the main thread's own words"),
          "showing only that agent's transcript (the main one is filtered out)");
    check(state.inspector.focusedLane == "coder", "the panel marks the lane the thread shows");

    // Back: the main transcript comes back exactly as it was.
    int backX = -1;
    int backY = -1;
    check(agentGrid.find("Back to main", backX, backY), "premise: the way back is on screen");
    clickAt(state, w, h, static_cast<float>(backX), static_cast<float>(backY));
    check(state.ui.focusedAgent.empty(), "`Back to main` leaves the drill-down");
    parkPointer(state, w, h);
    Grid restored;
    shellFrameGrid(state, w, h, restored);
    check(restored.allText() == mainText,
          "the main transcript is restored unchanged (the same cells, not a reload)");

    // A lane that is cleared away takes the drill-down with it: the focus cannot point at a
    // transcript that no longer exists.
    model::focusAgentLane(state.ui, "coder");
    shellFrame(state, w, h);
    model::clearAgentLanes(state.ui);
    Grid cleared;
    shellFrameGrid(state, w, h, cleared);
    check(state.ui.focusedAgent.empty() && !cleared.hasText("Back to main"),
          "a cleared pipeline takes the drill-down with it");
}

// ---------------------------------------------------------------------------
// 9. The rail and the inspector, standing at the same time
// ---------------------------------------------------------------------------

void checkRailAndInspector() {
    tui::TuiState state = pipelineState();
    Grid grid;
    const views::ShellFrame frameOut = shellFrameGrid(state, 100, 30, grid);
    check(frameOut.layout.rail.w > 0.0f, "the rail has its column at 100x30");
    check(frameOut.layout.inspector.w > 0.0f, "and the inspector has its own, at the same time");
    check(frameOut.layout.rail.right() <= frameOut.layout.thread.x,
          "the rail does not cover the thread");
    check(frameOut.layout.inspector.x >= frameOut.layout.thread.right(),
          "and the inspector sits on the other side of it");
    check(state.inspector.open && !state.inspector.overlay,
          "the panel is the docked column, not an overlay");
    check(grid.hasText("[5] Agents"), "the tab strip is on screen");
    check(grid.hasText("Search"), "premise: the rail's search field is on screen too");

    // The pipeline does not own the left slot: the rail keeps it (task 30).
    tui::TuiState leftSlot = pipelineState();
    leftSlot.left.agents.emplace_back();
    const views::ShellFrame railOwns = shellFrameGrid(leftSlot, 100, 30, grid);
    check(railOwns.layout.rail.w > 0.0f, "the rail keeps its column while the pipeline has agents");
    check(grid.hasText("Search"), "premise: the rail's search field is what draws that column");

    // 1280x820 px: the wide breakpoint, where the design asks for both columns.
    const ui::Metrics px = ui::metricsFor(false, 1.0f, 1.0f);
    ui::LayoutPrefs prefs;
    prefs.railOpen = true;
    prefs.inspectorOpen = true;
    const ui::ShellLayout wide = ui::computeShell(px, ImVec2(1280.0f, 820.0f), prefs);
    check(wide.rail.w > 0.0f && wide.inspector.w > 0.0f,
          "at 1280x820 the rail and the inspector are both part of the tiling");
    check(wide.inspector.w == px.inspectorW && wide.rail.w == px.railW,
          "...at the metrics' own column widths");
}

// ---------------------------------------------------------------------------
// 10. The goldens
// ---------------------------------------------------------------------------

struct Scene {
    const char* name;
    int w;
    int h;
    /// How the scene's lanes look: 0 = a running pipeline, 1 = a failed lane, 2 = a drill-down.
    int layout;
};

std::vector<Scene> scenes() {
    return {
        {"inspector-agents-lanes", 100, 30, 0},
        {"inspector-agents-failed", 100, 30, 1},
        {"inspector-agents-drilldown", 100, 30, 2},
    };
}

std::string goldenText(const Scene& scene) {
    tui::TuiState state = pipelineState();
    state.ui.prefs.inspectorOpen = 1;
    if (scene.layout == 1) {
        // The run ended without the reviewer's `done`: the lane settles into `failed`
        // (`model::settleAgentLanes`), which is what a failed lane really is.
        state.ui.agents[2] = lane("reviewer", model::kAgentStateRunning, true, "exec", 900, 5000,
                                  "reviewer: the parser does not compile");
        state.readyStatus = true;
        state.ui.agents[2].state = model::kAgentStateRunning; // the settle below is the producer
    }
    if (scene.layout == 2) {
        state.ui.focusedAgent = "coder";
        state.ui.view = model::ThreadView::Ready;
    }

    // Two frames, and the second is captured: every harness shares one ImGui context, so a
    // single frame would inherit whatever active id / nav request the checks above left
    // behind. The scene's own first frame consumes the composer's focus request.
    shellFrame(state, scene.w, scene.h);
    const views::ShellFrame frameOut = shellFrame(state, scene.w, scene.h);
    const Grid grid = capture();
    const ui::Metrics& m = gridMetrics();
    const views::InspectorParts parts =
        views::inspectorMeasure(state.inspector, state.ui.agents, frameOut.layout.inspector, m);

    std::string text = "name: ";
    text += scene.name;
    text += "\nscreen: " + std::to_string(scene.w) + "x" + std::to_string(scene.h);
    text += "\nrail: x=" + num(frameOut.layout.rail.x) + " w=" + num(frameOut.layout.rail.w);
    text += " thread: x=" + num(frameOut.layout.thread.x) + " w=" + num(frameOut.layout.thread.w);
    text += " inspector: x=" + num(frameOut.layout.inspector.x) +
            " w=" + num(frameOut.layout.inspector.w);
    text += "\nopen: " + std::string(state.inspector.open ? "1" : "0");
    text += " tab: " + std::string(views::inspectorTabName(state.inspector.tab));
    text += " focused: '" + state.ui.focusedAgent + "'";
    text += "\nlanes: " + std::to_string(parts.laneRows.size()) +
            " rows=" + std::to_string(parts.bodyRows) +
            " drawn=" + std::to_string(state.inspector.drawnRows);
    text += "\nmission: '" + parts.missionText + "'";
    text += "\naction: '" + parts.stopAllText + "'";
    for (const views::InspectorParts::LaneRow& row : parts.laneRows)
        text += "\nlane: '" + row.text + "'" + (row.focused ? " (focused)" : "");
    text += "\ntext:\n";
    for (int y = 0; y < grid.ny; ++y) {
        text += trimTrailingSpaces(grid.rowText(y));
        text += '\n';
    }
    text += "fills:\n";
    for (int y = 0; y < grid.ny; ++y) {
        std::string runs;
        int start = -1;
        int index = -1;
        for (int x = 0; x <= grid.nx; ++x) {
            const int value = x < grid.nx ? static_cast<int>(grid.bgAt(x, y)) : -1;
            const bool same = start >= 0 && value == index;
            if (start >= 0 && !same) {
                runs += (runs.empty() ? "" : ", ") + std::to_string(start) + "-" +
                        std::to_string(x - 1) + "=" + std::to_string(index);
                start = -1;
            }
            if (value > 0 && start < 0) {
                start = x;
                index = value;
            } else if (value <= 0) {
                start = -1;
            }
        }
        text += std::to_string(y) + ": " + runs + "\n";
    }
    return text;
}

void checkGoldens(bool update) {
    const std::string dir = TEST_AGENTS_GOLDEN_DIR;
    for (const Scene& scene : scenes()) {
        const std::string path = dir + "/" + scene.name + "-" + std::to_string(scene.w) + "x" +
                                 std::to_string(scene.h) + ".txt";
        const std::string text = goldenText(scene);
        if (update) {
            check(writeFile(path, text), "golden written: " + path);
            continue;
        }
        std::string expected;
        if (!readFile(path, expected)) {
            check(false, "golden is readable: " + path + " (run with --update to create it)");
            continue;
        }
        if (expected == text) {
            check(true, std::string("the ") + scene.name + " golden matches");
            continue;
        }
        std::size_t line = 1;
        std::size_t at_ = 0;
        while (at_ < expected.size() && at_ < text.size() && expected[at_] == text[at_]) {
            line += expected[at_] == '\n' ? 1 : 0;
            ++at_;
        }
        check(false, std::string("the ") + scene.name +
                         " golden matches (first difference on line " + std::to_string(line) +
                         ": " + path + ")");
    }
}

} // namespace

int main(int argc, char** argv) {
    bool update = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0)
            g_verbose = true;
        else if (std::strcmp(argv[i], "--update") == 0)
            update = true;
        else if (std::strcmp(argv[i], "--print") == 0)
            g_print = true;
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
            usage(argv[0], "print every check with -v, rewrite the goldens with --update, "
                           "dump the grids with --print");
        else
            usage(argv[0], "unknown argument");
    }

    // One glyph set for every frame, whatever terminal runs this: `LLMFUN_ASCII=0` outranks
    // TERM and the locale in the detection order, so a golden cannot depend on the environment.
    ::setenv("LLMFUN_ASCII", "0", 1);

    ImGui::CreateContext();
    ImTui_ImplText_Init();

    checkSource();
    checkMirror();
    checkTextIndependence();
    checkSettle();
    checkTabRows();
    checkBounded();
    checkInteraction();
    checkDrilldown();
    checkRailAndInspector();
    checkGoldens(update);

    ImGui::DestroyContext();

    if (g_failures == 0) {
        std::printf("test_agents_tab: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "test_agents_tab: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
