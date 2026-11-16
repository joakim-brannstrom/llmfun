## Task 2: C++ typed thread store + adapter rewrite (design P0-2)

Priority: P0. Dependencies: task 1 (P0-1). One additive commit; the v4 path stays the
live source until the cutover (task 5).

### Description
Add the typed thread store (turns + blocks) to the C++ side, wire the five v5 entry
points from task 1 into it, and rewrite `ThreadStoreAdapter` to project from the typed
store through the unchanged GroupView/BlockDesc seam. Pre-cutover the adapter keeps
both sources (v4-backed = still selected live; typed-backed = test-driven only); the
selector and the v4-backed path are deleted at the cutover.

### Changes Made
- llmfun/cpp_tui/store/typed_thread_store.h / .cpp (new):
```cpp
namespace llmfun::tui::store {

struct TypedTurn  { std::int64_t id = 0; int ordinal = 0; int state = 0; int messageCount = 0; };
struct TypedBlock {
    std::int64_t turnId = 0;
    std::string id, batchId, title, meta, body;
    int kind = 0, status = 0, flags = 0;
    std::int64_t durationMs = 0, tokensIn = 0, tokensOut = 0;
};

// Displayability (D3/D10): ONE predicate, shared by weights, thread rendering,
// drill-down, and the console formatter. A Tool block is hidden iff its tool
// name is in kHiddenToolNames; every other block kind is displayable.
// kHiddenToolNames mirrors today's D-side hiddenTools (config.d:423) on the
// wire's snake_case names; verify the exact set against today's isDisplayable
// behavior (source/llm/app_agent/package.d:1051-1060) and pin it in the unit
// test below. Hidden blocks: not rendered, weight 0.
bool blockDisplayable(const TypedBlock& b);
inline constexpr std::array<std::string_view, 1> kHiddenToolNames{"pipeline_output"};

// Step-count weights (D4), by status — N = the LLM messages the group covers:
//   Tool: Ok/Failed = 2 (call + result), Pending/Running/Cancelled = 1;
//   Prose (response content) = 1; Reasoning = 0 (rides the content message);
//   turn-opening Prose (user query) = 0; Answer = 0 (counted in its task_done
//   tool block); Notice = 1; hidden = 0. A result-only block (unknown callId)
//   has Ok/Failed status, so it weights 2; the call's upsert merges in later.
int stepCount(const TypedThreadStore&, std::int64_t turnId);

class TypedThreadStore {
public:
    // Bulk snapshot: clears, then loads turns (array order) + blocks. Replay.
    void setTurns(std::vector<TypedTurn> turns, std::vector<TypedBlock> blocks);
    void turnUpsert(TypedTurn turn);      // insert or merge in place (key = id)
    void blockUpsert(TypedBlock block);   // first upsert creates the (turnId,id)
                                          // slot, later upserts merge in place
    void turnRemove(std::int64_t turnId); // cascades to the turn's blocks (W6)
    void threadClear();
    std::size_t turnCount() const;
    const TypedTurn& turnAt(std::size_t k) const;   // snapshot/insertion order
    std::size_t blockCount(std::int64_t turnId) const;
    const TypedBlock& blockAt(std::int64_t turnId, std::size_t k) const;
    const TypedTurn* turnById(std::int64_t id) const;
    // Whole-turn FIFO eviction (design §4): oldest first, never the live turn
    // (the caller passes the live turn id; live = the newest upserted turn).
    void evictForCapacity(std::int64_t liveTurnId);
    static constexpr std::size_t kMaxTurns = 1000; // mirrors today's per-tab
                                                   // MaxChatMessages (tui.h:257)
};
}
```
- Storage: turns in insertion order (vector + id index), blocks per turn (vector
  keyed by insertion, (turnId,id) index). Upsert merge = field-by-field overwrite
  of the fields the payload carries; never re-parse or re-scan on upsert (§7).
- llmfun/cpp_tui/tui_api.cpp (edit: the five task-1 entry points get real bodies
  replacing the task-1 stubs): tuiSetTurns/tuiTurnUpsert/tuiBlockUpsert/
  tuiTurnRemove/tuiThreadClear forward into the typed store of the addressed tab
  (main = state.chat's store; the pipeline entry points keep their signatures and
  are retargeted in task 5 — here they write the main store only if trivially
  reachable, else stay stubbed and noted for task 5).
- llmfun/cpp_tui/store/thread_store.cpp: rewrite `ThreadStoreAdapter` to project
  GroupView/BlockDesc from the typed store — group id = the persisted turnId
  (design §4: overrides/expansion survive upserts, re-emission, rebuilds);
  oneLineSummary = the turn's presentation line; batches and merged tool cards
  ride the extended block vocabulary (BlockKind gains the v5 kinds — reuse
  model/block.h BlockKind where it matches, extend where it does not).
  Keep today's v4-backed projection path beside the new typed-backed one with
  an explicit source selector; v4 stays selected (live) until the cutover.
- Height-hint eviction: unchanged view-level mechanism, evaluated per rendered
  row as today (requirements_notes: N2) — the store adds only whole-turn FIFO.

### Verification
- [ ] Store unit tests (test file beside the existing store tests; CMake target
      updated per the CMakeLists.txt:839-844 pattern) covering design §8:
      upsert merge by callId (first creates, later merges); status transitions
      from scripted upsert streams (Pending->Running->Ok, ->Failed, ->Cancelled);
      batch grouping (blocks share batchId, tool-less batches included);
      stepCount weights incl. hidden = 0 and the kHiddenToolNames set;
      eviction (whole turns, oldest first, never live); turnRemove cascade;
      group id = turnId stable across re-emission and rebuild; unknown-callId
      result recorded then merged when the call lands; threadClear.
- [ ] Adapter tests (pattern of test_thread_window.cpp move/drop pins): the
      typed-backed projection feeds the EXISTING thread view unchanged — same
      GroupView/BlockDesc consumers compile and render.
- [ ] `cmake --build cpp_tui/build` green; `dub test` green; known pre-existing
      reds excluded (overview).

### Notes
- D3/D4/D10 are defined here (overview carries one line each); consumers restate.
- The v5 view/rendering code is NOT in this task (tasks 6-8); the adapter's
  typed-backed path is exercised by tests only pre-cutover.
- Eviction cap kMaxTurns=1000 mirrors the v4 per-tab cap; re-checked at task 9
  with the 1000-message session test.
- Design gap W2 (closed): eviction is per whole turn — never per block, never
  the live turn; the live emitter's upserts cannot fight eviction.
