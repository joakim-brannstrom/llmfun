// test_clear_output_state.cpp
//
// Headless regression test for the widened clear contract: tuiClearOutput / the C-API
// tuiClearChatMessages must reset ALL derived chat-display state, not just outputLines:
//
//   - outputLineOpen (the set of collapsed header-group positions): stale positions survived a
//   session switch, so replayed messages at positions that were open in the previous session
//   rendered without their header labels;
//   - the in-flight stream message (hasStreamMsg/streamMsg): a stream bubble captured at switch
//   time lingered at the bottom of the chat.
//
// Scenarios 1-4 drive a REAL llmfun::tui::TuiState through the same functions the C-API layer
// forwards to (tuiClearOutput / tuiStreamChatMessageClear) and assert on the C++ state (same style
// as test_session_filter_smoke's C++ state assertions). Scenario 6 goes end-to-end through the
// actual C API (tuiCreateState → tuiUpdateStreamChatMessage → tuiAddChatMessage →
// tuiClearChatMessages → tuiDestroyState) — exactly the session-switch path (activateSession →
// uiMsg.clearChat() → tuiClearChatMessages → tuiClearOutput
// ) — and asserts the reset through the
// handle (same white-box completion of the C API's opaque TuiState as test_tui_maxwidth.cpp). No
// terminal, no ImGui context, no rendering — pure state-level assertions.
//
// Also checks that outputLineNextId is NOT reset (monotonic per-message id, uniqueness
// only), TUI_API_VERSION unchanged, and the auto-scroll flag is unaffected by the clear.
//
// Exit codes: 0 = all scenarios pass; 1 = first assertion failure.
//
// Headless (no backend init needed) — run from the build dir: ./test_clear_output_state

#include "tui.h"     // llmfun::tui: TuiState, tuiAddOutputLine, tuiClearOutput, ...
#include "tui_api.h" // C API: tuiCreateState/tuiClearChatMessages/... (null-safety)

#include <cstdio>
#include <cstdlib>
#include <string>

// Legal completion of the C API's forward declaration
// `typedef struct TuiState TuiState;` (tui_api.h) — layout matches
// tui_api.cpp exactly. Lets the test read back the core state the C API
// wrapped, i.e. the white-box observation point for Scenario 6's switch-path
// assert (same pattern as test_tui_maxwidth.cpp).
struct TuiState {
    ::llmfun::tui::TuiState* inner;
};

// NOTE: no `using namespace llmfun::tui;` — it would make `TuiState`
// ambiguous against the C API's global typedef. Everything
// C++-side is qualified, exactly like test_tui_maxwidth.cpp.

