// typed_thread_store.h
//
// The typed thread store: turns + blocks as the wire
// carries them, projected by the adapter through the unchanged GroupView /
// BlockDesc seam. The store is the mutation surface - upserts are the ONLY
// mutating paths, a first upsert creates the slot and later upserts merge in
// place - and it never re-parses or re-scans on an upsert: every
// upsert is an index lookup plus a field-by-field overwrite.
//
// The message-backed projection stays the adapter's live source; the
// typed-backed path is test-driven only.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace llmfun::tui::store {

class TypedThreadStore;

struct TypedTurn {
    std::int64_t id = 0;
    int state = 0;
    int messageCount = 0;
};
struct TypedBlock {
    std::int64_t turnId = 0;
    std::string id, batchId, title, meta, body;
    int kind = 0, status = 0, flags = 0;
    std::int64_t durationMs = 0, tokensIn = 0, tokensOut = 0;
    std::vector<std::pair<std::string, std::string>> resultPairs;
    // The packed resultRaw fallback when the result is not an unpackable JSON object.
    std::string resultRaw;
    // resultMore is the pairs past the sender's cap, kept verbatim so the
    // renderer can draw the "... K more"
    std::uint32_t resultMore = 0;
};

// Displayability: ONE predicate, shared by weights, thread rendering,
// drill-down, and the console formatter. A Tool block is hidden iff its wire
// flags carry TuiBlockFlag_Hidden - the sender folds the config's display
// verdict (isHiddenTool) into the flags, so the receiver hides from the flag
// alone, never the name. Every other block kind is displayable. Hidden blocks:
// not rendered, weight 0.
bool blockDisplayable(const TypedBlock& b);

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
inline constexpr int kBlockFlagHidden = 8;      // TuiBlockFlag_Hidden

// The wire's turn states (TuiTurnState on the ABI): a turn is Running from the
// upsert that opens it until the done/stop upsert closes it, and the *running*
// turn is the live answer the thread window keeps outside its bound.
inline constexpr int kTurnStateRunning = 0;     // TuiTurnState_Running
inline constexpr int kTurnStateDone = 1;        // TuiTurnState_Done
inline constexpr int kTurnStateInterrupted = 2; // TuiTurnState_Interrupted

// Step-count weights (D4), by status — N = the LLM messages the group covers:
//   Tool: Ok/Failed = 2 (call + result), Pending/Running/Cancelled = 1;
//   Prose (response content) = 1; Reasoning = 0 (rides the content message);
//   turn-opening Prose (user query) = 0; Answer = 0 (counted in the tool block
//   whose result carried the answer); Notice = 1; hidden = 0. A result-only
//   block (unknown callId) has Ok/Failed status, so it weights 2; the call's
//   upsert merges in later.
//
// Implementations of the prose rule and the gaps the table leaves open (both
// documented in the .cpp, pinned in the unit test): the turn's FIRST Prose
// block in insertion order is the turn-opening query ("the turn
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
    void turnRemove(std::int64_t turnId); // cascades to the turn's blocks
    void threadClear();
    std::size_t turnCount() const;
    const TypedTurn& turnAt(std::size_t k) const; // snapshot/insertion order
    std::size_t blockCount(std::int64_t turnId) const;
    const TypedBlock& blockAt(std::int64_t turnId, std::size_t k) const;
    const TypedTurn* turnById(std::int64_t id) const;
    // Whole-turn FIFO eviction: oldest first, never the live turn
    // (the caller passes the live turn id; live = the newest upserted turn).
    void evictForCapacity(std::int64_t liveTurnId);
    static constexpr std::size_t kMaxTurns = 1000; // the per-tab cap
    // The mutation counter: bumped by every mutating path (setTurns, turnUpsert,
    // blockUpsert, turnRemove, threadClear, evictForCapacity). The adapter's
    // rebuild guard compares it O(1) per accessor call, so an unchanged store rebuilds nothing.
    std::uint64_t version() const { return version_; }

private:
    // Turns in insertion order, blocks per turn in insertion order, both with
    // O(1) indices. An upsert looks the slot up and overwrites it; nothing
    // walks the vectors. A block upsert for a turn that does not
    // exist yet creates an implicit empty turn slot (id = turnId) so a result
    // for an unknown callId is recorded, never dropped.
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
