// test_thread_window.cpp
//
// The thread's bounded window, its `Load earlier` page (views/thread.h) and the
// adapter the window draws through (store/thread_store.h): the arithmetic that
// decides which groups the thread draws, how many one press reveals, and what a
// group looks like once the page finally shows it.
//
// Pure: no ImGui context, no renderer, no terminal - the adapter answers from the
// v4 store and none of the accessors under test draw. The two window functions take
// integers and return a struct, which is why they were split out of `renderThread()`
// - this binary is the evidence that the split happened.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = usage error.
//
// Task 25 added the other half of the contract: a height hint and an expansion flag
// must not travel to another group while the FIFO evicts the oldest messages, the
// enumeration must stay incremental, and "one press is one page" must survive a
// compact window.
//
//   ./test_thread_window [-v]

#include "views/thread.h"

#include "store/thread_store.h"
#include "tui.h"
#include "views/palette.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace tui = llmfun::tui;

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

std::string windowText(const llmfun::tui::views::ThreadWindow& w) {
    return "first=" + std::to_string(w.first) + " count=" + std::to_string(w.count) +
           " hidden=" + std::to_string(w.hidden);
}

/// Every window is a suffix of the thread: `[first, first+count)` ends at
/// `groupCount`, `hidden` counts what is above it, and nothing overflows.
void checkShape(std::size_t groupCount, const llmfun::tui::views::ThreadWindow& w,
                const std::string& where) {
    check(w.first + w.count == groupCount,
          where + ": first + count == groupCount (" + windowText(w) + ")");
    check(w.hidden == w.first, where + ": hidden == first (" + windowText(w) + ")");
    check(w.count <= groupCount, where + ": count <= groupCount (" + windowText(w) + ")");
}

/// The live group ids, oldest first - the identity a hint or an expansion flag has to
/// keep pointing at (D-GROUPID: an id is assigned once and never reused).
std::vector<size_t> groupIds(const llmfun::tui::store::ThreadStoreAdapter& thread) {
    std::vector<size_t> ids;
    for (std::size_t k = 0; k < thread.groupCount(); ++k)
        ids.push_back(thread.groupAt(k).id);
    return ids;
}

