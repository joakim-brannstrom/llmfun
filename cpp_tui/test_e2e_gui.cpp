// test_e2e_gui.cpp
//
// One scripted pipeline run through the whole workbench, drawn by the real
// GLFW/OpenGL3 backend: the same frame composition the app runs (`tui.cpp` -
// the root window, the thread mount, `renderShell`, the overlay layer; no key
// dispatcher, the E2E sends no chords), driven
// through the C API the D host talks to. The separate per-view binaries pin
// their own views; this one answers what only the composition can:
//
//   1. the run: two agents stream, commit per step, and trade tool
//      call/result rows in the real event order (ticks, the per-step commit,
//      then the step's tool events) - the lanes' previews stay undecorated
//      and the stores carry only structured rows;
//   2. the drill-down: focusing a lane replaces the main thread with
//      the agent's transcript mounted over its own store, the mount follows
//      the live run and shows the whole turn (tool rows included), and
//      `Back to main` hands the main thread back at its real rectangle;
//   3. the Agents tab's activity mark: a live lane lights it, the mark
//      outlives the last lane's stop for the latch window, and past the
//      deadline it drops - measured on the strip's pixels, not just the flag;
//   4. the Raw Log drawer: wide records engage wide mode in the shell's own
//      drawer, a horizontal wheel notch scrolls the wide body sideways and
//      leaves the follower armed, and a narrow surface fed short records
//      keeps today's layout (no wide mode, no horizontal scrollbar);
//   5. the store's cap holds when the events keep coming.
//
// Needs a display (run under xvfb-run), exit codes: 0 = pass, 1 = a check
// failed, 2 = the backend could not start.
//
//   xvfb-run -s "-screen 0 1280x820x24" ./test_e2e_gui [--out DIR]

#include "model/ui_state.h"
#include "store/thread_store.h"
#include "test/tui_state_handle.h"
#include "tui.h"
#include "tui_api.h"
#include "tui_backend.h"
#include "tui_settings.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/tokens.h"
#include "views/inspector.h"
#include "views/rawlog.h"
#include "views/shell.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
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

constexpr ImGuiWindowFlags kSurfaceFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;

/// One committed message of a store, the D side's own summary shape.
tui::ChatMessage message(size_t id, tui::ChatMessageType type, const std::string& summary,
                         const std::string& text) {
    tui::ChatMessage msg;
    msg.id = id;
    msg.type = type;
    msg.summary = summary;
    msg.text = text;
    return msg;
}

/// One raw record, the shape the real logger emits (the same fixture epoch the
/// raw-log harnesses pin).
model::RawLogRecord record(const std::string& messageText) {
    model::RawLogRecord out;
    out.timestampMs = 1'782'000'000'000;
    out.level = model::kRawLogLevelInfo;
    out.action = 0;
    out.line = 412;
    out.message = messageText;
    out.module = "llm.agent.pool";
    out.function = "processTurn";
    out.file = "source/llm/agent/pool.d";
    return out;
}

/// One P6 capture: the GUI has no cell grid, so a claim about what a region
/// looks like has to be a claim about the frame the backend produced.
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

/// Where two captures disagree, inside the given window: the bounding box of
/// the differing pixels and how many there are. `count == 0` means the frames
/// are identical there, and `y1 < y0` is the empty box.
struct DiffBox {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = -1;
    int count = 0;
};

DiffBox diffBox(const Ppm& a, const Ppm& b, int x0, int y0, int x1, int y1) {
    DiffBox box;
    if (a.width != b.width || a.height != b.height)
        return box;
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(a.width - 1, x1);
    y1 = std::min(a.height - 1, y1);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
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

/// The raw log body's window record, whose scroll state outlives the frame.
/// The last active match: the harness never keeps two bodies alive at once.
ImGuiWindow* bodyWindow() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* out = nullptr;
    for (ImGuiWindow* window : g.Windows)
        if (window->Active && window->Name != nullptr &&
            std::strstr(window->Name, "##rawlog_body") != nullptr)
            out = window;
    return out;
}

} // namespace

