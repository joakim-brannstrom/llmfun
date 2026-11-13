#include "views/thread.h"

#include "views/blocks.h"

#include "imgui/imgui.h"

#include "model/block.h"
#include "model/thread_view.h"
#include "model/ui_state.h"
#include "store/thread_store.h"
#include "ui/input.h"
#include "ui/layout.h"

#include "ui/markdown.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/scroll.h"
#include "ui/text.h"
#include "ui/tokens.h"

#include <algorithm>
#include <cassert>
#include <string>
#include <utility>
#include <vector>

namespace llmfun::tui::views {
/// The thread's drawing body, mounted: declared here so the drill-down's
/// renderer can call it with the agent adapter before the definition below.
static void renderThreadBody(model::UiState& ui, store::ThreadStoreAdapter& thread,
                             model::ThreadState& ts, const ui::Rect& region,
                             const ui::Rect& loadEarlierRow, const ui::Metrics& m, int windowGroups,
                             bool compact, bool acceptKeys);

namespace {

/// One text line at an absolute position, clipped to `box`.
///
/// The non-thread bodies below are static: they never wrap and never scroll, so a
/// line is a position and a string. On the text grid the string is cut with the
/// profile's own ellipsis, because a cell renderer has to say "there is more" in
/// cells; in the pixel profile the clip rect does that job.
void lineAt(const ui::Metrics& m, const ui::Rect& box, float x, float y, const char* text,
            ui::ColorToken ink) {
    if (text == nullptr || text[0] == '\0' || y + m.row > box.bottom())
        return;
    const std::string shown =
        m.textGrid ? ui::truncate(text, static_cast<int>(std::max(0.0f, box.right() - x)))
                   : std::string(text);
    ImGui::PushClipRect(ImVec2(box.x, box.y), ImVec2(box.right(), box.bottom()), true);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::color(ink));
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::TextUnformatted(shown.c_str(), shown.c_str() + shown.size());
    ImGui::PopStyleColor();
    ImGui::PopClipRect();
}

/// The three bodies a thread with nothing to draw shows instead of a blank panel
/// (design §4.11). The copy is the model's, the stack is one line per string; the
/// restyled cards - starter buttons, the skeleton shimmer, the Retry action -
/// belong to the thread restyle, and the *decision* is the frame layer's
/// (`model::deriveThreadView`, published in `UiState::view`).
void renderThreadState(model::ThreadView view, const ui::Metrics& m, const ui::Rect& region) {
    const float x = region.x + m.pad2;
    float y = region.y + m.pad2;
    auto line = [&](const char* text, ui::ColorToken ink) {
        lineAt(m, region, x, y, text, ink);
        y += m.row;
    };
    switch (view) {
    case model::ThreadView::Ready:
        return;
    case model::ThreadView::Empty:
        line(model::kEmptyWordmark, ui::ColorToken::textHi);
        // The grid and the pixel profile get their own spelling of the sentence:
        // U+2014 is East-Asian ambiguous, so a cell renderer draws a hyphen.
        line(m.textGrid ? model::kEmptySentenceAscii : model::kEmptySentence,
             ui::ColorToken::textMid);
        for (std::size_t i = 0; i < model::kStarterActionCount; ++i)
            line(model::kStarterActions[i].label, ui::ColorToken::accent);
        line(model::kPaletteHint, ui::ColorToken::textLow);
        break;
    case model::ThreadView::Loading:
        for (int i = 0; i < model::kSkeletonRows; ++i)
            line(model::kSkeletonGlyphAscii, ui::ColorToken::textLow);
        break;
    case model::ThreadView::Offline:
        line(model::kOfflineHeadline, ui::ColorToken::stateDanger);
        line(model::kOfflineDetail, ui::ColorToken::textMid);
        line(model::kRetryLabel, ui::ColorToken::accent);
        break;
    }
}

/// The drill-down's way back (`Back to main`): the row above an agent transcript.
///
/// One label, so the control `ui::button` identifies cannot drift from the one the
/// golden reads; the design spells it exactly this way (design §5.21).
constexpr const char* kTranscriptBackLabel = "Back to main";

/// The `Load earlier` button's label, and nothing else: `ui::button`'s identity is
/// the label hashed with its rect, so a label that carried the count would be a
/// different button after every press and would drop the keyboard focus the user
/// just put there. The count is drawn beside it.
constexpr const char* kLoadEarlierLabel = "Load earlier";

/// Copy one block's text (task 26): the focused block's `Copy` control, the block menu's
/// `Copy` item and the `y` key all land here, so a block has exactly one copy
/// path - and the item that asked never chooses the text.
///
/// What reaches the clipboard is the wrapper's decision, not this file's: a `Code`
/// block copies its *code* (`ui::copyText`), everything else its body verbatim - and
/// the wrapper is the only place allowed to know what a fence is. A batch owner
/// copies its batch's content, and the fence answer is the content's own
/// kind - the adapter's, never re-derived here.
///
/// Answers whether the clipboard was written: a row with no body copies nothing, which is what
/// keeps a body-less block (the `WORK` header, a stream row before its first token) from
/// claiming a copy nobody got - and from raising the feedback item for one (task 45).
bool copyBlock(store::ThreadStoreAdapter& thread, std::size_t groupIndex, std::size_t block) {
    const std::string body = thread.blockText(groupIndex, block);
    if (body.empty())
        return false;
    const std::string text = ui::copyText(body, thread.blockCopyIsCode(groupIndex, block));
    if (text.empty())
        return false;
    ImGui::SetClipboardText(text.c_str());
    return true;
}

/// Flip one block's table source (design §6.8, D-TABLE, task 37): `s` draws the markdown of
/// the body's tables instead of the grid, and again the grid.
///
/// What reaches the renderer is the *body's text*, never a flag beside it: the transform
/// writes a `llmfun-table-source` fence instead of a `llmfun-table` one, so the block's own
/// state is the string it renders. The set in `UiState` is the toggle's memory, keyed by the
/// block's stable id (D-GROUPID). Whether there is anything to show is the *adapter's* answer
/// (`blockHasTable()`), so a row drawn verbatim - a tool transcript - and a body with no table
/// both leave the state alone instead of setting an invisible bit. Answers true when it flipped.
bool toggleTableSource(model::ThreadState& ts, store::ThreadStoreAdapter& thread, std::size_t k,
                       std::size_t b, const model::BlockDesc& desc) {
    const model::BlockId id = desc.id();
    if (!id.valid() || !thread.blockHasTable(k, b))
        return false;
    const auto found = ts.tableSource.find(id);
    if (found == ts.tableSource.end())
        ts.tableSource.insert(id);
    else
        ts.tableSource.erase(found);
    return true;
}

} // namespace

