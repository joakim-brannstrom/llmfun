/// The session rail (views/rail.h owns the contract and the anatomy).
///
/// The file is three layers, in this order: the pure arithmetic (row inks, the
/// two-line rule, the relative time, the visible window), the drawing helpers, and
/// one `renderRail()` that walks the list. Everything the rail *decides* is in the
/// first layer, which is why `./test_rail` can pin the grouping and windowing rules
/// without a window and the drawing without an application.
#include "views/rail.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "model/rail_keys.h"
#include "model/session_rows.h"
#include "model/ui_state.h"
#include "ui/input.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/text.h"
#include "ui/tokens.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace llmfun::tui::views {
namespace {

/// Drawn at the bottom of this file: the row keys, the search Enter and the rail's
/// `Esc` steps of the full anatomy.
void handleRailKeys(model::RailState& rail, const model::RailModel& list, const ui::Rect& region,
                    bool acceptKeys, bool searchActive, bool searchEnter);

using model::RailGroup;
using model::RailModel;
using model::SessionRow;

// ---------------------------------------------------------------------------
// Measurement and drawing helpers
// ---------------------------------------------------------------------------

/// Units a string occupies: cells on the text grid, pixels in the GUI. The view
/// only measures what it is about to draw, so the pixel path may ask the font.
float textUnits(const ui::Metrics& m, const std::string& text) {
    if (text.empty())
        return 0.0f;
    if (m.textGrid)
        return static_cast<float>(ui::displayWidth(text));
    return ImGui::CalcTextSize(text.c_str()).x;
}

/// One line at an absolute position, clipped to `box` and cut with the profile's
/// own ellipsis on the grid (a cell cannot be half drawn, so the cut is part of
/// the contract; in the pixel profile the clip rect does that job).
void lineAt(const ui::Metrics& m, const ui::Rect& box, const std::string& text, ui::ColorToken ink,
            float penX, float room) {
    if (text.empty() || room <= 0.0f)
        return;
    const std::string shown = m.textGrid ? ui::truncate(text, static_cast<int>(room)) : text;
    ImGui::PushClipRect(ImVec2(box.x, box.y), ImVec2(box.right(), box.bottom()), true);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::color(ink));
    ImGui::SetCursorScreenPos(ImVec2(penX, box.y));
    ImGui::TextUnformatted(shown.c_str(), shown.c_str() + shown.size());
    ImGui::PopStyleColor();
    ImGui::PopClipRect();
}

/// One row's hit box: a real `InvisibleButton` covering the row, at an absolute
/// position and claiming no flow space (the `ui::button` contract) - without that,
/// a scrolled child's content extent grows by its own scroll offset every frame.
///
/// The caller pushes the row's id, so two sessions that share a title are still two
/// items and the popup opened from a row belongs to that row alone.
struct RowHit {
    bool hovered = false;
    bool clicked = false;
    bool rightClicked = false;
    bool focused = false;
};

RowHit rowItem(const ui::Rect& r, bool hovered) {
    RowHit hit;
    hit.hovered = hovered;
    if (r.w <= 0.0f || r.h <= 0.0f || ImGui::GetCurrentWindow() == nullptr)
        return hit;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 extent = window->DC.CursorMaxPos;
    ImGui::SetCursorScreenPos(ImVec2(r.x, r.y));
    ImGui::InvisibleButton("##rail_row", ImVec2(r.w, r.h));
    hit.clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    hit.rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    hit.focused = ImGui::IsItemFocused();
    window->DC.CursorMaxPos = extent;
    return hit;
}

/// The search key, in one place for both shapes: `/` asks for the field from anywhere
/// the rail may take keys (design §4.3, "from anywhere except the composer").
///
/// The strip needs it as much as the full anatomy does - more, because the strip is the
/// shape that has no field to focus, so its `/` entry and the key have to mean the same
/// request. The frame layer turns that request into the width that gives the rail a
/// search row (a 7-cell column cannot grow one).
void takeSearchKey(model::RailState& rail, bool acceptKeys) {
    // `ui::takeCharacter()` is the one implementation of "a printable key was typed,
    // consume it" (plan/key_gate.md F2: the TUI delivers printable input as characters
    // only). The rail's row keys are its first consumer; the thread's `y` is the second.
    if (!acceptKeys || !ui::takeCharacter(model::kRailSearchKey))
        return;
    rail.filterFocus = true;
    rail.focusSearch = true;
}

/// The search field's text as the buffer holds it (never null, always terminated).
std::string filterQuery(const model::RailState& rail) { return std::string(rail.filterBuf.data()); }

/// Empty or blanks only is "no filter" - the shipped panel's rule, kept.
bool filterIsEmpty(const std::string& query) {
    for (const char c : query) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\f' && c != '\v')
            return false;
    }
    return true;
}

/// Clear the search field: reset the buffer and bump the id so the widget re-reads
/// it instead of its stale edit state (a select and the `Esc` clear both do this).
void clearFilter(model::RailState& rail) {
    rail.filterBuf.fill('\0');
    ++rail.filterSeq;
}

void initRenameBuf(model::RailState& rail, const std::string& title) {
    rail.renameBuf.fill('\0');
    if (title.size() < rail.renameBuf.size())
        std::copy(title.begin(), title.end(), rail.renameBuf.begin());
    // A title that does not fit the buffer opens the box empty instead of as a
    // silently truncated string - the shipped rail's rule, kept.
}

/// The row a row-key acts on: the row under the pointer, else the row that holds the
/// keyboard-nav focus - and nothing else.
///
/// The first version fell back to the active session and then to the first row, which
/// made a stray `d` (nothing focused, pointer on the thread) delete the *active* session.
/// A key that destroys something must act on a row the user pointed at or navigated to;
/// when there is none, the key does nothing - which is also what makes one `d` enough.
const SessionRow* targetRow(const RailModel& list, const model::RailState& rail) {
    const auto find = [&list](const std::string& id) -> const SessionRow* {
        if (id.empty())
            return nullptr;
        for (const RailGroup& group : list.groups) {
            for (const SessionRow& row : group.rows) {
                if (row.id == id)
                    return &row;
            }
        }
        return nullptr;
    };
    for (const std::string* candidate : {&rail.hoverRowId, &rail.navRowId}) {
        if (const SessionRow* row = find(*candidate))
            return row;
    }
    return nullptr;
}