int main(int argc, char** argv) {
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

    std::setlocale(LC_ALL, "");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;

    tui::TuiState state;
    state.readyStatus = false; // the run is live: the lanes settle only when D says so

    // The pixel profile the workbench loads in `tuiInit`; the harness stands in
    // for it the same way the other GUI goldens do, with the inspector opened
    // onto the Agents tab the way a user's `ui.conf` would carry it.
    model::ShellPrefs view;
    view.fontScaleMain = 1.0f;
    view.windowWidth = 1280;
    view.windowHeight = 820;
    const std::string ini = outDir + "/e2e-ui.ini";
    std::remove(model::uiConfPath(ini).c_str()); // a stale profile from an earlier run
    model::saveUiConf(view, ini);
    model::loadUiConf(state.ui.prefs, ini);
    state.ui.prefs.inspectorOpen = 1;
    state.ui.prefs.tab = model::InspectorTab::Agents;

    std::string error;
    state.backend = tui::makeGuiBackend(state.ui.prefs);
    if (!state.backend->init(error)) {
        std::fprintf(stderr, "GUI backend init failed (no display?): %s\n", error.c_str());
        ImGui::DestroyContext();
        return 2;
    }
    tui::initMarkdownGuiMode(state);
    tui::applyUiTheme();

    const ui::Metrics m = ui::metricsFor();
    check(!m.textGrid, "premise: the pixel profile is live");
    check(m.gu > 0.0f && m.row > 0.0f, "premise: the pixel metrics resolve");

    // The frame composition, copied from `tui.cpp`: the root window spans the
    // display, the thread mounts into it, the shell draws the six regions, and
    // the overlay layer runs after the root window closed. The config writer is
    // left out - the harness owns the preferences and must not churn the file.
    views::ShellFrame lastFrame;
    const auto frame = [&](const std::function<void()>& inject = {}) {
        state.backend->newFrame();
        if (inject)
            inject();
        const ImVec2 displaySize = ui::displaySize(); // the size this frame's backend reports
        ImGuiWindowFlags parentFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(displaySize, ImGuiCond_Always);
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("##TuiRoot", nullptr, parentFlags);
        {
            state.threadAdapter.bind(state);
            state.ui.offline = state.offline;
            state.ui.view = model::deriveThreadView(
                /*hasMessages=*/state.threadAdapter.groupCount() > 0,
                /*loadingSession=*/!state.ui.rail.loadingId.empty(),
                /*offline=*/state.offline);
            lastFrame = views::renderShell(state, displaySize);
        }
        ImGui::End();
        ImGui::PopStyleVar();
        views::renderOverlays(state, lastFrame);
        state.backend->renderFrame();
    };
    const auto frames = [&](int count, const std::function<void()>& inject = {}) {
        for (int i = 0; i < count; ++i)
            frame(inject);
    };
    const auto capture = [&](const char* name) {
        const std::string path = outDir + "/" + name;
        if (!state.backend->captureNextFramePpm(path))
            check(false, std::string("the capture could not be queued: ") + name);
        frames(1);
        // `captureNextFramePpm` only queues; the write happens in the frame's
        // `renderFrame` and a failed write is only a stderr line there. Read
        // the file back so a bad --out dir fails here, at the capture, with
        // the name - not later, as a phantom diff against a missing frame.
        Ppm written;
        check(readPpm(path, written), std::string("the capture was written: ") + name);
    };
    const auto readCapture = [&](const char* name, Ppm& out) {
        check(readPpm(outDir + "/" + name, out),
              std::string("the capture can be read back: ") + name);
    };

    // The pipeline, driven the way D drives it: ticks carry the streaming
    // text, the per-step commit lands at `done`, and the step's tool events
    // follow it (the step's own result rows can arrive with the next step).
    const auto wire = [](const char* s) { return String{s, std::strlen(s)}; };
    ::TuiState api{&state};
    const auto agentUpdate = [&](const char* agent, const char* content, const char* thinking,
                                 const char* tool, std::int64_t used, double rate) {
        PipelineChatMessage msg{};
        msg.content = wire(content);
        msg.reasoning = wire(thinking);
        msg.role = wire("assistant");
        msg.currentTool = wire(tool);
        msg.contextUsed = used;
        msg.contextMax = 8192;
        msg.tokensPerSecond = rate;
        tuiPipelineAgentUpdate(&api, wire(agent), msg);
    };
    const auto agentDone = [&](const char* agent) { tuiPipelineAgentDone(&api, wire(agent)); };
    const auto agentToolCall = [&](const char* agent, const char* name, const char* args,
                                   const char* id) {
        TuiToolCallParam call{};
        call.name = wire(name);
        call.arguments = wire(args);
        call.id = wire(id);
        tuiPipelineAgentToolCall(&api, wire(agent), call);
    };
    const auto agentToolResult = [&](const char* agent, const char* name, int ok,
                                     const char* text) {
        TuiToolResultParam result{};
        result.name = wire(name);
        result.ok = ok;
        result.text = wire(text);
        tuiPipelineAgentToolResult(&api, wire(agent), result);
    };

    const auto storeOf = [&](const char* agent) -> const tui::ChatTab* {
        for (const tui::AgentStream& stream : state.left.agents)
            if (stream.agentId == agent)
                return &stream.chat;
        return nullptr;
    };
    const auto laneOf = [&](const char* agent) { return model::findAgentLane(state.ui, agent); };
    const auto previewClean = [&](const char* agent, const std::string& content) {
        const model::AgentLane* lane = laneOf(agent);
        check(lane != nullptr && lane->content == content,
              std::string(agent) + "'s preview carries the streaming text alone");
        const bool decorated =
            lane != nullptr && lane->content.find("--- Tool ---") != std::string::npos;
        check(!decorated, std::string(agent) + "'s preview carries no tool decoration");
    };

    // The premise: the main thread draws its own conversation, and the run has
    // not spoken yet (the lanes are the pipeline's, not fixtures).
    state.chat.outputLines.push_back(message(1, tui::ChatMessageType::Assistant,
                                             "[1] the main thread's own words",
                                             "the main thread's own words"));
    frames(2);
    check(state.ui.thread.drawnGroups >= 1, "premise: the main thread draws its group");
    check(state.ui.agents.empty(), "premise: no lanes before the pipeline speaks");
    check(!state.inspector.agentsActive, "premise: the Agents tab's mark is off");

    // 1. The run. The planner streams and commits; the mark lights while its
    // lane is live.
    agentUpdate("planner", "planning the split", "", "", 1200, 11.5);
    frames(1);
    agentUpdate("planner", "splitting by module", "", "", 1250, 11.8);
    frames(1);
    // The latch budget: everything between the first tick and the drop check
    // below must fit in `kAgentsActivityLatchMs` (10 s) of wall clock - this
    // run needs about one second, so the deadline stays ahead of now.
    check(state.inspector.agentsActive, "the live lane lights the Agents tab's mark");
    check(state.inspector.agentsActiveUntilMs > model::agentClockMs(),
          "the mark's deadline is ahead of now");
    capture("e2e-agents-dot-on.ppm");
    agentDone("planner");
    frames(1);
    previewClean("planner", "splitting by module");

    // The coder streams with thinking and a tool in flight; the harness drills
    // into the lane mid-run, the way a reader would, and the mount follows.
    agentUpdate("coder", "reading the parser", "thinking about tokens", "", 2400, 9.0);
    frames(1);
    agentUpdate("coder", "reading the parser end", "thinking about tokens", "read_file", 2404, 9.1);
    frames(1);
    model::focusAgentLane(state.ui, "coder");
    frames(2);
    check(state.agentThreadAdapter.bound(), "the drill-down mounted over the agent's store");
    check(state.ui.thread.drawnGroups == 0,
          "the main thread drew nothing while the drill-down is up");
    capture("e2e-drilldown-early.ppm");
    agentDone("coder"); // commits the step's prose
    agentToolCall("coder", "read_file", "src/app.d", "call-1");
    frames(1);
    {
        const tui::ChatTab* store = storeOf("coder");
        check(store != nullptr && store->outputLines.size() == 2,
              "the step's commit and the tool call are the store's first rows");
        if (store != nullptr && store->outputLines.size() == 2) {
            check(store->outputLines[0].type == tui::ChatMessageType::Assistant &&
                      store->outputLines[0].text == "reading the parser end",
                  "the committed row is the streamed prose");
            check(store->outputLines[1].type == tui::ChatMessageType::ToolCall &&
                      store->outputLines[1].toolName == "read_file" &&
                      store->outputLines[1].toolArgs == "src/app.d",
                  "the call row carries its structured fields");
            check(store->outputLines[1].summary.empty() && store->outputLines[1].text.empty(),
                  "the tool rows stay structured (no duplicated display text)");
        }
    }

    // The second step: the call is answered and a second one goes out - the
    // event order the D side emits (the result may ride with the next step).
    agentUpdate("coder", "writing the fix", "", "", 2600, 9.4);
    agentDone("coder");
    agentToolResult("coder", "read_file", 1, "12 lines");
    agentToolCall("coder", "write_file", "out.txt", "call-2");
    frames(1);
    // The third step closes the turn with a failed write.
    agentUpdate("coder", "the parser is fixed", "", "", 2800, 9.6);
    agentDone("coder");
    agentToolResult("coder", "write_file", 0, "disk full");
    frames(1);
    {
        const tui::ChatTab* store = storeOf("coder");
        check(store != nullptr && store->outputLines.size() == 7,
              "the whole turn is in the store (got " +
                  std::to_string(store ? store->outputLines.size() : 0) + " rows)");
        if (store != nullptr && store->outputLines.size() == 7) {
            const std::vector<tui::ChatMessageType> types = {
                tui::ChatMessageType::Assistant,   tui::ChatMessageType::ToolCall,
                tui::ChatMessageType::Assistant,   tui::ChatMessageType::ToolResponse,
                tui::ChatMessageType::ToolCall,    tui::ChatMessageType::Assistant,
                tui::ChatMessageType::ToolResponse};
            bool typesMatch = true;
            for (std::size_t i = 0; i < types.size(); ++i)
                typesMatch = typesMatch && store->outputLines[i].type == types[i];
            check(typesMatch, "the rows keep the run's order (prose, call, prose, result...)");
            check(store->outputLines[3].toolName == "read_file" && store->outputLines[3].toolOk &&
                      store->outputLines[3].toolResult == "12 lines",
                  "the answered call states itself ok with its result text");
            check(store->outputLines[6].toolName == "write_file" && !store->outputLines[6].toolOk &&
                      store->outputLines[6].toolResult == "disk full",
                  "the failed call states itself failed with its result text");
        }
    }
    previewClean("planner", "splitting by module");
    previewClean("coder", "the parser is fixed");
    {
        const model::AgentLane* planner = laneOf("planner");
        const model::AgentLane* coder = laneOf("coder");
        check(planner != nullptr && planner->state == model::kAgentStateDone && coder != nullptr &&
                  coder->state == model::kAgentStateDone,
              "both lanes settle done after their last step");
    }

    // 2. The drill-down shows the whole turn: the mount is bound, it drew the
    // store's groups, and the tool rows the run made are readable through the
    // same adapter the transcript draws from.
    frames(2);
    check(state.agentThreadAdapter.bound(), "the mount is still bound after the run");
    check(state.ui.agentThread.drawnGroups >= 1,
          "the drill-down drew the agent's groups (" +
              std::to_string(state.ui.agentThread.drawnGroups) + ")");
    {
        const tui::store::ThreadStoreAdapter& mount = state.agentThreadAdapter;
        struct ToolRow {
            std::string name;
            model::BlockState state;
            std::string body;
        };
        std::vector<ToolRow> rows;
        for (std::size_t g = 0; g < mount.groupCount(); ++g) {
            for (std::size_t b = 0; b < 64; ++b) {
                const model::BlockDesc desc = mount.blockAt(g, b);
                if (desc.group == 0)
                    break; // the adapter answers zeroed descriptors past the group's end
                if (desc.label == "TOOL")
                    rows.push_back(ToolRow{desc.summary, desc.state, mount.blockText(g, b)});
            }
        }
        check(rows.size() == 4, "the transcript composes the run's four tool rows (got " +
                                    std::to_string(rows.size()) + ")");
        if (rows.size() == 4) {
            check(rows[0].name == "read_file" && rows[0].state == model::BlockState::Pending &&
                      rows[0].body == "src/app.d",
                  "the call row states itself pending with its arguments");
            check(rows[1].name == "read_file" && rows[1].state == model::BlockState::Ok &&
                      rows[1].body == "12 lines",
                  "the answered row states itself ok with its result text");
            check(rows[2].name == "write_file" && rows[2].state == model::BlockState::Pending &&
                      rows[2].body == "out.txt",
                  "the second call states itself pending too");
            check(rows[3].name == "write_file" && rows[3].state == model::BlockState::Failed &&
                      rows[3].body == "disk full",
                  "the failed row states itself failed with its result text");
        }
    }
    capture("e2e-drilldown-late.ppm");
    {
        // The pixel evidence: the drill-down's column grew with the run - the
        // frames before and after the tool rows differ inside the column.
        Ppm early;
        Ppm late;
        readCapture("e2e-drilldown-early.ppm", early);
        readCapture("e2e-drilldown-late.ppm", late);
        const ui::Rect& column = lastFrame.layout.threadColumn;
        const DiffBox diff =
            diffBox(early, late, static_cast<int>(column.x), static_cast<int>(column.y),
                    static_cast<int>(column.x + column.w), static_cast<int>(column.y + column.h));
        check(diff.count > 0, "the drill-down's column changed with the run (" +
                                  std::to_string(diff.count) + " px inside the thread column)");
    }

    // 3. `Back to main` at its own rectangle: a press frame and a release frame
    // at the same spot, and the main thread draws again.
    {
        const ui::ButtonSpec backSpec{"Back to main"};
        ui::Rect backRect = lastFrame.layout.threadColumn;
        backRect.h = m.row;
        backRect.w = std::min(lastFrame.layout.threadColumn.w, ui::measureButton(m, backSpec).w);
        const float clickX = backRect.x + std::min(4.0f * m.gu, backRect.w * 0.5f);
        const float clickY = backRect.y + backRect.h * 0.5f;
        frames(1, [&] {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(clickX, clickY);
            io.AddMouseButtonEvent(0, true);
        });
        frames(1, [&] {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(clickX, clickY);
            io.AddMouseButtonEvent(0, false);
        });
        check(state.ui.focusedAgent.empty(),
              "`Back to main` answered the press (the focus cleared)");
        frames(2);
        check(state.ui.thread.drawnGroups >= 1, "the main thread draws again after the way back");
    }
    model::focusAgentLane(state.ui, "coder");
    frames(2);
    check(state.agentThreadAdapter.bound(), "the drill-down mounts again on the way forward");

    // 4. The mark outlives the run for the latch window, then drops. The
    // deadline is faked the way the grid harness does: the lanes stopped
    // seconds ago, the strip still carries the mark until the deadline passes.
    // Same budget as above: the run is ~1 s against the 10 s latch, so the
    // deadline has not expired on its own by the time this check runs.
    check(state.inspector.agentsActive, "the mark outlives the run (the latch holds)");
    check(state.inspector.agentsActiveUntilMs > model::agentClockMs(),
          "the mark's deadline is still ahead of now");
    state.inspector.agentsActiveUntilMs = model::agentClockMs() - 1;
    frames(1);
    check(!state.inspector.agentsActive, "past the deadline the mark drops");
    capture("e2e-agents-dot-off.ppm");
    {
        // The pixel evidence: the strip the mark rides changed when the mark
        // dropped, and the change sits inside the inspector's own rectangle.
        Ppm on;
        Ppm off;
        readCapture("e2e-agents-dot-on.ppm", on);
        readCapture("e2e-agents-dot-off.ppm", off);
        const ui::Rect& inspector = lastFrame.layout.inspector;
        const int stripBottom =
            static_cast<int>(inspector.y + 3.0f * m.row); // the strip's row and its padding
        const DiffBox diff =
            diffBox(on, off, static_cast<int>(inspector.x), static_cast<int>(inspector.y),
                    static_cast<int>(inspector.x + inspector.w), stripBottom);
        check(diff.count > 0, "the strip's pixels changed when the mark dropped (" +
                                  std::to_string(diff.count) + " px in the strip)");
        check(diff.x0 >= static_cast<int>(inspector.x) &&
                  diff.x1 < static_cast<int>(inspector.x + inspector.w) &&
                  diff.y0 >= static_cast<int>(inspector.y) && diff.y1 <= stripBottom,
              "the strip's change stays inside the strip's band");
    }

    // 5. The Raw Log drawer, through the shell's own tiling: wide records
    // engage wide mode, and a horizontal wheel notch scrolls sideways without
    // taking the follower.
    for (int k = 0; k < 6; ++k)
        state.logRecords.push_back(record(std::string(300, 'x')));
    state.logRecords.push_back(record("session opened"));
    state.logRecords.push_back(record("cache warmed"));
    state.ui.rawLogOpen = true;
    frames(2);
    check(state.rawLog.wideMode, "the wide records engage wide mode in the shell's drawer");
    check(!state.rawLog.overlayShape, "the pixel profile's drawer is not the modal overlay");
    {
        ImGuiWindow* body = bodyWindow();
        check(body != nullptr, "the drawer's body child is findable");
        check(body != nullptr && body->ScrollMax.x > 0,
              "the wide body carries a horizontal scrollbar (max " +
                  std::to_string(body ? body->ScrollMax.x : -1.0f) + ")");
        if (body != nullptr && body->ScrollMax.x > 0) {
            const float midX = body->Pos.x + body->Size.x * 0.5f;
            const float midY = body->Pos.y + body->Size.y * 0.5f;
            // Hover first (the wheel routes to the window under the pointer),
            // then the horizontal notches: ImGui takes a positive wheel unit
            // as "towards the origin", so the sideways read goes negative.
            frames(2, [&] {
                ImGuiIO& io = ImGui::GetIO();
                io.AddMousePosEvent(midX, midY);
            });
            frames(1, [&] {
                ImGuiIO& io = ImGui::GetIO();
                io.AddMousePosEvent(midX, midY);
                io.AddMouseWheelEvent(-6.0f, 0.0f);
            });
            frames(2);
            ImGuiWindow* scrolled = bodyWindow();
            check(scrolled != nullptr && scrolled->Scroll.x > 0,
                  "the horizontal wheel scrolled the wide body sideways (x " +
                      std::to_string(scrolled ? scrolled->Scroll.x : -1.0f) + ")");
            check(state.ui.rawLogFollow, "the sideways wheel left the follower armed");
        }
        capture("e2e-rawlog-wide.ppm");
    }
    state.ui.rawLogOpen = false;
    frames(2);

    // The narrow surface keeps today's layout: short records in a narrow
    // drawer engage no wide mode and carry no horizontal scrollbar.
    {
        std::deque<model::RawLogRecord> shortRecords;
        shortRecords.push_back(record("session opened"));
        shortRecords.push_back(record("cache warmed"));
        model::RawLogRing ring;
        ring.records = &shortRecords;
        ring.arrivals = shortRecords.size();
        views::RawLogState narrow;
        narrow.open = true;
        narrow.minLevel = model::kRawLogLevelInfo;
        const ui::Rect region{0.0f, 0.0f, 2.0f * m.pad2 + 72.0f * m.gu,
                              2.0f * m.pad1 + 30.0f * m.row, false};
        const auto narrowFrame = [&] {
            state.backend->newFrame();
            ImGui::SetNextWindowPos(ImVec2(region.x, region.y));
            ImGui::SetNextWindowSize(ImVec2(region.w, region.h));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::Begin("e2e-rawlog-narrow", nullptr, kSurfaceFlags);
            views::renderRawLog(narrow, region, m, ring);
            ImGui::End();
            ImGui::PopStyleVar();
            state.backend->renderFrame();
        };
        narrowFrame();
        narrowFrame();
        check(!narrow.wideMode, "short records keep the narrow plan (no wide mode)");
        ImGuiWindow* body = bodyWindow();
        check(body != nullptr && body->ScrollMax.x <= 0.0f,
              "the narrow body carries no horizontal scrollbar (max " +
                  std::to_string(body ? body->ScrollMax.x : -1.0f) + ")");
    }
    frames(2); // back to the shell: the surface window the harness drew is gone

    // 6. The cap holds when the events keep coming: the store keeps its newest
    // rows and drops the turn's oldest ones.
    for (int k = 0; k < 8; ++k) {
        agentToolCall("coder", "probe", "note", "probe-call");
        agentToolResult("coder", "probe", 1, "note");
    }
    frames(1);
    {
        const tui::ChatTab* store = storeOf("coder");
        check(store != nullptr && store->outputLines.size() == tui::kAgentChatMaxMessages,
              "the store capped at its limit (got " +
                  std::to_string(store ? store->outputLines.size() : 0) + ")");
        if (store != nullptr && store->outputLines.size() == tui::kAgentChatMaxMessages) {
            check(store->outputLines.front().id == 14,
                  "the cap dropped the turn's oldest rows (the first kept row is " +
                      std::to_string(store->outputLines.front().id) + ")");
            check(store->outputLines.back().type == tui::ChatMessageType::ToolResponse &&
                      store->outputLines.back().toolName == "probe",
                  "the newest result row survived the cap");
        }
        check(state.agentThreadAdapter.bound() && state.ui.agentThread.drawnGroups >= 1,
              "the drill-down still draws the capped store");
    }

    std::printf("%s: %d checks, %d failures\n", "test_e2e_gui", g_checks, g_failures);
    state.backend->shutdown();
    ImGui::DestroyContext();
    return g_failures == 0 ? 0 : 1;
}
