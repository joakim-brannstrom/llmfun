/// The workbench state's non-inline half (model/ui_state.h owns the contract).
#include "ui_state.h"

#include <algorithm>

namespace llmfun::tui::model {

void resetForSessionSwitch(UiState& ui) {
    // The thread is being replaced: its paging window describes messages that are
    // about to disappear (D-BOUNDS), and its expansion state is the v4 store's -
    // `store::ThreadStoreAdapter::reset()` is the other half of this, called from
    // the same place (`tuiClearOutput`). The shell preferences are the user's and
    // survive the switch.
    ui.earlierGroups = 0;
    // The hint cache and the anchor describe the *old* thread's box: a height
    // measured for a group of the session we left would be applied to a group of the
    // next one, and the anchor names a group id that must not exist any more
    // (D-GROUPID: ids are reset, never reused).
    ui.thread.hints.clear();
    ui.thread.anchorGroupId = 0;
    ui.thread.anchorScreenY = 0.0f;
    ui.thread.anchorPending = false;
    ui.thread.drawnGroups = 0;
    // The observations name groups of the thread we just left, and ids are reset per
    // session: a stale first-group id could name a group of the *new* thread for a
    // frame. The page size is re-published by the next render, but until then a key
    // would page by the width of the window the previous session had.
    ui.thread.firstGroupId = 0;
    ui.thread.firstGroupY = 0.0f;
    ui.thread.pageGroups = kThreadWindowGroupsGrid;
    // The block focus names a block of the thread we just left, and ids are reset per
    // session: a press of `y` would copy a block of the *new* thread (or nothing), and
    // the address of the old one must not survive the switch (task 26). The thinking
    // disclosures go with it for the same reason.
    ui.thread.focus = {};
    ui.thread.thinkingOpen.clear();
    // The block menu's owner goes with them (task 44): a recycled `BlockId` must not
    // re-own an open popup, and a popup that is no longer submitted is closed one frame
    // later (`views/blocks.cpp` has the measured detail).
    ui.thread.menuBlockId = {};
    // A switch is a catch-up: the next frame's thread starts pinned to the newest
    // group instead of inheriting "the user had scrolled up" from the session
    // they left (D-FOLLOW: a session switch owes a catch-up).
    // The scrolling child outlives the session, so its scroll offset is the one the
    // session we left was parked at. Ask the next two drawn frames to open at the
    // newest content: the first measures the new thread, the second begins at the
    // max it measured (the child's ScrollMax is always one frame behind).
    ui.thread.scrollToBottom = 2;
    ui.follow = true;
    // The lanes belong to the run of the session being left, and D clears its own pipeline on
    // the same switch (`uiMsg.pipelineClear()`): keeping them would draw a transcript of an
    // agent from a conversation the user is no longer looking at. The focus goes with them -
    // it names a lane, not a place in the UI.
    clearAgentLanes(ui);
}

void toggleFocusMode(UiState& ui) { ui.focusMode = !ui.focusMode; }

void exitFocusMode(UiState& ui) { ui.focusMode = false; }

bool regionControlsApply(const UiState& ui) { return !ui.focusMode; }

AgentLane* findAgentLane(UiState& ui, std::string_view id) {
    if (id.empty())
        return nullptr;
    for (AgentLane& lane : ui.agents) {
        if (lane.id == id)
            return &lane;
    }
    return nullptr;
}

const AgentLane* findAgentLane(const UiState& ui, std::string_view id) {
    if (id.empty())
        return nullptr;
    for (const AgentLane& lane : ui.agents) {
        if (lane.id == id)
            return &lane;
    }
    return nullptr;
}

void clearAgentLanes(UiState& ui) {
    ui.agents.clear();
    ui.focusedAgent.clear();
}

void stopAgentLane(UiState& ui, std::string_view id) {
    AgentLane* lane = findAgentLane(ui, id);
    if (lane == nullptr)
        return;
    lane->state = kAgentStateStopped;
    lane->active = false;
    // The clock stops where the lane stopped, exactly like a lane D reported done - and only
    // this lane's: `refreshAgentLanes()` takes one `now` for all of them, and the other lanes
    // are still running.
    const std::int64_t elapsed = lane->lastUpdateMs - lane->startedMs;
    lane->elapsedMs = elapsed > 0 ? elapsed : 0;
    if (ui.focusedAgent == lane->id)
        ui.focusedAgent.clear();
}

void focusAgentLane(UiState& ui, std::string_view id) {
    ui.focusedAgent = findAgentLane(ui, id) != nullptr ? std::string(id) : std::string();
}

void settleAgentLanes(UiState& ui, bool runActive, bool cancelRequested) {
    for (AgentLane& lane : ui.agents) {
        if (lane.state != kAgentStateRunning)
            continue;
        if (cancelRequested) {
            lane.state = kAgentStateStopped;
            lane.active = false;
        } else if (!runActive) {
            lane.state = kAgentStateFailed;
            lane.active = false;
        }
    }
}

void refreshAgentLanes(UiState& ui, std::int64_t nowMs) {
    for (AgentLane& lane : ui.agents) {
        // A lane without a start stamp keeps the duration it carries: the mirror always sets
        // one, so this is the harness' case (a hand-built lane in a fixture) - and
        // `now - 0` would print the whole unix epoch as a run time.
        if (lane.startedMs <= 0)
            continue;
        const std::int64_t until = lane.active ? nowMs : lane.lastUpdateMs;
        const std::int64_t elapsed = until - lane.startedMs;
        lane.elapsedMs = elapsed > 0 ? elapsed : 0;
    }
}

bool anyLiveAgentLane(const std::vector<AgentLane>& lanes) {
    for (const AgentLane& lane : lanes) {
        if (lane.active)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The live answer's clock (stream_state.h owns the contract, task 38)
// ---------------------------------------------------------------------------

void resetStreamClock(StreamState& stream) { stream = StreamState{}; }

void refreshStreamClock(StreamState& stream, bool hasStream, bool hasText, bool hasThinking,
                        std::int64_t nowMs, bool cancelRequested) {
    if (!hasStream) {
        // No stream is a stream that is over: every timestamp belongs to the answer that
        // just ended, and keeping one would make the *next* stream inherit a start time (a
        // clock that begins at the previous answer's beginning). The counters stay: they
        // describe what this process's store did, and `resetStreamClock()` is their clear.
        stream.startedMs = 0;
        stream.thinkingStartedMs = 0;
        stream.thinkingEndedMs = 0;
        stream.cancelledAtMs = 0;
        stream.lastSeenMs = 0;
        stream.sawText = false;
        stream.elapsedMs = 0;
        stream.thinkingElapsedMs = 0;
        stream.cancelled = false;
        stream.caretOn = false;
        return;
    }
    // The wall clock is not monotone (an NTP step can move it backwards), and a clock that runs
    // backwards would print a shrinking elapsed time and flip the caret's phase. The stream's
    // reading never goes back: a frame that arrives "before" the last one is treated as the same
    // moment, exactly as the store's update window treats it (`tui.cpp`'s `streamWindowElapsed`).
    const std::int64_t now = std::max(nowMs, stream.lastSeenMs);
    stream.lastSeenMs = now;
    if (stream.startedMs == 0)
        stream.startedMs = now;
    // The reasoning's own start is its first sighting: D sends the reasoning before the
    // answer, so this is the moment the thinking row appeared rather than a guess. A provider
    // that interleaves the two (or a first snapshot that already carries both) begins and ends
    // the reasoning in that same reading - a duration it never got to spend, and 0 is the
    // honest answer rather than a timer that never stops.
    if (stream.thinkingStartedMs == 0 && hasThinking) {
        stream.thinkingStartedMs = now;
        if (stream.sawText && stream.thinkingEndedMs == 0)
            stream.thinkingEndedMs = now;
    }
    if (hasText && !stream.sawText) {
        // The answer's first character ends the reasoning: the disclosure keeps the
        // duration it took, and the body row starts saying `writing`.
        stream.sawText = true;
        if (stream.thinkingStartedMs != 0 && stream.thinkingEndedMs == 0)
            stream.thinkingEndedMs = now;
    }
    if (cancelRequested && stream.cancelledAtMs == 0) {
        // The user's stop freezes the clock where the text stopped arriving. The store is
        // not touched: the partial output is the answer (`/stop` keeps it).
        stream.cancelledAtMs = now != 0 ? now : 1;
    }
    const std::int64_t until = stream.cancelledAtMs != 0 ? stream.cancelledAtMs : now;
    stream.elapsedMs = until > stream.startedMs ? until - stream.startedMs : 0;
    stream.cancelled = stream.cancelledAtMs != 0;
    stream.thinkingElapsedMs = stream.thinkingEndedMs > stream.thinkingStartedMs
                                   ? stream.thinkingEndedMs - stream.thinkingStartedMs
                                   : 0;
    // The blink is a function of the clock, not of the frame: on for the first half-period,
    // off for the second. A stopped answer has no caret - the text stopped arriving, and a
    // blinking mark would claim it had not.
    stream.caretOn = !stream.cancelled && (until / kStreamCaretHalfPeriodMs) % 2 == 0;
}

const char* streamPhaseWord(const StreamState& stream, bool hasText) {
    if (stream.cancelled)
        return kStreamCancelledWord;
    return hasText ? phaseWord(Phase::Writing) : phaseWord(Phase::Thinking);
}

void applyAgentUpdate(UiState& ui, std::string_view id, std::string_view role,
                      std::string_view content, std::string_view currentTool,
                      std::int64_t contextUsed, std::int64_t nowMs) {
    if (id.empty())
        return;
    AgentLane* lane = findAgentLane(ui, id);
    if (lane == nullptr) {
        // The same capacity policy as the v4 store (`ChatTabLeftPanel::MaxAgents`, LRU by
        // `lastUpdate`): a lane list longer than the store would draw an agent the store has
        // already dropped, and `kAgentLaneMax` is pinned to it by a `static_assert` in
        // `tui_api.cpp`.
        if (ui.agents.size() >= kAgentLaneMax) {
            const auto oldest = std::min_element(ui.agents.begin(), ui.agents.end(),
                                                 [](const AgentLane& a, const AgentLane& b) {
                                                     return a.lastUpdateMs < b.lastUpdateMs;
                                                 });
            if (oldest != ui.agents.end()) {
                if (ui.focusedAgent == oldest->id)
                    ui.focusedAgent.clear(); // its transcript is gone with it
                ui.agents.erase(oldest);
            }
        }
        AgentLane fresh;
        fresh.id = std::string(id);
        fresh.label = std::string(id);
        fresh.state = kAgentStateRunning;
        fresh.active = true;
        fresh.startedMs = nowMs;
        ui.agents.push_back(std::move(fresh));
        lane = &ui.agents.back();
    } else if (lane->state == kAgentStateStopped) {
        return; // the user stopped this lane: the run keeps going, the row does not
    } else {
        lane->state = kAgentStateRunning;
        lane->active = true;
    }
    if (!role.empty())
        lane->role = std::string(role);
    lane->content = std::string(content);
    // TODO: there is a bug on the D side too that sometimes report zero context after a
    // compression. That should not happen A count D reports is drawn; a 0 is not, whether D means a
    // real zero (`0/8192`) or has no reading at all (`0/0`) - `views/inspector.cpp` prints the
    // number only when it is above 0. Nothing is latched, so an update cannot revive a count D
    // stopped reporting.
    lane->tokens = contextUsed > 0 ? contextUsed : 0;
    lane->currentTool = std::string(currentTool);
    lane->lastUpdateMs = nowMs;
    const std::int64_t elapsed = nowMs - lane->startedMs;
    lane->elapsedMs = elapsed > 0 ? elapsed : 0;
}

void finishAgentLane(UiState& ui, std::string_view id) {
    AgentLane* lane = findAgentLane(ui, id);
    if (lane == nullptr || lane->state == kAgentStateStopped)
        return;
    lane->state = kAgentStateDone;
    lane->active = false;
    const std::int64_t elapsed = lane->lastUpdateMs - lane->startedMs;
    lane->elapsedMs = elapsed > 0 ? elapsed : 0;
}

} // namespace llmfun::tui::model
