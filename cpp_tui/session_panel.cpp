/// Definitions for the session sidebar (session_panel.h).
#include "session_fuzzy.h"
#include "tui.h"
#include "tui_common.h"
#include "tui_widgets.h"

#include "imgui/imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace llmfun::tui {

// Session row title button with filter-match highlighting (R25). Same
// button and widget id as renderButton (id = the full label), same hover /
// active color, but the label bytes covered by `runs` (label offsets [begin,
// end)) are over-drawn in `matchColor`. The overdraw happens on top of the
// just-drawn base label, so an empty `runs` leaves the frame exactly as
// renderButton draws it (one Text call, no extra items). `runs` must stay
// within the title portion of the label so the ellipsis and the " [N]"
// count suffix are never highlighted (R25).
static bool renderTitleButton(const std::string& label, int width, bool active, ImVec4 colorActive,
                              const ImVec4& matchColor,
                              const std::vector<std::pair<std::size_t, std::size_t>>& runs) {
    bool result = false;

    ImGui::PushID(label.c_str());
    const auto p0 = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##but", ImVec2(width, 1))) {
        result = true;
    }

    int npop = 0;
    if (ImGui::IsItemHovered() || active) {
        ImGui::PushStyleColor(ImGuiCol_Text, colorActive);
        ++npop;
    }
    ImGui::SetCursorScreenPos(p0);
    ImGui::Text("%s", label.c_str());
    ImGui::PopStyleColor(npop);
    if (!runs.empty()) {
        const char* lbl = label.c_str();
        for (const auto& run : runs) {
            const float x = p0.x + ImGui::CalcTextSize(lbl, lbl + run.first).x;
            ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
            ImGui::PushStyleColor(ImGuiCol_Text, matchColor);
            ImGui::TextUnformatted(lbl + run.first, lbl + run.second);
            ImGui::PopStyleColor();
        }
        // The overdraw runs are real items, so after the loop SameLine's
        // anchor (DC.CursorPosPrevLine - each item's right edge, published
        // by ItemSize) is the last run's right edge, not the label's.
        // Restore both cursor positions to the label's right edge so
        // sameLineAfterButton places the del button where it sits without a
        // filter, instead of pulling it left over the title.
        const ImVec2 anchor(p0.x + ImGui::CalcTextSize(label.c_str()).x, p0.y);
        ImGui::SetCursorScreenPos(anchor);
        ImGui::GetCurrentWindow()->DC.CursorPosPrevLine = anchor;
        // ImGui::SetCursorScreenPos(p0);
    }
    ImGui::PopID();

    return result;
}

static bool isUtf8Continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// Word separator for the R25 highlight runs: the same four bytes the fuzzy
// matcher treats as word boundaries (A21, fuzzyIsBoundary). Separators are
// always whole ASCII characters, so walking over them never splits a
// multi-byte character.
static bool isWordSeparator(char c) { return c == ' ' || c == '-' || c == '_' || c == '/'; }

// Title bytes shown in a session row of width `rowWidth` (UTF-8-safe
// truncation, ellipsis only when something was cut). Shared by
// sessionRowLabel and the match highlight (R25) so the displayed prefix and
// the highlight clip never disagree.
static std::size_t sessionTitlePortionLen(const SessionEntry& entry, int rowWidth) {
    const std::string count = " [" + std::to_string(entry.messageCount) + "]";
    const int titleBudget = rowWidth - static_cast<int>(count.size());
    if (titleBudget <= 0) {
        return 0;
    }
    if (static_cast<int>(entry.title.size()) <= titleBudget) {
        return entry.title.size();
    }
    // Reserve 3 cells for the ellipsis when there is room for it.
    // titleBudget == 3 fits the ellipsis alone (the title text then
    // contributes nothing); smaller budgets drop the ellipsis entirely.
    int n = titleBudget >= 3 ? titleBudget - 3 : 0;
    while (n > 0 && isUtf8Continuation(entry.title[n - 1])) {
        --n;
    }
    return static_cast<std::size_t>(n);
}