namespace {

int g_scenario = 0;
const char* g_phase = "startup";

void fail(const std::string& what) {
    std::fprintf(stderr, "FAIL [scenario %d — %s]: %s\n", g_scenario, g_phase, what.c_str());
    std::exit(1);
}

void expect(bool cond, const std::string& what) {
    if (!cond)
        fail(what);
}

void phase(const char* what) { g_phase = what; }

// Same id convention as the C-API layer: pass the
// counter's current value, then advance it (post-increment).
llmfun::tui::ChatMessage makeMsg(const char* text, size_t id) {
    return llmfun::tui::ChatMessage{"summary", text, "", llmfun::tui::ChatMessageType::User, id};
}

// — Scenario 1: stale open-group positions are reset by the clear.
void scenario1_openGroupReset() {
    g_scenario = 1;
    phase("stale outputLineOpen positions suppressed header labels after clear");

    llmfun::tui::TuiState state;

    // Previous session: two messages, then the user collapses the header
    // group at position 0 (what the render loop does on a closed
    // CollapsingHeader / AssistantWork group).
    llmfun::tui::tuiAddOutputLine(state, makeMsg("first", state.chat.outputLineNextId++));
    llmfun::tui::tuiAddOutputLine(state, makeMsg("second", state.chat.outputLineNextId++));
    state.chat.outputLineOpen.insert(0);
    expect(state.chat.outputLineOpen.count(0) == 1,
           "precondition: position 0 recorded as collapsed group");

    // Session switch: the D side calls uiMsg.clearChat() → C-API
    // tuiClearChatMessages → tuiClearOutput. The fixed behavior must reset
    // the open-group set together with the lines.
    llmfun::tui::tuiClearOutput(state);
    expect(state.chat.outputLines.empty(), "output lines cleared");
    expect(state.chat.outputLineOpen.empty(), "open-group positions reset with the lines (fix)");

    // New session: replayed messages must ALL render with their header
    // labels (showHeader == outputLineOpen.count(i) == 0 per position).
    llmfun::tui::tuiAddOutputLine(state, makeMsg("replayed-a", state.chat.outputLineNextId++));
    llmfun::tui::tuiAddOutputLine(state, makeMsg("replayed-b", state.chat.outputLineNextId++));
    llmfun::tui::tuiAddOutputLine(state, makeMsg("replayed-c", state.chat.outputLineNextId++));
    for (size_t i = 0; i < state.chat.outputLines.size(); ++i)
        expect(state.chat.outputLineOpen.count(i) == 0, "replayed message at position " +
                                                            std::to_string(i) +
                                                            " renders with its header label");
}

// — Scenario 2: the stream bubble does not survive a clear.
void scenario2_streamBubbleCleared() {
    g_scenario = 2;
    phase("in-flight stream message cleared with the output area");

    llmfun::tui::TuiState state;
    llmfun::tui::tuiUpdateStreamChatMessage(
        state, llmfun::tui::ChatMessage{"", "streaming...", "thinking",
                                        llmfun::tui::ChatMessageType::Assistant,
                                        llmfun::tui::ChatTab::StreamChatMessageId});
    expect(state.chat.hasStreamMsg, "precondition: stream bubble displayed");
    expect(state.chat.streamMsg.text == "streaming...", "precondition: stream content captured");

    llmfun::tui::tuiClearOutput(state); // session switch
    expect(!state.chat.hasStreamMsg,
           "stream bubble from the previous session does not remain after a switch");
    expect(state.chat.streamMsg.text.empty(), "stream message storage reset");
    expect(state.chat.streamMsg.id == llmfun::tui::ChatTab::StreamChatMessageId,
           "stream message id reset to the stream sentinel");
}

// — Scenario 3: outputLineNextId is NOT reset (monotonic, uniqueness only).
void scenario3_nextIdMonotonic() {
    g_scenario = 3;
    phase("outputLineNextId not reset by the clear");

    llmfun::tui::TuiState state;
    llmfun::tui::tuiAddOutputLine(state, makeMsg("m1", state.chat.outputLineNextId++));
    const size_t lastId = state.chat.outputLines.back().id;
    llmfun::tui::tuiClearOutput(state);
    expect(state.chat.outputLineNextId == lastId + 1,
           "id counter preserved exactly by the clear (not reset to its "
           "initial value 1: one message used id 1, counter now 2)");
    expect(state.chat.outputLineNextId > lastId, "counter stays past the last used id");
    llmfun::tui::tuiAddOutputLine(state, makeMsg("m2", state.chat.outputLineNextId++));
    expect(state.chat.outputLines.back().id > lastId,
           "new message id is strictly greater than any pre-clear id (unique)");
}

// — Scenario 4: auto-scroll flag unaffected + TUI_API_VERSION unchanged.
void scenario4_scrollAndApiVersion() {
    g_scenario = 4;
    phase("auto-scroll flag and TUI_API_VERSION unaffected");

    llmfun::tui::TuiState state;
    state.autoScroll = false;
    llmfun::tui::tuiClearOutput(state);
    expect(state.autoScroll == false, "auto-scroll off stays off across a clear");
    state.autoScroll = true;
    llmfun::tui::tuiClearOutput(state);
    expect(state.autoScroll == true, "auto-scroll on stays on across a clear");

    // Documentation-only marker: the widened clear contract must not bump it.
    static_assert(TUI_API_VERSION == 3,
                  "TUI_API_VERSION must not change for the widened clear contract");
}

// — Scenario 5: C-API null-safety preserved.
void scenario5_cApiNullSafety() {
    g_scenario = 5;
    phase("C-API clear functions are no-ops on NULL state");

    ChatMessageParam param{};                   // zero-initialized: all String data NULL
    tuiClearChatMessages(nullptr);              // must not crash
    tuiStreamChatMessageClear(nullptr);         // must not crash
    tuiUpdateStreamChatMessage(nullptr, param); // must not crash
}

// — Scenario 6: end-to-end through the real C-API session-switch path.
void scenario6_cApiSwitchPath() {
    g_scenario = 6;
    phase("C API switch path: tuiClearChatMessages reaches the fixed tuiClearOutput");

    TuiState* st = tuiCreateState();
    expect(st != nullptr && st->inner != nullptr, "tuiCreateState works headless");

    // Capture a stream bubble (as at switch time), then append two lines so a stale open-group
    // position could collide with the replayed session.
    ChatMessageParam stream{};
    stream.text = String_NewBuf("partial stream", 14);
    stream.type = TuiChatMessageType_Assistant;
    tuiUpdateStreamChatMessage(st, stream);

    ChatMessageParam line{};
    line.summary = String_New("User");
    line.text = String_New("hello");
    line.type = TuiChatMessageType_User;
    tuiAddChatMessage(st, line);
    tuiAddChatMessage(st, line);

    // The switch-path clear (exactly what tuiClearChatMessages forwards to) must run without a
    // terminal/ImGui context and leave the state self-consistent.
    tuiClearChatMessages(st);
    expect(st->inner->chat.outputLines.empty(), "switch path: output lines cleared");
    expect(st->inner->chat.outputLineOpen.empty(), "switch path: open-group positions reset (fix)");
    expect(!st->inner->chat.hasStreamMsg,
           "switch path: stream bubble from the previous session cleared");

    String_Free(stream.text);
    String_Free(line.summary);
    String_Free(line.text);
    tuiDestroyState(st);
}

} // namespace

int main() {
    scenario1_openGroupReset();
    scenario2_streamBubbleCleared();
    scenario3_nextIdMonotonic();
    scenario4_scrollAndApiVersion();
    scenario5_cApiNullSafety();
    scenario6_cApiSwitchPath();
    std::printf("OK: all clear-output scenarios passed\n");
    return 0;
}
