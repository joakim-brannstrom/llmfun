// test_typed_thread_store.cpp
//
// The v5 typed thread store (store/typed_thread_store.h) and the typed-backed
// projection it feeds (store/thread_store.h's `TypedTurns` source): the upsert
// merge semantics, the step-count weights, the eviction, and the adapter's
// group/block view of a store the wire upserts.
//
// Pure: no ImGui context, no renderer, no terminal - the store answers from
// its vectors and the typed projection never draws, exactly like
// ./test_thread_window's adapter half.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = usage error.
//
//   ./test_typed_thread_store [-v]

#include "store/typed_thread_store.h"

#include "store/thread_store.h"
#include "tui.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace tui = llmfun::tui;
using tui::store::TypedBlock;
using tui::store::TypedThreadStore;
using tui::store::TypedTurn;

int g_checks = 0;
int g_failures = 0;
bool g_verbose = false;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    } else if (g_verbose) {
        std::printf("ok   %s\n", what.c_str());
    }
}

/// A wire turn. `state`/`messageCount` are informational (the header says so);
/// the tests pass them only where the field under test is the point.
TypedTurn turn(std::int64_t id, int ordinal = 1, int state = 1, int messageCount = 0) {
    TypedTurn t;
    t.id = id;
    t.ordinal = ordinal;
    t.state = state;
    t.messageCount = messageCount;
    return t;
}

