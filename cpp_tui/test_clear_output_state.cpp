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
// Also checks that the C-API clear functions are no-ops on a NULL state,
// that outputLineNextId is NOT reset (monotonic per-message id, uniqueness
// only), TUI_API_VERSION is pinned at the current generation (6), and the
// auto-scroll flag is unaffected by the clear.
//
// Exit codes: 0 = all scenarios pass; 1 = first assertion failure.
//
// Headless (no backend init needed) — run from the build dir: ./test_clear_output_state

#include "tui.h"     // llmfun::tui: TuiState, tuiAddOutputLine, tuiClearOutput, ...
#include "tui_api.h" // C API: tuiCreateState/tuiClearChatMessages/... (null-safety)

#include <cstdio>
#include <cstdlib>
#include <string>

// `TuiState`: the handle's private shape, shared by the contract tests that read back what a
// `tui*` call wrote (this one observes the clear path's state).
#include "test/tui_state_handle.h"

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

// Scenario 1: stale expansion state is reset by the clear.
void scenario1_openGroupReset() {
    g_scenario = 1;
    phase("stale outputLineOpen ids suppressed header labels after clear");

    llmfun::tui::TuiState state;

    // Previous session: two messages, then the user collapses the first one
    // (the render loop records the message id in outputLineOpen) and the
    // assistant-work group that starts at it (openWorkGroups).
    llmfun::tui::tuiAddOutputLine(state, makeMsg("first", state.chat.outputLineNextId++));
    llmfun::tui::tuiAddOutputLine(state, makeMsg("second", state.chat.outputLineNextId++));
    const size_t collapsedId = state.chat.outputLines.front().id;
    state.chat.outputLineOpen.insert(collapsedId);
    state.chat.openWorkGroups.insert(collapsedId);
    state.chat.expansionDefaultThroughId = state.chat.outputLines.back().id;
    expect(state.chat.outputLineOpen.count(collapsedId) == 1,
           "precondition: the message is recorded as collapsed");

    // Session switch: the D side calls uiMsg.clearChat() → C-API
    // tuiClearChatMessages → tuiClearOutput. The fixed behavior must reset
    // the expansion state together with the lines — including the default
    // watermark, or a replayed message would inherit "already seeded".
    llmfun::tui::tuiClearOutput(state);
    expect(state.chat.outputLines.empty(), "output lines cleared");
    expect(state.chat.outputLineOpen.empty(), "collapsed message ids reset with the lines (fix)");
    expect(state.chat.openWorkGroups.empty(), "work-group ids reset with the lines");
    expect(state.chat.expansionDefaultThroughId == 0, "default-expansion watermark reset");

    // New session: replayed messages must ALL render with their header
    // labels (none of their ids is in the open set, and the watermark is 0 so
    // each one seeds its own default on the first render).
    llmfun::tui::tuiAddOutputLine(state, makeMsg("replayed-a", state.chat.outputLineNextId++));
    llmfun::tui::tuiAddOutputLine(state, makeMsg("replayed-b", state.chat.outputLineNextId++));
    llmfun::tui::tuiAddOutputLine(state, makeMsg("replayed-c", state.chat.outputLineNextId++));
    for (const auto& msg : state.chat.outputLines)
        expect(state.chat.outputLineOpen.count(msg.id) == 0,
               "replayed message " + std::to_string(msg.id) + " is not pre-collapsed");
}

// Scenario 2: the stream bubble does not survive a clear.
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

// Scenario 3: outputLineNextId is NOT reset (monotonic, uniqueness only).
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

// Scenario 4: auto-scroll flag unaffected + TUI_API_VERSION pinned.
void scenario4_scrollAndApiVersion() {
    g_scenario = 4;
    phase("auto-scroll flag unaffected; TUI_API_VERSION pinned");

    llmfun::tui::TuiState state;
    state.autoScroll = false;
    llmfun::tui::tuiClearOutput(state);
    expect(state.autoScroll == false, "auto-scroll off stays off across a clear");
    state.autoScroll = true;
    llmfun::tui::tuiClearOutput(state);
    expect(state.autoScroll == true, "auto-scroll on stays on across a clear");

    // Documentation-only marker: the widened clear contract must not bump it.
    // 6 is the current generation (the legacy renderer's removal, task 41).
    static_assert(TUI_API_VERSION == 6, "TUI_API_VERSION must stay at the current v6 generation");
}

// Scenario 5: C-API null-safety preserved.
void scenario5_cApiNullSafety() {
    g_scenario = 5;
    phase("C-API clear functions are no-ops on NULL state");

    ChatMessageParam param{};                   // zero-initialized: all String data NULL
    tuiClearChatMessages(nullptr);              // must not crash
    tuiStreamChatMessageClear(nullptr);         // must not crash
    tuiUpdateStreamChatMessage(nullptr, param); // must not crash
}

// Scenario 6: end-to-end through the real C-API session-switch path.
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

// Scenario 7: the 1000-message cap prunes the expansion state of the message it
// evicts (review M2 — ids are never reused, so a stale entry would leak).
void scenario7_evictionPrunesExpansion() {
    g_scenario = 7;
    phase("evicted messages take their expansion state with them");

    llmfun::tui::TuiState state;
    const size_t cap = llmfun::tui::ChatTab::MaxChatMessages;

    // Fill to the cap and record the oldest message as expanded (message) and as
    // an expanded group (its id is also a group key).
    for (size_t i = 0; i < cap; ++i)
        llmfun::tui::tuiAddOutputLine(state, makeMsg("fill", state.chat.outputLineNextId++));
    expect(state.chat.outputLines.size() == cap, "the thread stops at MaxChatMessages");
    const size_t oldestId = state.chat.outputLines.front().id;
    state.chat.outputLineOpen.insert(oldestId);
    state.chat.openWorkGroups.insert(oldestId);

    // One more message evicts the oldest one.
    llmfun::tui::tuiAddOutputLine(state, makeMsg("newest", state.chat.outputLineNextId++));
    expect(state.chat.outputLines.size() == cap, "the thread stays at MaxChatMessages");
    expect(state.chat.outputLines.front().id > oldestId, "the oldest message was evicted");
    expect(state.chat.outputLineOpen.count(oldestId) == 0,
           "the evicted message's expansion state is pruned");
    expect(state.chat.openWorkGroups.count(oldestId) == 0,
           "the evicted message's work-group state is pruned");

    // A surviving message keeps its state (the pruning is keyed, not a reset).
    const size_t survivorId = state.chat.outputLines.front().id;
    state.chat.outputLineOpen.insert(survivorId);
    llmfun::tui::tuiAddOutputLine(state, makeMsg("newer", state.chat.outputLineNextId++));
    expect(state.chat.outputLines.front().id > survivorId, "the next oldest message was evicted");
    expect(state.chat.outputLines.size() == cap, "still capped");
}

} // namespace

int main() {
    scenario1_openGroupReset();
    scenario2_streamBubbleCleared();
    scenario3_nextIdMonotonic();
    scenario4_scrollAndApiVersion();
    scenario5_cApiNullSafety();
    scenario6_cApiSwitchPath();
    scenario7_evictionPrunesExpansion();
    std::printf("OK: all clear-output scenarios passed\n");
    return 0;
}