/// The first row of the list - the search field's Enter target.
const SessionRow* firstRow(const RailModel& list) {
    for (const RailGroup& group : list.groups) {
        if (!group.rows.empty())
            return &group.rows.front();
    }
    return nullptr;
}

/// The narrowest column that can still carry a marker and a few characters of a title.
/// Below it the rail draws its collapsed strip instead of the full anatomy (the width
/// breakpoint hands it 7 cells, where every word would be cut to nothing).
constexpr float kStripMaxGu = 12.0f;

/// One row of the 3-row skeleton (design §4.11): a shimmer bar where the rows will
/// be. The grid gets the static cut mark - a cell renderer cannot shimmer without a
/// redraw, and the design says so.
void renderSkeleton(const ui::Metrics& m, const ui::Rect& box) {
    for (int i = 0; i < 3; ++i) {
        const ui::Rect bar{box.x, box.y + static_cast<float>(i) * m.row, box.w, m.row, m.textGrid};
        if (bar.y >= box.bottom())
            break;
        ui::skeleton(m, bar, -1.0f);
    }
}

/// The row at a flat index (groups in display order) - the strip's index space.
const SessionRow* rowAt(const RailModel& list, std::size_t index) {
    std::size_t seen = 0;
    for (const RailGroup& group : list.groups) {
        if (index < seen + group.rows.size())
            return &group.rows[index - seen];
        seen += group.rows.size();
    }
    return nullptr;
}

/// The state a drawn row is in: rename, then a switch to it, then "you are here", then
/// the run, the dot and the pointer. One function for the full anatomy and the strip,
/// so the two shapes cannot disagree about what a row is.
RailRowState stateFor(const SessionRow& row, const model::RailState& rail, bool hovered) {
    const bool pending = (!rail.pendingId.empty() && rail.pendingId == row.id) ||
                         (!rail.loadingId.empty() && rail.loadingId == row.id);
    if (rail.renameRowId == row.id)
        return RailRowState::Rename;
    if (pending)
        return RailRowState::Pending;
    if (row.active)
        return RailRowState::Active;
    if (row.running)
        return RailRowState::Running;
    if (row.unread)
        return RailRowState::Unread;
    if (hovered)
        return RailRowState::Hover;
    return RailRowState::Rest;
}

/// The state's marker glyph and ink: pending ○ (a switch to it is in flight), else
/// active ●, else at rest ▸. The *state* decides, not the flags - a row a switch is
/// in flight to reads as "waiting", not as "you are here".
ui::Icon markerFor(RailRowState state, bool active) {
    if (state == RailRowState::Pending)
        return ui::Icon::Pending;
    return active ? ui::Icon::Active : ui::Icon::Rest;
}

ui::ColorToken markerInkFor(RailRowState state, bool active) {
    if (state == RailRowState::Pending)
        return ui::ColorToken::stateWarn;
    return active ? ui::ColorToken::accent : ui::ColorToken::textMid;
}

/// The strip's mark: the same state table, but the glyph is the *only* carrier.
///
/// The full anatomy says what a row is four ways - the fill, the left rule, the marker
/// and the time column (where the spinner stands in for the clock). A 7-cell column has
/// one cell per row, so every state a row can be in needs its own glyph: pending ○,
/// running ◐, unread •, active ●, rest ▸. `Running` uses the static mark the design gives
/// reduced motion (§4.9, "no spinners, static ◐") - a cell renderer cannot animate, and
/// the strip has no time column for a spinner to replace.
ui::Icon stripMarkFor(RailRowState state, bool active) {
    switch (state) {
    case RailRowState::Pending:
        return ui::Icon::Pending;
    case RailRowState::Running:
        return ui::Icon::Running;
    case RailRowState::Unread:
        return ui::Icon::Unread;
    case RailRowState::Rest:
    case RailRowState::Hover:
    case RailRowState::Active:
    case RailRowState::Rename:
    case RailRowState::Count:
        break;
    }
    return active ? ui::Icon::Active : ui::Icon::Rest;
}

ui::ColorToken stripMarkInkFor(RailRowState state, bool active) {
    switch (state) {
    case RailRowState::Pending:
        return ui::ColorToken::stateWarn;
    case RailRowState::Running:
    case RailRowState::Unread:
        return ui::ColorToken::accent;
    case RailRowState::Rest:
    case RailRowState::Hover:
    case RailRowState::Active:
    case RailRowState::Rename:
    case RailRowState::Count:
        break;
    }
    return active ? ui::ColorToken::accent : ui::ColorToken::textMid;
}

/// The design's `+ New session` footer, in the rail's own column: one button, as wide
/// as the rail allows.
void renderNewSessionFooter(model::RailState& rail, const ui::Rect& footer, const ui::Metrics& m) {
    if (footer.w <= 0.0f || footer.h <= 0.0f)
        return;
    const ui::ButtonSpec spec{"+ New session"};
    ui::Rect rect = footer;
    rect.w = std::min(footer.w, ui::measureButton(m, spec).w);
    ui::button(spec, m, rect);
    if (ui::buttonPressed(spec, m, rect))
        rail.newRequested = true;
}

} // namespace

// ---------------------------------------------------------------------------
// The pure layer
// ---------------------------------------------------------------------------

const char* railRowStateName(RailRowState state) {
    switch (state) {
    case RailRowState::Rest:
        return "rest";
    case RailRowState::Hover:
        return "hover";
    case RailRowState::Active:
        return "active";
    case RailRowState::Running:
        return "running";
    case RailRowState::Unread:
        return "unread";
    case RailRowState::Pending:
        return "pending";
    case RailRowState::Rename:
        return "rename";
    case RailRowState::Count:
        break;
    }
    return "rest";
}