/// A wire block. kind/status are the wire numbers (TuiBlockKind_/TuiBlockStatus_),
/// spelled as numbers here so the test pins the wire's vocabulary, not a C++ enum
/// that could be reordered.
TypedBlock block(std::int64_t turnId, const std::string& id, int kind, int status,
                 const std::string& title = "", const std::string& body = "",
                 const std::string& batchId = "", std::int64_t durationMs = 0) {
    TypedBlock b;
    b.turnId = turnId;
    b.id = id;
    b.kind = kind;
    b.status = status;
    b.title = title;
    b.body = body;
    b.batchId = batchId;
    b.durationMs = durationMs;
    return b;
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0) {
            g_verbose = true;
        } else {
            std::fprintf(stderr, "usage: %s [-v]\n", argv[0]);
            return 2;
        }
    }

    // --- The upsert merge: a first upsert creates the slot, later ones merge ---
    {
        TypedThreadStore store;
        store.turnUpsert(turn(7, 1, 0, 2));
        store.blockUpsert(block(7, "call-1", 2, 1, "grep", "call body")); // Pending
        check(store.turnCount() == 1 && store.blockCount(7) == 1,
              "first upserts create the slots");
        store.blockUpsert(block(7, "result-1", 2, 3, "grep", "result body")); // Ok
        check(store.blockCount(7) == 2, "a new callId is a new slot");
        // The merge: the same (turnId, id), the payload overwritten, the
        // position kept - a streamed call growing a result never reorders.
        store.blockUpsert(block(7, "call-1", 2, 3, "grep", "call body v2")); // Ok
        check(store.blockCount(7) == 2, "a merged block keeps its slot: no third row");
        check(store.blockAt(7, 0).status == 3 && store.blockAt(7, 0).body == "call body v2",
              "the merge overwrote the payload in place");
        check(store.blockAt(7, 1).id == "result-1", "the merge did not disturb the insertion order");
        // A turn upsert merges too: the slot (and with it any group id derived
        // from the insertion position) stays.
        store.turnUpsert(turn(7, 1, 1, 5));
        check(store.turnCount() == 1 && store.turnAt(0).messageCount == 5,
              "a turn upsert merges in place");
        check(store.blockCount(7) == 2, "the turn merge left the blocks alone");
        // The store merges; it never enforces a transition - D7 is the
        // emitter's, and a settled result re-sent as Pending is merged as-is.
        store.blockUpsert(block(7, "call-1", 2, 1, "grep", "call body v2"));
        check(store.blockAt(7, 0).status == 1, "the store accepts Ok -> Pending (D7 is the emitter's)");
    }

    // --- Batch grouping: insertion order per turn, tool-less batches included ---
    {
        TypedThreadStore store;
        store.turnUpsert(turn(11));
        // Batch #1: thinking + content (a tool-less batch carries a batchId too).
        store.blockUpsert(block(11, "11#0-think", 1, 0, "", "thinking text", "11#0"));
        store.blockUpsert(block(11, "11#0-say", 0, 0, "", "content text", "11#0"));
        // Batch #2: the call + its result.
        store.blockUpsert(block(11, "11#1-call", 2, 3, "grep", "call body", "11#1"));
        store.blockUpsert(block(11, "11#1-result", 2, 3, "grep", "result body", "11#1"));
        check(store.blockCount(11) == 4, "both batches are stored under the turn");
        check(store.blockAt(11, 0).id == "11#0-think" && store.blockAt(11, 1).id == "11#0-say" &&
                  store.blockAt(11, 2).id == "11#1-call" && store.blockAt(11, 3).id == "11#1-result",
              "the blocks keep their upsert order: batch #1 before batch #2");
        check(store.blockAt(11, 0).batchId == "11#0" && store.blockAt(11, 3).batchId == "11#1",
              "the batchId rides the block");
    }

    // --- Displayability (D3) and the step-count weights (D4) --------------------
    {
        check(tui::store::kHiddenToolNames.size() == 1 &&
                  tui::store::kHiddenToolNames[0] == "pipeline_output",
              "the hidden set is exactly the v5 wire name (pipeline_output)");
        TypedBlock prose = block(1, "p", 0, 0);
        check(tui::store::blockDisplayable(prose), "non-Tool blocks are always displayable (D3)");
        TypedBlock tool = block(1, "t", 2, 3, "grep");
        check(tui::store::blockDisplayable(tool), "a Tool block outside the hidden set is displayable");
        tool.title = "pipeline_output";
        check(!tui::store::blockDisplayable(tool), "a Tool block in the hidden set is not (D3)");

        TypedThreadStore store;
        store.turnUpsert(turn(100)); // hidden tool: weight 0
        store.blockUpsert(block(100, "h", 2, 3, "pipeline_output", "..."));
        check(tui::store::stepCount(store, 100) == 0, "a hidden tool block weighs 0 (D3)");

        store.turnUpsert(turn(101)); // task_done's card + its Answer: 2 + 0
        store.blockUpsert(block(101, "td", 2, 3, "task_done", "the answer"));
        store.blockUpsert(block(101, "ans", 3, 0, "", "the answer"));
        check(tui::store::stepCount(store, 101) == 2,
              "task_done is rendered: its card weighs 2 and its Answer rides it (D3/D4)");

        store.turnUpsert(turn(102)); // the turn-opening query + reasoning: 0 + 0
        store.blockUpsert(block(102, "q", 0, 0, "", "what is v5?"));
        store.blockUpsert(block(102, "r", 1, 0, "", "hmm"));
        check(tui::store::stepCount(store, 102) == 0,
              "the turn-opening query weighs 0 and reasoning rides it (D4)");

        store.turnUpsert(turn(103)); // response content: 1
        store.blockUpsert(block(103, "q", 0, 0, "", "query"));
        store.blockUpsert(block(103, "a", 0, 0, "", "reply"));
        check(tui::store::stepCount(store, 103) == 1, "response content weighs 1 (D4)");

        store.turnUpsert(turn(104)); // notice: 1
        store.blockUpsert(block(104, "n", 4, 0, "", "heads up"));
        check(tui::store::stepCount(store, 104) == 1, "a notice weighs 1 (D4)");

        store.turnUpsert(turn(105)); // tools in flight: 1 each
        store.blockUpsert(block(105, "p", 2, 1, "grep", "...")); // Pending
        store.blockUpsert(block(105, "g", 2, 2, "grep", "...")); // Running
        store.blockUpsert(block(105, "c", 2, 5, "grep", "...")); // Cancelled
        store.blockUpsert(block(105, "z", 2, 0, "grep", "...")); // None: a call the emitter started
        check(tui::store::stepCount(store, 105) == 4,
              "Pending/Running/Cancelled/None tools weigh 1 each (D4 + the None gap)");

        store.turnUpsert(turn(106)); // settled tools: 2 each
        store.blockUpsert(block(106, "ok", 2, 3, "grep", "..."));   // Ok
        store.blockUpsert(block(106, "bad", 2, 4, "grep", "...")); // Failed
        check(tui::store::stepCount(store, 106) == 4, "Ok/Failed tools weigh 2 each: call + result (D4)");

        store.turnUpsert(turn(107)); // a result-only block (unknown callId)
        store.blockUpsert(block(107, "late-result", 2, 3, "grep", "..."));
        check(tui::store::stepCount(store, 107) == 2,
              "a result-only block is settled: it weighs 2, and the call merges in later (D4)");

        store.turnUpsert(turn(108)); // an unknown kind: defensive
        store.blockUpsert(block(108, "odd", 9, 0, "", "mystery"));
        check(tui::store::stepCount(store, 108) == 1, "an unknown kind weights 1: a surprise counts as work (D4)");

        check(tui::store::stepCount(store, 999) == 0, "a turn the store does not hold has no steps");
    }

    // --- The mutation counter: the adapter's O(1) rebuild guard -----------------
    {
        TypedThreadStore store;
        store.turnUpsert(turn(1));
        const std::uint64_t v0 = store.version();
        store.blockUpsert(block(1, "b", 0, 0, "", "x"));
        check(store.version() > v0, "version() bumps on a mutation (the rebuild guard)");
        const std::uint64_t v1 = store.version();
        check(store.version() == v1, "version() is stable without mutations");
        store.threadClear();
        check(store.version() > v1, "threadClear bumps too: the adapter must rebuild");
        check(store.turnCount() == 0 && store.blockCount(1) == 0, "threadClear empties the store");
    }

    // --- Eviction: whole turns, oldest first, never the live one ----------------
    {
        TypedThreadStore store;
        const std::int64_t live = static_cast<std::int64_t>(TypedThreadStore::kMaxTurns) + 10;
        for (std::int64_t id = 1; id <= live; ++id) {
            store.turnUpsert(turn(id));
            store.blockUpsert(block(id, "b", 0, 0, "", "x"));
        }
        check(store.turnCount() == static_cast<std::size_t>(live),
              "the store grows past the cap before the evict call");
        store.evictForCapacity(live);
        check(store.turnCount() == TypedThreadStore::kMaxTurns, "the cap holds after evictForCapacity");
        check(store.turnById(1) == nullptr, "the oldest turn was evicted");
        check(store.blockCount(1) == 0, "the eviction cascaded to its blocks (W6)");
        check(store.turnById(live) != nullptr && store.blockCount(live) == 1,
              "the live turn is never evicted (W2), blocks and all");
    }

    // --- Removal: idempotent, and it cascades -----------------------------------
    {
        TypedThreadStore store;
        store.turnUpsert(turn(11));
        store.blockUpsert(block(11, "b", 0, 0, "", "x"));
        store.turnRemove(11);
        check(store.turnById(11) == nullptr && store.blockCount(11) == 0,
              "a turn remove cascades to its blocks");
        store.turnRemove(11);
        check(store.turnCount() == 0, "removing a turn that is gone clears nothing (idempotent)");
    }

    // --- The unknown-callId record: an implicit turn slot, then the real turn ---
    {
        TypedThreadStore store;
        // A result can beat its turn's snapshot onto the wire (§7): the block
        // lands under an implicit empty turn slot, never dropped.
        store.blockUpsert(block(55, "result-x", 2, 3, "grep", "..."));
        check(store.turnCount() == 1 && store.turnAt(0).id == 55 && store.turnAt(0).ordinal == 0,
              "a result for an unknown turn is recorded under an implicit empty slot");
        check(store.blockCount(55) == 1, "...and its block is addressable");
        // The real turn's upsert merges into that slot.
        store.turnUpsert(turn(55, 3, 1, 4));
        check(store.turnCount() == 1 && store.turnAt(0).ordinal == 3 && store.blockCount(55) == 1,
              "the real turn's upsert merges into the implicit slot");
    }

    // --- A snapshot is a wholesale replay ---------------------------------------
    {
        TypedThreadStore store;
        store.turnUpsert(turn(1));
        store.blockUpsert(block(1, "b", 0, 0, "", "x"));
        std::vector<TypedTurn> turns{turn(1, 1, 1, 2), turn(2, 2, 1, 1)};
        std::vector<TypedBlock> blocks{block(1, "b", 0, 0, "", "x"), block(2, "c", 0, 0, "", "y")};
        store.setTurns(std::move(turns), std::move(blocks));
        check(store.turnCount() == 2 && store.blockCount(1) == 1 && store.blockCount(2) == 1,
              "setTurns rebuilds the store wholesale");
        check(store.turnAt(0).messageCount == 2, "setTurns replaces a merge-marked turn with the snapshot's payload");
        // A block naming a turn the snapshot does not carry is recorded, not dropped.
        std::vector<TypedBlock> strays{block(99, "s", 0, 0, "", "z")};
        store.setTurns(std::vector<TypedTurn>{turn(1)}, std::move(strays));
        check(store.turnCount() == 2 && store.turnById(99) != nullptr && store.blockCount(99) == 1,
              "a block naming an unknown turn gets its implicit slot in a snapshot too");
    }

    // --- The typed-backed projection through the adapter ------------------------
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        thread.setSource(tui::store::ProjectionSource::TypedTurns);
        state.chat.typedTurns.turnUpsert(turn(42, 1, 1, 3));
        state.chat.typedTurns.blockUpsert(block(42, "q", 0, 0, "", "what is v5?"));
        state.chat.typedTurns.blockUpsert(
            block(42, "call-1", 2, 3, "grep", "call body", "", 1500));
        state.chat.typedTurns.blockUpsert(block(42, "hid", 2, 3, "pipeline_output", "secret"));

        check(thread.groupCount() == 1, "typed: one stored turn is one group");
        check(thread.groupAt(0).id == 42, "typed: the group id IS the persisted turnId (design §4)");
        check(thread.groupAt(0).kind == tui::store::GroupKind::AssistantWork,
              "typed: the group is a work group");
        check(thread.groupAt(0).oneLineSummary.find("WORK") == 0 &&
                  thread.groupAt(0).oneLineSummary.find("1 step") != std::string::npos,
              "typed: the header announces its step count (the hidden block weighs 0)");

        check(thread.blockCount(0) == 3, "typed: the hidden tool block is stored but not a row (D3)");
        const tui::model::BlockDesc header = thread.blockAt(0, 0);
        check(header.label == "WORK" && !header.hasBody && header.steps == 1,
              "typed: the header row announces the steps and has no body of its own");
        const tui::model::BlockDesc q = thread.blockAt(0, 1);
        check(q.label == "YOU" && q.kind == tui::model::BlockKind::User,
              "typed: the turn-opening prose is the query row");
        check(q.summary == "what is v5?" && q.hasBody, "typed: the query previews its body");
        const tui::model::BlockDesc tool = thread.blockAt(0, 2);
        check(tool.label == "TOOL" && tool.state == tui::model::BlockState::Ok,
              "typed: the merged tool card carries the wire status");
        check(tool.summary == "grep", "typed: the tool card previews its tool name");
        check(tool.timingS == 1.5, "typed: the wire duration becomes seconds (1500 ms -> 1.5)");

        // A rebuild observes the change: the revision moves, so a height hint
        // keyed on the old revision is stale (the v4 guard's job, typed).
        const std::uint64_t rev0 = thread.groupAt(0).revision;
        state.chat.typedTurns.blockUpsert(block(42, "call-1", 2, 3, "grep", "call body v2"));
        check(thread.groupAt(0).revision == rev0 + 1,
              "typed: a block merge bumps the group's revision (the hint guard sees it)");
        check(thread.blockCount(0) == 3, "typed: the merge stayed in its slot");

        // An unknown wire kind draws as a system notice, never as a neighbour's row.
        state.chat.typedTurns.blockUpsert(block(42, "odd", 9, 0, "", "mystery"));
        check(thread.blockCount(0) == 4, "typed: an unknown kind is still a row");
        check(thread.blockAt(0, 3).kind == tui::model::BlockKind::System &&
                  thread.blockAt(0, 3).label == "SYSTEM",
              "typed: an unknown wire kind draws as a system notice");

        // Expansion: the header shares the v4 work-group set, keyed by the
        // turnId; a row's disclosure lives in the adapter's own set. Nothing is
        // seeded - task 2's groups are born collapsed.
        check(!thread.expanded(42), "typed: a group is born collapsed (task 2 seeds no defaults)");
        thread.setExpanded(42, true);
        check(thread.expanded(42), "typed: setExpanded opens the header");
        check(state.chat.openWorkGroups.count(42) == 1,
              "typed: the header's bit lives in the shared work-group set, keyed by the turnId");
        check(!thread.blockExpanded(0, 2), "typed: a tool row is born collapsed");
        check(thread.setBlockExpanded(0, 2, true), "typed: opening a tool row reports open");
        check(thread.blockExpanded(0, 2), "typed: the row's disclosure took");
        thread.setBlockExpanded(0, 2, false);
        check(!thread.blockExpanded(0, 2), "typed: closing it clears the bit");

        // A second adapter bound to the same state answers the same thing where
        // the state is shared: the group header's bit lives in openWorkGroups,
        // which both adapters read through. A row's disclosure lives in the
        // adapter's own set, so the tool row this adapter never opened reads
        // closed - the row seam is per-adapter until the cutover.
        tui::store::ThreadStoreAdapter other(state);
        other.bind(state);
        other.setSource(tui::store::ProjectionSource::TypedTurns);
        check(other.groupCount() == 1 && other.groupAt(0).id == 42,
              "typed: a second adapter rebuilds the same groups from the store");
        check(other.blockAt(0, 2).label == "TOOL" && other.blockAt(0, 2).summary == "grep",
              "typed: ...and the same block view");
        check(other.expanded(42) && !other.blockExpanded(0, 2),
              "typed: the shared group bit answers through both adapters; a row's disclosure stays per-adapter");
    }

    // --- A gone turn takes its caches with it -----------------------------------
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        thread.setSource(tui::store::ProjectionSource::TypedTurns);
        state.chat.typedTurns.turnUpsert(turn(42));
        state.chat.typedTurns.blockUpsert(block(42, "q", 0, 0, "", "q"));
        check(thread.groupCount() == 1, "typed: the turn is a group");
        state.chat.typedTurns.turnRemove(42);
        check(thread.groupCount() == 0, "typed: the removed turn's group is gone");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    if (g_verbose)
        std::printf("all %d checks passed\n", g_checks);
    return 0;
}
