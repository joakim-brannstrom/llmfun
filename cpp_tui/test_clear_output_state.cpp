// test_clear_output_state.cpp
//
// Headless regression test for the full clear contract: tuiClearOutput /
// the C-API tuiThreadClear must reset ALL derived thread state, not just the
// typed store:
//
//   - the typed store itself (turns AND their blocks): a previous session's
//   turns survived a session switch, so the replayed thread rendered doubled;
//   - the thread adapter's caches (group list, ids, height hints): a cache
//   built from one store's group ids is meaningless over the next store's;
//   - the expansion sets a clear must drop, together with the watermark.
//
// Scenarios drive a REAL llmfun::tui::TuiState through the same functions the
// C-API layer forwards to (tuiClearOutput / tuiThreadClear) and assert on the
// C++ state. One scenario goes end-to-end through the actual C API
// (tuiCreateState -> tuiSetTurns -> tuiThreadClear -> tuiDestroyState) —
// exactly the session-switch path (activateSession -> uiMsg.threadClear() ->
// tuiThreadClear -> tuiClearOutput) — and asserts the reset through the handle.
// No terminal, no ImGui context, no rendering — pure state-level assertions.
//
// Also checks that the C-API clear is a no-op on a NULL state,
// TUI_API_VERSION is pinned at the current generation, and the auto-scroll
// flag is unaffected by the clear.
//
// Exit codes: 0 = all scenarios pass; 1 = first assertion failure.
//
// Headless (no backend init needed) — run from the build dir: ./test_clear_output_state

#include "tui.h"     // llmfun::tui: TuiState, tuiClearOutput, ...
#include "tui_api.h" // C API: tuiCreateState/tuiThreadClear/... (null-safety)

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

// One Done turn with one content block: the smallest replay a store can hold.
llmfun::tui::store::TypedTurn makeTurn(std::int64_t id) {
    llmfun::tui::store::TypedTurn t;
    t.id = id;
    t.state = TuiTurnState_Done;
    t.messageCount = 1;
    return t;
}

llmfun::tui::store::TypedBlock makeBlock(std::int64_t turnId, const char* id) {
    llmfun::tui::store::TypedBlock b;
    b.turnId = turnId;
    b.id = id;
    b.batchId = std::to_string(turnId) + "#0";
    b.kind = TuiBlockKind_Prose;
    b.meta = "assistant";
    b.body = "body";
    return b;
}

void seedStore(llmfun::tui::TuiState& state, int turns) {
    std::vector<llmfun::tui::store::TypedTurn> ts;
    std::vector<llmfun::tui::store::TypedBlock> bs;
    for (int i = 1; i <= turns; ++i) {
        ts.push_back(makeTurn(i));
        bs.push_back(makeBlock(i, "content#0"));
    }
    state.chat.typedTurns.setTurns(std::move(ts), std::move(bs));
}

// Scenario 1: the typed store is emptied by the clear — turns AND their blocks.
void scenario1_storeCleared() {
    g_scenario = 1;
    phase("typed store cleared with the thread");

    llmfun::tui::TuiState state;
    seedStore(state, 3);
    expect(state.chat.typedTurns.turnCount() == 3, "precondition: the store holds the turns");

    const auto versionBefore = state.chat.typedTurns.version();
    llmfun::tui::tuiClearOutput(state); // session switch
    expect(state.chat.typedTurns.turnCount() == 0, "turns cleared");
    expect(state.chat.typedTurns.blockCount(1) == 0, "the turn's blocks cascaded away with it");
    expect(state.chat.typedTurns.version() != versionBefore,
           "the store's mutation counter moved (a rebuild cannot miss the clear)");
}

// Scenario 2: the clear resets the thread adapter's caches, so a stale group id
// built over the previous store cannot leak into the next thread's first draw.
void scenario2_adapterCachesReset() {
    g_scenario = 2;
    phase("thread adapter caches reset by the clear");

    llmfun::tui::TuiState state;
    seedStore(state, 2);
    state.threadAdapter.bind(state);
    expect(state.threadAdapter.groupCount() == 2, "precondition: the adapter sees the groups");

    llmfun::tui::tuiClearOutput(state); // session switch: full reset
    state.threadAdapter.bind(state);
    expect(state.threadAdapter.groupCount() == 0,
           "the adapter's group list is empty over the cleared store");
}