RailRowInks railRowInks(RailRowState state) {
    // Title and preview inks never change with the state: a row says what it is
    // with its fill, its rule and its marker - never by dimming its label (the pair
    // this task replaces measured 1.18:1). `textDim` is not in this table at all;
    // NFR-09 bars it from carrying a glyph.
    RailRowInks inks{ui::ColorToken::surface, ui::ColorToken::textHi, ui::ColorToken::textMid,
                     ui::ColorToken::lineSoft};
    switch (state) {
    case RailRowState::Rest:
        break;
    case RailRowState::Hover:
        // One step up from the rail's surface: the row under the pointer.
        inks.fill = ui::ColorToken::raised;
        break;
    case RailRowState::Active:
        // Design §5.3's anatomy: the selection fill (accent at 14 %, the slot the
        // theme already binds to FrameBgActive) and a 2 px accent rule.
        inks.fill = ui::ColorToken::selection;
        inks.rule = ui::ColorToken::accent;
        break;
    case RailRowState::Running:
    case RailRowState::Unread:
        // Marks, not fills: the spinner and the dot are the state.
        break;
    case RailRowState::Pending:
        // A switch is queued and D has not answered: the input well, marked with the
        // warning colour - distinct from both "active" (accent) and "rest".
        inks.fill = ui::ColorToken::inset;
        inks.rule = ui::ColorToken::stateWarn;
        break;
    case RailRowState::Rename:
        // The field's own well, so the box reads as an editor and not a selection.
        inks.fill = ui::ColorToken::inset;
        break;
    case RailRowState::Count:
        break;
    }
    return inks;
}

int railRowLines(const ui::Metrics& m, float displayHeight) {
    // The design states the threshold in each profile's own unit (§4.3): 30
    // terminal rows on the grid, 600 px in the pixel profile. A negative or NaN
    // height is "no room", i.e. the compact row - never the roomier one.
    if (!(displayHeight > 0.0f))
        return 1;
    const float threshold = m.textGrid ? 30.0f : 600.0f;
    return displayHeight >= threshold ? 2 : 1;
}

std::string railTimeLabel(long long updatedAtMs, long long nowMs) {
    if (updatedAtMs <= 0)
        return std::string();
    long long delta = nowMs - updatedAtMs;
    if (delta < 0)
        delta = 0; // a future stamp (clock skew) is "now", not a negative age
    const long long minutes = delta / 60'000;
    if (minutes < 1)
        return "now";
    if (minutes < 60)
        return std::to_string(minutes) + "m";
    const long long hours = minutes / 60;
    if (hours < 24)
        return std::to_string(hours) + "h";
    const long long days = hours / 24;
    if (days < 7)
        return std::to_string(days) + "d";
    return std::to_string(days / 7) + "w";
}

RailWindow railWindow(std::size_t slots, float offset, float slotHeight, float viewportH,
                      float scrollY) {
    RailWindow window;
    if (slots == 0 || !(slotHeight > 0.0f) || !(offset == offset) || !(scrollY == scrollY))
        return window;
    const float span = std::max(0.0f, viewportH);
    const float firstSlot = (scrollY - offset) / slotHeight;
    const float lastSlot = (scrollY + span - offset) / slotHeight;
    const auto clampIndex = [slots](double v) {
        if (!(v > 0.0))
            return std::size_t{0};
        return v >= static_cast<double>(slots) ? slots : static_cast<std::size_t>(std::floor(v));
    };
    // The window, plus one item of overscan at its end; the clamp keeps the answer
    // inside the list, so "drawn rows" can never exceed the viewport plus that one
    // row (D-RAIL's scale rule).
    window.first = clampIndex(static_cast<double>(std::floor(firstSlot)));
    window.count = clampIndex(static_cast<double>(std::ceil(lastSlot)) + 1.0) - window.first;
    return window;
}

// ---------------------------------------------------------------------------
// The view
// ---------------------------------------------------------------------------