/// The drill-down (design §5.21, task 30): one agent's transcript, read-only, under the row
/// that leads back to the conversation.
///
/// It lives in the thread because that is the surface with room for a transcript - the
/// inspector column is 28 cells wide and the design says "the thread filters to that agent's
/// transcript". The *main* transcript is not touched to make this happen: the adapter keeps
/// its groups, the paging window and the expansion state, so going back is a redraw and not a
/// reload (which is what "the main transcript is restored unchanged" means).
///
/// The body is the real thread renderer over the agent's store: the same
/// `renderThreadBody` the main thread uses, driven by `agentThreadAdapter` bound to
/// the agent's `ChatTab` and `ui.agentThread` - so a focused agent shows thinking
/// rows, tool blocks and the group chrome, not a flat paragraph. The paragraph
/// below is the eviction fallback; a store change under live caches is a full reset.
void renderAgentTranscript(model::UiState& ui, store::ThreadStoreAdapter& agent,
                           model::ThreadState& agentThread, const model::AgentLane& lane,
                           const ui::Rect& region, const ui::Metrics& m, int windowGroups,
                           bool compact, bool acceptKeys) {
    // A region in the wrong unit, or one with no room, paints nothing: the guard the
    // main mount's `renderThread` carries, applied before the back button is measured -
    // a zero-width region would otherwise still draw a back row at `region.y`.
    if (region.w <= 0.0f || region.h <= 0.0f || !ui::rectMatchesProfile(region, m))
        return;
    // The row back. It is a real button and the first row of the region, the way the thread's
    // `Load earlier` row is: the control that leaves a filtered view belongs where the
    // filtered view starts.
    const ui::ButtonSpec backSpec{kTranscriptBackLabel};
    ui::Rect backRect = region;
    backRect.h = m.row;
    backRect.w = std::min(region.w, ui::measureButton(m, backSpec).w);
    {
        const ui::TypeScope labelType(ui::TypeStep::label, m.textGrid);
        ui::button(backSpec, m, backRect);
        if (ui::buttonPressed(backSpec, m, backRect)) {
            // The mode is the thread's own (UiState::focusedAgent), so the row that shows the
            // transcript is the row that clears it - the panel's lane menu goes through the
            // frame layer only because a menu item has no view-state to write from.
            ui.focusedAgent.clear();
        }
    }
    // What is being shown, beside the way back: the lane's identity, so a filtered thread says
    // which agent it is filtered to (a header, never a control). Clipped to the *region*, not to
    // the button's own box: the text starts past the button's right edge, so a clip on `backRect`
    // would draw nothing at all.
    const std::string title = lane.label.empty() ? lane.id : lane.label;
    {
        const ui::TypeScope titleType(ui::TypeStep::label, m.textGrid);
        lineAt(m, region, backRect.right() + m.pad1, backRect.y, title.c_str(),
               ui::ColorToken::textHi);
    }

    const float bodyTop = backRect.bottom();
    const float bodyH = region.bottom() - bodyTop;
    if (bodyH <= 0.0f)
        return;

    // The mount: when the shell bound a store for this lane, the body is
    // the real thread renderer over that store - the paragraph below is only the
    // eviction fallback. The bind is per frame and happens in the shell; a different
    // store than the caches were built from (a focus change) is the adapter's signal
    // to drop its caches and reset this view state.
    if (agent.bound()) {
        ui::Rect body = region;
        body.y = bodyTop;
        body.h = bodyH;
        // No `Load earlier` row in the drill-down: the agent store is capped at a
        // handful of messages, so there is nothing older to reveal. But the window
        // the layout budgets is the *main* thread's - one or three groups in compact -
        // and a full capped store is more groups than that: the mount shows the whole
        // store, however big the frame's window is.
        const int mountGroups = std::max(windowGroups, static_cast<int>(agent.groupCount()));
        renderThreadBody(ui, agent, agentThread, body, ui::Rect{}, m, mountGroups, compact,
                         acceptKeys);
        return;
    }

    // No store entry (evicted under the LRU cap, or the lane never streamed through
    // a store): the paragraph fallback below draws, and the shell leaves the agent
    // adapter unbound for such a frame - no frame ever reads through an evicted
    // store's address. The bind state is also the observable: a fallback frame
    // leaves the adapter unbound, a mount frame leaves it bound.

    ImGui::SetCursorScreenPos(ImVec2(region.x, bodyTop));
    ImGui::BeginChild("##thread_agent_transcript", ImVec2(region.w, bodyH), false);
    {
        const ui::TypeScope bodyType(ui::TypeStep::body, m.textGrid);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::color(ui::ColorToken::textMid));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        if (lane.content.empty()) {
            constexpr char waiting[] = "Waiting for this agent's first output";
            ImGui::TextUnformatted(waiting, waiting + sizeof(waiting) - 1);
        } else {
            const std::string& text = lane.content;
            ImGui::TextUnformatted(text.data(), text.data() + text.size());
        }
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    // Follow the newest, one implementation for every scroller (D-FOLLOW): the transcript is
    // a live stream while its agent runs, so it is read from the end.
    ui.follow = ui::followNewest(ui.follow);
    ImGui::EndChild();
}