/// One message in the store, with the id the D side would assign (monotonic from 1).
void addMessage(tui::TuiState& state, tui::ChatMessageType type, const std::string& text) {
    tui::ChatMessage msg;
    msg.id = state.chat.outputLineNextId++;
    msg.type = type;
    msg.summary = text;
    msg.text = text;
    tui::tuiAddOutputLine(state, msg);
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
    using llmfun::tui::views::loadEarlierPage;
    using llmfun::tui::views::ThreadWindow;
    using llmfun::tui::views::threadWindow;

    // A thread shorter than the window: everything is drawn, nothing is hidden, so
    // the `Load earlier` row stays off screen.
    {
        const ThreadWindow w = threadWindow(3, 40, 0);
        checkShape(3, w, "short thread");
        check(w.count == 3 && w.first == 0 && w.hidden == 0,
              "short thread: all 3 groups, no hidden ones (" + windowText(w) + ")");
    }
    // Exactly the window: still nothing hidden.
    {
        const ThreadWindow w = threadWindow(40, 40, 0);
        checkShape(40, w, "exact window");
        check(w.count == 40 && w.hidden == 0,
              "exact window: all 40 groups (" + windowText(w) + ")");
    }
    // One group more than the window: the oldest is hidden and offered back.
    {
        const ThreadWindow w = threadWindow(41, 40, 0);
        checkShape(41, w, "one over the window");
        check(w.first == 1 && w.count == 40 && w.hidden == 1,
              "one over the window: the oldest group is hidden (" + windowText(w) + ")");
    }
    // The window is the *last* groups, not the first: the newest content is always
    // in it (that is what makes follow-the-newest a scroll, not a jump).
    {
        const ThreadWindow w = threadWindow(1000, 30, 0);
        checkShape(1000, w, "long thread");
        check(w.first == 970 && w.count == 30 && w.hidden == 970,
              "long thread: the window is the newest 30 (" + windowText(w) + ")");
    }
    // `Load earlier` grows the window by whole pages.
    {
        const ThreadWindow w = threadWindow(1000, 30, 60);
        checkShape(1000, w, "two pages revealed");
        check(w.count == 90 && w.first == 910,
              "two pages revealed: 90 groups (" + windowText(w) + ")");
    }
    // Paging past the start clamps instead of overflowing or emptying the thread.
    {
        const ThreadWindow w = threadWindow(50, 30, 1000);
        checkShape(50, w, "paged past the start");
        check(w.count == 50 && w.first == 0 && w.hidden == 0,
              "paged past the start: the whole thread (" + windowText(w) + ")");
    }
    // Degenerate windows: a non-positive request is the smallest useful window, and
    // a negative page count is no page - never a negative or a wrapped size_t.
    {
        const ThreadWindow w = threadWindow(10, 0, 0);
        checkShape(10, w, "windowGroups 0");
        check(w.count == 1 && w.first == 9, "windowGroups 0: one group (" + windowText(w) + ")");
    }
    {
        const ThreadWindow w = threadWindow(10, -5, 0);
        checkShape(10, w, "windowGroups -5");
        check(w.count == 1 && w.first == 9, "windowGroups -5: one group (" + windowText(w) + ")");
    }
    {
        const ThreadWindow w = threadWindow(10, 4, -3);
        checkShape(10, w, "earlierGroups -3");
        check(w.count == 4 && w.first == 6,
              "earlierGroups -3: no page revealed (" + windowText(w) + ")");
    }
    // An empty thread: nothing to draw, nothing hidden (the empty body takes over).
    {
        const ThreadWindow w = threadWindow(0, 40, 5);
        checkShape(0, w, "empty thread");
        check(w.count == 0 && w.first == 0 && w.hidden == 0,
              "empty thread: no groups (" + windowText(w) + ")");
    }
    // The page a press reveals is the window, and always at least one group.
    check(loadEarlierPage(40) == 40, "loadEarlierPage(40) == 40");
    check(loadEarlierPage(1) == 1, "loadEarlierPage(1) == 1");
    check(loadEarlierPage(0) == 1, "loadEarlierPage(0) == 1 (a press always reveals something)");
    check(loadEarlierPage(-7) == 1, "loadEarlierPage(-7) == 1");

    check(loadEarlierPage(-7) == 1, "loadEarlierPage(-7) == 1");

    // --- What a group looks like when the page reveals it -----------------------
    // The adapter seeds a group's one-shot expansion default when it assigns the
    // group's id, not when a frame draws it. That is what a windowed renderer cannot
    // do with a high-water watermark alone: by the time `Load earlier` reveals an old
    // group, the committed watermark is already past its id, so the render path's
    // seeding is a no-op for it. Here the whole scenario runs without a renderer.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        check(thread.bound(), "adapter: a bound adapter reports itself bound");
        for (int i = 1; i <= 30; ++i) {
            addMessage(state, tui::ChatMessageType::User, "question " + std::to_string(i));
            addMessage(state, tui::ChatMessageType::FinalAnswer, "answer " + std::to_string(i));
        }
        check(thread.groupCount() == 60,
              "adapter: 30 pairs are 60 groups (" + std::to_string(thread.groupCount()) + ")");
        // The grid's window (40 groups) hides the first 20, so the first answer is
        // reachable only through `Load earlier`.
        const ThreadWindow closed = threadWindow(thread.groupCount(), 40, 0);
        checkShape(60, closed, "adapter: default window");
        check(closed.first == 20,
              "adapter: the default window hides the first 20 groups (" + windowText(closed) + ")");

        const size_t oldestAnswer = thread.groupAt(1).id;
        check(thread.groupAt(1).kind == tui::store::GroupKind::FinalAnswer,
              "adapter: group 1 is the first answer");
        // A final answer is open from birth (the run's result is never hidden), even
        // though it sits outside the window that draws it.
        check(thread.expanded(oldestAnswer),
              "adapter: an answer group is born expanded outside the window");
        check(state.chat.outputLineOpen.count(state.chat.outputLines[1].id) == 1,
              "adapter: ...and the one store agrees");
        check(!thread.expanded(thread.groupAt(0).id),
              "adapter: an old user query is born collapsed");

        // A committed pass plus a page that reveals the group must not change either
        // answer - the case the watermark alone could not express.
        thread.commitSeen();
        const ThreadWindow opened = threadWindow(thread.groupCount(), 40, 100);
        checkShape(60, opened, "adapter: paged past the start");
        check(opened.first == 0,
              "adapter: the page reveals the whole thread (" + windowText(opened) + ")");
        check(thread.expanded(oldestAnswer),
              "adapter: a paged-back answer keeps the default it was born with");
        check(!thread.expanded(thread.groupAt(0).id),
              "adapter: ...and a paged-back query keeps its collapsed one");
    }
    // The work-group rule: the newest group unfolds, the older ones do not, and the
    // items inside follow the "last five" default even while the group is collapsed.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        addMessage(state, tui::ChatMessageType::User, "q1");          // 1
        addMessage(state, tui::ChatMessageType::Assistant, "step 1"); // 2
        addMessage(state, tui::ChatMessageType::ToolCall, "tool");    // 3
        addMessage(state, tui::ChatMessageType::FinalAnswer, "a1");   // 4
        addMessage(state, tui::ChatMessageType::User, "q2");          // 5
        addMessage(state, tui::ChatMessageType::Assistant, "step 2"); // 6
        addMessage(state, tui::ChatMessageType::ToolResponse, "out"); // 7
        check(thread.groupCount() == 5, "adapter: q, answer, q, work is 5 groups (" +
                                            std::to_string(thread.groupCount()) + ")");
        check(thread.expanded(thread.groupAt(4).id), "adapter: the newest work group unfolds");
        check(!thread.expanded(thread.groupAt(1).id),
              "adapter: an older work group stays collapsed");
        check(state.chat.openWorkGroups.count(state.chat.outputLines[5].id) == 1,
              "adapter: the group's key is its first message id");
        check(state.chat.outputLineOpen.count(state.chat.outputLines[6].id) == 1,
              "adapter: the newest items of a work group unfold");
        check(state.chat.outputLineOpen.count(state.chat.outputLines[1].id) == 0,
              "adapter: ...and the old ones do not");

        // A toggle the user made survives the rebuild that follows it: that is the
        // half `commitSeen()` owns (the render path must not re-seed over the click).
        thread.setExpanded(thread.groupAt(4).id, false);
        thread.commitSeen();
        addMessage(state, tui::ChatMessageType::User, "q3");
        check(thread.groupCount() == 6, "adapter: a new message rebuilds the groups");
        check(!thread.expanded(thread.groupAt(4).id),
              "adapter: a toggle survives the rebuild and the commit");
    }
    // Live birth: a work group that is newest when it appears is auto-opened; when a
    // newer top-level group (the run's answer, or the next turn's query) is born, the
    // auto-opened group must close so a live session matches a reloaded one. A group
    // the user opened is not in the auto set and stays open.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        addMessage(state, tui::ChatMessageType::User, "q1");          // 1, group 1
        addMessage(state, tui::ChatMessageType::Assistant, "step 1"); // 2, group 2 (newest)
        addMessage(state, tui::ChatMessageType::ToolCall, "tool");    // 3
        const size_t workId = thread.groupAt(1).id;
        check(thread.expanded(workId), "adapter: a work group born newest opens");
        addMessage(state, tui::ChatMessageType::FinalAnswer, "answer"); // 4, group 3
        check(thread.groupCount() == 3, "adapter: answer appends a top-level group");
        check(!thread.expanded(workId),
              "adapter: an auto-opened work group closes when the answer arrives");
        check(thread.expanded(thread.groupAt(2).id), "adapter: the answer itself is open");

        // The user now opens the old work group: a later group must not close it.
        thread.setExpanded(workId, true);
        addMessage(state, tui::ChatMessageType::User, "q2"); // 5, group 4
        check(thread.expanded(workId), "adapter: a user-opened work group survives the next group");
    }
    // A live answer is not a stored group, so it must not close the auto-opened work group
    // either (task 38): the stream is drawn *after* the window and the group above it keeps
    // the shape it had when it was born - only the answer's arrival (or the next query)
    // closes it. `storedGroupCount()` is what the window budgets for, and the stream is not
    // part of it.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        addMessage(state, tui::ChatMessageType::User, "q1");          // 1, group 1
        addMessage(state, tui::ChatMessageType::Assistant, "step 1"); // 2, group 2 (newest)
        const size_t workId = thread.groupAt(1).id;
        check(thread.expanded(workId), "stream: a work group born newest opens");
        check(thread.storedGroupCount() == 2 && !thread.hasStream(),
              "stream: with no stream the stored count is the group count");

        state.chat.streamMsg = tui::ChatMessage{};
        state.chat.streamMsg.type = tui::ChatMessageType::Assistant;
        state.chat.streamMsg.text = "the answer is arriving";
        state.chat.hasStreamMsg = true;
        check(thread.groupCount() == 3, "stream: the live answer is a group of its own");
        check(thread.storedGroupCount() == 2, "stream: but the window does not budget for it");
        check(thread.hasStream(), "stream: and the thread knows it is last");
        check(thread.expanded(workId),
              "stream: a live answer does not close the auto-opened work group");

        // The answer arrives as a stored group: now the auto-opened group closes, exactly as it
        // does without a stream (the live session matches a reloaded one).
        state.chat.hasStreamMsg = false;
        addMessage(state, tui::ChatMessageType::FinalAnswer, "answer"); // 3, group 3
        check(!thread.expanded(workId),
              "stream: and the answer's arrival closes it (the stored rule, unchanged)");
    }
    // The unbound adapter. `bound()` has no consumer on the frame path (the frame
    // layer binds unconditionally), so a test is what keeps the guard alive: an
    // adapter must answer empty rather than dereference a state that moved away.
    {
        tui::TuiState state;
        addMessage(state, tui::ChatMessageType::FinalAnswer, "answer");
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        check(thread.bound() && thread.groupCount() == 1,
              "unbound: the bound adapter sees the store");

        tui::store::ThreadStoreAdapter moved(std::move(thread));
        // A move *drops* the pointer instead of carrying it: the adapter holds its
        // state's own address, so moving it into another adapter must not point at
        // the object that moved. What is left is a target that answers empty until
        // the frame layer binds it again.
        check(!moved.bound(), "unbound: a move does not carry the state into the target");
        check(moved.groupCount() == 0,
              "unbound: ...which answers no groups although the store still has one");
        check(moved.groupAt(0).id == 0, "unbound: groupAt() answers a zeroed view");
        check(!moved.expanded(1), "unbound: expanded() answers false");
        check(moved.measureHint(1) == 0.0f, "unbound: measureHint() answers 0");
        moved.setExpanded(1, true);
        check(!moved.bound(), "unbound: setExpanded() is a no-op");
        // The source keeps its own binding: it is untouched, and dropping the pointer
        // (rather than moving it) is what keeps `state = TuiState()` from handing the
        // new state an address inside the temporary.
        check(thread.bound() && thread.groupCount() == 1, "unbound: the source stays usable");
        moved.bind(state);
        check(moved.bound() && moved.groupCount() == 1,
              "unbound: the store is untouched, and one bind() makes an adapter usable");

        // The harness case the move semantics exist for.
        state = tui::TuiState();
        check(!state.threadAdapter.bound(),
              "unbound: a state assignment leaves its own adapter unbound");
        check(state.threadAdapter.groupCount() == 0, "unbound: ...and it answers empty");
    }
    // --- Incremental enumeration (design §4.4: "append-only") ------------------
    // An append costs one group rebuild. The per-group reads the window makes
    // (`groupAt`, once per visible group) rebuild nothing, and `measureHint` finds its
    // entry with a map probe instead of walking the thread - the counters make
    // "incremental" an assertion rather than a claim.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        for (int i = 0; i < 100; ++i)
            addMessage(state, tui::ChatMessageType::User, "q" + std::to_string(i));
        const size_t groups = thread.groupCount();
        check(groups == 100,
              "incremental: 100 messages are 100 groups (" + std::to_string(groups) + ")");
        const size_t buildsAtStart = thread.groupBuilds();
        const size_t walksAtStart = thread.entryWalks();

        // A frame's worth of window reads: 40 groups, each asked for its id and its
        // hint.
        for (std::size_t k = 60; k < 100; ++k) {
            const tui::store::GroupView view = thread.groupAt(k);
            thread.measureHint(view.id);
        }
        check(thread.groupBuilds() == buildsAtStart,
              "incremental: 40 group reads rebuild nothing (" +
                  std::to_string(thread.groupBuilds() - buildsAtStart) + " rebuild(s))");
        check(thread.entryWalks() == walksAtStart + 40,
              "incremental: 40 hints examine 40 entries, one each (" +
                  std::to_string(thread.entryWalks() - walksAtStart) + ")");
        // A lookup that finds nothing examines nothing: the counter measures entries
        // touched, so a scan (which would touch every entry of the thread) cannot hide
        // behind it.
        thread.measureHint(999999);
        thread.expanded(999999);
        check(thread.entryWalks() == walksAtStart + 40,
              "incremental: a miss examines no entry (" +
                  std::to_string(thread.entryWalks() - walksAtStart) + ")");

        addMessage(state, tui::ChatMessageType::User, "one more");
        check(thread.groupCount() == 101, "incremental: the append is visible");
        check(thread.groupBuilds() == buildsAtStart + 1,
              "incremental: one append is exactly one rebuild (" +
                  std::to_string(thread.groupBuilds() - buildsAtStart) + ")");
    }

    // --- 1200 messages, 200 evicted: nothing moves to another group ------------
    // The store is a 1000-message FIFO. Every id-keyed piece of state - the expansion
    // flags and the height hints - has to stay with the group it was recorded for, and
    // the entries of the evicted groups have to go away with them: a hint that outlived
    // its group would be applied to a *different* group the moment an id matched, and
    // an unpruned map would grow for the life of the session.
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        for (int i = 0; i < 1000; ++i) {
            addMessage(state, tui::ChatMessageType::User, "q" + std::to_string(i));
            addMessage(state, tui::ChatMessageType::FinalAnswer, "a" + std::to_string(i));
        }
        const std::vector<size_t> before = groupIds(thread);
        check(before.size() == 1000, "eviction: the store is capped at 1000 groups (" +
                                         std::to_string(before.size()) + ")");
        // A hint for the newest group and one for the oldest still live one: the first
        // must survive the appends, the second must be pruned with its group.
        const size_t survivor = before.back();
        const size_t doomed = before.front();
        for (size_t id : {survivor, doomed}) {
            tui::model::HeightHint hint;
            hint.groupId = id;
            hint.width = 80.0f;
            hint.row = 1.0f;
            hint.h = 7.0f;
            state.ui.thread.hints[id] = hint;
        }
        // The newest group is a final answer, so it is open from birth; the oldest one
        // is a user query and is born collapsed.
        const bool survivorOpen = thread.expanded(survivor);
        check(survivorOpen, "eviction: the newest answer is expanded");
        check(!thread.expanded(doomed), "eviction: the oldest query is collapsed");

        for (int i = 0; i < 200; ++i) {
            addMessage(state, tui::ChatMessageType::User, "later" + std::to_string(i));
            addMessage(state, tui::ChatMessageType::FinalAnswer,
                       "later answer" + std::to_string(i));
        }
        const std::vector<size_t> after = groupIds(thread);
        check(after.size() == 1000,
              "eviction: still capped at 1000 groups (" + std::to_string(after.size()) + ")");
        // Ids are monotonic and never reused, so the new window starts *after* the old
        // one's first id (D-GROUPID).
        check(after.front() > before.front(),
              "eviction: the window moved forward, ids are not reused (" +
                  std::to_string(after.front()) + " > " + std::to_string(before.front()) + ")");
        bool increasing = true;
        for (std::size_t i = 1; i < after.size(); ++i)
            increasing = increasing && after[i] > after[i - 1];
        check(increasing, "eviction: the live ids are strictly increasing (never a reused id)");
        check(state.ui.thread.hints.count(doomed) == 0,
              "eviction: the evicted group's hint is gone with it");
        check(state.ui.thread.hints.count(survivor) == 1,
              "eviction: the surviving group keeps its own hint");
        const auto hinted = state.ui.thread.hints.find(survivor);
        check(hinted != state.ui.thread.hints.end() && hinted->second.groupId == survivor,
              "eviction: a hint still names the group it was measured for");
        check(thread.expanded(survivor) == survivorOpen,
              "eviction: a surviving group keeps its expansion state");
        bool vanished = false;
        for (size_t id : after)
            vanished = vanished || (id <= doomed);
        check(!vanished, "eviction: no live group reuses an evicted group's id");
        thread.commitSeen();
    }

    // --- One press is one page, in every mode ---------------------------------
    // `PgUp`/`Ctrl+U` and the palette row both call `requestEarlierPage()`, and the page
    // is the window the thread was last drawn with - never a constant: in compact mode a
    // page of 40 would reveal the whole thread at once.
    {
        tui::model::UiState ui;
        check(ui.thread.pageGroups == tui::model::kThreadWindowGroupsGrid,
              "page: a state that was never drawn pages by the grid's window");
        llmfun::tui::views::requestEarlierPage(ui);
        check(ui.earlierGroups == 40,
              "page: one press is one window (" + std::to_string(ui.earlierGroups) + ")");
        llmfun::tui::views::requestEarlierPage(ui);
        check(ui.earlierGroups == 80, "page: a second press adds a second page");
        check(
            llmfun::tui::views::threadWindow(1000, ui.thread.pageGroups, ui.earlierGroups).count ==
                120,
            "page: the window grows by one page per press");
        // Compact: the layout answered one group, so one press reveals exactly one.
        ui.thread.pageGroups = 1;
        ui.earlierGroups = 0;
        llmfun::tui::views::requestEarlierPage(ui);
        check(ui.earlierGroups == 1, "page: in compact mode one press is one group (" +
                                         std::to_string(ui.earlierGroups) + ")");
        // A session switch clears the page budget, the hint cache and the anchor, and it
        // owes a catch-up.
        tui::model::HeightHint hint;
        hint.groupId = 7;
        ui.thread.hints[7] = hint;
        ui.thread.anchorGroupId = 7;
        ui.thread.anchorPending = true;
        ui.follow = false;
        tui::model::resetForSessionSwitch(ui);
        check(ui.earlierGroups == 0 && ui.thread.hints.empty() && ui.thread.anchorGroupId == 0 &&
                  !ui.thread.anchorPending,
              "switch: the hint cache, the page budget and the anchor are per thread");
        check(ui.follow, "switch: a new session owes a catch-up (follow is re-armed)");
    }

    // --- The cost of a frame does not scale with the thread (V4) ----------------
    // "Incremental" is a cost claim, not a call count: a per-frame walk of the store
    // would show up as growth when the same window is read from a longer thread. The
    // fixture builds two threads - 125 and 1000 messages, 1 KB bodies, the design's
    // fixture size - reads the last 40 groups from each (what `renderThread` asks for
    // per visible group), and compares. A linear lookup inside `expanded()`/
    // `measureHint()` multiplies the work by the message count; the map probe keeps it
    // flat.
    {
        const auto sampleRun = [](int messages) {
            tui::TuiState state;
            tui::store::ThreadStoreAdapter thread(state);
            thread.bind(state);
            const std::string body(1024, 'x');
            for (int i = 0; i < messages; ++i) {
                tui::ChatMessage msg;
                msg.id = state.chat.outputLineNextId++;
                msg.type =
                    (i % 2 == 0) ? tui::ChatMessageType::User : tui::ChatMessageType::FinalAnswer;
                msg.summary = "m" + std::to_string(i);
                msg.text = body;
                tui::tuiAddOutputLine(state, msg);
            }
            const auto t0 = std::chrono::steady_clock::now();
            const size_t groups = thread.groupCount(); // the first call builds the list
            const auto t1 = std::chrono::steady_clock::now();

            // One frame of the window's model work: 40 groups, each asked for its view,
            // its height hint and its expansion state - the three calls the render loop
            // makes per visible group.
            volatile double sink = 0.0;
            const int kFrames = 200;
            const auto t2 = std::chrono::steady_clock::now();
            for (int f = 0; f < kFrames; ++f) {
                const std::size_t first = groups > 40 ? groups - 40 : 0;
                for (std::size_t k = first; k < groups; ++k) {
                    const tui::store::GroupView view = thread.groupAt(k);
                    sink = sink + thread.measureHint(view.id);
                    sink = sink + (thread.expanded(view.id) ? 1.0 : 0.0);
                }
            }
            const auto t3 = std::chrono::steady_clock::now();
            const double buildMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            const double frameMs =
                std::chrono::duration<double, std::milli>(t3 - t2).count() / kFrames;
            return std::make_tuple(buildMs, frameMs, groups, thread.entryWalks());
        };

        // One timing sample of a microsecond-scale loop is at the mercy of the
        // scheduler: a 0.5 ms stall over 200 frames already doubles the mean, and a
        // single sample can also understate the ratio when the small side is the
        // unlucky one. Take the minimum of five runs per side - noise only adds time,
        // so the minimum is the closest estimate of the real cost - and keep the 2x
        // threshold (an O(n) lookup lands at 10-20x, so it stays discriminating).
        constexpr int kRepetitions = 5;
        const auto bestOf = [&sampleRun](int messages) {
            auto best = sampleRun(messages);
            for (int r = 1; r < kRepetitions; ++r) {
                const auto run = sampleRun(messages);
                if (std::get<0>(run) < std::get<0>(best))
                    std::get<0>(best) = std::get<0>(run);
                if (std::get<1>(run) < std::get<1>(best))
                    std::get<1>(best) = std::get<1>(run);
            }
            return best;
        };
        const auto small = bestOf(125);
        const auto large = bestOf(1000);
        const double smallBuild = std::get<0>(small);
        const double smallFrame = std::get<1>(small);
        const double largeBuild = std::get<0>(large);
        const double largeFrame = std::get<1>(large);
        const std::size_t largeWalks = std::get<3>(large);

        std::printf("cost (min of %d): build(125)=%.3f ms frame(125)=%.4f ms | build(1000)=%.3f ms "
                    "frame(1000)=%.4f ms | walks(1000)=%zu\n",
                    kRepetitions, smallBuild, smallFrame, largeBuild, largeFrame, largeWalks);

        // The task's budget for opening the fixture: well under a frame of either
        // profile (150 ms is the documented ceiling).
        check(largeBuild <= 150.0,
              "cost: a 1000-message thread builds in " + std::to_string(largeBuild) + " ms");
        // A frame's model work stays inside the tightest frame budget by two orders of
        // magnitude (16.7 ms at 60 fps), so a regression has to be gross to slip through
        // the absolute check - the ratio below is the sensitive one.
        check(largeFrame <= 2.0,
              "cost: a frame of window reads costs " + std::to_string(largeFrame) + " ms");
        // Eight times the thread, same window: anything that walks the store scales with
        // it. The factor is loose because even a minimum of five runs of a
        // microsecond-scale loop carries noise; an O(n) lookup lands far above it
        // (measured ~10-20x when probed).
        const double ratio = smallFrame > 0.0 ? largeFrame / smallFrame : 0.0;
        check(ratio <= 2.0,
              "cost: the frame does not scale with the thread (1000 vs 125 messages: " +
                  std::to_string(ratio) + "x)");
        // 40 groups x 2 id lookups per frame = 80 entries examined per frame, whatever
        // the thread length: the counter agrees with the timing.
        check(largeWalks == 80u * 200u,
              "cost: 80 entries examined per frame (" + std::to_string(largeWalks) + ")");
    }

    // --- The palette's `Load earlier` row is live while the window hides something ---
    // The row is a real control in the thread, so it is in the table (D-PALETTE); it is
    // enabled exactly while the bounded window hides a group, which is the thread's own
    // arithmetic (`threadHiddenGroups()`).
    {
        tui::TuiState state;
        tui::store::ThreadStoreAdapter thread(state);
        thread.bind(state);
        for (int i = 0; i < 60; ++i)
            addMessage(state, tui::ChatMessageType::User, "q" + std::to_string(i));
        state.ui.thread.pageGroups = 40;
        std::deque<std::string> idStorage;
        const auto rowEnabled = [&]() {
            const auto items = llmfun::tui::views::paletteActions(state, "", idStorage);
            for (const auto& item : items) {
                if (std::strcmp(item.id, "actions.loadEarlier") == 0)
                    return item.enabled;
            }
            return false;
        };
        check(rowEnabled(), "palette: 'Load earlier' is enabled while the window hides groups");
        state.ui.earlierGroups = 1000; // every group drawn
        check(!rowEnabled(), "palette: ...and disabled once the whole thread is on screen");
        state.ui.earlierGroups = 0;
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf(
        "OK: thread window, Load earlier paging and the group defaults pinned (%d checks)\n",
        g_checks);
    return 0;
}
