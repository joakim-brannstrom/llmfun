// typed_thread_store.h
//
// The v5 typed thread store (task 2, design P0-2): turns + blocks as the wire
// carries them, projected by the adapter through the unchanged GroupView /
// BlockDesc seam. The store is the mutation surface - upserts are the ONLY
// mutating paths, a first upsert creates the slot and later upserts merge in
// place - and it never re-parses or re-scans on an upsert (design §7): every
// upsert is an index lookup plus a field-by-field overwrite.
//
// v5 is additive until the cutover (task 5): the v4 message-backed projection
// stays the adapter's live source; the typed-backed path is test-driven only.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace llmfun::tui::store {

class TypedThreadStore;

struct TypedTurn {
    std::int64_t id = 0;
    int ordinal = 0;
    int state = 0;
    int messageCount = 0;
};
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
// wire's snake_case names; verified against today's isDisplayable behavior
// (source/llm/app_agent/package.d:1051-1057) - see the unit test for the pin,
// including the deliberate task_done divergence (v5 renders the task_done step;
// its answer body arrives as the Answer block). The tool name rides `title` on
// Tool blocks (TuiBlockKind_Tool == 2 on the wire). Hidden blocks: not
// rendered, weight 0.
bool blockDisplayable(const TypedBlock& b);
inline constexpr std::array<std::string_view, 1> kHiddenToolNames{"pipeline_output"};

// The wire's vocabulary
inline constexpr int kBlockKindProse = 0;       // TuiBlockKind_Prose
inline constexpr int kBlockKindReasoning = 1;   // TuiBlockKind_Reasoning
inline constexpr int kBlockKindTool = 2;        // TuiBlockKind_Tool
inline constexpr int kBlockKindAnswer = 3;      // TuiBlockKind_Answer
inline constexpr int kBlockKindNotice = 4;      // TuiBlockKind_Notice
inline constexpr int kBlockStatusNone = 0;      // TuiBlockStatus_None
inline constexpr int kBlockStatusPending = 1;   // TuiBlockStatus_Pending
inline constexpr int kBlockStatusRunning = 2;   // TuiBlockStatus_Running
inline constexpr int kBlockStatusOk = 3;        // TuiBlockStatus_Ok
inline constexpr int kBlockStatusFailed = 4;    // TuiBlockStatus_Failed
inline constexpr int kBlockStatusCancelled = 5; // TuiBlockStatus_Cancelled

// The wire's turn states (TuiTurnState on the ABI): a turn is Running from the
// upsert that opens it until the done/stop upsert closes it, and the *running*
// turn is the live answer the thread window keeps outside its bound.
inline constexpr int kTurnStateRunning = 0;     // TuiTurnState_Running
inline constexpr int kTurnStateDone = 1;        // TuiTurnState_Done
inline constexpr int kTurnStateInterrupted = 2; // TuiTurnState_Interrupted

// Step-count weights (D4), by status — N = the LLM messages the group covers:
//   Tool: Ok/Failed = 2 (call + result), Pending/Running/Cancelled = 1;
//   Prose (response content) = 1; Reasoning = 0 (rides the content message);
//   turn-opening Prose (user query) = 0; Answer = 0 (counted in its task_done
//   tool block); Notice = 1; hidden = 0. A result-only block (unknown callId)
//   has Ok/Failed status, so it weights 2; the call's upsert merges in later.
//
// Implementations of the prose rule and the gaps the table leaves open (both
// documented in the .cpp, pinned in the unit test): the turn's FIRST Prose
// block in insertion order is the turn-opening query (design §5 "the turn
// upsert opens the turn"); a Tool block still at status None weights 1 (a
// call the emitter has upserted is work the run started, like Pending); an
// unknown kind or status weights 1 (defensive - a surprise counts as work,
// never as silence). Recomputed per call, never stored.
int stepCount(const TypedThreadStore& store, std::int64_t turnId);

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
    const TypedTurn& turnAt(std::size_t k) const; // snapshot/insertion order
    std::size_t blockCount(std::int64_t turnId) const;
    const TypedBlock& blockAt(std::int64_t turnId, std::size_t k) const;
    const TypedTurn* turnById(std::int64_t id) const;
    // Whole-turn FIFO eviction (design §4): oldest first, never the live turn
    // (the caller passes the live turn id; live = the newest upserted turn).
    void evictForCapacity(std::int64_t liveTurnId);
    static constexpr std::size_t kMaxTurns = 1000; // the per-tab cap the v4
                                                   // path carried
    // The mutation counter: bumped by every mutating path (setTurns, turnUpsert,
    // blockUpsert, turnRemove, threadClear, evictForCapacity). The adapter's
    // rebuild guard compares it O(1) per accessor call (design §4.4's
    // incremental enumeration - the v4 guard's store-shape key serves the same
    // job on the message store), so an unchanged store rebuilds nothing.
    std::uint64_t version() const { return version_; }

private:
    // Turns in insertion order, blocks per turn in insertion order, both with
    // O(1) indices. An upsert looks the slot up and overwrites it; nothing
    // walks the vectors (design §7). A block upsert for a turn that does not
    // exist yet creates an implicit empty turn slot (id = turnId, ordinal 0)
    // so a result for an unknown callId is recorded, never dropped (§7);
    // the real turn's upsert merges into that slot.
    std::vector<TypedTurn> turns_;
    std::vector<std::vector<TypedBlock>> blocks_;             // parallel to turns_
    std::unordered_map<std::int64_t, std::size_t> turnIndex_; // turn id -> slot
    std::vector<std::unordered_map<std::string, std::size_t>> blockIndex_;
    // per turn: block id -> slot
    // The mutation counter `version()` serves (bumped by every mutating path;
    // never wraps in practice - a rebuild happens between upserts).
    std::uint64_t version_ = 0;
};

} // namespace llmfun::tui::store
