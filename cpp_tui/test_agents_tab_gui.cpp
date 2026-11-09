// test_agents_tab_gui.cpp
//
// The drill-down through the real GLFW/OpenGL3 backend: the focused
// agent's transcript drawn by the same thread renderer the main thread uses,
// mounted over the agent's own `ChatTab` store and the drill-down's own
// `ui.agentThread` view state, through a second `ThreadStoreAdapter`.
//
// The behavioural half - what the rows say, the thinking toggle, the copy
// chip, follow - is the text grid's (`./test_agents_tab`, whose drill-down
// checks seed a store and assert the mounted rows). This binary answers
// the things only the real frame code and real pixels can:
//
//   1. the mount runs inside the real frame loop: after two frames the agent
//      adapter is bound, `ui.agentThread` reports drawn groups and the main
//      thread reports none (the drill-down replaced it, not overlaid it);
//   2. the mount is stable across frames (the same drawn count twice) and it
//      is the mount that drew - the adapter's own render counter moved;
//   3. `Back to main` answers a real click at its real rectangle and the main
//      thread draws again;
//   4. the eviction fallback: with the store entry evicted the paragraph draws
//      and the adapter is left unbound - never holding the evicted store's
//      address;
//   5. the screenshots (`agents-gui-main.ppm`, `agents-gui-drilldown.ppm`)
//      are the visual evidence, captured through the backend's own
//      glReadPixels.
//
// Needs a display (run under xvfb-run), exit codes: 0 = pass, 1 = a check
// failed, 2 = the backend could not start.
//
//   xvfb-run -s "-screen 0 1280x820x24" ./test_agents_tab_gui --out DIR

#include "model/ui_state.h"
#include "store/thread_store.h"
#include "tui.h"
#include "tui_backend.h"
#include "tui_settings.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/tokens.h"
#include "views/thread.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

/// The coder's store: two committed tool rows (a call and a failed result -
/// the not-ok variant is part of the drill-down's evidence) and the live
/// stream entry, the way `tuiPipelineAgentUpdate` / `...ToolCall` /
/// `...ToolResult` leave them.
tui::AgentStream coderStream() {
    tui::AgentStream stream;
    stream.agentId = "coder";
    tui::ChatMessage call = message(1, tui::ChatMessageType::ToolCall, "[2] read_file src/app.d",
                                    "read_file src/app.d");
    call.toolName = "read_file";
    call.toolArgs = "src/app.d";
    tui::ChatMessage result = message(2, tui::ChatMessageType::ToolResponse, "[3] read_file failed",
                                      "read_file: no such file");
    result.toolName = "read_file";
    result.toolResult = "read_file: no such file";
    result.toolOk = false;
    stream.chat.outputLines.push_back(call);
    stream.chat.outputLines.push_back(result);
    stream.chat.outputLineNextId = 3;
    stream.chat.streamMsg =
        message(tui::ChatTab::StreamChatMessageId, tui::ChatMessageType::Assistant, "",
                "the coder is rewriting the parser");
    stream.chat.streamMsg.thinking = "the coder is thinking about tokens";
    stream.chat.hasStreamMsg = true;
    return stream;
}

