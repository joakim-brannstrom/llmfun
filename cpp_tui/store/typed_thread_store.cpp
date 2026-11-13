// typed_thread_store.cpp
//
// The typed thread store's mutation and projection surface (see
// typed_thread_store.h for the contract). Everything here is O(1) per upsert:
// an index lookup plus a field-by-field overwrite, never a scan.

#include "typed_thread_store.h"

namespace llmfun::tui::store {

bool blockDisplayable(const TypedBlock& b) {
    if (b.kind != kBlockKindTool)
        return true;
    return (b.flags & kBlockFlagHidden) == 0;
}

void TypedThreadStore::setTurns(std::vector<TypedTurn> turns, std::vector<TypedBlock> blocks) {
    // Every mutating path opens with the bump: the adapter's rebuild guard keys
    // on this counter, so the bump is what marks the store "changed".
    ++version_;
    // A snapshot is the one bulk path, so it may rebuild the vectors wholesale -
    // still no re-parse: the payloads arrive typed.
    turns_ = std::move(turns);
    turnIndex_.clear();
    turnIndex_.reserve(turns_.size());
    // D owns the uniqueness contract:
    // a duplicate id in a snapshot would orphan the later turn - `emplace` keeps
    // the first slot and the later turn's blocks would hang off an unreachable
    // slot. The emitter's contract guarantees it never happens; the merge paths
    // (turnUpsert/blockUpsert) are find-guarded and cannot orphan anything.
    for (std::size_t k = 0; k < turns_.size(); ++k)
        turnIndex_.emplace(turns_[k].id, k);
    blocks_.assign(turns_.size(), {});
    blockIndex_.assign(turns_.size(), {});
    for (TypedBlock& block : blocks) {
        if (turnIndex_.find(block.turnId) == turnIndex_.end()) {
            // A block naming a turn the snapshot does not carry: record it under
            // an implicit empty turn (same rule as blockUpsert) instead of
            // silently dropping it.
            turnIndex_.emplace(block.turnId, turns_.size());
            turns_.push_back(TypedTurn{block.turnId, 0, 0});
            blocks_.emplace_back();
            blockIndex_.emplace_back();
        }
        const std::size_t slot = turnIndex_.at(block.turnId);
        // Same uniqueness contract for block ids within a turn: a duplicate id
        // in a snapshot would keep the first slot and silently drop the later
        // payload from every projection (the index is the only address).
        blockIndex_[slot].emplace(block.id, blocks_[slot].size());
        blocks_[slot].push_back(std::move(block));
    }
}

void TypedThreadStore::turnUpsert(TypedTurn turn) {
    ++version_; // a merge overwrites content: the adapter must rebuild
    const auto it = turnIndex_.find(turn.id);
    if (it != turnIndex_.end()) {
        // Merge in place: every field is overwritten, the slot (and with it the
        // insertion position and any group id derived from it) stays.
        turns_[it->second] = std::move(turn);
        return;
    }
    turnIndex_.emplace(turn.id, turns_.size());
    turns_.push_back(std::move(turn));
    blocks_.emplace_back();
    blockIndex_.emplace_back();
}

void TypedThreadStore::blockUpsert(TypedBlock block) {
    ++version_; // a merge overwrites content: the adapter must rebuild
    if (turnIndex_.find(block.turnId) == turnIndex_.end()) {
        // The turn has not been upserted yet (a result for an unknown callId
        // can beat its turn's snapshot): an implicit empty turn slot keeps the
        // block addressable until the real turn merges into the slot.
        turnIndex_.emplace(block.turnId, turns_.size());
        turns_.push_back(TypedTurn{block.turnId, 0, 0});
        blocks_.emplace_back();
        blockIndex_.emplace_back();
    }
    const std::size_t slot = turnIndex_.at(block.turnId);
    auto& index = blockIndex_[slot];
    const auto it = index.find(block.id);
    if (it != index.end()) {
        auto& slotBlock = blocks_[slot][it->second];
        if (block.resultPairs.empty() && block.resultRaw.empty()) {
            block.resultPairs = std::move(slotBlock.resultPairs);
            block.resultRaw = std::move(slotBlock.resultRaw);
            block.resultMore = slotBlock.resultMore;
        }
        slotBlock = std::move(block);
        return;
    }
    index.emplace(block.id, blocks_[slot].size());
    blocks_[slot].push_back(std::move(block));
}

void TypedThreadStore::turnRemove(std::int64_t turnId) {
    const auto it = turnIndex_.find(turnId);
    if (it == turnIndex_.end())
        return; // idempotent: removing a turn that is gone clears nothing
    ++version_; // only an actual removal changes the store
    const std::size_t slot = it->second;
    turns_.erase(turns_.begin() + static_cast<std::ptrdiff_t>(slot));
    blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(slot));
    blockIndex_.erase(blockIndex_.begin() + static_cast<std::ptrdiff_t>(slot));
    turnIndex_.erase(it);
    // The indices name slots, and the erase moved every later slot down by one:
    // re-key them. O(remaining turns) - a remove is a rare, user-visible event.
    for (std::size_t k = slot; k < turns_.size(); ++k)
        turnIndex_.at(turns_[k].id) = k;
}