/// The collapsed rail (design §4.3 "Degradation"): the width breakpoint hands the rail
/// a 7-cell column below the medium width, where the full anatomy has no room for a
/// title. The strip draws what fits and nothing it would have to cut:
///
///     [/]            <- the search entry: asks for the search the strip cannot hold
///     [▸]            <- one mark per session, centred: ● active, ○ pending, • unread,
///     [◐]               ◐ running, ▸ rest (the glyph is the only carrier here)
///     [+ ]           <- new session
///
/// A mark is a real hit box: a click selects that session (the same deferred/queued
/// path the full row takes, through `selectId`), which is what keeps every session
/// reachable while the rail is collapsed.
///
/// The strip carries no dialog and no query. Entering it closes what the full anatomy
/// opened: a queued menu request would open against a row the user has stopped pointing
/// at, a rename box the strip cannot draw would keep an edit alive behind a glyph, and a
/// filter the strip cannot show would hide rows with no way to see or clear it. One
/// policy for all of them, so no rail-local state is ever live but invisible; that is
/// also why `Esc` in the strip belongs to the frame's chain (the rail has nothing of its
/// own to close).
///
/// The `/` entry and the `/` key report the same `focusSearch`; the frame layer turns it
/// into the width request that gives the rail a search row, because a 7-cell column
/// cannot grow one.
void renderStrip(model::RailState& rail, const RailModel& list, const ui::Rect& region,
                 const ui::Metrics& m, bool acceptKeys) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The policy above, applied every strip frame (so a state set *after* the collapse -
    // by the palette, say - cannot survive either). Guarded, because `clearFilter()`
    // bumps the search field's id and that is a one-shot reset, not a per-frame one.
    if (!rail.menuRequestId.empty())
        rail.menuRequestId.clear();
    // The strip draws no row menu either: the mirror goes for the same reason the rename box
    // does (a state the strip cannot show must not stay live behind it).
    if (!rail.menuRowId.empty())
        rail.menuRowId.clear();
    if (!rail.renameRowId.empty()) {
        rail.renameRowId.clear();
        rail.renameFocus = false;
        rail.renameHadFocus = false;
    }
    if (!filterIsEmpty(filterQuery(rail)))
        clearFilter(rail);

    // The search entry. `/` is the key and the same glyph is the affordance: both report
    // `focusSearch`, and the frame layer answers with the width that gives the rail a field.
    const ui::Rect search{region.x + m.pad1, region.y, std::max(0.0f, region.w - 2.0f * m.pad1),
                          m.row, m.textGrid};
    if (search.w > 0.0f) {
        const ui::ButtonSpec searchSpec{"/"};
        ui::button(searchSpec, m, search);
        if (ui::buttonPressed(searchSpec, m, search)) {
            rail.focusSearch = true;
            rail.filterFocus = true;
        }
    }
    takeSearchKey(rail, acceptKeys);

    // The marks. One text row each, in a scrolling child so a long list stays reachable.
    const float rowsTop = region.y + m.row + m.border;
    const float rowsH = std::max(0.0f, region.bottom() - m.row - rowsTop);
    std::string hoveredId;
    bool navOnRow = false;
    if (rowsH > 0.0f) {
        ImGui::SetCursorScreenPos(ImVec2(region.x, rowsTop));
        ImGui::BeginChild("##rail_strip", ImVec2(region.w, rowsH), ImGuiChildFlags_None);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(1.0f, std::max(m.row * static_cast<float>(list.rowCount), rowsH)));
        const float scrollY = ImGui::GetScrollY();
        const RailWindow window = railWindow(list.rowCount, 0.0f, m.row, rowsH, scrollY);
        for (std::size_t i = 0; i < window.count; ++i) {
            const std::size_t index = window.first + i;
            const SessionRow* row = rowAt(list, index);
            if (row == nullptr)
                continue;
            const ui::Rect rowRect{origin.x, origin.y + m.row * static_cast<float>(index), region.w,
                                   m.row, m.textGrid};
            const bool pointerOnRow =
                ImGui::IsMouseHoveringRect(ImVec2(rowRect.x, rowRect.y),
                                           ImVec2(rowRect.right(), rowRect.bottom())) &&
                ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
            ImGui::PushID(row->id.c_str());
            const RowHit hit = rowItem(rowRect, pointerOnRow);
            const RailRowState state = stateFor(*row, rail, pointerOnRow);
            if (state != RailRowState::Rest)
                ui::panel(dl, m, rowRect, railRowInks(state).fill, false);
            const float penX = rowRect.x + std::floor(std::max(0.0f, rowRect.w - m.gu) * 0.5f);
            ui::iconMark(m, ui::Rect{penX, rowRect.y, m.gu, m.row, m.textGrid},
                         stripMarkFor(state, row->active), stripMarkInkFor(state, row->active));
            if (pointerOnRow)
                hoveredId = row->id;
            if (hit.focused) {
                navOnRow = true;
                rail.navRowId = row->id;
            }
            if (hit.clicked) {
                if (!row->active)
                    rail.selectId = row->id;
                clearFilter(rail);
            }
            ImGui::PopID();
            ++rail.drawnRows;
        }
        ImGui::EndChild();
    }
    rail.hoverRowId = hoveredId;
    if (!navOnRow && !rail.navRowId.empty())
        rail.navRowId.clear();

    // The new-session entry: the design's `+`, the widest label seven cells can carry.
    const ui::Rect footer{region.x + m.pad1, region.bottom() - m.row,
                          std::max(0.0f, region.w - 2.0f * m.pad1), m.row, m.textGrid};
    if (footer.w > 0.0f && footer.h > 0.0f) {
        const ui::ButtonSpec plusSpec{"+"};
        ui::Rect plusRect = footer;
        plusRect.w = std::min(footer.w, ui::measureButton(m, plusSpec).w);
        ui::button(plusSpec, m, plusRect);
        if (ui::buttonPressed(plusSpec, m, plusRect))
            rail.newRequested = true;
    }
}