ThreadWindow threadWindow(std::size_t groupCount, int windowGroups, int earlierGroups) {
    // A window of zero groups would look like a rendering bug rather than a
    // policy, so a non-positive request means the smallest useful window.
    const std::size_t window = static_cast<std::size_t>(windowGroups > 0 ? windowGroups : 1);
    const std::size_t earlier = static_cast<std::size_t>(earlierGroups > 0 ? earlierGroups : 0);
    const std::size_t wanted = window + earlier;
    ThreadWindow out;
    out.count = std::min(groupCount, wanted);
    out.first = groupCount - out.count;
    out.hidden = out.first;
    return out;
}

int loadEarlierPage(int windowGroups) { return windowGroups > 0 ? windowGroups : 1; }

void requestEarlierPage(model::UiState& ui) {
    // The page is the window the last frame drew with: one press is one page in
    // every mode (40 on the grid, 30 in the pixel profile, 1 or 3 in compact).
    ui.earlierGroups += loadEarlierPage(ui.thread.pageGroups);
}

void renderThread(model::UiState& ui, store::ThreadStoreAdapter& thread, const ui::Rect& region,
                  const ui::Rect& loadEarlierRow, const ui::Metrics& m, int windowGroups,
                  bool compact, bool acceptKeys) {
    // A region in the wrong unit, or one with no room, paints nothing: fail
    // closed rather than draw a plausible thread in the wrong coordinate space.
    if (region.w <= 0.0f || region.h <= 0.0f || !ui::rectMatchesProfile(region, m))
        return;

    // The window this frame is drawn with, published before anything reads it: the
    // `Load earlier` key is dispatched after the shell (tui.cpp), so it pages by the
    // number the thread just used - the frame layer does not own the layout. The
    // drill-down publishes its own window into `ui.agentThread` (the body below does
    // it for whichever view state it was mounted with) and leaves these zeroed on a
    // frame that draws no main-thread group.
    ui.thread.pageGroups = std::max(1, windowGroups);
    ui.thread.drawnGroups = 0;
    ui.thread.firstGroupId = 0;
    ui.thread.firstGroupY = 0.0f;

    // A frame that draws no group cannot anchor anything, and the anchor latch is a
    // one-shot "apply it on the next frame": leaving it armed across a Loading/Offline body
    // or a region with no room would keep the follow suspended until something drew again.
    //
    // The block menu's owner goes with it (task 44): a menu whose block is not drawn is
    // closed by ImGui at the end of this frame, and the latch must not outlive the popup
    // it describes - a recycled `BlockId` would otherwise re-own it. Both latches name
    // something that only exists while the thread draws. The body path does *not* drop
    // them here: the body consumes the anchor latch itself, the one path where the latch
    // must survive into the draw.
    if (ui.view != model::ThreadView::Ready) {
        ui.thread.anchorGroupId = 0;
        ui.thread.anchorPending = false;
        ui.thread.menuBlockId = {};
        renderThreadState(ui.view, m, region);
        return;
    }

    renderThreadBody(ui, thread, ui.thread, region, loadEarlierRow, m, windowGroups, compact,
                     acceptKeys);
}

