/// The thread region (views/thread.h owns the contract).
///
/// What this file does *not* do: draw a message. It asks the v4 adapter how many
/// groups there are, which one sits where and how tall it expects to be, and then
/// hands each visible group back to the adapter to draw in the current window's
/// flow. The adapter is the only v4-aware code here (D-ADAPTER); the renderer it
/// used to feed is gone (task 41), so what it owns now is the store walk and the
/// block model.
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
/// `Copy` item (task 44) and the `y` key all land here, so a block has exactly one copy
/// path - and the item that asked never chooses the text.
///
/// What reaches the clipboard is the wrapper's decision, not this file's: a `Code`
/// block copies its *code* (`ui::copyText`), everything else its body verbatim - and
/// the wrapper is the only place allowed to know what a fence is.
///
/// Answers whether the clipboard was written: a row with no body copies nothing, which is what
/// keeps a body-less block (the `WORK` header, a stream row before its first token) from
/// claiming a copy nobody got - and from raising the feedback item for one (task 45).
bool copyBlock(store::ThreadStoreAdapter& thread, std::size_t groupIndex, std::size_t block,
               const model::BlockDesc& desc) {
    const std::string body = thread.blockText(groupIndex, block);
    if (body.empty())
        return false;
    const std::string text = ui::copyText(body, desc.kind == model::BlockKind::Code);
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
bool toggleTableSource(model::UiState& ui, store::ThreadStoreAdapter& thread, std::size_t k,
                       std::size_t b, const model::BlockDesc& desc) {
    const model::BlockId id = desc.id();
    if (!id.valid() || !thread.blockHasTable(k, b))
        return false;
    const auto found = ui.thread.tableSource.find(id);
    if (found == ui.thread.tableSource.end())
        ui.thread.tableSource.insert(id);
    else
        ui.thread.tableSource.erase(found);
    return true;
}

/// The drill-down (design §5.21, task 30): one agent's transcript, read-only, under the row
/// that leads back to the conversation.
///
/// It lives in the thread because that is the surface with room for a transcript - the
/// inspector column is 28 cells wide and the design says "the thread filters to that agent's
/// transcript". The *main* transcript is not touched to make this happen: the adapter keeps
/// its groups, the paging window and the expansion state, so going back is a redraw and not a
/// reload (which is what "the main transcript is restored unchanged" means).
///
/// The body is plain flowing text, one body step, wrapped by ImGui's own wrapper - a
/// transcript is prose, not a cell-exact row list - and it scrolls with the one follower
/// every scroller here uses (`ui::followNewest`, D-FOLLOW). No markdown: a tool block would
/// need the fenced-code renderer and the design's tool cards are P3 (task 30's own note).
void renderAgentTranscript(model::UiState& ui, const model::AgentLane& lane, const ui::Rect& region,
                           const ui::Metrics& m) {
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

} // namespace

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
    // number the thread just used - the frame layer does not own the layout.
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
    // something that only exists while the thread draws.
    const auto dropLatches = [&ui] {
        ui.thread.anchorGroupId = 0;
        ui.thread.anchorPending = false;
        ui.thread.menuBlockId = {};
    };

    // The drill-down outranks the state cards: while a lane is focused the thread *is* that
    // agent's transcript, and an Empty/Loading/Offline card would claim the thread has nothing
    // to show while it is showing something (design §5.21, task 30). The focus names a lane
    // that may have been cleared away in the meantime - then it is not a mode any more and the
    // ordinary body draws, exactly like a stale row id in the rail.
    if (const model::AgentLane* focused = model::findAgentLane(ui, ui.focusedAgent)) {
        dropLatches();
        renderAgentTranscript(ui, *focused, region, m);
        return;
    }

    if (ui.view != model::ThreadView::Ready) {
        dropLatches();
        renderThreadState(ui.view, m, region);
        return;
    }

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
            requestEarlierPage(ui);
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
    if (ui.thread.scrollToBottom > 0) {
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
    const bool copyKey = acceptKeys && ui.thread.focus.valid() && ui::takeCharacter('y');
    // `s` shows the focused block's table source (design §6.8, task 37). The same character
    // route as `y` for the same reason (plan/key_gate.md §4f: the grid never delivers a
    // printable key as a key event), and taken only while a block holds the focus.
    const bool sourceKey = acceptKeys && ui.thread.focus.valid() && ui::takeCharacter('s');
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
        const float headerScreenY = ImGui::GetCursorScreenPos().y;
        headerRows.emplace_back(group.id, headerScreenY - ImGui::GetWindowPos().y);
        if (ui.thread.firstGroupId == 0 && headerScreenY >= clipMin.y &&
            headerScreenY + m.row <= clipMax.y) {
            ui.thread.firstGroupId = group.id;
            ui.thread.firstGroupY = headerScreenY;
        }
        // The cell is what the caller reserves for the group: its width is the content
        // width and its height the adapter's own expectation. The hint is a hint - the
        // group is drawn in full even when it is taller.
        ui::Rect cell;
        cell.w = contentW;
        cell.h = thread.measureHint(group.id);
        cell.cells = m.textGrid;
        if (!thread.beginGroup(index, cell))
            continue;

        // One block per row (design §4.5): the group's header, then a message's thinking
        // disclosure and its body row, or - for a work group - one row per step. The view
        // draws each row's chrome and the adapter draws the body under it, which is what
        // keeps markdown in the one translation unit that owns it.
        const std::size_t blocks = thread.blockCount(index);
        for (std::size_t b = 0; b < blocks; ++b) {
            const model::BlockDesc desc = thread.blockAt(index, b);
            ui::Rect row;
            row.x = ImGui::GetCursorScreenPos().x;
            row.y = ImGui::GetCursorScreenPos().y;
            row.w = std::max(0.0f, ImGui::GetContentRegionAvail().x);
            row.cells = m.textGrid;
            const bool focused = desc.id() == ui.thread.focus;
            // The block menu's owner (task 44): the one block the state names may submit
            // the popup, so an ImGui id that a transcript reset recycled cannot re-own a
            // menu the user opened on a different message.
            const BlockDraw draw{desc.expanded, focused, desc.id() == ui.thread.menuBlockId};
            row.h = measureBlockHeader(desc, m, draw);
            const BlockResult result = renderBlock(desc, row, m, draw);
            if (result.menuRequested) {
                // The right-click asked for *this* row's menu: latch the owner, and the
                // next frames submit it from here alone (`blockMenuOpen` below keeps the
                // latch alive exactly as long as the popup really is open).
                ui.thread.menuBlockId = desc.id();
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
                ui.thread.focus = desc.id();
            }
            if (result.toggled) {
                thread.setBlockExpanded(index, b, !desc.expanded);
                toggledGroup = group.id;
            }
            if (result.copyPressed || (focused && copyKey)) {
                if (copyBlock(thread, index, b, desc)) {
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
            if (focused && sourceKey && toggleTableSource(ui, thread, index, b, desc))
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
            // A collapsed work group hides the rows under its header: the header *is* the
            // group, and its steps are the rows `blockCount()` reports.
            if (b == 0 && desc.kind == model::BlockKind::WorkGroup && !desc.expanded)
                break;
        }
        thread.endGroup(index, cell);
    }
    ui.thread.drawnGroups = drawn.size();
    // The menu's owner is validated on every drawn frame (task 44): a popup ImGui has
    // closed, or a block that is no longer in the window, drops the latch here - so the
    // next frame cannot submit it from a recycled id.
    if (!blockMenuOpen && ui.thread.menuBlockId.valid())
        ui.thread.menuBlockId = {};
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
        ui.thread.anchorGroupId = toggled;
        ui.thread.anchorScreenY = headerRowOf(toggled);
        ui.thread.anchorPending = true;
    } else if (ui.thread.anchorPending) {
        const float rowNow = headerRowOf(ui.thread.anchorGroupId);
        if (rowNow >= 0.0f && ui.thread.anchorScreenY >= 0.0f) {
            const float delta = rowNow - ui.thread.anchorScreenY;
            if (delta != 0.0f)
                ImGui::SetScrollY(ImGui::GetScrollY() + delta);
        }
        ui.thread.anchorGroupId = 0;
        ui.thread.anchorPending = false;
    } else {
        // Follow the newest, one implementation for every scroller (D-FOLLOW): the
        // pointer owns the flag, so this is read back into the model every frame.
        ui.follow = ui::followNewest(ui.follow);
    }

    ImGui::EndChild();

    if (ui.thread.scrollToBottom > 0)
        --ui.thread.scrollToBottom;
}

} // namespace llmfun::tui::views