void TypedThreadStore::threadClear() {
    ++version_;
    turns_.clear();
    blocks_.clear();
    turnIndex_.clear();
    blockIndex_.clear();
}

std::size_t TypedThreadStore::turnCount() const { return turns_.size(); }

const TypedTurn& TypedThreadStore::turnAt(std::size_t k) const {
    static const TypedTurn empty;
    return k < turns_.size() ? turns_[k] : empty;
}

std::size_t TypedThreadStore::blockCount(std::int64_t turnId) const {
    const auto it = turnIndex_.find(turnId);
    return it == turnIndex_.end() ? 0 : blocks_[it->second].size();
}

const TypedBlock& TypedThreadStore::blockAt(std::int64_t turnId, std::size_t k) const {
    static const TypedBlock empty;
    const auto it = turnIndex_.find(turnId);
    if (it == turnIndex_.end() || k >= blocks_[it->second].size())
        return empty;
    return blocks_[it->second][k];
}

const TypedTurn* TypedThreadStore::turnById(std::int64_t id) const {
    const auto it = turnIndex_.find(id);
    return it == turnIndex_.end() ? nullptr : &turns_[it->second];
}

void TypedThreadStore::evictForCapacity(std::int64_t liveTurnId) {
    // Whole turns only, oldest first, never the live turn: the emitter's
    // upserts for the live turn cannot fight an eviction of the turn it is
    // writing. The cap exceeded means at least two turns are held, so skipping
    // the live one always leaves an older turn to evict - the loop terminates.
    while (turns_.size() > kMaxTurns) {
        const std::size_t slot = turns_.front().id == liveTurnId ? 1 : 0;
        const std::int64_t evicted = turns_[slot].id;
        turns_.erase(turns_.begin() + static_cast<std::ptrdiff_t>(slot));
        blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(slot));
        blockIndex_.erase(blockIndex_.begin() + static_cast<std::ptrdiff_t>(slot));
        turnIndex_.erase(evicted);
        ++version_; // per eviction: the adapter's guard only needs "changed"
        for (std::size_t k = slot; k < turns_.size(); ++k)
            turnIndex_.at(turns_[k].id) = k;
    }
}

int stepCount(const TypedThreadStore& store, std::int64_t turnId) {
    const TypedTurn* turn = store.turnById(turnId);
    if (!turn)
        return 0; // no turn, no steps: the group has nothing to announce
    int steps = 0;
    bool seenProse = false;
    for (std::size_t k = 0, n = store.blockCount(turnId); k < n; ++k) {
        const TypedBlock& b = store.blockAt(turnId, k);
        if (!blockDisplayable(b) || b.kind == kBlockKindReasoning || b.kind == kBlockKindAnswer)
            continue;
        if (b.kind == kBlockKindProse) {
            if (!seenProse) {
                seenProse = true;
                continue;
            }
            steps += 1;
            continue;
        }
        if (b.kind == kBlockKindNotice) {
            steps += 1;
            continue;
        }
        if (b.kind == kBlockKindTool) {
            // Tool by status: a settled result carries call + result (2);
            // everything still in flight - or a call the emitter has not
            // stamped yet (status None: work the run started) - carries the
            // call (1). A result-only block arrives settled, so it weights 2
            // and the call's upsert merges into the same slot later.
            steps += (b.status == kBlockStatusOk || b.status == kBlockStatusFailed) ? 2 : 1;
            continue;
        }
        steps += 1; // unknown kind: defensive - a surprise counts as work
    }
    return steps;
}

} // namespace llmfun::tui::store