/// The thread's body, shared by the main thread and the drill-down: the same
/// store walk, the same block loop, the same anchoring and follow, mounted over whatever
/// store/view pair the caller bound. The main thread passes the shell's thread adapter
/// and `ui.thread`; the drill-down passes the agent adapter and `ui.agentThread`. Each
/// mount keeps its own drawing-specific `ThreadState`, so the two share the body, not the state.
void renderThreadBody(model::UiState& ui, store::ThreadStoreAdapter& thread, model::ThreadState& ts,
                      const ui::Rect& region, const ui::Rect& loadEarlierRow, const ui::Metrics& m,
                      int windowGroups, bool compact, bool acceptKeys) {
    // The same latch rule the dispatcher applies to the main view state, applied to
    // the view state this body was mounted with.
    const auto dropLatches = [&ts] {
        ts.anchorGroupId = 0;
        ts.anchorPending = false;
        ts.menuBlockId = {};
    };

    // The window this mount draws with, published into its own view state: the
    // `Load earlier` row below pages by the number the *last frame of this mount*
    // used (`requestEarlierPage` keeps paging the main thread - the shell's key path).
    ts.pageGroups = std::max(1, windowGroups);
    ts.drawnGroups = 0;
    ts.firstGroupId = 0;
    ts.firstGroupY = 0.0f;
    // The indices this frame draws. The live answer is not part of the window (task 38): it is
    // appended after it, so `Load earlier` pages the *stored* conversation and a live answer can
    // never be paged out from under the user, whatever the window arithmetic does. When there is
    // no stream, this is exactly the window and nothing changes.
    const std::size_t storedGroups = thread.storedGroupCount();
    const bool liveAnswer = thread.hasStream();
    const auto drawnIndices = [storedGroups, liveAnswer](const ThreadWindow& window) {
        std::vector<std::size_t> indices;
        indices.reserve(window.count + (liveAnswer ? 1 : 0));
        for (std::size_t i = 0; i < window.count; ++i)
            indices.push_back(window.first + i);
        // The stream is the last group, so its index is the number of stored groups.
        if (liveAnswer)
            indices.push_back(storedGroups);
        return indices;
    };
    ThreadWindow window = threadWindow(storedGroups, windowGroups, ui.earlierGroups);
    std::vector<std::size_t> drawn = drawnIndices(window);

    // The `Load earlier` row is the thread's first row and does not scroll away:
    // it is the only way back to a group the bounded window dropped (D-BOUNDS).
    // The press is applied before the groups are drawn, so the page it reveals
    // appears on the frame the user clicked.
    float firstRowY = region.y;
    if (window.hidden != 0 && loadEarlierRow.h > 0.0f) {
        const ui::ButtonSpec spec{kLoadEarlierLabel};
        ui::Rect buttonRect = loadEarlierRow;
        buttonRect.w = std::min(loadEarlierRow.w, ui::measureButton(m, spec).w);
        ui::button(spec, m, buttonRect);
        // The press is answered before the count is drawn: the page it reveals is
        // part of *this* frame (the groups below are drawn after this row), so a
        // count taken from the window the row was entered with would print "88
        // more" beside a thread that already shows 48.
        if (ui::buttonPressed(spec, m, buttonRect)) {
            ui.earlierGroups += loadEarlierPage(ts.pageGroups);
            window = threadWindow(storedGroups, windowGroups, ui.earlierGroups);
            drawn = drawnIndices(window);
        }
        if (window.hidden != 0) {
            const std::string count = std::to_string(window.hidden) + " more";
            lineAt(m, loadEarlierRow, buttonRect.right() + m.pad1, loadEarlierRow.y, count.c_str(),
                   ui::ColorToken::textLow);
        }
        firstRowY = loadEarlierRow.bottom();
    }

    const float contentH = region.bottom() - firstRowY;
    if (contentH <= 0.0f || drawn.empty()) {
        dropLatches();
        return;
    }

    ImGui::SetCursorScreenPos(ImVec2(region.x, firstRowY));
    // No horizontal scrollbar: a code block renders unwrapped, and one overwide
    // line would otherwise turn into a grey bar along the bottom of the child.
    if (ts.scrollToBottom > 0) {
        // The switch request: `SetNextWindowScroll` is consumed by `BeginChild`,
        // but it clamps against the ScrollMax the child measured *last* frame. The
        // first request therefore lands on the old extent; the second one lands on
        // the extent the first frame measured. That is why the counter survives two
        // drawn frames (a session switch sets it to 2).
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 1.0e9f));
    }
    ImGui::BeginChild("##thread_groups", ImVec2(region.w, contentH), false);
    const float contentW = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    // The box the heights are measured in (design §4.4: a hint is keyed by the width,
    // the line height and the mode). Announced before the first `measureHint()`,
    // because a hint is only valid for the box it was drawn in.
    thread.setHintContext(contentW, compact);

    const std::size_t before = thread.renderCalls();
    // `y` copies the block that holds the focus (task 26, S-13's TUI keyboard path).
    // It is a *character*, not a chord: the text grid never delivers a printable key as
    // a key event (plan/key_gate.md F2), which is why the rail's row keys take the same
    // route. `ui::takeCharacter()` is what keeps a text field's typing its own, and the
    // press is only taken while a block actually holds the focus: a `y` with nothing
    // focused stays in the queue for whoever else may want it this frame.
    const bool copyKey = acceptKeys && ts.focus.valid() && ui::takeCharacter('y');
    // `s` shows the focused block's table source (design §6.8, task 37). The same character
    // route as `y` for the same reason (plan/key_gate.md §4f: the grid never delivers a
    // printable key as a key event), and taken only while a block holds the focus.
    const bool sourceKey = acceptKeys && ts.focus.valid() && ui::takeCharacter('s');
    // Every drawn group's header row, sampled as the group is drawn: the anchor compare
    // needs the row of one specific group, and this is the only place that knows where a
    // group starts. The rows are *child-relative* (net of the window position): the
    // anchor compares a row against the one recorded a frame earlier, and an absolute
    // row would fold a window move between those frames into `Scroll.y`.
    std::vector<std::pair<std::size_t, float>> headerRows;
    headerRows.reserve(drawn.size());
    // The visible band of the child: a group above the clip rect is still drawn (and
    // still occupies content height) but its row cannot be hit, so the "first group"
    // observation the harness clicks is the first *visible* one.
    const ImVec2 clipMin = ImGui::GetWindowDrawList()->GetClipRectMin();
    const ImVec2 clipMax = ImGui::GetWindowDrawList()->GetClipRectMax();
    // The group whose content this frame changed: the anchor follows it. A block reports
    // its own toggle, and the adapter's latch (`takeExpandedGroup`) is the fallback for a
    // change the view did not perform - one answer, two sources, compared rather than
    // guessed at.
    std::size_t toggledGroup = 0;
    // Whether any block submitted an open block menu this frame (task 44). A latch that
    // nothing submitted describes a popup ImGui has closed (Esc, a click elsewhere, the
    // item acting) or a block that is no longer drawn - both mean the owner is stale.
    bool blockMenuOpen = false;
    for (const std::size_t index : drawn) {
        const store::GroupView group = thread.groupAt(index);
        const bool styleMinimal = thread.groupStyleIsMinimal(group.id);
        // The cell is what the caller reserves for the group: its width is the content
        // width and its height the adapter's own expectation. The hint is a hint - the
        // group is drawn in full even when it is taller.
        ui::Rect cell;
        cell.w = contentW;
        cell.h = thread.measureHint(group.id);
        cell.cells = m.textGrid;
        if (!thread.beginGroup(index, cell))
            continue;

        // One block per row: the group's header, then a message's thinking
        // disclosure and its body row, or - for a work group - one row per step. The view
        // draws each row's chrome and the adapter draws the body under it, which is what
        // keeps markdown in the one translation unit that owns it.
        const std::size_t blocks = thread.blockCount(index);
        // The rows up front: the chips line draws between the header and the
        // rows, so the live steps must be known before the first row is drawn -
        // and the work loop below reads the prefetched rows instead of probing
        // the adapter once per row.
        //
        // the query opens the turn above the WORK header and the answer
        // closes it after the group - both are turn-level rows, addressed
        // through the sentinels; a turn without one answers an empty descriptor
        // (group == 0) and draws nothing. The WORK header renders only
        // while the group has work rows - an empty group (the adapter's list
        // always carries the header itself, so one entry) renders headerless
        // and the turn's rows stand alone.
        const model::BlockDesc queryRow = thread.blockAt(index, store::kQueryRow);
        const model::BlockDesc answerRow = thread.blockAt(index, store::kAnswerRow);
        const bool hasWorkRows = blocks >= 2;
        std::vector<model::BlockDesc> rows;
        rows.reserve(blocks);
        if (hasWorkRows)
            for (std::size_t b = 0; b < blocks; ++b)
                rows.push_back(thread.blockAt(index, b));
        // The live edge: the newest drawn group - the last one drawn, a turn
        // only ever starting after every earlier one ended - draws exactly one
        // live element, and only while its style is minimal: the fold is what
        // the edge stands in for, and a drawn group's rows are on screen. The
        // chips line while a call runs: one chip per RUNNING call, tool name
        // only, orange, not clickable. Pending calls draw no chip. The line
        // folds the moment a call goes dormant, mid-batch included - the
        // selection re-reads the store's states every frame.
        bool runningSteps = false;
        std::vector<std::string> runningCalls;
        for (const model::BlockDesc& d : rows) {
            if (d.kind != model::BlockKind::ToolStep || d.state != model::BlockState::Running)
                continue;
            runningSteps = true;
            runningCalls.push_back(d.summary);
        }
        // The chips line's one text, the store's own composer: the same
        // `[bash] [grep]`, `+N` past three, no duplicates the owner header
        // spells - a batch's names read one way, whichever row carries them.
        const std::string liveChips = store::chipsText(runningCalls);
        // Else the folded, count-only Thinking row while the model streams
        // thinking: the live turn's last Thinking-kind row. The rows that
        // follow it are its batch's calls - the store keeps first-emission
        // order and a batch's reasoning precedes its calls - so the row draws
        // while none of them has landed, and folds the moment one does. Its
        // count is the response's live call count, pending calls included,
        // read off those rows. A tool-less batch draws the bare word. A
        // finished turn draws nothing: the fold is the live edge, so it dies
        // with the turn.
        const model::BlockDesc* liveThinking = nullptr;
        if (!runningSteps && liveAnswer && index == drawn.back() && !rows.empty())
            for (std::size_t b = rows.size(); b-- > 0;) {
                if (rows[b].kind != model::BlockKind::Thinking)
                    continue;
                bool landed = false;
                for (std::size_t k = b + 1; k < rows.size(); ++k)
                    if (rows[k].kind == model::BlockKind::ToolStep &&
                        rows[k].state != model::BlockState::Pending)
                        landed = true;
                if (!landed)
                    liveThinking = &rows[b];
                break;
            }
        // The live edge's one element: the chips line while a call runs,
        // else the folded Thinking row while thinking streams, else nothing.
        // `x`/`y` name the line's top-left and `w` its width. The answer is the
        // height the caller must skip (one row or none).
        const auto drawLiveEdge = [&](float x, float y, float w) -> float {
            if (runningSteps) {
                // One line: the composer's answer carries the spacing, so the
                // view places the whole string and measures nothing.
                const float chipsX = x + m.pad2;
                lineAt(m, ui::Rect{chipsX, y, std::max(0.0f, x + w - chipsX), m.row, m.textGrid},
                       chipsX, y, liveChips.c_str(), ui::ColorToken::stateWarn);
                return m.row;
            }
            if (liveThinking != nullptr) {
                // The folded Thinking row's own chrome: the closed disclosure
                // mark, then the word - bare when the response carries no calls
                // yet, the count once they exist - in the ink a folded Thinking
                // row draws. Not clickable: the row is the stream's shadow, the
                // list's own rows are the surface.
                ui::iconMark(m, ui::Rect{x, y, m.gu, m.row, m.textGrid}, ui::Icon::DisclosureClosed,
                             ui::ColorToken::textLow);
                std::string text = "Thinking";
                // The shadow's count is its own: the Tool rows after the
                // shadow's row are the batch's calls (reasoning precedes
                // calls), pending calls included. Not a descriptor
                // field - the row is the stream's shadow, and the drawn
                // rows are what the stream has.
                int liveTools = 0;
                for (auto it = liveThinking + 1; it != rows.data() + rows.size(); ++it)
                    if (it->kind == model::BlockKind::ToolStep)
                        ++liveTools;
                if (liveTools > 0) {
                    text += m.textGrid ? " - " : " \xC2\xB7 ";
                    text += std::to_string(liveTools);
                    text += liveTools == 1 ? " tool call" : " tool calls";
                }
                lineAt(m, ui::Rect{x, y, std::max(0.0f, w), m.row, m.textGrid}, x + m.gu + m.pad1,
                       y, text.c_str(), ui::ColorToken::roleAgent);
                return m.row;
            }
            return 0.0f;
        };
        // One draw for every row the turn owns - the query, the work rows, the
        // answer - so the turn's anatomy shares the one chrome/body path. `b` is
        // the row's adapter address: a work row's list index, a turn-level row's
        // sentinel. The bool is the work list's continuation answer, and it is
        // `true` everywhere: the fold is the rows' `visible`, so
        // no early stop and no exception list survives in the view.
        const auto drawRow = [&](const model::BlockDesc& desc, std::size_t b) -> bool {
            ui::Rect row;
            row.x = ImGui::GetCursorScreenPos().x;
            row.y = ImGui::GetCursorScreenPos().y;
            row.w = std::max(0.0f, ImGui::GetContentRegionAvail().x);
            row.cells = m.textGrid;
            // The batch's tool cards: each card hangs indented beneath the
            // batch's owner row, sharing one left rule - the indent starts
            // before the row is measured, so the card's own body narrows with
            // it. The predicate is ownership, not the mode: a
            // minimal fold hides the card through `visible`, so the shape
            // needs no mode test here.
            const bool card = desc.batchOwned && desc.kind == model::BlockKind::ToolStep;
            // The batch's children - thinking, content, cards - share one
            // indent and one rail under the owner row; the owner itself stays
            // at the group's level. The minimal live-tail segment keeps the
            // turn-level indent: it draws with no owner row above it.
            const bool child = !styleMinimal && (card || (desc.batchOwned && !desc.ownsBatch));
            const bool focused = desc.id() == ts.focus;
            // The loop's whole visibility contract: nothing else in the view
            // may decide whether a row draws - the adapter already folded the
            // owner's openness into it.
            // A row the predicate hides paints nothing, takes no focus, opens
            // no menu, and the loop's caller keeps going.
            if (!desc.visible)
                return true; // paints nothing, the loop's caller keeps going
            // A body segment: no header, so no measure, no
            // InvisibleButton, no menu, no copy chip and no `expanded` read -
            // the body is the row, and the renderer owns its height. Under its
            // owner it takes the batch's pad2, the same indent the batch's
            // cards draw at. With no owner row on screen - the minimal
            // live tail's segment - it takes the mark column instead: the
            // body indent one grid cell in
            // from the group's edge.
            if (desc.bodySegment) {
                const float segIndent = child ? m.pad2 : m.gu;
                ImGui::Indent(segIndent);
                ui::Rect body;
                body.x = ImGui::GetCursorScreenPos().x;
                body.y = ImGui::GetCursorScreenPos().y;
                body.w = std::max(0.0f, ImGui::GetContentRegionAvail().x);
                body.h = 0.0f; // the renderer measures what it draws
                body.cells = m.textGrid;
                thread.renderBlockBody(index, b, body);
                ImGui::Unindent(segIndent);
                return true;
            }
            if (child)
                ImGui::Indent(m.pad2);
            // The block menu's owner: the one block the state names may submit
            // the popup, so an ImGui id that a transcript reset recycled cannot re-own a
            // menu the user opened on a different message.
            // The openness is the adapter's resolved answer:
            // the turn-level sentinels are hard-coded open on the wire side, a
            // pin overrides, the live defaults decide the rest. The view keeps
            // no mode special case: the fold's exceptions are clauses
            // of the one predicate, never branches here.
            const bool rowOpen = desc.expanded;
            const BlockDraw draw{rowOpen, focused, desc.id() == ts.menuBlockId};
            row.h = measureBlockHeader(desc, m, draw);
            const BlockResult result = renderBlock(desc, row, m, draw);
            if (result.menuRequested) {
                // The right-click asked for *this* row's menu: latch the owner, and the
                // next frames submit it from here alone (`blockMenuOpen` below keeps the
                // latch alive exactly as long as the popup really is open).
                ts.menuBlockId = desc.id();
                blockMenuOpen = true;
            }
            if (result.menuOpen)
                blockMenuOpen = true;
            if (result.focusTaken) {
                // Clicking any row focuses that block - and nothing else is focused
                // afterwards, because the focus is one BlockId: the group, the key the
                // adapter gave the block, and its kind, never a position that paging or a
                // new row in front of it could move (D-GROUPID's rule, applied to the
                // focus).
                ts.focus = desc.id();
            }
            if (result.toggled) {
                thread.setBlockExpanded(index, b, !desc.expanded);
                toggledGroup = group.id;
            }
            if (result.copyPressed || (focused && copyKey)) {
                if (copyBlock(thread, index, b)) {
                    // The copy's answer (task 45): exactly one item per press, naming the block
                    // that was copied - the label the row itself draws, or its kind's word when
                    // the descriptor carried none. All three doors (the chip, the menu, `y`) reach
                    // this one line, so a press cannot be answered twice.
                    const std::string label =
                        desc.label.empty() ? std::string(defaultBlockLabel(desc.kind)) : desc.label;
                    model::raiseToast(
                        ui,
                        model::Toast{model::ToastKind::Success, std::string("Copied ") + label, {}},
                        model::agentClockMs(), m.textGrid);
                }
            }
            // The source toggle changes what the body draws, so it is a content change for the
            // anchor exactly like a disclosure: the group's header is what stays put while the
            // body under it grows or shrinks.
            if (focused && sourceKey && toggleTableSource(ts, thread, index, b, desc))
                toggledGroup = group.id;
            if (result.drawBody) {
                // The body sits one step in from the row's rule - the indent a message body
                // takes under its role row.
                ImGui::Indent(m.pad1);
                ui::Rect body;
                body.x = ImGui::GetCursorScreenPos().x;
                body.y = ImGui::GetCursorScreenPos().y;
                // The width the body may use. Its height is the renderer's own (rich_md
                // measures the text it draws), so no height is claimed here - the rect
                // exists to carry the width and the unit the adapter checks.
                body.w = std::max(0.0f, ImGui::GetContentRegionAvail().x);
                body.cells = m.textGrid;
                thread.renderBlockBody(index, b, body);
                ImGui::Unindent(m.pad1);
            }
            if (card) {
                // The shared left rule: from the card's top to its bottom,
                // at the x the indent started from - every card's segment lands
                // on the same vertical, so the batch's cards read as one rail.
                const float cardBottom = ImGui::GetCursorScreenPos().y;
                ImGui::Unindent(m.pad2);
                const float ruleW = std::min(m.textGrid ? 1.0f : 2.0f, row.w);
                if (ruleW > 0.0f && cardBottom > row.y)
                    ui::rule(m, ui::Rect{row.x, row.y, ruleW, cardBottom - row.y, m.textGrid},
                             ui::ColorToken::roleTool);
            } else if (child) {
                ImGui::Unindent(m.pad2);
            }
            // A collapsed work group hides the rows under its header: the header *is* the
            // group, and its steps are the rows `blockCount()` reports. The fold
            // itself is those rows' `visible == false`: the loop
            // draws on, and each hidden row returns at the contract above - no
            // early return, no kind list here. The turn's rows (the query, the
            // answer) stand outside the fold, drawn before and after the loop,
            // and the folded live group names its live steps in one chips line:
            // its edge draws directly under the header, where no other row of
            // the group renders. The edge is the newest drawn group's alone -
            // an older fold's Running call is history the turn left behind, and
            // it paints nothing.
            if (b == 0 && desc.kind == model::BlockKind::WorkGroup && !desc.expanded) {
                if (styleMinimal && group.typedTurn && index == drawn.back()) {
                    const float used = drawLiveEdge(row.x, row.y + row.h, row.w);
                    // The SetPos below is a boundary extension: ImGui only
                    // tolerates one when an item validates it (a Dummy after
                    // the fact). When nothing of the group draws past the
                    // chips - every row the fold hid and no answer row - the
                    // position has no consumer, so it is not moved: the
                    // natural cursor after the chips line is what the group's
                    // height hint and the next group's rows read.
                    const bool drawsOn =
                        answerRow.group != 0 ||
                        std::any_of(rows.begin() + 1, rows.end(),
                                    [](const model::BlockDesc& d) { return d.visible; });
                    if (used > 0.0f && drawsOn)
                        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + row.h + used));
                }
            }
            return true; // the work list draws on
        };
        // The query row opens the turn, above the WORK header.
        if (queryRow.group != 0)
            drawRow(queryRow, store::kQueryRow);
        // The anchor contract: the group's
        // header row is the anchor, and it is sampled HERE - immediately
        // before that row draws - so the anchor is the header's own row,
        // never "the cursor after the query row": an owner row or a body
        // segment the projection invents above the header cannot shift it.
        // A fold changes how many rows follow the header, never where the
        // header starts. A group with no work rows draws no header
        // and records nothing: the cursor then sits on the next group's
        // query row, which is not a header row.
        if (hasWorkRows) {
            const float headerScreenY =
                ImGui::GetCursorScreenPos().y; // BEFORE the header row is drawn
            headerRows.emplace_back(group.id, headerScreenY - ImGui::GetWindowPos().y);
            if (ts.firstGroupId == 0 && headerScreenY >= clipMin.y &&
                headerScreenY + m.row <= clipMax.y) {
                ts.firstGroupId = group.id;
                ts.firstGroupY = headerScreenY;
            }
        }
        if (hasWorkRows)
            for (std::size_t b = 0; b < blocks; ++b)
                if (!drawRow(rows[b], b))
                    break;
        // The answer closes the turn, after the group's rows.
        if (answerRow.group != 0)
            drawRow(answerRow, store::kAnswerRow);
        // The live edge's second site, at the group's bottom after the answer
        // row. The minimal style draws the edge under the folded header (the
        // drawRow case), so a folded group never reaches this site, and a
        // group whose style is not minimal draws its rows on screen - the
        // edge has no row to stand in for there. What does reach it in the
        // minimal style is a group the fold case does not cover: an untyped
        // (legacy) group, and a group with no work rows - the edge stands in
        // for whatever rows such a group hides, and a group with no live
        // element draws nothing (the lambda answers 0).
        const bool foldedRows =
            styleMinimal && group.typedTurn && !rows.empty() && !rows[0].expanded;
        if (index == drawn.back() && styleMinimal && !foldedRows) {
            const ImVec2 edge = ImGui::GetCursorScreenPos();
            (void)drawLiveEdge(edge.x, edge.y, std::max(0.0f, ImGui::GetContentRegionAvail().x));
        }
        // The hint keeps using the height the group drew (not the `cell.h`
        // reservation): `endGroup` stores the drawn height, so a batch folding
        // in place changes the next hint, never the anchor row sampled above
        // the fold.
        thread.endGroup(index, cell);
    }
    ts.drawnGroups = drawn.size();
    // The menu's owner is validated on every drawn frame (task 44): a popup ImGui has
    // closed, or a block that is no longer in the window, drops the latch here - so the
    // next frame cannot submit it from a recycled id.
    if (!blockMenuOpen && ts.menuBlockId.valid())
        ts.menuBlockId = {};
    // One group draw per visible group per frame: the shell mounted the thread once,
    // not twice. (An `assert` is compiled out of the shipped build, so the golden
    // harness reads `renderCalls()` deltas itself - that is the real guard.)
    assert(thread.renderCalls() - before == drawn.size() &&
           "renderThread must draw every visible group exactly once");
    (void)before;

    // Commit the pass: the adapter seeded each group's default expansion when the
    // group first appeared (so a group the window draws now and one `Load earlier`
    // reveals later answer the same), and this is what stops the render path from
    // re-seeding that default over a header the user just toggled.
    thread.commitSeen();

    // The group's header row this frame, or -1 when the group is not in the window
    // (scrolled out, or paged away by `Load earlier`).
    const auto headerRowOf = [&headerRows](std::size_t id) -> float {
        for (const auto& row : headerRows)
            if (row.first == id)
                return row.second;
        return -1.0f;
    };

    // Scroll anchoring and follow-the-newest (D-FOLLOW, task 25).
    //
    // A header click is the user taking the view over, exactly like a wheel notch - so
    // it drops the follow flag, and the frame that answers it never runs the pin. The
    // toggled group's header row is the anchor: recorded now, restored on the next
    // frame - which is the frame ImGui clamps the scroll after the group grew (or
    // shrank) above the fold, so "the header stays where it was clicked" is a property
    // of the anchor rather than of a position test that the pin would confirm itself.
    //
    // The toggle is the *block's* report when the user clicked a row, and the adapter's
    // latch when something else changed the store - the same answer either way, and the
    // latch is read exactly once per frame so a stale id cannot anchor a later one.
    const std::size_t adapterToggled = thread.takeExpandedGroup();
    const std::size_t toggled = toggledGroup != 0 ? toggledGroup : adapterToggled;
    if (toggled != 0) {
        ui.follow = false;
        ts.anchorGroupId = toggled;
        ts.anchorScreenY = headerRowOf(toggled);
        ts.anchorPending = true;
    } else if (ts.anchorPending) {
        const float rowNow = headerRowOf(ts.anchorGroupId);
        if (rowNow >= 0.0f && ts.anchorScreenY >= 0.0f) {
            const float delta = rowNow - ts.anchorScreenY;
            if (delta != 0.0f)
                ImGui::SetScrollY(ImGui::GetScrollY() + delta);
        }
        ts.anchorGroupId = 0;
        ts.anchorPending = false;
    } else {
        // Follow the newest, one implementation for every scroller (D-FOLLOW): the
        // pointer owns the flag, so this is read back into the model every frame.
        ui.follow = ui::followNewest(ui.follow);
    }

    ImGui::EndChild();

    if (ts.scrollToBottom > 0)
        --ts.scrollToBottom;
}

} // namespace llmfun::tui::views