/// The fixture: a ready main thread with one committed message, two lanes and
/// the coder's store entry.
tui::TuiState fixtureState() {
    tui::TuiState state;
    state.readyStatus = false;
    state.ui.view = model::ThreadView::Ready;
    state.chat.outputLines.push_back(message(1, tui::ChatMessageType::Assistant,
                                             "[1] the main thread's own words [2026-06-18]",
                                             "the main thread's own words"));
    state.ui.agents.push_back(model::AgentLane{});
    state.ui.agents.back().id = "planner";
    state.ui.agents.back().state = model::kAgentStateDone;
    state.ui.agents.push_back(model::AgentLane{});
    state.ui.agents.back().id = "coder";
    state.ui.agents.back().state = model::kAgentStateRunning;
    state.ui.agents.back().content = "the coder's private notes";
    state.left.agents.push_back(coderStream());
    return state;
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

    tui::TuiState state = fixtureState();

    // The pixel profile the workbench loads in `tuiInit`; the harness stands in
    // for it the same way the other GUI goldens do.
    model::ShellPrefs view;
    view.fontScaleMain = 1.0f;
    view.windowWidth = 1280;
    view.windowHeight = 820;
    const std::string ini = outDir + "/agents-tab-ui.ini";
    std::remove(model::uiConfPath(ini).c_str()); // a stale profile from an earlier run
    model::saveUiConf(view, ini);
    model::loadUiConf(state.ui.prefs, ini);

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

    // The thread drawn into a surface window the harness owns: the same call the
    // shell makes, with the same two rectangles (no `Load earlier` row - the
    // agent store is capped), so the drill-down is exercised exactly where the
    // shell mounts it.
    const ui::Rect region{0.0f, 0.0f, 90.0f * m.gu, 30.0f * m.row, m.textGrid};
    const ui::Rect earlier{0.0f, 0.0f, 0.0f, 0.0f, m.textGrid};

    // The shell's mount decision (views/shell.cpp): a focused lane is the drill-down,
    // bound to the agent store found in `left.agents`; no focus draws the main thread.
    // Replicated here because this harness mounts the views directly, without the shell.
    const auto mount = [&]() {
        if (const model::AgentLane* focused =
                model::findAgentLane(state.ui, state.ui.focusedAgent)) {
            tui::AgentStream* stream = nullptr;
            for (tui::AgentStream& candidate : state.left.agents)
                if (candidate.agentId == focused->id)
                    stream = &candidate;
            if (stream != nullptr)
                state.agentThreadAdapter.bind(stream->chat, state.ui.agentThread);
            else
                state.agentThreadAdapter = llmfun::tui::store::ThreadStoreAdapter{};
            // The main mount drew nothing this frame: its window and its latches are
            // dormant while the drill-down is up.
            state.ui.thread.drawnGroups = 0;
            state.ui.thread.firstGroupId = 0;
            state.ui.thread.firstGroupY = 0.0f;
            state.ui.thread.anchorGroupId = 0;
            state.ui.thread.anchorPending = false;
            state.ui.thread.menuBlockId = {};
            views::renderAgentTranscript(state.ui, state.agentThreadAdapter, state.ui.agentThread,
                                         *focused, region, m,
                                         /*windowGroups=*/8, /*compact=*/false,
                                         /*acceptKeys=*/true);
        } else {
            views::renderThread(state.ui, state.threadAdapter, region, earlier, m,
                                /*windowGroups=*/8, /*compact=*/false, /*acceptKeys=*/true);
        }
    };

    const auto frame = [&](const std::function<void()>& inject = {}) {
        state.backend->newFrame();
        if (inject)
            inject();
        ImGui::SetNextWindowPos(ImVec2(region.x, region.y));
        ImGui::SetNextWindowSize(ImVec2(region.w, region.h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("thread-frame", nullptr, kSurfaceFlags);
        mount();
        ImGui::End();
        ImGui::PopStyleVar();
        state.backend->renderFrame();
    };
    const auto frames = [&](int count, const std::function<void()>& inject = {}) {
        for (int i = 0; i < count; ++i)
            frame(inject);
    };
    const auto capture = [&](const char* name) {
        const std::string path = outDir + "/" + name;
        if (!state.backend->captureNextFramePpm(path))
            check(false, std::string("the capture could not be written: ") + name);
        frames(1);
    };

    // 1. The main thread, before the drill-down.
    frames(2);
    check(state.ui.thread.drawnGroups >= 1, "premise: the main thread draws its group");
    capture("agents-gui-main.ppm");

    // 2. The mount: the focus replaces the main body with the agent's transcript.
    const std::size_t mountCallsBefore = state.agentThreadAdapter.renderCalls();
    model::focusAgentLane(state.ui, "coder");
    frames(2);
    check(state.agentThreadAdapter.bound(), "the mount bound the agent adapter");
    check(state.ui.agentThread.drawnGroups >= 1,
          "the drill-down drew the agent's groups (" +
              std::to_string(state.ui.agentThread.drawnGroups) + ")");
    check(state.ui.thread.drawnGroups == 0,
          "the main thread drew nothing while the drill-down is up");
    const std::size_t mountCallsAfter = state.agentThreadAdapter.renderCalls();
    check(mountCallsAfter > mountCallsBefore,
          "the agent adapter rendered the groups (its own counter moved)");
    const int drawnFirst = static_cast<int>(state.ui.agentThread.drawnGroups);
    frames(1);
    check(static_cast<int>(state.ui.agentThread.drawnGroups) == drawnFirst,
          "the mount is stable across frames (the same window drawn twice)");
    capture("agents-gui-drilldown.ppm");

    // 3. `Back to main` at its own rectangle: the first row of the region, as wide
    // as its label. A click is a press frame and a release frame at the same spot.
    const ui::ButtonSpec backSpec{"Back to main"};
    ui::Rect backRect = region;
    backRect.h = m.row;
    backRect.w = std::min(region.w, ui::measureButton(m, backSpec).w);
    const float clickX = backRect.x + std::min(4.0f * m.gu, backRect.w * 0.5f);
    const float clickY = backRect.y + backRect.h * 0.5f;
    // The injection goes through the event queue, the way the backend itself feeds
    // ImGui: `NewFrame` consumes the queue, so an event queued after one frame's
    // new-frame call is the input of the next frame - hover, press and release all
    // land where the widget code sees them.
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
    check(state.ui.focusedAgent.empty(), "`Back to main` answered the press (the focus cleared)");
    frames(2);
    check(state.ui.thread.drawnGroups >= 1, "the main thread draws again after the way back");

    // 4. The eviction fallback: the lane keeps its mirror content but the store
    // entry is evicted, so the paragraph draws and the adapter is left unbound -
    // it must never carry the evicted store's address into a later frame.
    model::focusAgentLane(state.ui, "coder");
    frames(2);
    check(state.agentThreadAdapter.bound(), "premise: the mount is up again");
    state.left.agents.clear();
    frames(1);
    check(!state.agentThreadAdapter.bound(),
          "the eviction fallback left the agent adapter unbound (no dangling store)");
    frames(1);
    check(!state.agentThreadAdapter.bound(), "and it stays unbound while the fallback draws");

    std::printf("%s: %d checks, %d failures\n", "test_agents_tab_gui", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
