// test_typed_thread_store.cpp
//
// The typed thread store (store/typed_thread_store.h) and the typed-backed
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
#include "test/tui_state_handle.h"
#include "tui.h"
#include "tui_api.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
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
TypedTurn turn(std::int64_t id, int state = 1, int messageCount = 0) {
    TypedTurn t;
    t.id = id;
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

    // The upsert merge: a first upsert creates the slot, later ones merge
    {
        TypedThreadStore store;
        store.turnUpsert(turn(7, 1, 2));
        store.blockUpsert(block(7, "call-1", 2, 1, "grep", "call body")); // Pending
        check(store.turnCount() == 1 && store.blockCount(7) == 1, "first upserts create the slots");
        store.blockUpsert(block(7, "result-1", 2, 3, "grep", "result body")); // Ok
        check(store.blockCount(7) == 2, "a new callId is a new slot");
        // The merge: the same (turnId, id), the payload overwritten, the
        // position kept - a streamed call growing a result never reorders.
        store.blockUpsert(block(7, "call-1", 2, 3, "grep", "call body v2")); // Ok
        check(store.blockCount(7) == 2, "a merged block keeps its slot: no third row");
        check(store.blockAt(7, 0).status == 3 && store.blockAt(7, 0).body == "call body v2",
              "the merge overwrote the payload in place");
        check(store.blockAt(7, 1).id == "result-1",
              "the merge did not disturb the insertion order");
        // A turn upsert merges too: the slot (and with it any group id derived
        // from the insertion position) stays.
        store.turnUpsert(turn(7, 1, 5));
        check(store.turnCount() == 1 && store.turnAt(0).messageCount == 5,
              "a turn upsert merges in place");
        check(store.blockCount(7) == 2, "the turn merge left the blocks alone");
        // The store merges; it never enforces a transition - D7 is the
        // emitter's, and a settled result re-sent as Pending is merged as-is.
        store.blockUpsert(block(7, "call-1", 2, 1, "grep", "call body v2"));
        check(store.blockAt(7, 0).status == 1,
              "the store accepts Ok -> Pending (D7 is the emitter's)");
    }

    // Batch grouping: insertion order per turn, tool-less batches included
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
                  store.blockAt(11, 2).id == "11#1-call" &&
                  store.blockAt(11, 3).id == "11#1-result",
              "the blocks keep their upsert order: batch #1 before batch #2");
        check(store.blockAt(11, 0).batchId == "11#0" && store.blockAt(11, 3).batchId == "11#1",
              "the batchId rides the block");
    }

    // Displayability (D3) and the step-count weights (D4)
    {
        TypedBlock prose = block(1, "p", 0, 0);
        check(tui::store::blockDisplayable(prose), "non-Tool blocks are always displayable (D3)");
        TypedBlock tool = block(1, "t", 2, 3, "grep");
        check(tui::store::blockDisplayable(tool),
              "a Tool block without the Hidden flag is displayable");
        tool.flags |= tui::store::kBlockFlagHidden;
        check(!tui::store::blockDisplayable(tool),
              "a Tool block carrying TuiBlockFlag_Hidden is not (D3)");
        TypedBlock titled = block(1, "t2", 2, 3, "pipeline_output");
        check(tui::store::blockDisplayable(titled),
              "displayability is flag-only: the hidden NAME alone is displayable (D3)");

        TypedThreadStore store;
        store.turnUpsert(turn(100)); // hidden tool: weight 0
        TypedBlock hidden = block(100, "h", 2, 3, "anything", "...");
        hidden.flags |= tui::store::kBlockFlagHidden;
        store.blockUpsert(hidden);
        check(tui::store::stepCount(store, 100) == 0, "a hidden tool block weighs 0 (D3)");

        store.turnUpsert(turn(101)); // task_done's card + its Answer: 2 + 0
        store.blockUpsert(block(101, "td", 2, 3, "task_done", "the answer"));
        store.blockUpsert(block(101, "ans", 3, 0, "", "the answer"));
        check(tui::store::stepCount(store, 101) == 2,
              "task_done is rendered: its card weighs 2 and its Answer rides it (D3/D4)");

        store.turnUpsert(turn(104)); // a real run's shape: the config's verdict
        // flags task_done hidden, so its card weighs 0 and the Answer rides alone.
        TypedBlock flaggedDone = block(104, "td", 2, 3, "task_done", "the answer");
        flaggedDone.flags |= tui::store::kBlockFlagHidden;
        store.blockUpsert(flaggedDone);
        store.blockUpsert(block(104, "ans", 3, 0, "", "the answer"));
        check(tui::store::stepCount(store, 104) == 0,
              "a flagged task_done card weighs 0: the Answer rides alone");

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

        store.turnUpsert(turn(105));                             // tools in flight: 1 each
        store.blockUpsert(block(105, "p", 2, 1, "grep", "...")); // Pending
        store.blockUpsert(block(105, "g", 2, 2, "grep", "...")); // Running
        store.blockUpsert(block(105, "c", 2, 5, "grep", "...")); // Cancelled
        store.blockUpsert(block(105, "z", 2, 0, "grep", "...")); // None: a call the emitter started
        check(tui::store::stepCount(store, 105) == 4,
              "Pending/Running/Cancelled/None tools weigh 1 each (D4 + the None gap)");

        store.turnUpsert(turn(106));                               // settled tools: 2 each
        store.blockUpsert(block(106, "ok", 2, 3, "grep", "..."));  // Ok
        store.blockUpsert(block(106, "bad", 2, 4, "grep", "...")); // Failed
        check(tui::store::stepCount(store, 106) == 4,
              "Ok/Failed tools weigh 2 each: call + result (D4)");

        store.turnUpsert(turn(107)); // a result-only block (unknown callId)
        store.blockUpsert(block(107, "late-result", 2, 3, "grep", "..."));
        check(tui::store::stepCount(store, 107) == 2,
              "a result-only block is settled: it weighs 2, and the call merges in later (D4)");

        store.turnUpsert(turn(108)); // an unknown kind: defensive
        store.blockUpsert(block(108, "odd", 9, 0, "", "mystery"));
        check(tui::store::stepCount(store, 108) == 1,
              "an unknown kind weights 1: a surprise counts as work (D4)");

        check(tui::store::stepCount(store, 999) == 0,
              "a turn the store does not hold has no steps");
    }

    // The mutation counter: the adapter's O(1) rebuild guard
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

    // Eviction: whole turns, oldest first, never the live one
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
        check(store.turnCount() == TypedThreadStore::kMaxTurns,
              "the cap holds after evictForCapacity");
        check(store.turnById(1) == nullptr, "the oldest turn was evicted");
        check(store.blockCount(1) == 0, "the eviction cascaded to its blocks");
        check(store.turnById(live) != nullptr && store.blockCount(live) == 1,
              "the live turn is never evicted, blocks and all");
    }

    // Removal: idempotent, and it cascades
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

    // The unknown-callId record: an implicit turn slot, then the real turn
    {
        TypedThreadStore store;
        // A result can beat its turn's snapshot onto the wire: the block
        // lands under an implicit empty turn slot, never dropped.
        store.blockUpsert(block(55, "result-x", 2, 3, "grep", "..."));
        check(store.turnCount() == 1 && store.turnAt(0).id == 55,
              "a result for an unknown turn is recorded under an implicit empty slot");
        check(store.blockCount(55) == 1, "...and its block is addressable");
        // The real turn's upsert merges into that slot.
        store.turnUpsert(turn(55, 1, 4));
        check(store.turnCount() == 1 && store.turnAt(0).state == 1 && store.blockCount(55) == 1,
              "the real turn's upsert merges into the implicit slot");
    }

    // A snapshot is a wholesale replay
    {
        TypedThreadStore store;
        store.turnUpsert(turn(1));
        store.blockUpsert(block(1, "b", 0, 0, "", "x"));
        std::vector<TypedTurn> turns{turn(1, 1, 2), turn(2, 1, 1)};
        std::vector<TypedBlock> blocks{block(1, "b", 0, 0, "", "x"), block(2, "c", 0, 0, "", "y")};
        store.setTurns(std::move(turns), std::move(blocks));
        check(store.turnCount() == 2 && store.blockCount(1) == 1 && store.blockCount(2) == 1,
              "setTurns rebuilds the store wholesale");
        check(store.turnAt(0).messageCount == 2,
              "setTurns replaces a merge-marked turn with the snapshot's payload");
        // A block naming a turn the snapshot does not carry is recorded, not dropped.
        std::vector<TypedBlock> strays{block(99, "s", 0, 0, "", "z")};
        store.setTurns(std::vector<TypedTurn>{turn(1)}, std::move(strays));
        check(store.turnCount() == 2 && store.turnById(99) != nullptr && store.blockCount(99) == 1,
              "a block naming an unknown turn gets its implicit slot in a snapshot too");
    }

    // The typed-backed projection through the adapter
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.typedTurns.turnUpsert(turn(42, 1, 3));
        state.chat.typedTurns.blockUpsert(block(42, "q", 0, 0, "", "what is v5?", "42#q"));
        state.chat.typedTurns.blockUpsert(
            block(42, "call-1", 2, 3, "grep", "call body", "42#0", 1500));
        TypedBlock hid = block(42, "hid", 2, 3, "pipeline_output", "secret", "42#0");
        hid.flags |= tui::store::kBlockFlagHidden;
        state.chat.typedTurns.blockUpsert(hid);

        check(thread.groupCount() == 1, "typed: one stored turn is one group");
        check(thread.groupAt(0).id == 42, "typed: the group id IS the persisted turnId");
        check(thread.groupAt(0).kind == tui::store::GroupKind::AssistantWork,
              "typed: the group is a work group");
        check(thread.groupAt(0).oneLineSummary.find("WORK") == 0 &&
                  thread.groupAt(0).oneLineSummary.find("2 steps") != std::string::npos,
              "typed: the header announces its step count (the hidden block weighs 0, "
              "the settled tool carries call + result)");

        check(thread.blockCount(0) == 3,
              "typed: the query projects outside the group, the hidden tool block is "
              "stored but not a row, and the batch's owner row is a row");
        const tui::model::BlockDesc header = thread.blockAt(0, 0);
        check(header.label == "WORK" && !header.hasBody && header.steps == 2,
              "typed: the header row announces the steps and has no body of its own");
        const tui::model::BlockDesc q = thread.blockAt(0, tui::store::kQueryRow);
        check(q.group != 0 && q.label == "YOU" && q.kind == tui::model::BlockKind::User,
              "typed: the turn-opening prose is the query row, addressed turn-level");
        check(q.summary == "what is v5?" && q.hasBody,
              "typed: the query previews its body, no number prefix");
        const tui::model::BlockDesc owner = thread.blockAt(0, 1);
        check(owner.label == "AGENT" && owner.kind == tui::model::BlockKind::Assistant,
              "typed: the response's owner row is the synthetic AGENT");
        check(owner.timingS == 0.0, "typed: a batch without a content block carries no duration");
        const tui::model::BlockDesc tool = thread.blockAt(0, 2);
        check(tool.label == "TOOL" && tool.state == tui::model::BlockState::Ok,
              "typed: the merged tool card carries the wire status");
        check(tool.summary == "grep", "typed: the tool card previews its tool name");
        check(tool.timingS == 1.5, "typed: the wire duration becomes seconds (1500 ms -> 1.5)");

        // A rebuild observes the change: the revision moves, so a height hint
        // keyed on the old revision is stale (the guard's job, typed).
        const std::uint64_t rev0 = thread.groupAt(0).revision;
        state.chat.typedTurns.blockUpsert(
            block(42, "call-1", 2, 3, "grep", "call body v2", "42#0"));
        check(thread.groupAt(0).revision == rev0 + 1,
              "typed: a block merge bumps the group's revision (the hint guard sees it)");
        check(thread.blockCount(0) == 3, "typed: the merge stayed in its slot");

        // An unknown wire kind draws as a system notice, never as a neighbour's row.
        state.chat.typedTurns.blockUpsert(block(42, "odd", 9, 0, "", "mystery", "42#0"));
        check(thread.blockCount(0) == 4, "typed: an unknown kind is still a row");
        check(thread.blockAt(0, 3).kind == tui::model::BlockKind::System &&
                  thread.blockAt(0, 3).label == "SYSTEM",
              "typed: an unknown wire kind draws as a system notice");

        // Expansion: the header's fold and a row's disclosure are the
        // adapter's derived predicate - both read
        // the user's absolute pins. Nothing is seeded - the groups are born
        // on the mode's liveness default, and a Done turn's default is folded.
        check(!thread.expanded(42),
              "typed: a group is born collapsed (a Done turn holds nothing open)");
        check(!thread.blockExpanded(0, 2),
              "typed: a tool row is born collapsed (the group is folded)");
        check(!thread.blockAt(0, 2).visible,
              "typed: the folded group's rows are invisible (the adapter owns visibility, B1)");
        check(thread.setBlockExpanded(0, 2, true), "typed: opening a tool row reports open");
        check(thread.blockExpanded(0, 2), "typed: the row's pin took");
        thread.setBlockExpanded(0, 2, false);
        check(!thread.blockExpanded(0, 2), "typed: closing it stores closed");

        // A second adapter bound to the same state answers the same thing:
        // every pin lives in the shared ChatTab maps, keyed by the
        // persisted turn id - no per-adapter expansion state survives.
        tui::store::ThreadStoreAdapter other(state);
        other.bind(state);
        check(other.groupCount() == 1 && other.groupAt(0).id == 42,
              "typed: a second adapter rebuilds the same groups from the store");
        check(other.blockAt(0, 2).label == "TOOL" && other.blockAt(0, 2).summary == "grep",
              "typed: ...and the same block view");
        check(other.expanded(42) == thread.expanded(42) && !other.expanded(42),
              "typed: the shared group pin answers through both adapters");
        check(other.setBlockExpanded(0, 2, true) && thread.blockExpanded(0, 2),
              "typed: a row's pin is shared state, written through either adapter");

        // Opening the group pins it open: the header draws open - and a
        // settled batch inside a re-opened group shows its owner folded -
        // ownerOpen = pin ?? batchLive, a Done turn's batchLive is false).
        thread.setExpanded(42, true);
        check(thread.expanded(42) && other.expanded(42), "typed: setExpanded pins the header open");
        check(state.chat.groupPins.count(42) == 1 && state.chat.groupPins.at(42),
              "typed: the group pin lives in the shared map, keyed by the turnId");
        check(thread.blockAt(0, 0).visible && thread.blockExpanded(0, 0),
              "typed: the pinned group's owner row draws, folded (a settled batch)");
        check(!thread.blockAt(0, 2).visible, "typed: the folded owner keeps its children hidden");

        // The header row toggles through the SAME map `setExpanded()` uses, so
        // the two seams agree on the group header by construction: close it
        // through the row seam and the group seam reads closed, then reopen.
        thread.setBlockExpanded(0, 0, false);
        check(!thread.expanded(42),
              "typed: the row seam's header toggle lands in the group seam's map");
        check(thread.setBlockExpanded(0, 0, true),
              "typed: reopening the header through the row seam reports open");
        check(thread.blockExpanded(0, 0) && thread.expanded(42),
              "typed: ...and both seams read the header open again");
    }

    // The query row: meta decides, position falls back
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.typedTurns.turnUpsert(turn(42, 1, 3));
        // The pipeline's shape: the streamed assistant content is the FIRST
        // prose, meta carries the agent role, and the tool batches follow.
        auto content = block(42, "content#0", 0, 0, "", "working on it");
        content.meta = "assistant";
        state.chat.typedTurns.blockUpsert(content);
        state.chat.typedTurns.blockUpsert(
            block(42, "call-1", 2, 3, "edit_file", "call body", "", 1500));

        check(thread.groupAt(0).oneLineSummary.find("2 steps") != std::string::npos,
              "typed pipeline: the assistant prose is work, the header counts the settled tool");
        check(thread.blockAt(0, 2).label == "AGENT" &&
                  thread.blockAt(0, 2).kind == tui::model::BlockKind::Assistant,
              "typed pipeline: the streamed assistant prose is an AGENT row, never YOU "
              "(it sits behind its batch's owner row)");

        // The main tab's shape: the query prose carries the meta. It is the
        // query row wherever it sits in the turn.
        auto query = block(42, "q", 0, 0, "", "what is v5?");
        query.meta = "user";
        state.chat.typedTurns.blockUpsert(query);
        check(thread.blockAt(0, 1).label == "AGENT",
              "typed main: the streamed assistant prose stays an AGENT row beside the query");
        check(thread.blockAt(0, tui::store::kQueryRow).label == "YOU" &&
                  thread.blockAt(0, tui::store::kQueryRow).kind == tui::model::BlockKind::User,
              "typed main: the meta-user prose is the query row wherever it sits");

        // A turn with no meta at all keeps the pre-meta shape: the first prose
        // is the query row.
        tui::TuiState legacy;
        tui::store::ThreadStoreAdapter legacyThread(legacy);
        legacyThread.bind(legacy);
        legacy.chat.typedTurns.turnUpsert(turn(7, 1, 2));
        legacy.chat.typedTurns.blockUpsert(block(7, "q", 0, 0, "", "what is v5?"));
        check(legacyThread.blockAt(0, tui::store::kQueryRow).label == "YOU" &&
                  legacyThread.blockAt(0, tui::store::kQueryRow).kind ==
                      tui::model::BlockKind::User,
              "typed legacy: a meta-less first prose is still the query row");
    }

    // The mode matrix and the absolute pins
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.typedTurns.turnUpsert(turn(42, 1, 3)); // Done: never live
        state.chat.typedTurns.blockUpsert(
            block(42, "q", 0, 0, "", "what is v5?")); // the query row (batchId "")
        state.chat.typedTurns.blockUpsert(block(42, "th", 1, 1, "", "hmm", "b0"));
        state.chat.typedTurns.blockUpsert(block(42, "content#0", 0, 0, "", "working on it", "b0"));
        state.chat.typedTurns.blockUpsert(
            block(42, "call-1", 2, 2, "grep", "call body", "b0", 1500)); // in flight
        // The layout: the header, the batch's owner row, then the children in
        // The batch's order (reasoning, content, calls), then the plain notice. The
        // answer projects as the turn-level row outside the group.
        state.chat.typedTurns.blockUpsert(block(42, "ans", 3, 1, "", "done"));
        state.chat.typedTurns.blockUpsert(block(42, "note", 4, 1, "", "heads up"));
        check(thread.blockAt(0, 0).kind == tui::model::BlockKind::WorkGroup &&
                  thread.blockAt(0, 1).label == "AGENT" && thread.blockAt(0, 1).ownsBatch &&
                  thread.blockAt(0, 2).label == "Thinking" &&
                  thread.blockAt(0, 3).label == "AGENT" && thread.blockAt(0, 4).label == "TOOL" &&
                  thread.blockAt(0, 5).label == "SYSTEM" &&
                  thread.blockAt(0, tui::store::kQueryRow).label == "YOU" &&
                  thread.blockAt(0, tui::store::kAnswerRow).label == "ANSWER",
              "the fixture projects as header, owner, children, notice, query, answer");

        // MINIMAL, settled (the factory cell): born folded, minimal-style; the
        // query and the answer draw beside the folded header, nothing else does.
        check(state.chat.threadMinimalMode && !thread.groupOpen(42) &&
                  thread.groupStyleIsMinimal(42),
              "minimal + settled folds the group, minimal-style");
        check(thread.blockAt(0, 0).visible && !thread.blockAt(0, 0).expanded,
              "...the header draws folded");
        check(!thread.blockAt(0, 1).visible && !thread.blockAt(0, 2).visible &&
                  !thread.blockAt(0, 3).visible && !thread.blockAt(0, 4).visible &&
                  !thread.blockAt(0, 5).visible,
              "...the owner, the children and the notice draw nothing");
        check(thread.blockAt(0, tui::store::kQueryRow).visible &&
                  thread.blockAt(0, tui::store::kAnswerRow).visible,
              "...the query and the answer draw in both modes");
        check(thread.blockAt(0, tui::store::kQueryRow).expanded &&
                  thread.blockAt(0, tui::store::kAnswerRow).expanded,
              "...and the turn-level rows are never folded state");

        // MAXIMAL, settled: still folded (a settled group folds in both modes)
        // but the SHAPE is maximal - never the minimal chips.
        state.chat.threadMinimalMode = false;
        check(!thread.groupOpen(42) && !thread.groupStyleIsMinimal(42),
              "maximal + settled folds the group, maximal-style");
        check(!thread.blockAt(0, 1).visible, "...the owner still draws nothing");

        // MAXIMAL, live: the one cell whose DEFAULT is open - the liveness
        // default, not a pin: the map stays empty.
        state.chat.typedTurns.turnUpsert(turn(42, 0, 3)); // Running
        check(thread.groupOpen(42) && !thread.groupStyleIsMinimal(42) &&
                  state.chat.groupPins.empty(),
              "maximal + live opens the group on its own, no pin stored");
        check(thread.blockAt(0, 1).visible && thread.blockAt(0, 1).expanded,
              "...the live batch's owner draws open");
        check(thread.blockAt(0, 2).visible && thread.blockAt(0, 2).expanded &&
                  thread.blockAt(0, 3).visible && thread.blockAt(0, 3).expanded &&
                  thread.blockAt(0, 4).visible && thread.blockAt(0, 4).expanded,
              "...the in-flight children draw open with it");
        check(thread.blockAt(0, 5).visible,
              "...and the notice draws with the open group (plainVisible = groupOpen)");

        // MINIMAL, live: folded and minimal-style, but the streamed content
        // segment draws on its own - never the owner or the cards.
        state.chat.threadMinimalMode = true;
        check(!thread.groupOpen(42) && thread.groupStyleIsMinimal(42),
              "minimal + live keeps the group folded, minimal-style");
        check(!thread.blockAt(0, 1).visible && !thread.blockAt(0, 2).visible &&
                  !thread.blockAt(0, 4).visible,
              "...the owner, the reasoning and the call draw nothing");
        check(thread.blockAt(0, 3).visible, "...the live content segment draws with no owner row");

        // The leaked Running call: a Done turn holds nothing open even with a
        // call still in flight - turnLive gates every liveness default.
        state.chat.typedTurns.turnUpsert(turn(42, 1, 3)); // Done again
        state.chat.threadMinimalMode = false;
        check(!thread.groupOpen(42) && !thread.blockAt(0, 1).expanded &&
                  !thread.blockAt(0, 4).expanded,
              "a leaked Running call in a Done turn holds nothing open");

        // The row pin: a click stored closed stays closed across the
        // mode flip AND the liveness change, then a click stored open stays
        // open where the liveness default is fold. Absolute, always stored.
        state.chat.typedTurns.turnUpsert(turn(42, 0, 3)); // live again
        check(!thread.setBlockExpanded(0, 4, false),
              "closing the in-flight card in the live maximal group reports closed");
        check(!thread.blockAt(0, 4).expanded, "the card's pin took");
        check(state.chat.rowPins.count(42) == 1 && state.chat.rowPins.at(42).at("call-1") == false,
              "the row pin lives in the shared map, keyed by the wire block id");

        // B2, in a group the user re-opened: pin the folded minimal group
        // open, then pin its settled owner open - the owner draws open, the
        // content draws under it folded, the reasoning and the card closed.
        state.chat.threadMinimalMode = true;
        state.chat.typedTurns.turnUpsert(turn(42, 1, 3)); // settled
        thread.setExpanded(42, true);                     // the group pin: open in minimal
        check(thread.groupOpen(42) && !thread.groupStyleIsMinimal(42),
              "the group pinned open in minimal draws maximal-style");
        check(thread.blockAt(0, 1).visible && !thread.blockAt(0, 1).expanded,
              "the group draws its owner, folded (the settled batch)");
        check(thread.setBlockExpanded(0, 1, true), "pinning the owner open reports open");
        check(thread.blockAt(0, 1).visible && thread.blockAt(0, 1).expanded,
              "the owner's pin draws it open in the pinned-open group");
        check(thread.blockAt(0, 3).visible && !thread.blockAt(0, 3).expanded,
              "...the settled content draws under it, folded");
        check(thread.blockAt(0, 2).visible && !thread.blockAt(0, 2).expanded &&
                  thread.blockAt(0, 4).visible && !thread.blockAt(0, 4).expanded,
              "...the reasoning and the card draw closed under the open owner");
        state.chat.threadMinimalMode = false;
        check(thread.blockAt(0, 1).visible && thread.blockAt(0, 1).expanded &&
                  thread.blockAt(0, 4).visible && !thread.blockAt(0, 4).expanded,
              "the pins survive the flip to maximal - open owner, closed card");
        check(thread.setBlockExpanded(0, 4, true) && thread.blockAt(0, 4).expanded,
              "reopening the card stores open over the closed pin");

        // segmentVisible: a live batch whose owner the user closed hides its
        // content segment too - the minimal-only clause does not rescue a
        // maximal group's closed owner.
        state.chat.typedTurns.turnUpsert(turn(42, 0, 3)); // live
        state.chat.threadMinimalMode = false;
        check(!thread.setBlockExpanded(0, 1, false), "closing the live owner reports closed");
        check(!thread.blockAt(0, 1).expanded && !thread.blockAt(0, 3).visible,
              "the closed owner hides its live segment as well (segmentVisible)");
        check(!thread.blockAt(0, 2).visible && !thread.blockAt(0, 4).visible,
              "...and its reasoning and card (childVisible = ownerVisible && ownerOpen)");

        // The STYLE is the fold, not the mode. A minimal group pinned
        // open draws maximal-style; a folded maximal group never reports the
        // minimal style.
        check(!thread.groupStyleIsMinimal(42), "the maximal group pinned open is maximal-style");
        state.chat.threadMinimalMode = true;
        state.chat.typedTurns.turnUpsert(turn(42, 1, 3)); // settled: the default folds
        state.chat.groupPins.erase(42);                   // drop the group pin: the default answers
        check(thread.groupStyleIsMinimal(42), "the settled minimal group is minimal-style");
        thread.setExpanded(42, true);
        check(!thread.groupStyleIsMinimal(42), "the minimal group pinned open draws maximal-style");
        check(thread.blockAt(0, 1).visible, "...the pinned-open minimal group draws its owner");
        thread.setExpanded(42, false);
        check(thread.groupStyleIsMinimal(42) && !thread.blockAt(0, 1).visible,
              "...and pinning it closed restores the minimal fold");

        // The folded group of either mode keeps the query and the ANSWER but
        // not the notice (the turn-level rows vs the group-level plain row).
        check(thread.blockAt(0, tui::store::kQueryRow).visible &&
                  thread.blockAt(0, tui::store::kAnswerRow).visible &&
                  !thread.blockAt(0, 5).visible,
              "the folded group keeps the query and the answer, drops the notice");
        state.chat.threadMinimalMode = false;
        check(thread.blockAt(0, tui::store::kQueryRow).visible &&
                  thread.blockAt(0, tui::store::kAnswerRow).visible &&
                  !thread.blockAt(0, 5).visible,
              "the same in maximal - the fold, not the mode, decides");
    }

    // FR-4: the thinking body is visible while it streams. A live turn's newest
    // batch holds no call yet while the model thinks - its children draw open
    // with their owner, never folded behind a call that has not arrived.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.threadMinimalMode = false; // maximal: the liveness default is the question
        state.chat.typedTurns.turnUpsert(turn(52, 0, 0)); // Running
        auto query = block(52, "q", 0, 0, "", "what is v5?", "52#q");
        query.meta = "user";
        state.chat.typedTurns.blockUpsert(query);
        // The response so far: reasoning and streamed content, no call yet.
        state.chat.typedTurns.blockUpsert(
            block(52, "think#0", 1, 0, "", "reading the tree", "52#0"));
        state.chat.typedTurns.blockUpsert(
            block(52, "content#0", 0, 0, "", "I will list it", "52#0"));

        check(thread.groupOpen(52), "a live maximal turn opens its group");
        // Rows: [header, owner, thinking, content segment].
        check(thread.blockAt(0, 1).ownsBatch && thread.blockAt(0, 1).expanded,
              "the call-less newest batch's owner draws open");
        check(thread.blockAt(0, 2).kind == tui::model::BlockKind::Thinking &&
                  thread.blockAt(0, 2).visible && thread.blockAt(0, 2).expanded,
              "the streaming Thinking row draws open - no call has landed yet");
        check(thread.blockAt(0, 3).bodySegment && thread.blockAt(0, 3).visible,
              "the streamed content segment draws under the open owner");

        // A call landing does not fold the thinking: the batch is live until
        // every call settles.
        state.chat.typedTurns.blockUpsert(
            block(52, "call#0", 2, 1, "bash", "ls", "52#0")); // Pending
        check(thread.blockAt(0, 2).expanded, "the thinking stays open while the call runs");
        state.chat.typedTurns.blockUpsert(block(52, "call#0", 2, 3, "bash", "ls", "52#0")); // Ok
        check(!thread.blockAt(0, 2).expanded,
              "the settled batch folds its thinking with its owner");
    }
    // The entry points reset the pins
    {
        TuiState* state = tuiCreateState();
        check(state != nullptr && state->inner != nullptr,
              "tuiCreateState succeeds for the pin cases");
        auto wstr = [](const char* s) { return String{s, std::strlen(s)}; };
        TuiTurn wt{};
        wt.id = 77;
        wt.state = 1;
        wt.messageCount = 1;
        TuiBlock wb{};
        wb.turnId = 77;
        wb.id = wstr("q");
        wb.kind = 0;
        wb.status = 1;
        tuiSetTurns(state, &wt, 1, &wb, 1);
        auto& groupPins = state->inner->chat.groupPins;
        auto& rowPins = state->inner->chat.rowPins;
        // The clear must run on maps that hold something: on a
        // fresh state the empty maps would make the asserts vacuous.
        groupPins[77] = false;
        rowPins[77]["q"] = true;
        tuiSetTurns(state, &wt, 1, &wb, 1);
        check(groupPins.empty() && rowPins.empty(), "a session switch leaves no pin behind");
        groupPins[77] = false;
        rowPins[77]["q"] = true;
        // A compression re-emit (turnRemove + re-upsert) drops the turn's pins.
        tuiTurnRemove(state, 77);
        check(groupPins.count(77) == 0 && rowPins.count(77) == 0,
              "the turn's pins die with the turn");
        groupPins[77] = true;
        rowPins[77]["q"] = true;
        tuiThreadClear(state);
        check(groupPins.empty() && rowPins.empty(), "a clear empties both maps");
        tuiSetTurns(state, &wt, 1, &wb, 1);
        tuiDestroyState(state);
    }
    // The upsert prunes dead pins, and `collapseAll` forgets them all
    {
        TuiState* state = tuiCreateState();
        check(state != nullptr && state->inner != nullptr,
              "tuiCreateState succeeds for the prune cases");
        auto& groupPins = state->inner->chat.groupPins;
        auto& rowPins = state->inner->chat.rowPins;
        TuiTurn wt{};
        wt.state = 1;
        wt.messageCount = 1;
        wt.reserved = 0;
        wt.id = 1;
        tuiTurnUpsert(state, &wt);
        // Pin the oldest turn before it is the one capacity evicts.
        groupPins[1] = true;
        rowPins[1]["q"] = true;
        for (std::int64_t id = 2; id <= static_cast<std::int64_t>(TypedThreadStore::kMaxTurns) + 1;
             ++id) {
            wt.id = id;
            tuiTurnUpsert(state, &wt);
        }
        check(state->inner->chat.typedTurns.turnCount() == TypedThreadStore::kMaxTurns,
              "the upserts filled the window to the cap");
        check(groupPins.count(1) == 0 && rowPins.count(1) == 0,
              "the evicted turn's pins are pruned by the upsert");
        // The pins of a turn the store still holds survive the same upserts.
        groupPins[TypedThreadStore::kMaxTurns] = true;
        rowPins[TypedThreadStore::kMaxTurns]["q"] = true;
        wt.id = static_cast<std::int64_t>(TypedThreadStore::kMaxTurns);
        tuiTurnUpsert(state, &wt);
        check(groupPins.count(wt.id) == 1 && rowPins.count(wt.id) == 1,
              "a live turn's pins survive its own upsert");
        tuiDestroyState(state);
    }
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.typedTurns.turnUpsert(turn(42));
        state.chat.threadMinimalMode = true; // the default folds: what a cleared pin falls back to
        thread.setExpanded(42, true);
        state.chat.rowPins[42]["q"] = true;
        check(thread.expanded(42), "the pinned group draws open");
        thread.collapseAll();
        check(state.chat.groupPins.empty() && state.chat.rowPins.empty(),
              "collapseAll forgets every pin");
        check(!thread.expanded(42), "...so the next frame draws on the mode default, folded");
        check(state.chat.groupPins.empty() && state.chat.rowPins.empty(),
              "collapseAll clears rather than writes - no pin against the default");
        tui::store::ThreadStoreAdapter unbound;
        unbound.collapseAll(); // a no-op, never a crash
        check(!unbound.bound(), "collapseAll on an unbound adapter is a no-op");
    }

    // A gone turn takes its caches with it
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.typedTurns.turnUpsert(turn(42));
        state.chat.typedTurns.blockUpsert(block(42, "q", 0, 0, "", "q"));
        check(thread.groupCount() == 1, "typed: the turn is a group");
        state.chat.typedTurns.turnRemove(42);
        check(thread.groupCount() == 0, "typed: the removed turn's group is gone");
    }

    // The five entry points: the C API surface over the same store
    // Full behavioral observation waits for the read-side snapshot API;
    // here the surface is exercised and odr-used - a missing definition is a
    // link error in this binary too - and the inert-argument contract is pinned.
    {
        TuiState* state = tuiCreateState();
        check(state != nullptr, "api: tuiCreateState succeeds for the typed cases");

        auto wstr = [](const char* s) { return String{s, std::strlen(s)}; };
        TuiTurn wt{};
        wt.id = 77;
        wt.state = 1;
        wt.messageCount = 2;
        wt.reserved = 0;
        TuiBlock wb{};
        wb.turnId = 77;
        wb.id = wstr("call-1");
        wb.batchId = wstr("");
        wb.title = wstr("grep");
        wb.meta = wstr("");
        wb.body = wstr("call body");
        wb.durationMs = 1500;
        wb.tokensIn = 0;
        wb.tokensOut = 0;
        wb.kind = 2;   // TuiBlockKind_Tool
        wb.status = 1; // TuiBlockStatus_Pending
        tuiSetTurns(state, &wt, 1, &wb, 1);
        tuiTurnUpsert(state, &wt);
        tuiBlockUpsert(state, &wb);
        tuiTurnRemove(state, 77);
        tuiThreadClear(state);
        check(true, "api: the five entry points accept a valid state (and link)");

        // The inert-argument contract, same as the other entry points: a null
        // state or a null element is a no-op, never a crash.
        tuiSetTurns(nullptr, &wt, 1, &wb, 1);
        tuiSetTurns(state, nullptr, 1, nullptr, 0);
        tuiTurnUpsert(nullptr, &wt);
        tuiTurnUpsert(state, nullptr);
        tuiBlockUpsert(nullptr, &wb);
        tuiBlockUpsert(state, nullptr);
        tuiTurnRemove(nullptr, 77);
        tuiThreadClear(nullptr);
        check(true, "api: a null state or a null element is an inert no-op");
        tuiDestroyState(state);
    }

    // The upsert storm + the result-for-unknown-callId merge
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter view(state);
        view.bind(state);
        TypedThreadStore& store = state.chat.typedTurns;
        const std::uint64_t vBegin = store.version();

        // A. The upsert storm: thousands of content ticks and call/result
        // merges leave the store consistent and the render correct. The
        // no-re-parse/no-re-scan property is structural - every slot stays a
        // slot (merges never duplicate), the last write wins, and version()
        // bumps exactly once per mutating call - never a timing gate.
        store.turnUpsert(turn(900, 1, 0));
        for (int i = 0; i < 1500; ++i)
            store.blockUpsert(
                block(900, "content#0", 0, 0, "assistant", "tick " + std::to_string(i)));
        for (int i = 0; i < 300; ++i) {
            store.blockUpsert(block(900, "call-1", 2, 1, "grep", "args")); // Pending
            store.blockUpsert(
                block(900, "call-1", 2, 3, "grep", "result " + std::to_string(i))); // merged Ok
        }
        check(store.turnCount() == 1 && store.blockCount(900) == 2,
              "storm: thousands of upserts never duplicate a slot");
        check(store.blockAt(900, 0).body == "tick 1499",
              "storm: the streamed tick's last write wins");
        check(store.blockAt(900, 1).body == "result 299",
              "storm: the merged call/result's last write wins");
        check(store.version() - vBegin == 1 + 1500 + 600,
              "storm: one version bump per mutating call - no hidden re-parse, no rescan");
        // The render is correct: one group, and its header announces the
        // settled step count (the first prose is the turn-opening query, so
        // the ticks weigh 0; the merged Ok tool weighs 2).
        check(view.groupCount() == 1, "storm: one store turn is one group");
        check(view.groupAt(0).oneLineSummary.find("WORK") == 0 &&
                  view.groupAt(0).oneLineSummary.find("2 steps") != std::string::npos,
              "storm: the group header announces the settled step count");

        // B. A result for an unknown callId (live): the result lands under the
        // call's id - an implicit slot, never dropped - then the call's
        // upsert merges in and ONE settled step renders (Ok status, weight 2).
        store.blockUpsert(block(77, "call-2", 2, 3, "grep", "the result text"));
        check(store.turnCount() == 2 && store.blockCount(77) == 1,
              "storm: a result for an unknown callId is recorded under its callId");
        check(store.blockAt(77, 0).status == 3,
              "storm: the result-only block carries its Ok outcome");
        store.blockUpsert(block(77, "call-2", 2, 3, "grep", "the call arguments"));
        check(store.blockCount(77) == 1,
              "storm: the call's upsert merges into the result's slot - one step, never two");
        check(store.blockAt(77, 0).status == 3 && store.blockAt(77, 0).body == "the call arguments",
              "storm: the merged step renders Ok with the call's arguments as the body");
        store.turnUpsert(turn(77, 1, 2));
        check(tui::store::stepCount(store, 77) == 2,
              "storm: the merged step weighs 2 (call + result), never a dropped half");
    }

    // The v13 result pair transport: the store's merge-by-presence
    {
        TypedThreadStore store;
        store.turnUpsert(turn(50, 1, 1));
        TypedBlock withPairs = block(50, "call-1", 2, 3, "grep", "the result text");
        withPairs.resultPairs = {{"path", "a.txt"}, {"status", "ok"}};
        withPairs.resultRaw = "{\"path\":\"a.txt\",\"status\":\"ok\"}";
        store.blockUpsert(withPairs);
        check(store.blockAt(50, 0).resultPairs.size() == 2 &&
                  store.blockAt(50, 0).resultPairs[0].first == "path" &&
                  store.blockAt(50, 0).resultPairs[0].second == "a.txt" &&
                  store.blockAt(50, 0).resultPairs[1].first == "status" &&
                  store.blockAt(50, 0).resultPairs[1].second == "ok" &&
                  store.blockAt(50, 0).resultRaw == "{\"path\":\"a.txt\",\"status\":\"ok\"}" &&
                  store.blockAt(50, 0).resultMore == 0,
              "transport: an upsert carrying a pair set stores pairs and raw verbatim");

        // A later upsert with its own pair set replaces the result fields
        // together - never half old, half new, and the more-count flips
        // with them.
        TypedBlock reResult = block(50, "call-1", 2, 3, "grep", "the result text v2");
        reResult.resultPairs = {{"status", "failed"}};
        reResult.resultRaw = "{\"status\":\"failed\"}";
        reResult.resultMore = 7;
        store.blockUpsert(reResult);
        check(store.blockAt(50, 0).resultPairs.size() == 1 &&
                  store.blockAt(50, 0).resultPairs[0].first == "status" &&
                  store.blockAt(50, 0).resultPairs[0].second == "failed" &&
                  store.blockAt(50, 0).resultRaw == "{\"status\":\"failed\"}" &&
                  store.blockAt(50, 0).resultMore == 7,
              "transport: a pair-carrying merge replaces all result fields together");

        // A pair-less upsert (a call payload re-emit) merges the regular
        // payload but keeps what the slot already holds - it must not clear
        // a result that already landed, more-count included.
        store.blockUpsert(block(50, "call-1", 2, 3, "grep", "the call arguments"));
        check(store.blockAt(50, 0).body == "the call arguments",
              "transport: the pair-less merge still overwrites the regular payload");
        check(store.blockAt(50, 0).resultPairs.size() == 1 &&
                  store.blockAt(50, 0).resultPairs[0].second == "failed" &&
                  store.blockAt(50, 0).resultRaw == "{\"status\":\"failed\"}" &&
                  store.blockAt(50, 0).resultMore == 7,
              "transport: the pair-less merge preserves the existing pair set");

        // A pair-less first upsert simply leaves the transport empty (the
        // pending call before its result lands).
        store.blockUpsert(block(50, "call-2", 2, 1, "sed", "args"));
        check(store.blockAt(50, 1).resultPairs.empty() && store.blockAt(50, 1).resultRaw.empty(),
              "transport: a pending call starts with an empty transport");
        // The storm variant: a pair-carrying upsert repeated per tick replaces
        // the transport together every time - the pair set is part of the
        // merged block and never grows per frame.
        const std::uint64_t vPairs = store.version();
        for (int i = 0; i < 300; ++i) {
            TypedBlock tick = block(50, "call-1", 2, 3, "grep", "result " + std::to_string(i));
            tick.resultPairs = {{"path", "a.txt"}, {"status", "ok"}};
            tick.resultRaw = "{\"path\":\"a.txt\",\"status\":\"ok\"}";
            tick.resultMore = 3;
            store.blockUpsert(tick);
        }
        check(store.blockAt(50, 0).resultPairs.size() == 2 &&
                  store.blockAt(50, 0).resultPairs[0].first == "path" &&
                  store.blockAt(50, 0).resultPairs[0].second == "a.txt" &&
                  store.blockAt(50, 0).resultPairs[1].second == "ok" &&
                  store.blockAt(50, 0).resultRaw == "{\"path\":\"a.txt\",\"status\":\"ok\"}" &&
                  store.blockAt(50, 0).resultMore == 3,
              "storm: a pair-carrying upsert repeated per tick never grows the pair set");
        check(store.version() - vPairs == 300,
              "storm: one version bump per pair-carrying upsert - no rescan");
    }

    // Turn order. Live turns order by the store's insertion order; replayed
    // turns order by the snapshot's history sequence - never by the ids: the
    // projection must not re-derive a sort from the turn ids.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter view(state);
        view.bind(state);
        TypedThreadStore& store = state.chat.typedTurns;

        // A. Live: three turns upserted in conversation order, ids chosen
        // NON-ascending - the upsert order is the order, the ids say nothing.
        store.turnUpsert(turn(40, 0, 1)); // Running: the live turn
        store.turnUpsert(turn(12, 0, 1));
        store.turnUpsert(turn(9, 1, 1)); // Done
        check(view.groupCount() == 3 && view.groupAt(0).id == 40 && view.groupAt(1).id == 12 &&
                  view.groupAt(2).id == 9,
              "typed order: live turn order is the upsert order, not the ids'");

        // B. A compression re-emission: the emitter re-upserts the turns it
        // still owns (a state flip per turn). A merge keeps its slot - no
        // append, no reorder, no duplicate group.
        store.turnUpsert(turn(40, 1, 1)); // the live turn settled
        store.turnUpsert(turn(12, 1, 1));
        check(view.groupCount() == 3 && view.groupAt(0).id == 40 && view.groupAt(1).id == 12 &&
                  view.groupAt(2).id == 9,
              "typed order: a compression re-upsert merges in place - order held");

        // C. An emitter re-attach after a session switch: the whole snapshot
        // re-emitted through the bulk path, same conversation order. The
        // snapshot's array order IS the projection order - the store must
        // not sort it, and each turn's blocks keep the snapshot's order too.
        std::vector<TypedTurn> snapshotTurns;
        std::vector<TypedBlock> snapshotBlocks;
        for (const std::int64_t id : {9, 12, 40}) { // the history's own order
            snapshotTurns.push_back(turn(id, 1, 1));
            snapshotBlocks.push_back(block(id, "q", 0, 0, "user", "query " + std::to_string(id)));
            snapshotBlocks.push_back(block(id, "answer", 3, 0, "", "answer " + std::to_string(id)));
        }
        store.threadClear();
        store.setTurns(std::move(snapshotTurns), std::move(snapshotBlocks));
        check(view.groupCount() == 3 && view.groupAt(0).id == 9 && view.groupAt(1).id == 12 &&
                  view.groupAt(2).id == 40,
              "typed order: the re-attached snapshot keeps the history order");
        check(view.blockText(0, tui::store::kQueryRow) == "query 9" &&
                  view.blockText(0, tui::store::kAnswerRow) == "answer 9" &&
                  view.blockText(2, tui::store::kQueryRow) == "query 40" &&
                  view.blockText(2, tui::store::kAnswerRow) == "answer 40",
              "typed order: the snapshot's per-turn block order is the projection's");
        // The Answer sentinel's presence is the GroupView flag the palette's
        // copy-answer stamp reads: a turn with an answer block answers true.
        check(view.groupAt(0).hasAnswer && view.groupAt(2).hasAnswer,
              "typed order: GroupView.hasAnswer tracks the Answer sentinel");
    }

    // The projection buckets the response batches
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.typedTurns.turnUpsert(turn(42, 1, 5));
        // The query rides the query batch; a notice rides along with it -
        // the batch of the query row is not a response batch, ids are opaque).
        auto query = block(42, "q", 0, 0, "", "what is v5?", "42#q");
        query.meta = "user";
        state.chat.typedTurns.blockUpsert(query);
        state.chat.typedTurns.blockUpsert(block(42, "notice", 4, 0, "", "a notice", "42#q"));
        // Three response batches: 42#0 (reasoning + content + call), 42#1
        // (content + call), 42#2 (a call alone).
        state.chat.typedTurns.blockUpsert(block(42, "think#0", 1, 3, "", "reasoning", "42#0"));
        state.chat.typedTurns.blockUpsert(
            block(42, "content#0", 0, 3, "", "working", "42#0", 1200));
        state.chat.typedTurns.blockUpsert(block(42, "call#0", 2, 3, "bash", "call body", "42#0"));
        state.chat.typedTurns.blockUpsert(
            block(42, "content#1", 0, 3, "", "more work", "42#1", 900));
        state.chat.typedTurns.blockUpsert(block(42, "call#1", 2, 3, "grep", "call body", "42#1"));
        state.chat.typedTurns.blockUpsert(
            block(42, "call#2", 2, 3, "read_file", "call body", "42#2"));

        check(thread.batchCount(0) == 3,
              "three response batches, the query's batch is never counted");
        // Wire order kept; one owner row inserted before each batch's first row:
        // [header, notice, owner0, think, content0, call0, owner1, content1, call1,
        //  owner2, call2]
        check(thread.blockCount(0) == 11,
              "one owner row per response batch, emitted at the batch's first row");
        check(thread.blockAt(0, 1).label == "SYSTEM",
              "the query batch's notice stays plain, before the owner rows");
        const tui::model::BlockDesc owner0 = thread.blockAt(0, 2);
        check(owner0.label == "AGENT" && owner0.kind == tui::model::BlockKind::Assistant &&
                  owner0.state == tui::model::BlockState::None && owner0.expandable,
              "the owner row is the synthetic AGENT, no wire status written");
        check(owner0.key == std::hash<std::string>{}("batch#42#0"),
              "the owner row's key is the namespaced batch id's hash");
        check(owner0.timingS == 1.2,
              "the owner carries the batch's content duration (1200 ms -> 1.2)");
        check(thread.blockAt(0, 3).kind == tui::model::BlockKind::Thinking &&
                  thread.blockAt(0, 4).kind == tui::model::BlockKind::Assistant &&
                  thread.blockAt(0, 5).kind == tui::model::BlockKind::ToolStep,
              "the batch's own rows keep their wire order behind the owner");
        check(thread.blockAt(0, 6).label == "AGENT" && thread.blockAt(0, 6).timingS == 0.9 &&
                  thread.blockAt(0, 7).kind == tui::model::BlockKind::Assistant &&
                  thread.blockAt(0, 8).kind == tui::model::BlockKind::ToolStep,
              "the second batch's owner carries its own content duration (900 ms -> 0.9)");
        check(thread.blockAt(0, 9).label == "AGENT" && thread.blockAt(0, 9).timingS == 0.0 &&
                  thread.blockAt(0, 10).kind == tui::model::BlockKind::ToolStep,
              "a batch without a content block carries no duration");
        check(thread.blockAt(0, tui::store::kQueryRow).label == "YOU",
              "the query row is still the turn-level YOU, untouched by the buckets");
    }

    // The owner's facts and the batch's body segment
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.threadMinimalMode = false; // maximal: the mode's defaults are the pins' baseline
        state.chat.typedTurns.turnUpsert(turn(42, 1, 5));
        auto query = block(42, "q", 0, 0, "", "what is v5?", "42#q");
        query.meta = "user";
        state.chat.typedTurns.blockUpsert(query);
        // 42#0: reasoning + content + a settled call. 42#1: content + a failed
        // call (the FAIL count reads the batch). A call alone - its
        // owner copies nothing.
        state.chat.typedTurns.blockUpsert(block(42, "think#0", 1, 3, "", "reasoning", "42#0"));
        state.chat.typedTurns.blockUpsert(
            block(42, "content#0", 0, 3, "", "working", "42#0", 1200));
        state.chat.typedTurns.blockUpsert(block(42, "call#0", 2, 3, "bash", "call body", "42#0"));
        state.chat.typedTurns.blockUpsert(
            block(42, "content#1", 0, 3, "", "more work", "42#1", 900));
        state.chat.typedTurns.blockUpsert(block(42, "callfail", 2, 4, "bash", "boom", "42#1"));
        state.chat.typedTurns.blockUpsert(
            block(42, "call#2", 2, 3, "read_file", "call body", "42#2"));

        // Row list: [header, owner0, think, content0, call0, owner1, content1,
        // callfail, owner2, call2].
        check(thread.blockCount(0) == 10, "the fixture's row list is the one the checks index");

        const tui::model::BlockDesc owner0 = thread.blockAt(0, 1);
        check(owner0.ownsBatch && !owner0.hasBody && owner0.expandable && owner0.summary.empty() &&
                  owner0.chips == "[bash]" && owner0.failedCalls == 0 && owner0.copyable,
              "the owner draws from the batch's facts - chips, FAIL count, copy offer");
        check(thread.blockText(0, 1) == "working",
              "the owner's copy target is its batch's content body");

        const tui::model::BlockDesc segment = thread.blockAt(0, 3);
        check(segment.bodySegment && segment.batchOwned && !segment.ownsBatch &&
                  !segment.expandable && segment.kind == tui::model::BlockKind::Assistant &&
                  segment.hasBody,
              "the batch's content child is a body segment - no disclosure of its own");
        check(!thread.setBlockExpanded(0, 3, true) && !thread.blockAt(0, 3).expanded &&
                  state.chat.rowPins.empty(),
              "a body segment cannot be opened - the guard answers false, pins nothing, "
              "and the open state stays the derived one");
        check(thread.blockText(0, 3) == "working", "the segment's own text is still its body");

        const tui::model::BlockDesc owner1 = thread.blockAt(0, 5);
        check(owner1.ownsBatch && owner1.failedCalls == 1 && owner1.copyable &&
                  thread.blockText(0, 5) == "more work",
              "the failed call counts on its own batch's owner");

        const tui::model::BlockDesc owner2 = thread.blockAt(0, 8);
        check(owner2.ownsBatch && !owner2.copyable && thread.blockText(0, 8).empty(),
              "a calls-only batch's owner copies nothing");
        check(thread.setBlockExpanded(0, 8, true),
              "the owner itself still opens - it is the batch's disclosure");
    }

    // The stream predicates read the batch's content, not the owner's
    // (always empty) body - an Assistant row with no text is not streamed text.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.typedTurns.turnUpsert(turn(9, tui::store::kTurnStateRunning, 2));
        state.chat.typedTurns.blockUpsert(block(9, "think#0", 1, 0, "", "reasoning", "9#0"));
        state.chat.typedTurns.blockUpsert(block(9, "call#0", 2, 0, "bash", "call body", "9#0"));
        check(!thread.streamHasText() && thread.streamHasThinking(),
              "a thinking-plus-call stream has no streamed text - the owner is not an "
              "answer");
    }

    // A turn whose query row is missing gives every batch an owner.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.typedTurns.turnUpsert(turn(7, 0, 2)); // a live turn, no prose at all
        state.chat.typedTurns.blockUpsert(block(7, "call-1", 2, 2, "bash", "call body", "", 100));
        check(thread.batchCount(0) == 1, "a turn with no query row owns every batch");
        check(thread.blockAt(0, 1).label == "AGENT", "the owner row draws for the orphan batch");
    }

    // A query batch alone buckets nothing - no owner row at all.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.typedTurns.turnUpsert(turn(5, 1, 2));
        auto query = block(5, "q", 0, 0, "", "what is v5?", "5#q");
        query.meta = "user";
        state.chat.typedTurns.blockUpsert(query);
        state.chat.typedTurns.blockUpsert(block(5, "notice", 4, 0, "", "a notice", "5#q"));
        check(thread.batchCount(0) == 0 && thread.blockCount(0) == 2,
              "the query's batch is not a response batch - no owner row");
        check(thread.blockAt(0, 1).label == "SYSTEM", "the notice rides the query batch, plain");
    }

    // Malformed batch ids bucket like any other - nothing parses them.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.typedTurns.turnUpsert(turn(9, 1, 3));
        state.chat.typedTurns.blockUpsert(block(9, "a", 2, 3, "bash", "call", "#"));
        state.chat.typedTurns.blockUpsert(block(9, "b", 2, 3, "grep", "call", "abc"));
        state.chat.typedTurns.blockUpsert(block(9, "c", 2, 3, "edit_file", "call", "1#q#2"));
        check(thread.batchCount(0) == 3,
              "malformed batch ids bucket like any other (ids are opaque)");
        // Each owner is followed by its own row, so the owners sit at 1, 3, 5.
        check(thread.blockAt(0, 1).key == std::hash<std::string>{}("batch##") &&
                  thread.blockAt(0, 3).key == std::hash<std::string>{}("batch#abc") &&
                  thread.blockAt(0, 5).key == std::hash<std::string>{}("batch#1#q#2"),
              "each owner's key is its own namespaced batch id, malformed included");
    }

    // A batch whose only child is a body-less content row emits
    // no owner and no row (no bare AGENT header above nothing). The bucket
    // stays a bucket - the drop is the rows', not the batching's.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.typedTurns.turnUpsert(turn(8, 0, 1));
        auto content = block(8, "content#0", 0, 2, "", "", "8#0", 900);
        content.meta = "assistant"; // a streamed content block is not the query
        state.chat.typedTurns.blockUpsert(content);
        check(thread.batchCount(0) == 1,
              "a body-less content batch is still a bucket (the drop is the rows')");
        check(thread.blockCount(0) == 1, "the body-less batch emits no owner and no row");
        check(thread.blockAt(0, 0).label == "WORK",
              "the group header is the only row the empty batch leaves");
    }

    // The duration survives the drop - a body-less content row
    // vanishes, but its batch's owner still carries the content's duration when
    // a sibling call keeps the owner alive.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.typedTurns.turnUpsert(turn(10, 1, 3));
        auto content = block(10, "content#0", 0, 2, "", "", "10#0", 900);
        content.meta = "assistant"; // not the query: the batch is a response batch
        state.chat.typedTurns.blockUpsert(content);
        state.chat.typedTurns.blockUpsert(block(10, "call#0", 2, 3, "bash", "ls -R", "10#0"));
        check(thread.blockCount(0) == 3,
              "the dropped content leaves header, owner and the call card");
        check(thread.blockAt(0, 1).label == "AGENT" && thread.blockAt(0, 1).timingS == 0.9,
              "the owner still carries the dropped content's duration");
        check(thread.blockAt(0, 2).kind == tui::model::BlockKind::ToolStep,
              "the content row is gone, the call card is the block's last child");
    }

    // The drop is kind-blind - a body-less `Thinking` row
    // vanishes exactly like a body-less content row: no owner (the batch's
    // only kept child is gone), no row, just the header.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.typedTurns.turnUpsert(turn(11, 0, 1));
        state.chat.typedTurns.blockUpsert(block(11, "think#0", 1, 3, "", "", "11#0"));
        check(thread.batchCount(0) == 1,
              "a body-less reasoning batch is still a bucket (the drop is kind-blind)");
        check(thread.blockCount(0) == 1 && thread.blockAt(0, 0).label == "WORK",
              "a body-less Thinking row drops too - no owner, no row");
    }

    // The block is canonical - order, contiguity, placement
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.typedTurns.turnUpsert(turn(21, 1, 4));
        auto query = block(21, "q", 0, 0, "", "sweep the tree", "21#q");
        query.meta = "user";
        state.chat.typedTurns.blockUpsert(query);
        // The LIVE wire order of one response batch: content first, then the
        // call, the reasoning last (replay's order is the reverse). The
        // projection re-orders whatever the wire sent.
        state.chat.typedTurns.blockUpsert(
            block(21, "content#0", 0, 3, "", "listing the tree", "21#0", 1200));
        state.chat.typedTurns.blockUpsert(block(21, "call#0", 2, 3, "bash", "ls -R", "21#0"));
        state.chat.typedTurns.blockUpsert(block(21, "think#0", 1, 3, "", "reasoning", "21#0"));
        // A notice riding the QUERY batch, parked between the batch's rows: it
        // is a plain row, and the block's emission must jump over it and
        // emit it after the block (H8) - never between two children.
        state.chat.typedTurns.blockUpsert(
            block(21, "notice#1", 4, 0, "", "context compacted", "21#q"));
        check(thread.blockCount(0) == 6,
              "header, owner, three children, then the notice - one block, no interleave");
        check(thread.blockAt(0, 1).kind == tui::model::BlockKind::Assistant &&
                  thread.blockAt(0, 1).expandable && !thread.blockAt(0, 1).hasBody,
              "the owner row opens the block at the batch's first row");
        check(thread.blockAt(0, 2).kind == tui::model::BlockKind::Thinking &&
                  thread.blockAt(0, 3).kind == tui::model::BlockKind::Assistant &&
                  thread.blockAt(0, 4).kind == tui::model::BlockKind::ToolStep,
              "the children draw reasoning, content, card - the wire sent content, card, "
              "reasoning");
        check(thread.blockAt(0, 5).kind == tui::model::BlockKind::System,
              "the interleaved plain row is emitted after the block's last child");
        check(thread.blockAt(0, tui::store::kQueryRow).label == "YOU",
              "the query row stays turn-level, untouched by the block's emission");
    }

    // StepCount walks the WIRE store, one layer below the
    // projection - the drops and the order cannot move it, only the hidden
    // flag can. Two config pins:
    {
        TypedThreadStore store;
        // The default config's verdict: taskDone and pipeline_output are hidden,
        // so a finished task_done response's card weighs 0. query 0 + content 1 +
        // the settled call (2 visible -> 0 hidden): 3 with the card, 1 without.
        store.turnUpsert(turn(31));
        auto query = block(31, "q", 0, 0, "", "what is v5?", "31#q");
        query.meta = "user";
        store.blockUpsert(query);
        auto content = block(31, "content#0", 0, 3, "", "working", "31#0", 1200);
        content.meta = "assistant";
        store.blockUpsert(content);
        TypedBlock done = block(31, "done#0", 2, 3, "task_done", "the answer", "31#0");
        done.flags |= tui::store::kBlockFlagHidden;
        store.blockUpsert(done);
        check(tui::store::stepCount(store, 31) == 1,
              "the default config's hidden task_done card weighs 0 - the turn "
              "counts content 1");
        // hiddenTools = [] - the same turn with nothing hidden: the settled
        // card weighs 2 again, the count is today's expectation, unchanged.
        store.blockUpsert(block(31, "done#0", 2, 3, "task_done", "the answer", "31#0"));
        check(tui::store::stepCount(store, 31) == 3,
              "with hiddenTools = [] the same turn counts 3 - query 0, content 1, "
              "the settled card 2");
    }

    // The Answer's own batch suppresses its content
    // child - the answer text draws once, on the turn-level ANSWER row.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.threadMinimalMode =
            false; // maximal: the rows' visibility is not the question here
        state.chat.typedTurns.turnUpsert(turn(42, 1, 5));
        auto query = block(42, "q", 0, 0, "", "what is v5?", "42#q");
        query.meta = "user";
        state.chat.typedTurns.blockUpsert(query);
        // Batch 42#1: reasoning + content + a settled call - untouched by the
        // drop (the drop is the ANSWER's batch's, not every batch's).
        state.chat.typedTurns.blockUpsert(block(42, "think#1", 1, 3, "", "reasoning", "42#1"));
        state.chat.typedTurns.blockUpsert(
            block(42, "content#1", 0, 3, "", "an interim note", "42#1", 900));
        state.chat.typedTurns.blockUpsert(block(42, "call#1", 2, 3, "bash", "ls -R", "42#1"));
        // Batch 42#2, the ANSWER's batch: a content child whose preamble DIFFERS
        // from the answer and a settled call. The Answer block names 42#2.
        state.chat.typedTurns.blockUpsert(
            block(42, "content#2", 0, 3, "", "a differing preamble", "42#2", 300));
        state.chat.typedTurns.blockUpsert(block(42, "call#2", 2, 3, "grep", "grep -r x", "42#2"));
        state.chat.typedTurns.blockUpsert(block(42, "ans", 3, 0, "", "the answer body", "42#2"));

        // Row list: [header, owner1, think, content1, call1, owner2, call2] -
        // 42#2's content child is gone, the owner survived on its call.
        check(thread.blockCount(0) == 7, "the ANSWER's batch contributes no content child");
        check(thread.blockAt(0, 3).kind == tui::model::BlockKind::Assistant &&
                  thread.blockText(0, 3) == "an interim note",
              "another batch's content child still draws - the drop is the answer batch's");
        // The differing preamble is lost: no group row
        // carries it, and the answer text exists exactly once - on the ANSWER row.
        int preambleRows = 0;
        int answerTextRows = 0;
        for (std::size_t b = 0; b < thread.blockCount(0); ++b) {
            const std::string text = thread.blockText(0, b);
            preambleRows += text == "a differing preamble" ? 1 : 0;
            answerTextRows += text == "the answer body" ? 1 : 0;
        }
        check(preambleRows == 0, "the answer batch's differing preamble is lost");
        check(answerTextRows == 0, "the answer text draws on no group row");
        check(thread.blockText(0, tui::store::kAnswerRow) == "the answer body" &&
                  thread.blockAt(0, tui::store::kAnswerRow).label == "ANSWER",
              "the answer text draws once, on the turn-level ANSWER row");
        // The dropped child's facts survived it: the owner's duration was read
        // in the bucket pass, and the owner copies nothing - the text
        // lives on the ANSWER row, so a second copy offer would duplicate it.
        check(thread.blockAt(0, 5).label == "AGENT" && thread.blockAt(0, 5).timingS == 0.3,
              "the owner still carries the dropped content's duration");
        check(!thread.blockAt(0, 5).copyable,
              "the answer batch's owner copies nothing - the ANSWER row is the copy");
    }

    // No Answer row, no drop - every batch keeps its content child.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.threadMinimalMode = false;
        state.chat.typedTurns.turnUpsert(turn(43, 0, 2)); // a live turn, never answered
        auto query = block(43, "q", 0, 0, "", "what is v5?", "43#q");
        query.meta = "user";
        state.chat.typedTurns.blockUpsert(query);
        state.chat.typedTurns.blockUpsert(
            block(43, "content#0", 0, 2, "", "still working", "43#0", 300));
        state.chat.typedTurns.blockUpsert(block(43, "call#0", 2, 2, "bash", "ls -R", "43#0"));
        check(thread.blockCount(0) == 4,
              "with no Answer row nothing is dropped - header, owner, content, call");
        check(thread.blockText(0, 2) == "still working",
              "the content child draws when the turn has no ANSWER");
    }

    // A batch whose ONLY child was the suppressed content row
    // loses its owner too - and the header's step count stays what the wire
    // says (stepCount reads the store, above the projection - an accepted
    // hole).
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.typedTurns.turnUpsert(turn(44, 1, 2));
        auto query = block(44, "q", 0, 0, "", "what is v5?", "44#q");
        query.meta = "user";
        state.chat.typedTurns.blockUpsert(query);
        state.chat.typedTurns.blockUpsert(
            block(44, "content#0", 0, 3, "", "the answer, streamed", "44#0", 700));
        state.chat.typedTurns.blockUpsert(
            block(44, "ans", 3, 0, "", "the answer, streamed", "44#0"));
        check(thread.batchCount(0) == 1, "the emptied batch is still a bucket - "
                                         "the drop is the rows', not the batching's");
        check(thread.blockCount(0) == 1 && thread.blockAt(0, 0).label == "WORK",
              "no content child, no owner - the header is the batch's only row");
        check(thread.blockAt(0, 0).steps == 1,
              "the step count stays what the wire says (1) - the accepted hole");
        check(thread.blockText(0, tui::store::kAnswerRow) == "the answer, streamed",
              "the answer text still draws once");
    }
    // The one chips composer.
    {
        using tui::store::chipsText;
        check(chipsText({}).empty(), "no calls, no chips");
        check(chipsText({"bash"}) == "[bash]", "one call, one chip");
        check(chipsText({"bash", "grep", "read_file"}) == "[bash] [grep] [read_file]",
              "three calls, three chips");
        check(chipsText({"bash", "grep", "read_file", "edit_file"}) ==
                  "[bash] [grep] [read_file] +1",
              "the +N tail past three");
        check(chipsText({"bash", "bash", "grep"}) == "[bash] [grep]",
              "a repeated name chips once (first occurrence wins)");
        check(chipsText({"", "bash"}) == "[bash]", "an empty name never chips");
    }
    // The no-screen scene review: the
    // store answers alone.
    { // the query batch never grows an owner (restated).
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.threadMinimalMode = false;
        state.chat.typedTurns.turnUpsert(turn(45, 0, 2)); // a live turn, never answered
        auto query = block(45, "q", 0, 0, "", "what is v5?", "45#q");
        query.meta = "user";
        state.chat.typedTurns.blockUpsert(query);
        state.chat.typedTurns.blockUpsert(
            block(45, "content#0", 0, 2, "", "still working", "45#0", 900));
        state.chat.typedTurns.blockUpsert(block(45, "call#0", 2, 2, "bash", "ls -R", "45#0"));
        int owners = 0;
        for (std::size_t b = 0; b < thread.blockCount(0); ++b)
            owners += thread.blockAt(0, b).ownsBatch ? 1 : 0;
        check(owners == 1 && thread.blockCount(0) == 4, "the query batch never grows an owner");
    }
    { // the hidden verdict's weight: with hiddenTools = [] the
      // step count is unchanged; with the default each hidden call weighs 2.
        TypedThreadStore store;
        store.turnUpsert(turn(46, 1, 4));
        auto query = block(46, "q", 0, 0, "", "what is v5?", "46#q");
        query.meta = "user";
        store.blockUpsert(query);
        auto content = block(46, "content#0", 0, 3, "", "working", "46#0", 1200);
        content.meta = "assistant";
        store.blockUpsert(content);
        TypedBlock done1 = block(46, "done#0", 2, 3, "task_done", "the answer", "46#0");
        done1.flags |= tui::store::kBlockFlagHidden;
        store.blockUpsert(done1);
        TypedBlock done2 = block(46, "done#1", 2, 3, "task_done", "the answer", "46#0");
        done2.flags |= tui::store::kBlockFlagHidden;
        store.blockUpsert(done2);
        const int withDefault = tui::store::stepCount(store, 46);
        store.blockUpsert(block(46, "done#0", 2, 3, "task_done", "the answer", "46#0"));
        store.blockUpsert(block(46, "done#1", 2, 3, "task_done", "the answer", "46#0"));
        const int withEmpty = tui::store::stepCount(store, 46);
        check(withDefault == 1 && withEmpty == 5 && withEmpty - withDefault == 4,
              "stepCount with hiddenTools = [] is unchanged; with the default it is "
              "-2 per hidden call");
    }
    { // no row id repeats within a turn - the owner's namespace cannot
      // collide with the plain rows'.
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.threadMinimalMode = false;
        state.chat.typedTurns.turnUpsert(turn(47, 1, 6));
        auto query = block(47, "q", 0, 0, "", "what is v5?", "47#q");
        query.meta = "user";
        state.chat.typedTurns.blockUpsert(query);
        state.chat.typedTurns.blockUpsert(
            block(47, "think#0", 1, 0, "", "reading the layout", "47#1", 0));
        state.chat.typedTurns.blockUpsert(
            block(47, "content#1", 0, 3, "", "moving the loader", "47#1", 4100));
        state.chat.typedTurns.blockUpsert(block(47, "call#1", 2, 3, "bash", "ls -R", "47#1", 1100));
        state.chat.typedTurns.blockUpsert(
            block(47, "content#2", 0, 3, "", "the answer body", "47#2", 700));
        state.chat.typedTurns.blockUpsert(
            block(47, "call#2", 2, 3, "grep", "grep -r x", "47#2", 800));
        state.chat.typedTurns.blockUpsert(block(47, "ans", 3, 0, "", "the answer body", "47#2"));
        std::set<tui::model::BlockId> ids;
        bool repeat = false;
        const std::size_t rows = thread.blockCount(0);
        for (std::size_t b = 0; b < rows; ++b)
            if (!ids.insert(thread.blockAt(0, b).id()).second)
                repeat = true;
        // The count is the projection's too: header, batch 47#1's owner and
        // three children, batch 47#2's owner and its kept call (the answer
        // batch's content child is dropped) - an unexpected row drop or
        // insert fails this check with the id set's repeat answer spare.
        check(rows == 7 && !repeat, "no row id repeats within a turn");
    }
    { // a pin survives the default flipping - the owner's
      // default flips open and closed around a call's lifecycle and the pin
      // answers the same way the whole time.
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        state.chat.threadMinimalMode = false;
        state.chat.typedTurns.turnUpsert(turn(48, 0, 2)); // a live turn
        auto query = block(48, "q", 0, 0, "", "what is v5?", "48#q");
        query.meta = "user";
        state.chat.typedTurns.blockUpsert(query);
        state.chat.typedTurns.blockUpsert(
            block(48, "content#0", 0, 3, "", "still working", "48#0", 900));
        state.chat.typedTurns.blockUpsert(
            block(48, "call#0", 2, 3, "bash", "ls -R", "48#0", 1100)); // settled: default closed
        state.chat.groupPins[48] = true; // the group stays drawn the whole time
        // The owner row by kind, not by index (the plan's row rule): the one
        // ownsBatch row in the turn's list, so a projection reorder fails with
        // a diagnosable count instead of a misleading "pin lost".
        std::size_t ownerRow = 0;
        int ownerCount = 0;
        for (std::size_t b = 0; b < thread.blockCount(0); ++b)
            if (thread.blockAt(0, b).ownsBatch) {
                ownerRow = b;
                ++ownerCount;
            }
        const bool pinned = ownerCount == 1 && thread.setBlockExpanded(0, ownerRow, true) &&
                            thread.blockAt(0, ownerRow).expanded;
        state.chat.typedTurns.blockUpsert(
            block(48, "call#0", 2, 2, "bash", "ls -R", "48#0")); // back in flight: default open
        const bool overOpenDefault = thread.blockAt(0, ownerRow).expanded;
        state.chat.typedTurns.blockUpsert(block(48, "call#0", 2, 3, "bash", "ls -R", "48#0",
                                                1100)); // settled again: default closed
        const bool overClosedDefault = thread.blockAt(0, ownerRow).expanded;
        check(pinned && overOpenDefault && overClosedDefault,
              "a pin survives the default flipping");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    if (g_verbose)
        std::printf("all %d checks passed\n", g_checks);
    return 0;
}