static std::string sessionRowLabel(const SessionEntry& entry, int rowWidth) {
    const std::string count = " [" + std::to_string(entry.messageCount) + "]";
    const std::size_t portion = sessionTitlePortionLen(entry, rowWidth);
    if (portion >= entry.title.size()) {
        return entry.title + count;
    }
    std::string title = entry.title.substr(0, portion);
    // The ellipsis is kept whenever there is room for it or any title text survived.
    const int titleBudget = rowWidth - static_cast<int>(count.size());
    if (portion > 0 || titleBudget >= 3) {
        title += "...";
    }
    return title + count;
}

static void titleMatchRuns(const std::string& query, const std::string& title, std::size_t portion,
                           std::vector<std::pair<std::size_t, std::size_t>>& runs) {
    runs.clear();
    std::vector<std::size_t> pos;
    if (!fuzzyMatchPositions(query, title, pos)) {
        return;
    }
    const std::size_t n = title.size();
    for (std::size_t i = 0; i < pos.size();) {
        std::size_t s = pos[i];
        std::size_t e = s;
        while (i < pos.size() && pos[i] == e) {
            ++e;
            ++i;
        }
        // Snap to the enclosing character(s): a character is highlighted iff
        // any of its bytes matched (no mid-character splits, N7). A matched
        // continuation byte walks s back to its character start; a matched
        // byte whose character continues walks e forward to the end.
        while (s > 0 && isUtf8Continuation(title[s])) {
            --s;
        }
        while (e < n && isUtf8Continuation(title[e])) {
            ++e;
        }
        // The highlight covers the whole word enclosing the matched
        // character(s) (word = maximal span between the A21 separator
        // bytes), so an ASCII prefix match lights an accented word whole
        // ("caf" -> "Café") instead of leaving the accented tail unlit
        // (S13). The walks stop only on separator bytes - always whole
        // ASCII characters - so s/e remain at character starts (N7).
        while (s > 0 && !isWordSeparator(title[s - 1])) {
            --s;
        }
        while (e < n && !isWordSeparator(title[e])) {
            ++e;
        }
        if (s >= portion) {
            continue; // matched only beyond the displayed prefix
        }
        if (e > portion) {
            e = portion; // clip to the character-boundary prefix
        }
        // Two raw runs can snap into the same character; merge on touch.
        if (!runs.empty() && s <= runs.back().second) {
            runs.back().second = std::max(runs.back().second, e);
        } else {
            runs.emplace_back(s, e);
        }
    }
}

// One dimmed preview line: the first user message truncated to the row width
// with an ellipsis. Returns empty for an empty preview so the row stays
// single-line. Control bytes are collapsed to spaces as a backstop on top of
// the D-side normalization, so a preview can never break the two-line layout.
static std::string previewRowLabel(const SessionEntry& entry, int rowWidth) {
    if (entry.preview.empty() || rowWidth <= 0) {
        return std::string();
    }
    std::string preview = entry.preview;
    for (char& c : preview) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7f) {
            c = ' ';
        }
    }
    if (static_cast<int>(preview.size()) <= rowWidth) {
        return preview;
    }
    // Reserve 3 cells for the ellipsis when there is room for it.
    // rowWidth == 3 fits the ellipsis alone; smaller budgets drop it.
    int n = rowWidth >= 3 ? rowWidth - 3 : 0;
    while (n > 0 && isUtf8Continuation(preview[n - 1])) {
        --n;
    }
    std::string out;
    out.assign(preview, 0, n);
    if (n > 0 || rowWidth >= 3) {
        out += "...";
    }
    return out;
}

// Queue one sidebar action for the D side to poll (A2/A7).
static void queueSessionAction(ChatTabSessionPanel& panel, SessionActionType type,
                               const std::string& id, const std::string& title) {
    panel.actions.push_back(SessionAction{type, id, title});
}

// Continue a button row past the previous button's click rect.
// renderButton draws its label at the button origin, so ImGui::SameLine
// anchors on the TEXT item - a label shorter than the button would pull
// the next button left, inside the previous button's rect, making the
// second button unclickable (the first button owns the hover id). Move the
// cursor to the end of the previous button rect plus one spacing instead.
static void sameLineAfterButton(float buttonWidth, float labelWidth) {
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + buttonWidth - labelWidth +
                         ImGui::GetStyle().ItemSpacing.x);
}