void renderRail(model::RailState& rail, const RailModel& list, const ui::Rect& region,
                const ui::Metrics& m, int rowLines, long long nowMs, bool acceptKeys) {
    // Fail closed: a zero, mixed-unit or unmetered region paints nothing rather than a
    // plausible rail in the wrong coordinate space.
    if (region.w <= 0.0f || region.h <= 0.0f || m.gu <= 0.0f || m.row <= 0.0f ||
        !ui::rectMatchesProfile(region, m))
        return;

    rail.rowLines = rowLines >= 2 ? 2 : 1;
    rail.drawnRows = 0;
    rail.escapeConsumed = false;
    const float rowH = m.row * static_cast<float>(rail.rowLines);

    // The *panel* shape gets a window of its own (the inspector's rule, views/inspector.cpp).
    //
    // Not a detail of ImGui: a child window's draw list is rendered *after* its parent's, so a
    // panel painted into the root window's list cannot cover the thread's own child whatever the
    // call order - the thread's glyphs came through the fill (` YOUarch` at 60x20, task 40's
    // first golden). A window created after it is the only surface that can cover it, and the
    // frame draws this shape after the composer for the same reason. The docked column and the
    // collapsed strip overlap nothing and keep drawing in the root window, where their cells stay
    // exact with the thread beside them.
    if (rail.overlay) {
        ImGui::SetCursorScreenPos(ImVec2(region.x, region.y));
        ImGui::BeginChild("##rail_panel", ImVec2(region.w, region.h), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The rail is its own surface: a fill, and on the grid one run of spaces per row
    // so the fill is a *background* rather than a ghost behind the transcript (the
    // panel shape draws over the thread).
    ui::panel(dl, m, region, ui::ColorToken::surface, false);
    if (m.textGrid) {
        const std::string blank(static_cast<std::size_t>(region.w), ' ');
        for (float y = region.y; y < region.bottom(); y += m.row)
            dl->AddText(ImVec2(region.x, y), ImGui::GetColorU32(ui::color(ui::ColorToken::surface)),
                        blank.c_str());
    }

    const float innerX = region.x + m.pad1;
    const float innerW = std::max(0.0f, region.w - 2.0f * m.pad1);

    // --- the collapsed strip ----------------------------------------------
    // Below the medium width breakpoint the layout hands the rail a 7-cell column,
    // where the full anatomy has no room for a title (the search hint, the group
    // header and every title would be cut to nothing). The design's "degradation" is a
    // strip: a search entry, one mark per session, and the `+` row.
    //
    // The strip takes no row keys: rename and delete need the room the open rail has,
    // and `Ctrl+B` is what opens it. What it does answer is a click (select), the `/`
    // entry and the `/` key - the same `focusSearch` request the full anatomy makes, so
    // a search asked for here is answered here rather than in a shape the user left.
    if (innerW < kStripMaxGu * m.gu) {
        renderStrip(rail, list, region, m, acceptKeys);
        rail.filterHadFocus = false;
        rail.filterNonEmptyLastFrame = !filterIsEmpty(filterQuery(rail));
        if (rail.overlay)
            ImGui::EndChild();
        return;
    }

    const float footerH = m.row;
    const float rowsTop = region.y + m.row + m.border;
    const float rowsH = std::max(0.0f, region.bottom() - footerH - rowsTop);
    const bool loading = !rail.loadingId.empty();

    // --- the search row ----------------------------------------------------
    const ui::Rect search{innerX, region.y, innerW, m.row, m.textGrid};
    bool searchEnter = false;
    bool searchActive = false;
    if (search.w > 0.0f && search.h > 0.0f) {
        ImGui::SetCursorScreenPos(ImVec2(search.x, search.y));
        ImGui::SetNextItemWidth(search.w);
        // The frame that *creates* the field is the one that hands it the keyboard, and
        // `IsItemActive()` answers a frame later in some backends: both count as "the
        // search holds the keyboard" for the end-of-mode rule below, or the frame that
        // opens the search could close it again.
        bool handedFocus = rail.filterFocus;
        if (rail.filterFocus) {
            ImGui::SetKeyboardFocusHere();
            rail.filterFocus = false;
        }
        const std::string searchId = "##rail_search_" + std::to_string(rail.filterSeq);
        searchEnter =
            ImGui::InputTextWithHint(searchId.c_str(), "Search", rail.filterBuf.data(),
                                     rail.filterBuf.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        // The field owns Up/Down while it edits, so the nav focus cannot wander off
        // mid-edit (the shipped panel's hardening, kept).
        if (ImGui::IsItemActive()) {
            ImGuiContext& g = *ImGui::GetCurrentContext();
            g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Up) | (1 << ImGuiDir_Down);
        }
        searchActive = ImGui::IsItemActive() || handedFocus;
    }

    // --- the rows ----------------------------------------------------------
    const ui::Rect rowsBox{region.x, rowsTop, region.w, rowsH, m.textGrid};
    std::string hoveredId;
    bool navOnRow = false;
    // Did any drawn row anchor the row menu this frame? The mirror is the popup's only record of
    // *which* row it belongs to, and a row culled out of the drawn band takes its popup with it
    // (ImGui closes a popup that is not submitted) - so a mirror no row saw has to be dropped, or
    // `views::railOwnsEscape()` keeps warning the composer that an overlay is up (review finding
    // 1).
    bool menuRowSeen = false;
    // The skeleton is for "nothing to show yet", never for "a switch is in flight":
    // a list that already has rows keeps drawing them (the target row shows the
    // pending state), so a switch - acknowledged or not - can never hide the sessions.
    const bool skeleton = loading && list.rowCount == 0;
    if (rowsH > 0.0f) {
        ImGui::SetCursorScreenPos(ImVec2(rowsBox.x, rowsBox.y));
        ImGui::BeginChild("##rail_rows", ImVec2(rowsBox.w, rowsBox.h), ImGuiChildFlags_None);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        // The child's scrollbar overlays the last `ScrollbarSize` pixels of the
        // child, and its padding is outside the work rect. Size the rows to the
        // content region, not to the rail's outer width: a row sized to `rowsBox.w`
        // puts its right-aligned time under the bar (a long list showed "10m" as
        // "1"). `GetContentRegionAvail()` is the width left of the bar.
        const float rowW = std::max(0.0f, ImGui::GetContentRegionAvail().x);

        // The content extent, in O(groups): rows are uniform, so a group is "one
        // header line plus n rows" and a 2000-session snapshot costs six additions.
        float contentH = 0.0f;
        for (const RailGroup& group : list.groups)
            contentH += m.row + rowH * static_cast<float>(group.rows.size());
        ImGui::Dummy(ImVec2(1.0f, std::max(contentH, rowsBox.h)));
        const float scrollY = ImGui::GetScrollY();
        const float bandTop = scrollY;
        const float bandBottom = scrollY + rowsBox.h;

        if (skeleton || list.rowCount == 0) {
            // The bodies a rail with nothing to draw shows instead of a blank
            // column (design §4.11, and the filter's own "no matches").
            const ui::Rect box{origin.x + m.pad1, origin.y, std::max(0.0f, rowW - 2.0f * m.pad1),
                               rowsBox.h, m.textGrid};
            if (skeleton)
                renderSkeleton(m, box);
            else if (list.filteredOut > 0)
                lineAt(m, box, "No matches", ui::ColorToken::textLow, box.x, box.w);
            else
                lineAt(m, box, "No sessions yet", ui::ColorToken::textLow, box.x, box.w);
        }

        float groupTop = 0.0f;
        for (const RailGroup& group : list.groups) {
            const float headerTop = groupTop;
            const float listTop = headerTop + m.row;
            groupTop = listTop + rowH * static_cast<float>(group.rows.size());
            const bool inBand = headerTop < bandBottom && groupTop > bandTop;

            if (!skeleton && inBand && headerTop + m.row > bandTop) {
                const ui::Rect header{origin.x + m.pad1, origin.y + headerTop,
                                      std::max(0.0f, rowW - 2.0f * m.pad1), m.row, m.textGrid};
                lineAt(m, header, model::groupLabel(group.group), ui::ColorToken::textLow, header.x,
                       header.w);
            }
            if (skeleton)
                continue;

            const RailWindow window =
                railWindow(group.rows.size(), listTop, rowH, rowsBox.h, bandTop);
            for (std::size_t i = 0; i < window.count; ++i) {
                const std::size_t index = window.first + i;
                const SessionRow& row = group.rows[index];
                const float rowTop = listTop + rowH * static_cast<float>(index);
                const ui::Rect rowRect{origin.x, origin.y + rowTop, rowW, rowH, m.textGrid};
                // The visibility test is in *content* coordinates (`rowTop`), never in
                // screen ones: `bandTop` is the child's scroll offset, and comparing it
                // with a screen y culls the last rows of the viewport (and draws rows
                // that scrolled out above it).
                if (rowTop >= bandBottom || rowTop + rowH <= bandTop)
                    continue;

                const bool renaming = rail.renameRowId == row.id;
                ImGui::PushID(row.id.c_str());
                // Hover is geometry: the pointer is over the row *and* this window owns
                // it. The shipped rail reads it the same way (task 7's `rowHovered`),
                // because `IsItemHovered()` also answers "did the keyboard take over
                // navigation", which must not make a row stop reporting the pointer on
                // it. A renaming row submits no item at all: the field is over it, and a
                // click there must not select the session *and* discard the draft.
                const bool pointerOnRow =
                    ImGui::IsMouseHoveringRect(ImVec2(rowRect.x, rowRect.y),
                                               ImVec2(rowRect.right(), rowRect.bottom())) &&
                    ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
                // The row menu's affordance is submitted *before* the row's own hit box,
                // so a press on it belongs to the menu and not to the row under it
                // (ImGui gives the hover id to the first item that claims it). While it is
                // up it takes the time's column, so the two never overlap.
                bool affordancePressed = false;
                float affordanceW = 0.0f;
                if (pointerOnRow && !renaming) {
                    affordanceW = std::min(textUnits(m, "...") + 2.0f * m.pad1,
                                           std::max(0.0f, rowRect.w * 0.25f));
                    const ui::Rect affordance{rowRect.right() - m.pad1 - affordanceW, rowRect.y,
                                              affordanceW, m.row, m.textGrid};
                    const ui::ButtonSpec menuSpec{"..."};
                    ui::button(menuSpec, m, affordance);
                    affordancePressed = ui::buttonPressed(menuSpec, m, affordance);
                }
                RowHit hit;
                if (!renaming) {
                    hit = rowItem(rowRect, pointerOnRow);
                } else {
                    hit.hovered = pointerOnRow;
                }

                // "A switch to it is queued or in flight": the deferred slot while a run
                // is active, and the sent-but-unacknowledged id while D loads it. Both
                // read as one state, so a select in flight is visible on its row.
                const RailRowState state = stateFor(row, rail, hit.hovered);
                const RailRowInks inks = railRowInks(state);

                // Fill first: a row always paints its own fill, so its label is read
                // against exactly one colour.
                if (state != RailRowState::Rest)
                    ui::panel(dl, m, rowRect, inks.fill, false);

                // The left rule: the design's 2 px bar in the pixel profile and its
                // one-cell rule glyph on the grid (a background cell would read as a
                // fill, not as the state's mark).
                float textX = rowRect.x + m.pad1;
                const float ruleW = m.textGrid ? m.gu : 2.0f;
                if (state == RailRowState::Active || state == RailRowState::Pending) {
                    if (m.textGrid) {
                        ui::iconMark(m, ui::Rect{textX, rowRect.y, m.gu, m.row, m.textGrid},
                                     ui::Icon::Rule, inks.rule);
                    } else {
                        ui::rule(m, ui::Rect{textX, rowRect.y, ruleW, rowRect.h, false}, inks.rule);
                    }
                }
                textX += ruleW;

                // The marker is the state's second carrier beside the fill and the
                // rule: pending ○ (a switch to it is in flight), else active ●, else at
                // rest ▸ (ASCII `-`/`o`/`>`). The state decides - an active row that a
                // switch is in flight to reads as "waiting", not as "you are here".
                const ui::Icon marker = markerFor(state, row.active);
                const ui::ColorToken markerInk = markerInkFor(state, row.active);
                ui::iconMark(m, ui::Rect{textX, rowRect.y, m.gu, m.row, m.textGrid}, marker,
                             markerInk);
                textX += m.gu + m.pad1;

                // The right-hand side: the relative time, with the unread dot beside it,
                // or the menu affordance while the pointer is on the row. Measured before
                // the title so the title's budget is exact.
                const std::string time = railTimeLabel(row.updatedAtMs, nowMs);
                const float timeW = std::max(textUnits(m, time), row.running ? m.gu : 0.0f);
                const float timeX = rowRect.right() - m.pad1 - timeW;
                const float dotW = (row.unread && !row.active) ? m.gu : 0.0f;
                float reservedRight = 0.0f;
                if (affordanceW > 0.0f) {
                    reservedRight = affordanceW + m.pad1;
                } else {
                    reservedRight = timeW + dotW + (timeW > 0.0f ? m.pad1 : 0.0f);
                    if (dotW > 0.0f) {
                        ui::iconMark(m, ui::Rect{timeX - dotW, rowRect.y, m.gu, m.row, m.textGrid},
                                     ui::Icon::Unread, ui::ColorToken::accent);
                    }
                    if (row.running) {
                        // "running (spinner replaces time)": the clock is replaced by
                        // the mark, so a row with no time still says why it is quiet.
                        ui::spinner(m, ui::Rect{timeX, rowRect.y, timeW, m.row, m.textGrid},
                                    static_cast<double>(nowMs) / 1000.0);
                    } else if (!time.empty()) {
                        lineAt(m, rowRect, time, ui::ColorToken::textLow, timeX, timeW);
                    }
                }

                const float titleRoom =
                    std::max(0.0f, rowRect.right() - m.pad1 - reservedRight - textX);
                if (!renaming) {
                    lineAt(m, ui::Rect{rowRect.x, rowRect.y, rowRect.w, m.row, m.textGrid},
                           row.title, inks.title, textX, titleRoom);
                    if (rail.rowLines == 2) {
                        lineAt(m,
                               ui::Rect{rowRect.x, rowRect.y + m.row, rowRect.w, m.row, m.textGrid},
                               row.preview, inks.preview, textX, titleRoom);
                    }
                }

                // --- interaction -------------------------------------------
                if (hit.hovered) {
                    hoveredId = row.id;
                    // The count lives in the tooltip (the design removed the `[N]`
                    // suffix from the row, not the fact).
                    std::string tip = row.title;
                    if (!row.preview.empty())
                        tip += "\n" + row.preview;
                    tip += "\n" + std::to_string(row.messageCount) + " messages";
                    ui::tooltip(tip);
                }
                if (hit.focused) {
                    navOnRow = true;
                    rail.navRowId = row.id;
                }
                if (hit.clicked) {
                    // Selecting the active session is a no-op; a click while a run is
                    // active is deferred in the single pending slot (last click wins),
                    // which is the shipped semantics - the rail only reports it.
                    if (!row.active)
                        rail.selectId = row.id;
                    clearFilter(rail);
                }

                // The row menu: right-click anywhere on the row, the `m` latch, or a
                // press on the "..." affordance the hovered row carries.
                if (affordancePressed) {
                    rail.menuRowId = row.id;
                    ImGui::OpenPopup("##rail_row_menu");
                }
                if (rail.menuRequestId == row.id) {
                    rail.menuRequestId.clear();
                    rail.menuRowId = row.id;
                    ImGui::OpenPopup("##rail_row_menu");
                }
                if (!renaming && hit.rightClicked &&
                    ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
                    rail.menuRowId = row.id;
                    ImGui::OpenPopup("##rail_row_menu");
                }
                if (rail.menuRowId == row.id) {
                    menuRowSeen = true;
                    // Anchored to its row, and flipped above it when it would not fit
                    // below: ImGui's own clamp bottom-aligns a popup at the display
                    // edge, which paints its spare rows over the row it acts on.
                    const float menuH = rail.menuHeightPx > 0.0f ? rail.menuHeightPx : 4.0f * m.row;
                    float menuY = rowRect.y + rowRect.h;
                    if (menuY + menuH > region.bottom())
                        menuY = std::max(region.y, rowRect.y - menuH);
                    ImGui::SetNextWindowPos(ImVec2(rowRect.x + m.pad1, menuY), ImGuiCond_Appearing);
                    if (ImGui::BeginPopup("##rail_row_menu")) {
                        rail.menuHeightPx = ImGui::GetWindowSize().y;
                        if (ImGui::MenuItem("Rename")) {
                            rail.renameRowId = row.id;
                            initRenameBuf(rail, row.title);
                            rail.renameFocus = true;
                            ++rail.renameSeq;
                        }
                        if (ImGui::MenuItem(row.pinned ? "Unpin" : "Pin"))
                            rail.pinToggleId = row.id;
                        if (ImGui::MenuItem("Delete"))
                            rail.deleteId = row.id;
                        ImGui::EndPopup();
                    }
                    if (!ImGui::IsPopupOpen("##rail_row_menu"))
                        rail.menuRowId.clear();
                }

                // --- the inline rename box ---------------------------------
                if (renaming) {
                    const float fieldX = textX;
                    const float fieldW = std::max(0.0f, rowRect.right() - m.pad1 - fieldX);
                    if (fieldW > 0.0f) {
                        ImGui::PushID(rail.renameSeq);
                        ImGui::SetCursorScreenPos(ImVec2(fieldX, rowRect.y));
                        ImGui::SetNextItemWidth(fieldW);
                        if (rail.renameFocus) {
                            ImGui::SetKeyboardFocusHere();
                            rail.renameFocus = false;
                        }
                        const bool committed = ImGui::InputText(
                            "##rail_rename", rail.renameBuf.data(), rail.renameBuf.size(),
                            ImGuiInputTextFlags_EnterReturnsTrue);
                        const bool fieldActive = ImGui::IsItemActive();
                        ImGui::PopID();
                        if (committed) {
                            const std::string title(rail.renameBuf.data());
                            if (!filterIsEmpty(title)) {
                                rail.renameId = row.id;
                                rail.renamedTitle = title;
                                rail.renameRowId.clear();
                                rail.renameFocus = false;
                                rail.renameHadFocus = false;
                            }
                            // An empty title is refused here and again in D: the box
                            // stays open so the draft is not lost silently.
                        } else if (rail.renameHadFocus && !fieldActive) {
                            // Focus left the field: terminate like Esc and discard the draft.
                            // The latch is needed because ImGui drops a field's active id inside
                            // NewFrame. When the press that took the focus away was `Esc` - the
                            // field's own `cancel_edit` runs *inside* the widget, before
                            // `handleRailKeys()` gets to the key - the press is spent here, or
                            // the frame's chain would cancel the running agent on the same press
                            // (D-ESC: one step per press).
                            rail.renameRowId.clear();
                            rail.renameFocus = false;
                            if (acceptKeys && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                                rail.escapeConsumed = true;
                        }
                        if (!rail.renameId.empty())
                            rail.renameHadFocus = false;
                        else
                            rail.renameHadFocus = fieldActive;
                    }
                }
                ImGui::PopID();
                ++rail.drawnRows;
            }
        }
        ImGui::EndChild();
    }

    // Latches: the pointer's row is kept for the next frame's row keys (cleared when
    // the pointer leaves), and a nav latch no row owns is dropped, so a single click
    // cannot become a permanent target (task 7's rule). A `m` request no drawn row
    // consumed (filtered out, scrolled away) is dropped rather than carried to a row
    // the user is not looking at.
    rail.hoverRowId = hoveredId;
    if (!navOnRow && !rail.navRowId.empty())
        rail.navRowId.clear();
    if (!rail.menuRequestId.empty())
        rail.menuRequestId.clear();
    // A mirror no drawn row saw is a menu whose row left the band: the popup went with the row
    // (ImGui closes a popup it was not asked to submit), so the mirror has to go in the same
    // frame - see `menuRowSeen`'s comment (review finding 1).
    if (!rail.menuRowId.empty() && !menuRowSeen)
        rail.menuRowId.clear();

    // --- the footer --------------------------------------------------------
    const ui::Rect footer{innerX, region.bottom() - footerH, innerW, footerH, m.textGrid};
    renderNewSessionFooter(rail, footer, m);

    handleRailKeys(rail, list, region, acceptKeys, searchActive, searchEnter);
    if (rail.overlay)
        ImGui::EndChild();
}

namespace {

/// The full anatomy's tail: the row keys, the search field's Enter, and the rail's own
/// `Esc` steps.
///
/// The target rule (`targetRow`), `d`'s two presses, `/` from anywhere and the `Esc`
/// order live here once, so the strip cannot grow a second, different policy: it
/// simply does not call this.
void handleRailKeys(model::RailState& rail, const RailModel& list, const ui::Rect& region,
                    bool acceptKeys, bool searchActive, bool searchEnter) {
    // The row keys are characters (plan/key_gate.md F2); `acceptKeys` is the frame's
    // "may this region take this frame's keys", and the row keys additionally need the
    // rail itself: the pointer somewhere on it, or the keyboard focus on one of its
    // rows. Without that, a `d` typed while the user reads the thread would act on a
    // session nobody pointed at (the first version fell back to the active session, so
    // one stray `d` asked to delete it and a second one queued the delete).
    //
    // `/` is the exception the design asks for: "focus search from anywhere except the
    // composer", so it only needs `acceptKeys`.
    const bool pointerOnRail = ImGui::IsMouseHoveringRect(ImVec2(region.x, region.y),
                                                          ImVec2(region.right(), region.bottom()));
    const bool railOwnsKeys = acceptKeys && (pointerOnRail || !rail.navRowId.empty());
    const bool menuKey = railOwnsKeys && ui::takeCharacter(model::kRailMenuKey);
    const bool renameKey = railOwnsKeys && ui::takeCharacter(model::kRailRenameKey);
    const bool pinKey = railOwnsKeys && ui::takeCharacter(model::kRailPinKey);
    takeSearchKey(rail, acceptKeys);
    // `d` deletes the row the user aimed at, in one press. The guard is the *target*
    // (`targetRow()`: the pointer's row or the nav-focused one), not a second press: a
    // destructive key that acts only on a row the user is looking at is already a
    // deliberate act, and the menu's `Delete` - the other path - is a two-click one.
    const bool deleteChar = acceptKeys && ui::takeCharacter(model::kRailDeleteKey);

    if (const SessionRow* target =
            (menuKey || renameKey || deleteChar || pinKey) ? targetRow(list, rail) : nullptr) {
        if (menuKey)
            rail.menuRequestId = target->id;
        if (renameKey) {
            rail.renameRowId = target->id;
            initRenameBuf(rail, target->title);
            rail.renameFocus = true;
            ++rail.renameSeq;
        }
        if (deleteChar)
            rail.deleteId = target->id;
        if (pinKey)
            rail.pinToggleId = target->id;
    }

    // Enter in the search field selects the best match: a non-empty filter ranks
    // best-first, so the first row is the top match (and with no filter it is the
    // first session) - the shipped panel's rule.
    if (searchEnter) {
        if (const SessionRow* top = firstRow(list)) {
            if (!top->active)
                rail.selectId = top->id;
        }
        clearFilter(rail);
    }

    // The rail's own `Esc`, in the D-ESC order: close the rename box, then clear the
    // search. It stands down while the composer owns the keyboard (a press there is the
    // composer's) and marks the press consumed only when it acted on it, so the frame's
    // chain still sees an unconsumed press.
    const bool esc = acceptKeys && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (esc) {
        const std::string query = filterQuery(rail);
        const bool filterReverted = rail.filterNonEmptyLastFrame && filterIsEmpty(query);
        const bool filterOwnsEscape = rail.filterHadFocus || !ImGui::GetIO().WantTextInput;
        if (!rail.renameRowId.empty()) {
            rail.renameRowId.clear();
            rail.renameFocus = false;
            rail.renameHadFocus = false;
            rail.escapeConsumed = true;
        } else if (filterOwnsEscape && (!filterIsEmpty(query) || filterReverted)) {
            clearFilter(rail);
            rail.escapeConsumed = true;
        }
    }

    // End-of-frame snapshot for the `Esc` clear's revert detection next frame: the
    // widget reverts its buffer to its activation value inside NewFrame, so "the
    // query was real" has to be read from the frame before. The focus latch is taken
    // right after the field, before later widgets can move the keyboard.
    rail.filterNonEmptyLastFrame = !filterIsEmpty(filterQuery(rail));
    rail.filterHadFocus = searchActive;

    // The search is a mode and it ends with the mode: the rail was widened to hold a
    // field a 7-cell column cannot, so it gives that width back once the query is empty
    // and the field has let go of the keyboard. `filterHadFocus` is the previous frame's
    // answer, so the frame after the one that handed the keyboard over cannot end it, and
    // a query that is still filtering keeps it wide (`Esc` clears the query first - the
    // step above - and the rail collapses on the frame after that).
    if (rail.searchWide && filterIsEmpty(filterQuery(rail)) && !searchActive &&
        !rail.filterHadFocus)
        rail.searchEnded = true;
}

} // namespace

} // namespace llmfun::tui::views