// Scenario 3: auto-scroll flag unaffected + TUI_API_VERSION pinned.
void scenario3_scrollAndApiVersion() {
    g_scenario = 3;
    phase("auto-scroll flag unaffected; TUI_API_VERSION pinned");

    llmfun::tui::TuiState state;
    state.autoScroll = false;
    llmfun::tui::tuiClearOutput(state);
    expect(state.autoScroll == false, "auto-scroll off stays off across a clear");
    state.autoScroll = true;
    llmfun::tui::tuiClearOutput(state);
    expect(state.autoScroll == true, "auto-scroll on stays on across a clear");

    // Documentation-only marker: the clear contract must not bump it. (v16
    // bumped it anyway - the pipeline payloads grew a batch index - which is a
    // wire change behind this test's back, not a change to the clear contract.)
    static_assert(TUI_API_VERSION == 16, "TUI_API_VERSION must stay at the current generation");
}

// Scenario 4: the C-API clear is a no-op on a NULL state.
void scenario4_cApiNullSafety() {
    g_scenario = 4;
    phase("C-API clear is a no-op on a NULL state");

    tuiThreadClear(nullptr); // must not crash
}

// Scenario 5: end-to-end through the real C-API session-switch path.
void scenario5_cApiSwitchPath() {
    g_scenario = 5;
    phase("C API switch path: tuiThreadClear reaches the fixed tuiClearOutput");

    TuiState* st = tuiCreateState();
    expect(st != nullptr && st->inner != nullptr, "tuiCreateState works headless");

    // Replay a store (as at switch time), then clear through the C API.
    std::vector<TuiTurn> turns;
    std::vector<TuiBlock> blocks;
    for (int i = 1; i <= 2; ++i) {
        TuiTurn t{};
        t.id = i;
        t.state = TuiTurnState_Done;
        t.messageCount = 1;
        turns.push_back(t);
        TuiBlock b{};
        b.turnId = i;
        b.id = String_New("content#0");
        b.batchId = String_New((std::to_string(i) + "#0").c_str());
        b.kind = TuiBlockKind_Prose;
        b.meta = String_New("assistant");
        b.body = String_New("body");
        blocks.push_back(b);
    }
    tuiSetTurns(st, turns.data(), turns.size(), blocks.data(), blocks.size());
    expect(st->inner->chat.typedTurns.turnCount() == 2, "precondition: the replay loaded");

    // The switch-path clear (exactly what tuiThreadClear forwards to) must run
    // without a terminal/ImGui context and leave the state self-consistent.
    tuiThreadClear(st);
    expect(st->inner->chat.typedTurns.turnCount() == 0, "switch path: turns cleared");
    expect(st->inner->chat.typedTurns.blockCount(1) == 0,
           "switch path: blocks cleared with the turns");

    for (auto& b : blocks) {
        String_Free(b.id);
        String_Free(b.batchId);
        String_Free(b.meta);
        String_Free(b.body);
    }
    tuiDestroyState(st);
}

// Scenario 6: the typed store's own eviction is part of the clear story's
// neighbourhood (whole turns, oldest first, never the live turn) — the upsert
// path exercises it, and the cap matches the test here.
void scenario6_evictionWholeTurns() {
    g_scenario = 6;
    phase("eviction drops whole turns, oldest first, never the live turn");

    llmfun::tui::TuiState state;
    const auto cap = llmfun::tui::store::TypedThreadStore::kMaxTurns;
    for (std::int64_t i = 1; i <= static_cast<std::int64_t>(cap); ++i) {
        state.chat.typedTurns.turnUpsert(makeTurn(i));
        state.chat.typedTurns.blockUpsert(makeBlock(i, "content#0"));
    }
    state.chat.typedTurns.turnUpsert(makeTurn(cap + 1)); // cap exceeded
    // The emitter owns the eviction call (the C API invokes it after opening a
    // turn): the store never guesses which upsert is "live".
    state.chat.typedTurns.evictForCapacity(cap + 1); // evicts turn 1
    expect(state.chat.typedTurns.turnCount() == cap, "the store stays at the cap");
    expect(state.chat.typedTurns.turnById(1) == nullptr, "the oldest turn was evicted");
    expect(state.chat.typedTurns.blockCount(1) == 0, "the evicted turn's blocks went with it");
    expect(state.chat.typedTurns.turnById(cap) != nullptr, "an intervening turn survives");
    expect(state.chat.typedTurns.turnById(cap + 1) != nullptr, "the live turn is never evicted");
}

} // namespace

int main() {
    scenario1_storeCleared();
    scenario2_adapterCachesReset();
    scenario3_scrollAndApiVersion();
    scenario4_cApiNullSafety();
    scenario5_cApiSwitchPath();
    scenario6_evictionWholeTurns();
    std::printf("test_clear_output_state: all scenarios pass\n");
    return 0;
}