// Initialize the rename buffer from a title. A title that does not fit the
// 128-byte buffer initializes the buffer empty - no silent truncation (L4).
static void initRenameBuf(ChatTabSessionPanel& panel, const std::string& title) {
    const size_t bufSize = sizeof(panel.renameBuf);
    if (title.size() < bufSize) {
        std::strncpy(panel.renameBuf, title.c_str(), bufSize - 1);
        panel.renameBuf[bufSize - 1] = '\0';
    } else {
        panel.renameBuf[0] = '\0';
    }
}

// Programmatic filter clear (A23): reset the buffer and bump filterSeq so
// the InputText id changes and the widget re-reads the now-empty user
// buffer instead of its stale internal edit state (C10).
static void clearFilter(ChatTabSessionPanel& panel) {
    panel.filterBuf.fill('\0');
    ++panel.filterSeq;
}

void renderTabChatSessionPanel(TuiState& state, Log& log) {
    auto& panel = state.sessionPanel;

    // Pending-switch flush (A12/R14): a session row clicked while the
    // agent was busy becomes an ordinary Select on the first ready frame.
    // This runs at the VERY TOP, before every early return (mutual
    // exclusion below and the collapsed-state branch further down, M8),
    // so the queued switch applies even when the pipeline panel currently
    // owns the left slot or the panel was collapsed after the click; if
    // the panel is not rendered at all, the slot simply survives until it
    // is. The slot is cleared here, so the flush fires exactly once.
    if (!panel.pendingSelectId.empty() && state.readyStatus) {
        if (panel.pendingSelectId != panel.activeId) {
            queueSessionAction(panel, SessionActionType::Select, panel.pendingSelectId, "");
            log("session panel: flushed pending select %s\n", panel.pendingSelectId.c_str());
        } else {
            // The queued target became the active session in the meantime
            // (e.g. a slash /switch); the flush must not re-select it.
            log("session panel: pending select %s already active; dropped\n",
                panel.pendingSelectId.c_str());
        }
        panel.pendingSelectId.clear();
    }

    // Mutual exclusion (R6/A6/H1): the pipeline panel renders whenever it
    // has agents - open or collapsed - so the session panel must not. Panel
    // state is preserved so the session panel reappears when the pipeline
    // clears.
    if (!state.left.agents.empty())
        return;

    // First open: never offset the output area by 0 on the first frame (F5).
    if (panel.panelW == 0)
        panel.panelW = panel.PanelWActivated;

    // The rename input is bound to the active row; when the active row is
    // absent from the snapshot (deleted session), close the box - an open
    // box bound to a missing row is dead state.
    if (panel.renameActive) {
        auto it = std::find_if(panel.sessions.begin(), panel.sessions.end(),
                               [&panel](const SessionEntry& e) { return e.id == panel.activeId; });
        if (it == panel.sessions.end()) {
            panel.renameActive = false;
            panel.renameFocus = false;
        } else if (panel.renameRowId != panel.activeId) {
            // The active row changed but still exists: re-initialize the
            // buffer from the new active row (L3) and rebind the box.
            panel.renameRowId = panel.activeId;
            initRenameBuf(panel, it->title);
        }
    }

    const auto panelWClosed = 8;
    const bool canQueue = state.readyStatus;

    // Outer child: panel border/background, id unchanged (A15). The header
    // (Close/Open, New, separator) stays fixed; the rows live in their own
    // scrollable child sized to the remaining panel height (R15).
    ImGui::BeginChild("Session child window", ImVec2(panel.panelW - 1, 0), true);
    if (!panel.panelOpen) {
        panel.panelW = panelWClosed;
        if (renderButton("Open", panelWClosed, false, panel.activeButton)) {
            panel.pendingDeleteId.clear();
            panel.panelOpen = true;
            panel.panelW = panel.PanelWActivated;
            log("session panel: open\n");
        }
        ImGui::EndChild();
        return;
    }

    if (renderButton("Close", panelWClosed, false, panel.activeButton)) {
        panel.pendingDeleteId.clear();
        panel.renameActive = false;
        panel.renameFocus = false;
        panel.panelOpen = false;
        panel.panelW = panelWClosed;
        log("session panel: close\n");
    }
    sameLineAfterButton(panelWClosed, ImGui::CalcTextSize("Close").x);
    if (renderButton("New", 3, false, panel.activeButton)) {
        panel.pendingDeleteId.clear();
        if (canQueue) {
            queueSessionAction(panel, SessionActionType::New, "", "");
            log("session panel: queued new session\n");
        } else {
            log("session panel: new skipped (busy)\n");
        }
    }

    renderSeparator("Sessions ", "-", ImGui::GetContentRegionMax().x, true);

    // Filter input (A19): a single-line InputText fixed in the header,
    // between the separator and the rows child, so it stays put while the
    // rows scroll (A15). Click-to-focus only (C11): no
    // SetKeyboardFocusHere, so the always-rendered input never steals
    // keyboard focus from the main query input. The id is suffixed by
    // filterSeq so a programmatic clear (Escape, A23) can force a fresh InputText
    // state (C10). EnterReturnsTrue: the return value is handled after the
    // visible list is computed below (Enter selects the top match, A22/R23).
    // S11: snapshot for the rename-Esc compensation at the end of the rows
    // loop. 1.81's InputText treats Escape as cancel_edit: an ACTIVE input
    // reverts its buffer to the value at activation (InitialTextA,
    // imgui_widgets.cpp:4260-4275). While the rename box is open, Escape
    // must close the box only - if the filter input is the focused one,
    // its same-frame revert would wipe the query along with the box.
    const std::string filterPreFrame(panel.filterBuf.data());
    bool renameEscClosedThisFrame = false;

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const std::string filterId = "##session_filter_" + std::to_string(panel.filterSeq);
    const bool filterEnter =
        ImGui::InputText(filterId.c_str(), panel.filterBuf.data(), panel.filterBuf.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue);

    // while the filter input is active, arrow keys must
    // not move the keyboard nav focus. A single-line InputText declares only
    // Left/Right in ActiveIdUsingNavDirMask when it activates
    // (imgui_widgets.cpp:3983-3985), so Up/Down fall through to NavUpdate's
    // directional checks (imgui.cpp:9085-9088) and move g.NavId onto the next
    // widget mid-edit. Declare Up/Down as used by this widget while it is
    // active; NavUpdate then skips the move request because the active id
    // claims those directions. The mask is only cleared when the active id
    // changes (SetActiveID) or drops to 0 (NewFrame), so the declaration
    // persists for the whole interaction.
    if (ImGui::IsItemActive()) {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Up) | (1 << ImGuiDir_Down);
    }

    // Real-time filter + ranking (A20/A21/A27/A28): compute the visible
    // (filtered + ranked) list each frame from the local snapshot. A
    // whitespace-only filter is "no filter" (A19): all entries in snapshot
    // order, no reordering. Otherwise keep the entries whose title OR preview
    // matches (fuzzyScoreFields >= 0, R26) and rank them by descending score;
    // stable ties keep snapshot order.
    // Per-frame local (N5, no caching): store an index into panel.sessions
    // plus the score, not a copy of the SessionEntry (three std::strings)
    // per match per frame.
    struct VisibleRow {
        std::size_t index; // index into panel.sessions
        int score;         // fuzzyScoreFields (0 when unfiltered)
    };
    std::vector<VisibleRow> visible;
    const std::string filterQuery(panel.filterBuf.data());
    if (isWhitespaceOnly(filterQuery)) {
        visible.reserve(panel.sessions.size());
        for (std::size_t i = 0; i < panel.sessions.size(); ++i) {
            visible.push_back(VisibleRow{i, 0});
        }
    } else {
        for (std::size_t i = 0; i < panel.sessions.size(); ++i) {
            const SessionEntry& e = panel.sessions[i];
            // Multi-field match (R26): title + preview, title weighted
            // 2x. A session shows if either field matches.
            const int s = fuzzyScoreFields(filterQuery, e.title, e.preview);
            if (s >= 0) {
                visible.push_back(VisibleRow{i, s});
            }
        }
        std::stable_sort(
            visible.begin(), visible.end(),
            [](const VisibleRow& a, const VisibleRow& b) { return a.score > b.score; });
    }

    // A28: if the filter hides the active row while the rename box is open,
    // close the box - an open box bound to a hidden row is dead state
    // (mirrors the tuiSetSessionList "active row absent" rule). This also
    // keeps the two Esc branches disjoint: the rename's own Esc check (inside
    // the row loop, active row only) is the only live rename-Esc path, and it
    // runs only when the active row is visible.
    if (panel.renameActive) {
        const bool activeVisible =
            std::any_of(visible.begin(), visible.end(), [&panel](const VisibleRow& v) {
                return panel.sessions[v.index].id == panel.activeId;
            });
        if (!activeVisible) {
            panel.renameActive = false;
            panel.renameFocus = false;
            log("session panel: rename closed (active row filtered out, A28)\n");
        }
    }

    // Escape clears the filter (A23): the filter input has no key binding
    // of its own (InputText only drops focus on Escape), so the key is
    // caught here, before the row loop. The rename box owns Escape while
    // open: its own check runs in the row loop (active row only), and
    // renameActive is read AFTER the A28 close above, so the frame where
    // A28 closed the box (the active row was filtered out by a keystroke in
    // this very input) still clears the filter, while a frame where the box
    // is still live only closes the box - the two Escape paths stay
    // disjoint. A whitespace-only query is already "no filter" (A19), so it
    // never triggers the clear (no pointless id churn). While the input is
    // ACTIVE, 1.81's cancel_edit (NavUpdate Cancel -> ClearActiveID,
    // imgui.cpp:9010-9016) reverts the buffer to its activation value
    // during NewFrame - before this code runs - so "was the query real" is
    // also read from the end-of-last-frame snapshot: a query that was
    // non-empty last frame and is empty now counts as the clear, which
    // keeps C10's filterSeq bump firing on the Esc frame (S18).
    const bool filterRevertedEmpty = panel.filterNonEmptyLastFrame && isWhitespaceOnly(filterQuery);
    if (!panel.renameActive && (!isWhitespaceOnly(filterQuery) || filterRevertedEmpty) &&
        ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape))) {
        clearFilter(panel);
        log("session panel: filter cleared (Escape)\n");
    }

    // Enter selects the top visible match (A22/R23/A24): with a non-empty
    // visible list, Enter in the filter input selects visible[0]'s id. A
    // non-empty filter ranks best-first, so visible[0] is the best match; an
    // empty filter keeps snapshot order, so visible[0] is the first session.
    // Reuses the existing Select path: queue when ready, else defer in the
    // single pending slot (A12, last action wins; it flushes as an ordinary
    // Select on the first ready frame at the top of this function). Selecting
    // the already-active session is a no-op (log only, no action). An empty
    // visible list is a no-op. The filter clears at selection time
    // (clearFilter), independent of the async switch. Enter only fires while the
    // filter input itself is active (EnterReturnsTrue), so it cannot race the
    // rename input's own Enter handling.
    if (filterEnter && !visible.empty()) {
        const std::string& topId = panel.sessions[visible[0].index].id;
        if (topId == panel.activeId) {
            log("session panel: filter select %s skipped (already active)\n", topId.c_str());
        } else if (canQueue) {
            queueSessionAction(panel, SessionActionType::Select, topId, "");
            log("session panel: filter queued select %s\n", topId.c_str());
        } else {
            panel.pendingSelectId = topId;
            log("session panel: filter pending select %s (busy)\n", topId.c_str());
        }
        clearFilter(panel);
    }

    // The rows live in a child of their own, sized to the remaining panel
    // height, so ImGui 1.81 enables the scrollbar when the list overflows
    // (R15). The id is stable, so the scroll position survives snapshot
    // refreshes (the full-replace list does not reset it).
    ImGui::BeginChild("session_rows", ImVec2(panel.panelW - 1, ImGui::GetContentRegionAvail().y),
                      true);

    const int delWidth = 4;
    // Row width budget from the actual clip rect, not the content size. The
    // rows child is bordered and nested inside the bordered panel child, and
    // each border level costs one clipped column, so the draw clip rect is
    // two cells narrower than GetContentRegionAvail().x (29 vs 27 at the
    // default panel width). The vendored imtui backend also drops the last
    // clip column (text vertices clamp at ClipRect.Max - 1 and cells at
    // >= Max are discarded), so a del label sized against the content width
    // loses its last character. ClipRect.Max stays put when a scrollbar
    // appears (the WorkRect shrinks instead), so rows narrow with the
    // scrollbar instead of clipping. The trailing -1 keeps the del button
    // one cell short of the clip edge, like before.
    const int usableWidth = static_cast<int>(std::floor(ImGui::GetContentRegionMax().x)) -
                            static_cast<int>(std::floor(ImGui::GetCursorScreenPos().x)) - 1;
    const int rowWidth =
        usableWidth - delWidth - static_cast<int>(ImGui::GetStyle().ItemSpacing.x) - 1;
    for (const auto& row : visible) {
        const SessionEntry& entry = panel.sessions[row.index];
        const bool isActive = (entry.id == panel.activeId);
        const bool delPending = (panel.pendingDeleteId == entry.id);
        const std::string label = sessionRowLabel(entry, rowWidth);
        const float labelWidth = ImGui::CalcTextSize(label.c_str()).x;
        std::vector<std::pair<std::size_t, std::size_t>> matchRuns;
        if (!isWhitespaceOnly(filterQuery)) {
            titleMatchRuns(filterQuery, entry.title, sessionTitlePortionLen(entry, rowWidth),
                           matchRuns);
        }
        const ImVec4& rowColor =
            (entry.id == panel.pendingSelectId) ? panel.pendingButton : panel.activeButton;
        // The session id keeps row widgets unique even when titles collide.
        ImGui::PushID(entry.id.c_str());
        if (renderTitleButton(label, rowWidth, isActive, rowColor, panel.matchColor, matchRuns)) {
            panel.pendingDeleteId.clear(); // non-delete control (L1)
            if (canQueue && !isActive) {
                queueSessionAction(panel, SessionActionType::Select, entry.id, "");
                log("session panel: queued select %s\n", entry.id.c_str());
            } else if (!canQueue && !isActive) {
                // Busy (A12): defer the switch in the single pending slot
                // (last click wins); it flushes as an ordinary Select on
                // the first ready frame (top of this function). Selecting
                // is safe to defer - the in-flight query completes in the
                // session it was typed in; New/Rename/Delete stay
                // guard-and-skip.
                panel.pendingSelectId = entry.id;
                log("session panel: pending select %s (busy)\n", entry.id.c_str());
            } else {
                // Active-row click: no-op, and any pending target stays
                // armed (A12).
                log("session panel: select %s skipped (already active)\n", entry.id.c_str());
            }
            clearFilter(panel);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(entry.title.c_str());
            if (!entry.preview.empty()) {
                ImGui::TextUnformatted(entry.preview.c_str());
            }
            ImGui::EndTooltip();
        }
        sameLineAfterButton(rowWidth, labelWidth);
        if (renderButton(delPending ? "del?" : "del", delWidth, false, rowColor)) {
            if (canQueue) {
                if (delPending) {
                    // Second press: confirmed (A5).
                    queueSessionAction(panel, SessionActionType::Delete, entry.id, "");
                    panel.pendingDeleteId.clear();
                    log("session panel: queued delete %s\n", entry.id.c_str());
                } else {
                    // First press - or the pending target moves to this row (L1).
                    panel.pendingDeleteId = entry.id;
                    log("session panel: delete armed %s\n", entry.id.c_str());
                }
            } else {
                log("session panel: delete %s skipped (busy)\n", entry.id.c_str());
            }
        }
        if (isActive) {
            // Rename toggle + input on the active row only (R4/L8).
            if (renderButton("Rename", 6, panel.renameActive, panel.activeButton)) {
                panel.pendingDeleteId.clear(); // non-delete control (L1)
                panel.renameActive = !panel.renameActive;
                if (panel.renameActive) {
                    panel.renameRowId = entry.id;
                    initRenameBuf(panel, entry.title);
                    panel.renameFocus = true;
                    ++panel.renameSeq; // fresh InputText state per open
                    log("session panel: rename opened %s\n", entry.id.c_str());
                }
            }
            if (panel.renameActive) {
                ImGui::SameLine();
                if (panel.renameFocus) {
                    ImGui::SetKeyboardFocusHere();
                    panel.renameFocus = false;
                }
                // Fill the rest of the row (the default 0.65 * content width
                // would clip the input's right edge in the narrow panel).
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                const std::string inputId = "##session_rename_" + std::to_string(panel.renameSeq);
                const bool enter =
                    ImGui::InputText(inputId.c_str(), panel.renameBuf, sizeof(panel.renameBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue);

                // The vendored InputText clears its own ActiveId on Escape
                // (1.81 behavior, no EscapeClearsAll flag needed), so
                // IsItemActive() is false by the time we check - gate on
                // the key alone. Trade-off: Escape closes the rename box
                // even when another widget holds the keyboard focus (e.g.
                // the query input); acceptable, because Escape has no other
                // TUI binding - the filter clear (A23) is gated on
                // !renameActive - and canceling the rename is the safe action.
                const bool esc = ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape));
                if (esc) {
                    panel.renameActive = false;
                    panel.renameFocus = false;
                    renameEscClosedThisFrame = true;
                    log("session panel: rename cancelled\n");
                } else if (enter) {
                    const std::string newTitle(panel.renameBuf);
                    if (isWhitespaceOnly(newTitle)) {
                        // Empty titles are rejected here and again in D (L4).
                        log("session panel: empty rename rejected\n");
                    } else if (canQueue) {
                        queueSessionAction(panel, SessionActionType::Rename, entry.id, newTitle);
                        panel.renameActive = false;
                        panel.renameFocus = false;
                        log("session panel: queued rename %s\n", entry.id.c_str());
                    } else {
                        log("session panel: rename skipped (busy)\n");
                    }
                }
            }
        }
        // The rename input stays inside the row's PushID scope so its widget
        // id changes with the row: when the active row changes, the InputText
        // re-reads the (re-initialized) user buffer instead of its stale
        // internal edit state (L3).
        // Line 2: dimmed non-interactive preview of the first user message
        // (A14). Skipped entirely when the preview is empty so rows without
        // one stay single-line; non-interactive (no button, no hover
        // action, no tooltip of its own).
        const std::string preview = previewRowLabel(entry, rowWidth);
        if (!preview.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, panel.previewColor);
            ImGui::TextUnformatted(preview.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    // S11 compensation: when the box just closed via Escape above, the
    // filter widget (if it was the active input) reverted its buffer to
    // InitialTextA on the same frame (cancel_edit, see snapshot above).
    // Restore the pre-frame query and bump filterSeq so the next frame
    // re-initializes a fresh InputText state from the restored buffer
    // (C10 pattern); without the bump the deactivated widget's stale stb
    // (holding the reverted text) would resurface on the next
    // click-to-focus.
    if (renameEscClosedThisFrame && strcmp(panel.filterBuf.data(), filterPreFrame.c_str()) != 0) {
        panel.filterBuf.fill('\0');
        std::copy_n(filterPreFrame.begin(),
                    std::min<std::size_t>(filterPreFrame.size(), panel.filterBuf.size() - 1),
                    panel.filterBuf.begin());
        ++panel.filterSeq;
        log("session panel: filter restored (rename Esc frame)\n");
    }

    // No-match indicator (A26): when a non-empty filter matches nothing
    // but the snapshot is non-empty, render a single dimmed "no matches"
    // line instead of a blank area. The !panel.sessions.empty() clause
    // keeps this consistent with the edge-case table: an empty snapshot
    // renders no rows and no indicator regardless of the filter.
    if (!isWhitespaceOnly(filterQuery) && visible.empty() && !panel.sessions.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, panel.previewColor);
        ImGui::TextUnformatted("no matches");
        ImGui::PopStyleColor();
    }
    // End-of-frame snapshot of the rendered query for A23's revert
    // detection next frame (filterNonEmptyLastFrame); reads the final
    // buffer so the S11 rename-Esc restore is counted.
    panel.filterNonEmptyLastFrame = !isWhitespaceOnly(std::string(panel.filterBuf.data()));
    ImGui::EndChild(); // session_rows
    ImGui::EndChild(); // Session child window
}

int leftPanelWidth(const TuiState& s) {
    // One left-panel slot (A6/H1): the pipeline panel wins whenever it has
    // agents - open or collapsed; otherwise the session panel renders when
    // open (30 wide) and stays an 8-wide "Open" strip when closed, so the
    // output area never covers the panel's Open button (mirrors the
    // pipeline panel's collapsed width).
    return !s.left.agents.empty() ? s.left.panelW
                                  : (s.sessionPanel.panelOpen ? s.sessionPanel.panelW : 8);
}

} // namespace llmfun::tui
