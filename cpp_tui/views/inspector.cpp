/// The inspector panel (views/inspector.h owns the contract and the anatomy).
///
/// What this file deliberately does not do:
///   * it does not read the aggregate, the display size or the profile flag - the panel is a
///     geometry taker, so `./test_inspector` can draw it into a cell grid with no application
///     state;
///   * it does not own the tab preference: a click or `[`/`]` *asks* (`requestTab`), and the
///     frame layer writes `ShellPrefs::tab` in the same frame - one owner, one writer;
///   * it does not write a colour or a glyph literal: inks come from `ui::color()` and marks
///     from `ui/icons.h`, like every other view (D-THEME).
///
/// Layering note (`tools/gate_ratchet.txt`): no profile branch of its own (`m.textGrid` is
/// data the metrics handed it), no hand-made colour, no v4 type and no display size, so the
/// file needs no ledger row.
#include "views/inspector.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "ui/icons.h"
#include "ui/input.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/text.h"
#include "ui/tokens.h"

#include "model/ui_state.h"
#include "views/topbar.h" // elapsedLabel() - the one clock formatter

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace llmfun::tui::views {

namespace {

using ui::ColorToken;
using ui::Icon;

// ---------------------------------------------------------------------------
// Text helpers (the two every view carries: one measure, one clipped line)
// ---------------------------------------------------------------------------

/// `text`'s width in the profile's own unit: cells on the grid (a wide glyph counts two) and
/// pixels in the pixel profile (the atlas). The pixel half needs `ImGui::CalcTextSize`, which
/// `ui/text.h` deliberately does not reach for, so this per-view copy is what that layering
/// costs (`views/topbar.cpp`, `views/nowbar.cpp` and `ui/primitives.cpp` carry the same lines).
float textUnits(const ui::Metrics& m, std::string_view text) {
    if (text.empty())
        return 0.0f;
    if (m.textGrid)
        return static_cast<float>(ui::displayWidth(text));
    if (ImGui::GetCurrentContext() == nullptr || ImGui::GetFont() == nullptr)
        return 0.0f;
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

/// The icon set this profile draws: the grid's is the terminal's answer, the pixel profile's
/// the atlas' (`ui/icons.h`).
ui::GlyphSet glyphSetFor(const ui::Metrics& m) {
    return m.textGrid ? ui::textGlyphSet() : ui::pixelGlyphSet();
}

/// A measured part as the rectangle the primitives take: the unit comes from the metrics the
/// panel was measured with, stated once here instead of on every part.
ui::Rect rectOf(const InspectorPart& part, const ui::Metrics& m) {
    return ui::Rect{part.x, part.y, part.w, part.h, m.textGrid};
}

/// One line of text left-aligned at `x`, vertically centred in `box` and clipped to it.
///
/// A cell renderer cuts in cells with the profile's own ellipsis (a cell cannot be half
/// drawn) - the same rule `ui::truncate()` implements for every cell-exact surface.
void lineAt(const ui::Metrics& m, const ui::Rect& box, float x, std::string_view text,
            ColorToken ink) {
    if (box.w <= 0.0f || box.h <= 0.0f || text.empty() || x >= box.right())
        return;
    const int room = static_cast<int>(std::max(0.0f, box.right() - x));
    if (room <= 0)
        return;
    const float lineH = ImGui::GetCurrentContext() != nullptr ? ImGui::GetTextLineHeight() : m.row;
    const ImVec2 pen(x, box.y + std::floor((box.h - lineH) * 0.5f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ui::color(ink));
    dl->PushClipRect(ImVec2(box.x, box.y), ImVec2(box.right(), box.bottom()), true);
    if (m.textGrid) {
        const std::string fitted = ui::truncate(text, room);
        dl->AddText(pen, col, fitted.c_str(), fitted.c_str() + fitted.size());
    } else {
        dl->AddText(pen, col, text.data(), text.data() + text.size());
    }
    dl->PopClipRect();
}

/// A fill is a *background* in a cell renderer, so the glyphs of the frame before would show
/// through it. One run of spaces per row is what an opaque fill means on the text grid (the
/// rail, the shell's regions and the composer do the same).
void blankCells(const ui::Metrics& m, const ui::Rect& r, ColorToken fill) {
    if (!m.textGrid || m.row <= 0.0f)
        return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::string blank(static_cast<std::size_t>(std::max(0.0f, r.w)), ' ');
    const ImU32 col = ImGui::GetColorU32(ui::color(fill));
    for (float row = r.y; row < r.bottom(); row += m.row)
        dl->AddText(ImVec2(r.x, row), col, blank.c_str());
}

// ---------------------------------------------------------------------------
// The panel's vocabulary
// ---------------------------------------------------------------------------

/// A tab's label as the strip draws it: the digit first, so the chord is learnable from the
/// screen (`[5] Agents`).
std::string tabText(InspectorTab tab) {
    const int digit = tab == InspectorTab::Agents ? kInspectorAgentsDigit : kInspectorActivityDigit;
    const char* label =
        tab == InspectorTab::Agents ? kInspectorAgentsLabel : kInspectorActivityLabel;
    return "[" + std::to_string(digit) + "] " + label;
}

/// The unread mark: the table's `•` (`Icon::Unread`), the same glyph the rail puts on a
/// session the user has not looked at yet - one meaning, one mark.
std::string unreadMark(const ui::Metrics& m) {
    return std::string(ui::icon(Icon::Unread, glyphSetFor(m)));
}

// ---------------------------------------------------------------------------
// The Agents tab's vocabulary (design §5.21, task 30)
// ---------------------------------------------------------------------------

/// The join between two finished segments of a lane row: ` | ` on the grid and the design's
/// ` · ` in the pixel profile - the now bar's split (`views/nowbar.cpp`), for the same
/// reason: U+00B7 MIDDLE DOT is East-Asian *Ambiguous* (one cell in imtui, two in a CJK
/// terminal), so the cell renderer gets the ASCII pipe.
const char* laneSeparator(const ui::Metrics& m) { return m.textGrid ? " | " : " \u00b7 "; }

/// The mark a lane's state is drawn with (D-BLOCK: glyph + word + colour, and the word is
/// what carries the meaning on a surface that draws the ASCII column).
///
/// `queued` is the table's empty ring (a slot nothing has filled yet), `running` the live
/// mark, `done` the check, `failed` the cross - and `stopped` is *not* a failure: the user
/// asked for it, so it takes the informational mark rather than the error one.
Icon laneStateIcon(std::string_view state) {
    if (state == model::kAgentStateRunning)
        return Icon::Running;
    if (state == model::kAgentStateDone)
        return Icon::Ok;
    if (state == model::kAgentStateFailed)
        return Icon::Error;
    if (state == model::kAgentStateStopped)
        return Icon::Info;
    return Icon::Pending;
}

/// `text` cut to at most `budget` of the profile's own units, with the profile's ellipsis.
///
/// The cell profile cuts with `ui::truncate()` (a cell cannot be half drawn); the pixel
/// profile walks UTF-8 codepoints and measures each prefix with `ImGui::CalcTextSize` - the
/// same shape `ui::fitText()` answers for the same question, which `ui/text.h`
/// cannot answer because a pixel measure needs ImGui.
std::string cutToUnits(const ui::Metrics& m, std::string_view text, float budget) {
    if (text.empty() || budget <= 0.0f)
        return std::string();
    if (textUnits(m, text) <= budget)
        return std::string(text);
    const std::string ellipsis = ui::icon(Icon::Ellipsis, glyphSetFor(m));
    if (m.textGrid)
        return ui::truncate(text, static_cast<int>(budget), ellipsis.c_str());
    const float ellipsisW = textUnits(m, ellipsis);
    const float room = std::max(0.0f, budget - ellipsisW);
    std::size_t end = 0;
    std::size_t best = 0;
    while (end < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[end]);
        std::size_t length = 1;
        if ((lead & 0xE0) == 0xC0)
            length = 2;
        else if ((lead & 0xF0) == 0xE0)
            length = 3;
        else if ((lead & 0xF8) == 0xF0)
            length = 4;
        if (end + length > text.size())
            break;
        end += length;
        if (textUnits(m, text.substr(0, end)) > room)
            break;
        best = end;
    }
    while (best > 0 && text[best - 1] == ' ')
        --best;
    return std::string(text.substr(0, best)) + ellipsis;
}

/// One lane's row: `<mark> <label> | <state> | 00:12 | 1.2k tok | tool: read_file`.
///
/// The degradation is the panel's rule (the strip's own): the *state* - a glyph and a word -
/// is reserved first and never dropped, because it is what the row exists to say (D-BLOCK);
/// the label is cut to whatever room is left beside it; the clock, the token count and the
/// current tool are dropped whole, from the right, when the row cannot hold them. A partial
/// clock or half a tool name would be noise, and every one of them is also visible elsewhere
/// (the now bar's clock, the chips' count, the transcript) - which is why they are the part
/// that goes.
std::string laneRowText(const model::AgentLane& lane, bool focused, const ui::Metrics& m,
                        float budget) {
    const ui::GlyphSet set = glyphSetFor(m);
    // The focus column: the rail's rest mark (`▸`) says "this row is the one you are looking
    // at", and a blank keeps every other row on the same column.
    std::string row = focused ? std::string(ui::icon(Icon::Rest, set)) : std::string(" ");
    row += ' ';
    float used = textUnits(m, row);
    const std::string sep = laneSeparator(m);
    const float sepW = textUnits(m, sep);

    const std::string state =
        std::string(ui::icon(laneStateIcon(lane.state), set)) + " " + lane.state;
    const float stateW = textUnits(m, state);

    std::string label = !lane.label.empty() ? lane.label : lane.role;
    if (label.empty())
        label = lane.id;
    const float labelRoom = std::max(0.0f, budget - used - sepW - stateW);
    label = cutToUnits(m, label, labelRoom);
    if (!label.empty()) {
        row += label;
        row += sep;
        used += textUnits(m, label) + sepW;
    }
    row += state;
    used += stateW;

    // The detail tail, in the design's order, each piece whole or not at all.
    std::vector<std::string> tail;
    if (lane.elapsedMs > 0)
        tail.push_back(views::elapsedLabel(lane.elapsedMs));
    if (lane.tokens > 0)
        tail.push_back(ui::formatCount(lane.tokens) + " tok");
    if (!lane.currentTool.empty())
        tail.push_back("tool: " + lane.currentTool);
    for (const std::string& piece : tail) {
        const float pieceW = textUnits(m, piece);
        if (used + sepW + pieceW > budget)
            continue;
        row += sep;
        row += piece;
        used += sepW + pieceW;
    }
    return row;
}

/// The mission strip: every lane in order, `planner ✓ -> coder ◐ -> reviewer ○`.
///
/// One row, always: it is a summary of the run, so the row is cut at the column's edge
/// rather than wrapped (`ui::truncate()` marks the cut). The lanes keep their rows even when
/// the strip gives up - the strip is the pre-agreed cut (design §9.2 #2), the list is not.
std::string missionRowText(const std::vector<model::AgentLane>& lanes, const ui::Metrics& m,
                           float budget) {
    const ui::GlyphSet set = glyphSetFor(m);
    std::string text;
    for (const model::AgentLane& lane : lanes) {
        if (!text.empty())
            text += kInspectorMissionArrow;
        std::string label = !lane.label.empty() ? lane.label : lane.id;
        text += label;
        text += ' ';
        text += ui::icon(laneStateIcon(lane.state), set);
        // A wide lane list stops being built as soon as the row cannot hold another entry:
        // the cut marker then says there is more, and the wasted work is bounded.
        if (textUnits(m, text) >= budget)
            break;
    }
    if (textUnits(m, text) > budget)
        text = cutToUnits(m, text, budget);
    return text;
}

// ---------------------------------------------------------------------------
// The Activity tab's vocabulary (design §4.8, task 31)
// ---------------------------------------------------------------------------

/// A row's clock as `HH:MM:SS`, in the reader's own zone: a log line is about *when* it
/// happened for the person reading it. The archive keeps UTC (one comparable stamp across
/// machines), and `./test_activity` pins `TZ=UTC` around its captures, so a golden never
/// depends on where it was produced. A record with no stamp (a harness fixture) prints the
/// dashes rather than `01:00:00` on the epoch.
std::string activityClock(long long timestampMs) {
    if (timestampMs <= 0)
        return "--:--:--";
    const std::time_t seconds = static_cast<std::time_t>(timestampMs / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%02d:%02d:%02d", local.tm_hour, local.tm_min,
                  local.tm_sec);
    return std::string(buffer);
}

/// The severity's mark: the table's `◇`/`▲`/`✕` on the glyph column and `i`/`!`/`x` on the
/// ASCII one (`ui/icons.h`). The word beside it is what carries the meaning; the glyph only
/// has to be recognisable (D-BLOCK).
Icon activityLevelIcon(int level) {
    if (level == model::kActivityLevelWarn)
        return Icon::Warn;
    if (level == model::kActivityLevelError)
        return Icon::Error;
    return Icon::Info;
}

/// The ink a whole row is drawn in: the state tokens for warning and error and the ordinary
/// ink for information. Never colour alone - the row prints the word too.
ColorToken activityLevelInk(int level) {
    if (level == model::kActivityLevelWarn)
        return ColorToken::stateWarn;
    if (level == model::kActivityLevelError)
        return ColorToken::stateDanger;
    return ColorToken::textHi;
}

/// The label an action id prints, or `""` for `None` (and for an id the UI has no word for:
/// a row with an unknown action draws no action rather than an empty pair of brackets).
const char* activityActionLabel(model::LogAction action) {
    switch (action) {
    case model::LogAction::Retry:
        return kActivityActionRetry;
    case model::LogAction::Details:
        return kActivityActionDetails;
    case model::LogAction::None:
        break;
    }
    return "";
}

/// One Activity row: `12:04:31  i  info  tool read_file failed  [Retry]`.
///
/// The degradation is the panel's rule: the time and the severity (a glyph *and* a word) are
/// reserved first, because they are what tells two rows apart; the action is dropped whole
/// when the row cannot hold it (half a label is noise, and the record keeps its id whether
/// or not the column can print it); the message takes whatever is left and is cut with the
/// profile's ellipsis, which is also what `cut` reports so the renderer can offer the full
/// text on hover.
///
/// Cost, stated because the ring holds 500 records: this is called for the rows the viewport
/// shows (plus one of overscan), never for the whole list - §6.4 forbids a per-record string
/// on the render path.
struct ActivityRowText {
    std::string text;
    bool cut = false;
};

ActivityRowText activityRowText(const model::LogRecord& record, const ui::Metrics& m,
                                float budget) {
    ActivityRowText out;
    const ui::GlyphSet set = glyphSetFor(m);
    std::string head = activityClock(record.timestampMs);
    head += "  ";
    head += ui::icon(activityLevelIcon(record.level), set);
    head += ' ';
    head += model::activityLevelWord(record.level);
    head += "  ";
    const float used = textUnits(m, head);

    // The action, whole or not at all. One glyph of air must remain for the message beside
    // it: an action that pushes the message off the row would be the only thing the row says.
    std::string action;
    const char* label = activityActionLabel(record.action);
    if (label[0] != '\0') {
        const std::string candidate =
            std::string(kActivityActionOpen) + label + kActivityActionClose;
        const float candidateW = textUnits(m, candidate);
        // Eight cells of message are reserved before the action is drawn: at the docked 28
        // cells a `[Retry]` would otherwise leave one cell of the sentence it belongs to, and
        // the message is what the row exists to say.
        const float kMessageReserve = 8.0f * m.gu;
        if (used + candidateW + 2.0f * m.gu + kMessageReserve <= budget)
            action = candidate;
    }

    const float actionRoom = action.empty() ? 0.0f : textUnits(m, action) + 2.0f * m.gu;
    const float messageRoom = budget - used - actionRoom;
    out.text = head;
    if (!record.message.empty()) {
        out.cut = textUnits(m, record.message) > messageRoom;
        if (messageRoom > 0.0f)
            out.text += cutToUnits(m, record.message, messageRoom);
    }
    if (!action.empty()) {
        out.text += "  ";
        out.text += action;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Hit boxes (the rail's `rowItem`: submit, read, restore the flow extent)
// ---------------------------------------------------------------------------

/// Submit `part` as a hit box and answer whether it was clicked this frame.
///
/// The item claims no flow space (the content extent is restored), so a clickable strip row
/// does not grow the window's content rect - the composer's `button()` does the same for the
/// same reason.
bool partClicked(const InspectorPart& part, const char* id) {
    if (part.w <= 0.0f || part.h <= 0.0f || ImGui::GetCurrentWindow() == nullptr)
        return false;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 extent = window->DC.CursorMaxPos;
    ImGui::SetCursorScreenPos(ImVec2(part.x, part.y));
    ImGui::InvisibleButton(id, ImVec2(part.w, part.h));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    window->DC.CursorMaxPos = extent;
    return clicked;
}

/// The filter's chips: the active one in the inverted pair the tab strip uses (`accent` fill,
/// `canvas` ink) and the rest on the panel's own surface. A press is the whole interaction -
/// there is no menu and no second press to confirm a filter.
void drawFilterChip(InspectorState& state, const InspectorPart& part, const std::string& text,
                    model::ActivityFilter filter, const char* id, const ui::Metrics& m) {
    if (part.empty())
        return;
    const bool active = state.activityFilter == filter;
    ui::chip(m, rectOf(part, m), text, active ? ColorToken::canvas : ColorToken::textMid,
             active ? ColorToken::accent : ColorToken::inset);
    if (partClicked(part, id)) {
        state.activityFilter = filter;
        // A new filter is a new question about the same history: show it from the newest
        // record, exactly like opening the tab does. Never a per-frame pin - one shot.
        state.activityScrollToEnd = true;
    }
}

/// The search box. Live (no Enter needed): the list is a filter over an in-memory ring, so
/// every keystroke can answer immediately, and a search that waits for Enter on a 500-row
/// list would be a mode where the user cannot see what they are narrowing.
void drawActivitySearch(InspectorState& state, const InspectorPart& part, const ui::Metrics& m) {
    if (part.empty() || part.w <= m.pad1 || part.h <= 0.0f)
        return;
    ImGui::SetCursorScreenPos(ImVec2(part.x, part.y));
    ImGui::SetNextItemWidth(part.w);
    ImGui::InputTextWithHint("##inspector_activity_search", kActivitySearchHint,
                             state.activitySearch.data(), state.activitySearch.size());
}

/// One tab: the active one inverted ("reverse video"), the pixel profile underlined, and both
/// a real hit box.
///
/// The grid's inversion is the design's one inverted pair - `accent` fill with `canvas` ink,
/// the pair the primary button uses (`ui/primitives.h`) - and not the 14 % `selection` fill the
/// rail puts behind a selected *row*. Two reasons: a row highlight is a background for a list
/// of rows, while a tab is a one-row label whose whole job is to say "this one"; and at 256
/// colours a dark translucent accent and the panel's own `surface` quantize to the *same* cell
/// colour, so a `selection` fill would be invisible exactly where the grid needs it. The pixel
/// profile keeps the design's underline tabs (a second line under the label, the accent under
/// the active one).
void drawTab(InspectorState& state, const InspectorPart& part, const std::string& text,
             InspectorTab tab, const char* id, const ui::Metrics& m) {
    if (part.empty())
        return;
    const bool active = state.tab == tab;
    ColorToken ink = active ? ColorToken::textHi : ColorToken::textMid;
    if (active && m.textGrid) {
        ui::rule(m, rectOf(part, m), ColorToken::accent);
        ink = ColorToken::canvas;
    }
    lineAt(m, rectOf(part, m), part.x, text, ink);
    if (!m.textGrid) {
        ui::Rect underline{part.x, part.bottom() - m.border, part.w, m.border, m.textGrid};
        ui::rule(m, underline, active ? ColorToken::accent : ColorToken::lineSoft);
    }
    if (partClicked(part, id))
        state.requestTab(tab);
}

/// The keys the panel owns: the character-level `[`/`]` pair (plan/key_gate.md §10/§11 option
/// (a)). The terminal delivers no key event for a printable character, so the chord form
/// (`Ctrl+5`/`Ctrl+6`) cannot reach the grid - and `ui::takeCharacter()` refuses while a text
/// field owns the keyboard, which is what keeps `[` typed into the composer as typing.
///
/// `acceptKeys` is the shell's permission (`renderInspector()`): a surface that owns the `Esc`
/// chain's first step - the palette, or a panel floating over this one - keeps the character
/// keys, so this is checked *before* `ui::takeCharacter()` consumes anything.
void takeTabKeys(InspectorState& state, bool acceptKeys) {
    if (!acceptKeys)
        return;
    if (ui::takeCharacter('['))
        state.requestTab(inspectorPreviousTab(state.tab));
    else if (ui::takeCharacter(']'))
        state.requestTab(inspectorNextTab(state.tab));
}

/// One row's hit boxes: the item is submitted once and both buttons are read off it, so a
/// lane row's click (drill down) and its context menu (right click) cannot be two items that
/// disagree about where the row is.
struct RowHit {
    bool left = false;
    bool right = false;
    bool hovered = false;
};

RowHit submitRow(const ui::Rect& rect, const char* id) {
    RowHit hit;
    if (rect.w <= 0.0f || rect.h <= 0.0f || ImGui::GetCurrentWindow() == nullptr)
        return hit;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 extent = window->DC.CursorMaxPos;
    ImGui::SetCursorScreenPos(ImVec2(rect.x, rect.y));
    ImGui::InvisibleButton(id, ImVec2(rect.w, rect.h));
    hit.left = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    hit.right = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    hit.hovered = ImGui::IsItemHovered();
    window->DC.CursorMaxPos = extent;
    return hit;
}

/// The lane's context menu: stop this lane, arm the run's stop, or leave a drill-down.
///
/// A menu rather than a row of controls because the column is 28 cells wide and a lane row
/// has no room for three buttons; the same shape the rail's row menu has (right click or the
/// `m` character key), and the same reason for it. `Stop lane` is disabled for a lane that is
/// not live - a stopped or finished lane has nothing to stop - and `Stop every agent` only
/// arms the confirmation the action row draws (the two-press rule the design asks for).
void drawLaneMenu(InspectorState& state, const model::AgentLane& lane, const ui::Rect& row,
                  const ui::Rect& bounds, const ui::Metrics& m, bool anyLive, bool open) {
    if (open) {
        state.menuLaneId = lane.id;
        ImGui::OpenPopup("##inspector_lane_menu");
    }
    if (state.menuLaneId != lane.id)
        return;
    // Above the row when the room below it is not enough for the menu (the rail's flip).
    const float menuH = state.menuHeightPx > 0.0f ? state.menuHeightPx : 4.0f * m.row;
    float menuY = row.bottom();
    if (menuY + menuH > bounds.bottom())
        menuY = std::max(bounds.y, row.y - menuH);
    ImGui::SetNextWindowPos(ImVec2(row.x + m.pad1, menuY), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##inspector_lane_menu")) {
        state.menuHeightPx = ImGui::GetWindowSize().y;
        const bool focused = state.focusedLane == lane.id;
        if (ImGui::MenuItem(kInspectorMenuStopLane, nullptr, false, lane.active))
            state.stopLaneId = lane.id;
        if (ImGui::MenuItem(kInspectorMenuStopAll, nullptr, false, anyLive))
            state.stopAllArmed = true;
        ImGui::Separator();
        if (ImGui::MenuItem(kInspectorMenuBack, nullptr, false, focused))
            state.backRequested = true;
        ImGui::EndPopup();
    }
    if (!ImGui::IsPopupOpen("##inspector_lane_menu"))
        state.menuLaneId.clear();
}

/// The panel's whole body, drawn into the window that is current at the call: the root window
/// for the docked column, the panel's own child window while it floats (see `renderInspector()`
/// for why). It opens no window of its own that an early return could leave behind.
void drawPanelBody(InspectorState& state, const std::vector<model::AgentLane>& lanes,
                   const ui::Rect& region, const ui::Metrics& m, bool acceptKeys,
                   const std::deque<model::LogRecord>& activity) {
    // The surface. A docked column needs no edge (the thread beside it is the edge); the
    // floating shape is opaque and bordered, because it covers the transcript.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ui::panel(dl, m, region, ColorToken::surface, state.overlay);
    blankCells(m, region, ColorToken::surface);

    takeTabKeys(state, acceptKeys);

    const InspectorParts parts = inspectorMeasure(state, lanes, region, m, activity);
    // The strip's type scope ends before the body's child window: a `TypeScope` that spans
    // `BeginChild()`/`EndChild()` pushes a font in one window and pops it in another, which
    // ImGui reports as a missing `PopFont()` in the child and one too many in the parent.
    {
        const ui::TypeScope stripType(ui::TypeStep::label, m.textGrid);
        drawTab(state, parts.agents, parts.agentsText, InspectorTab::Agents, "##inspector_agents",
                m);
        drawTab(state, parts.activity, parts.activityText, InspectorTab::Activity,
                "##inspector_activity", m);
        if (!parts.badge.empty()) {
            lineAt(m, rectOf(parts.badge, m), parts.badge.x, parts.badgeText, ColorToken::accent);
            // The badge belongs to the Activity tab: a click on it shows that tab, like a click
            // on the label beside it.
            if (partClicked(parts.badge, "##inspector_badge"))
                state.requestTab(InspectorTab::Activity);
        }
        if (!parts.notice.empty())
            lineAt(m, rectOf(parts.notice, m), parts.notice.x, parts.noticeText,
                   ColorToken::stateWarn);
    }

    // The Activity tab's chrome (task 31): the severity filter and the search box, above the
    // list and outside its scroller - a filter that scrolls away is not a filter. The chips
    // are drawn in the label step they were measured in; the field sizes itself.
    if (state.tab == InspectorTab::Activity) {
        {
            const ui::TypeScope chipType(ui::TypeStep::label, m.textGrid);
            drawFilterChip(state, parts.filterAll, parts.filterAllText, model::ActivityFilter::All,
                           "##inspector_filter_all", m);
            drawFilterChip(state, parts.filterInfo, parts.filterInfoText,
                           model::ActivityFilter::Info, "##inspector_filter_info", m);
            drawFilterChip(state, parts.filterWarn, parts.filterWarnText,
                           model::ActivityFilter::Warn, "##inspector_filter_warn", m);
            drawFilterChip(state, parts.filterError, parts.filterErrorText,
                           model::ActivityFilter::Error, "##inspector_filter_error", m);
        }
        drawActivitySearch(state, parts.search, m);
    }

    // The mission strip (design §5.21): the run in one row, above the list it summarises and
    // outside the scroller - a summary that scrolls away is not a summary.
    if (!parts.mission.empty()) {
        const ui::TypeScope missionType(ui::TypeStep::label, m.textGrid);
        lineAt(m, rectOf(parts.mission, m), parts.mission.x, parts.missionText,
               ColorToken::textMid);
    }

    if (parts.body.w <= 0.0f || parts.body.h <= 0.0f)
        return;

    // The body scrolls independently of the region: its own child, so a long tab body never
    // moves the strip and never paints over the composer below.
    ImGui::SetCursorScreenPos(ImVec2(parts.body.x, parts.body.y));
    ImGui::BeginChild("##inspector_body", ImVec2(parts.body.w, parts.body.h), ImGuiChildFlags_None);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    // The child's scrollbar overlays the last column of its content, and its padding is
    // outside the work rect - the rail's lesson: size a row to the *content* region, not to
    // the panel's outer width.
    const float rowW = std::max(0.0f, ImGui::GetContentRegionAvail().x);
    ImGui::Dummy(ImVec2(1.0f, std::max(m.row * static_cast<float>(parts.bodyRows), parts.body.h)));
    // Quiet by default (task 31): nothing pins the list to its newest row while records
    // arrive - the *content* grows, the view stays where the user left it. The one exception
    // is the frame the tab is shown (an open, a tab switch, a new filter), which is exactly
    // why this is a latch and not a per-frame `SetScrollHereY` (D-FOLLOW's rule: a pin that
    // runs every frame makes the position test self-confirming).
    if (state.tab == InspectorTab::Activity && state.activityScrollToEnd) {
        ImGui::SetScrollHereY(1.0f);
        state.activityScrollToEnd = false;
    }
    const float scrollY = ImGui::GetScrollY();
    const InspectorWindow window = inspectorWindow(parts.bodyRows, scrollY, parts.body.h, m.row);
    // The lane a row id names - the list is the frame's input, so the lookup is local and
    // cannot read a second store (`model::findAgentLane()` is the UiState's).
    const auto laneOf = [&lanes](const std::string& id) -> const model::AgentLane* {
        for (const model::AgentLane& lane : lanes) {
            if (lane.id == id)
                return &lane;
        }
        return nullptr;
    };
    const bool anyLive = model::anyLiveAgentLane(lanes);
    {
        const ui::TypeScope bodyType(ui::TypeStep::body, m.textGrid);
        for (std::size_t i = 0; i < window.count; ++i) {
            const std::size_t index = window.first + i;
            const ui::Rect row{origin.x, origin.y + m.row * static_cast<float>(index), rowW, m.row,
                               m.textGrid};
            // The window can reach one row past the body (that is the overscan); a row the
            // region cannot show is not drawn and not counted, so `drawnRows` stays the number a
            // scale test can compare with what is on screen.
            if (row.y >= parts.body.bottom() || row.bottom() <= parts.body.y)
                continue;
            ++state.drawnRows;
            if (state.tab == InspectorTab::Activity) {
                // The Activity rows (task 31): the empty sentence on the same path as the
                // list, exactly like the Agents tab - a second, throw-away renderer for
                // "nothing here" is the duplication the tab bodies exist to avoid.
                if (parts.activityRows.empty()) {
                    lineAt(m, row, row.x, parts.emptyText, ColorToken::textMid);
                    continue;
                }
                if (index >= parts.activityRows.size())
                    continue;
                const std::size_t recordIndex = parts.activityRows[index];
                if (recordIndex >= activity.size())
                    continue;
                const model::LogRecord& record = activity[recordIndex];
                const ActivityRowText text = activityRowText(record, m, rowW);
                lineAt(m, row, row.x, text.text, activityLevelInk(record.level));
                // A cut message keeps its full text on hover: the row must stay one row, but
                // nothing the user came to read may become unreachable (`kActivityNoMatches`'s
                // rule: never claim less than the tab knows).
                const RowHit hit =
                    submitRow(row, ("##inspector_activity_" + std::to_string(index)).c_str());
                if (hit.hovered && text.cut)
                    ui::tooltip(record.message);
                continue;
            }
            if (parts.laneRows.empty()) {
                // The tab's empty state: the same sentence the panel has always drawn.
                lineAt(m, row, row.x, parts.emptyText, ColorToken::textMid);
                continue;
            }
            if (index >= parts.laneRows.size())
                continue;
            const InspectorParts::LaneRow& laneRow = parts.laneRows[index];
            // A focused lane is tinted (the row says "the thread is showing this one") and the
            // rest keep the ordinary ink: the accent is the app's "this one" colour, used for
            // exactly one row at a time.
            lineAt(m, row, row.x, laneRow.text,
                   laneRow.focused ? ColorToken::accent : ColorToken::textHi);
            const model::AgentLane* lane = laneOf(laneRow.id);
            const RowHit hit = submitRow(row, ("##inspector_lane_" + laneRow.id).c_str());
            if (hit.left)
                state.focusRequest = laneRow.id;
            // The row menu: right click on the row, or `m` while it is under the pointer - the
            // rail's two paths to the same menu, and the terminal's only spelling of a key
            // that is not a chord (`ui::takeCharacter`).
            const bool menuKey = hit.hovered && acceptKeys && ui::takeCharacter('m');
            if ((hit.right || menuKey) && lane != nullptr)
                drawLaneMenu(state, *lane, row, rectOf(parts.body, m), m, anyLive, /*open=*/true);
            else if (state.menuLaneId == laneRow.id && lane != nullptr)
                drawLaneMenu(state, *lane, row, rectOf(parts.body, m), m, anyLive, /*open=*/false);
        }
    }
    ImGui::EndChild();

    // The action row: `Stop all` and its confirmation, at the body's bottom so the control
    // cannot scroll away from the list it acts on.
    if (!parts.stopAll.empty()) {
        const ui::TypeScope actionType(ui::TypeStep::label, m.textGrid);
        const ui::ButtonSpec stopSpec{parts.stopAllText.c_str()};
        ui::Rect stopRect = rectOf(parts.stopAll, m);
        stopRect.w = std::min(stopRect.w, ui::measureButton(m, stopSpec).w);
        ui::button(stopSpec, m, stopRect);
        if (ui::buttonPressed(stopSpec, m, stopRect)) {
            if (state.stopAllArmed)
                state.stopAllRequested = true;
            else
                state.stopAllArmed = true;
        }
        if (!parts.stopNo.empty()) {
            const ui::ButtonSpec noSpec{parts.stopNoText.c_str()};
            ui::Rect noRect = rectOf(parts.stopNo, m);
            noRect.w = std::min(noRect.w, ui::measureButton(m, noSpec).w);
            ui::button(noSpec, m, noRect);
            if (ui::buttonPressed(noSpec, m, noRect))
                state.stopAllArmed = false;
        }
    }
}

} // namespace

/// The `End` catch-up (`views/inspector.h`): the same one-shot latch the filter press arms, on
/// the same rule - show the list from its newest record, once, never per frame. It lives here,
/// outside the anonymous namespace, because the frame's keymap dispatcher calls it (`tui.cpp`).
bool scrollActivityToNewest(InspectorState& state) {
    if (!activityVisible(state))
        return false;
    state.activityScrollToEnd = true;
    return true;
}

const char* inspectorTabName(InspectorTab tab) {
    switch (tab) {
    case InspectorTab::Agents:
        return "agents";
    case InspectorTab::Activity:
        return "activity";
    }
    return "activity";
}

InspectorTab inspectorNextTab(InspectorTab tab) {
    return tab == InspectorTab::Agents ? InspectorTab::Activity : InspectorTab::Agents;
}

InspectorTab inspectorPreviousTab(InspectorTab tab) { return inspectorNextTab(tab); }

const char* inspectorEmptyText(InspectorTab tab) {
    return tab == InspectorTab::Agents ? kInspectorEmptyAgents : kInspectorEmptyActivity;
}

InspectorWindow inspectorWindow(std::size_t rows, float scrollY, float viewportH, float rowH) {
    InspectorWindow window;
    if (rows == 0 || !(rowH > 0.0f) || !(scrollY == scrollY) || !(viewportH == viewportH))
        return window;
    const float span = std::max(0.0f, viewportH);
    const auto clampIndex = [rows](double v) {
        if (!(v > 0.0))
            return std::size_t{0};
        return v >= static_cast<double>(rows) ? rows : static_cast<std::size_t>(std::floor(v));
    };
    // The window: every row the viewport touches, plus one row of overscan at its end (and
    // one above it when `scrollY` is fractional, which the pixel profile's smooth scrolling
    // makes normal; `views/inspector.h` states the exact bound). The clamp keeps the answer
    // inside the rows, and `drawnRows` counts only the rows the body can show.
    window.first = clampIndex(static_cast<double>(std::floor(scrollY / rowH)));
    window.count =
        clampIndex(static_cast<double>(std::ceil((scrollY + span) / rowH)) + 1.0) - window.first;
    return window;
}

InspectorParts inspectorMeasure(const InspectorState& state,
                                const std::vector<model::AgentLane>& lanes, const ui::Rect& region,
                                const ui::Metrics& m,
                                const std::deque<model::LogRecord>& activity) {
    InspectorParts out;
    if (!state.open || region.w <= 0.0f || region.h <= 0.0f || m.gu <= 0.0f || m.row <= 0.0f)
        return out;

    // The strip is set in the label step; the measure pushes it so a number taken outside a
    // draw is the number the drawn strip uses (`nowBarMeasure()`'s rule).
    const ui::TypeScope stripType(ui::TypeStep::label, m.textGrid);

    const float left = region.x + m.pad1;
    const float right = region.right() - m.pad1;
    const float stripY = region.y + (m.textGrid ? 0.0f : m.pad1);

    out.agentsText = tabText(InspectorTab::Agents);
    out.activityText = tabText(InspectorTab::Activity);
    out.emptyText = inspectorEmptyText(state.tab);

    float x = left;
    out.agents = InspectorPart{x, stripY, textUnits(m, out.agentsText), m.row};
    x = out.agents.right() + m.pad1;
    out.activity = InspectorPart{x, stripY, textUnits(m, out.activityText), m.row};

    // The badge: the count while it fits, the bare mark after that (the strip keeps "there is
    // something new" even where the number cannot be shown), nothing when there is nothing
    // unread. It sits after the Activity label and belongs to that tab.
    if (state.unreadActivity > 0) {
        const std::string mark = unreadMark(m);
        const float bx = out.activity.right() + static_cast<float>(kInspectorBadgeGapGu) * m.gu;
        const float room = std::max(0.0f, right - bx);
        const std::string counted = mark + std::to_string(state.unreadActivity);
        const float countedW = textUnits(m, counted);
        const float markW = textUnits(m, mark);
        if (countedW <= room) {
            out.badgeText = counted;
            out.badge = InspectorPart{bx, stripY, countedW, m.row};
        } else if (markW <= room) {
            out.badgeText = mark;
            out.badge = InspectorPart{bx, stripY, markW, m.row};
        }
    }

    float nextY = stripY + m.row;
    // The reserved-digit notice: a second strip row, only while it is up. It is the one
    // string here that may be cut - a notice is not a control.
    if (state.reservedPressed) {
        out.noticeText = kInspectorReservedHint;
        out.notice = InspectorPart{
            left, nextY, std::min(textUnits(m, out.noticeText), std::max(0.0f, right - left)),
            m.row};
        nextY += m.row;
    }

    // The body: the rest of the region, inset by the panel's own padding. The hairline is the
    // pixel profile's edge between the strip and the content; a cell row already separates
    // them on the grid.
    const float pad = m.textGrid ? 0.0f : m.pad1;
    const float bodyTop = nextY + pad;
    const float bodyBottom = region.bottom() - pad;
    const float bodyW = std::max(0.0f, region.w - 2.0f * m.pad1);
    float top = bodyTop;
    float bottom = bodyBottom;

    // ---- the Agents tab (design §5.21, task 30) ----------------------------
    //
    // Three bands, top to bottom: the mission strip, the lane list (the scroller) and the
    // action row. The strip and the action row are *fixed* - a summary that scrolls away is
    // not a summary, and a `Stop all` that scrolls away is a control the user cannot reach -
    // so the scroller is what is left between them. Both are given up before the list is:
    // two rows of chrome for one lane row is the wrong trade at a 15-row body.
    // The row text is built against the row's own budget, so a golden, a test and the screen
    // read the same string; the scroller's scrollbar overlays its last column, hence the
    // `pad1` of slack in the budget (a pixel-profile scrollbar is `pad1` wide there).
    const bool agentsTab = state.tab == InspectorTab::Agents && !lanes.empty();
    const float rowBudget = std::max(0.0f, bodyW - pad);
    const bool roomForChrome = bottom - top >= 3.0f * m.row;
    if (agentsTab && roomForChrome) {
        out.missionText = missionRowText(lanes, m, rowBudget);
        out.mission = InspectorPart{left, top, bodyW, m.row};
        top += m.row;
    }
    const bool anyLive = model::anyLiveAgentLane(lanes);
    if (agentsTab && anyLive && roomForChrome) {
        bottom -= m.row;
        out.stopAllText = state.stopAllArmed ? kInspectorStopAllQuestion : kInspectorStopAllLabel;
        const float allW = textUnits(m, out.stopAllText) + 2.0f * m.pad1;
        out.stopAll = InspectorPart{left, bottom, allW, m.row};
        if (state.stopAllArmed) {
            out.stopNoText = kInspectorStopCancelLabel;
            const float noW = textUnits(m, out.stopNoText) + 2.0f * m.pad1;
            const float noX = out.stopAll.right() + m.pad1;
            // The answer keeps its room even when the question has to be cut: a question
            // whose `No` fell off the row would be a trap, not a confirmation.
            if (noX + noW <= right) {
                out.stopNo = InspectorPart{noX, bottom, noW, m.row};
            } else {
                out.stopAll.w = std::max(0.0f, right - left);
            }
        }
    }
    // ---- the Activity tab (design §4.8, task 31) --------------------------
    //
    // Two chrome rows above the list, in the order the tab reads: the severity filter, then
    // the search box. Both are given up before the list is - one row of log is still a log,
    // while a search box over nothing is a control that cannot do anything - and each chip is
    // placed whole or not at all. The docked column is 28 cells and its body 26, where the four
    // chips measure 24 (`all info warn error`); a narrower column loses its last chip rather
    // than drawing half of one.
    //
    // The search text is read here, once, and the same string decides the row count below and
    // the label the field draws: one query, one answer.
    std::string activityQuery;
    if (state.tab == InspectorTab::Activity) {
        activityQuery = std::string(state.activitySearch.data());
        const float bodyH = bottom - top;
        const bool wantFilter = bodyH >= 2.0f * m.row;
        const bool wantSearch = bodyH >= 3.0f * m.row;
        if (wantFilter) {
            out.filterAllText = kActivityFilterAll;
            out.filterInfoText = kActivityFilterInfo;
            out.filterWarnText = kActivityFilterWarn;
            out.filterErrorText = kActivityFilterError;
            float chipX = left;
            const auto placeChip = [&](const std::string& text, InspectorPart& part) {
                const float w = ui::measureChip(m, text).w;
                if (chipX + w > right)
                    return;
                part = InspectorPart{chipX, top, w, m.row};
                // No gap between the chips: each one's fill already carries a cell of padding
                // inside its own edge, and the docked body is 26 cells wide - `all info warn
                // error` measure 24, so a gap each would silently drop the last filter.
                chipX += w;
            };
            placeChip(out.filterAllText, out.filterAll);
            placeChip(out.filterInfoText, out.filterInfo);
            placeChip(out.filterWarnText, out.filterWarn);
            placeChip(out.filterErrorText, out.filterError);
            top += m.row;
        }
        if (wantSearch) {
            out.search = InspectorPart{left, top, bodyW, m.row};
            top += m.row;
        } else {
            // No field, no filter: a query left over from a taller panel would keep hiding
            // rows with no control to see or clear it, so the body would read `No records
            // match` and the user could not find out why (task-31 review M3).
            activityQuery.clear();
        }
    }
    out.body = InspectorPart{left, top, bodyW, std::max(0.0f, bottom - top)};

    // The rows the body scrolls. A tab with nothing to show has one row - its empty sentence -
    // so the bounded rendering and this draw path are the same code in both cases.
    //
    // The rows are measured in the *body* step because that is the step they are drawn in
    // (`drawPanelBody()`'s `bodyType` scope): the pixel profile sets a label at 13 px and a body
    // at 15 px, so a row measured under the strip's scope would be accepted 15 % too wide and
    // then clipped mid-glyph when painted - a cell grid cannot see it, because there both steps
    // are one row tall.
    out.bodyRows = 1;
    if (state.tab == InspectorTab::Agents && !lanes.empty()) {
        const ui::TypeScope bodyType(ui::TypeStep::body, m.textGrid);
        out.bodyRows = lanes.size();
        out.laneRows.reserve(lanes.size());
        for (const model::AgentLane& lane : lanes) {
            InspectorParts::LaneRow row;
            row.id = lane.id;
            row.focused = state.focusedLane == lane.id;
            row.live = lane.active;
            row.text = laneRowText(lane, row.focused, m, rowBudget);
            out.laneRows.push_back(std::move(row));
        }
    }
    if (state.tab == InspectorTab::Activity) {
        // The filter and the search are one predicate (`model::activityMatches`), so the row
        // count, the window bound and the drawn rows cannot disagree. Only *indices* are
        // collected: a row's text is the renderer's, for the rows the viewport shows.
        out.activityRows.reserve(activity.size());
        for (std::size_t i = 0; i < activity.size(); ++i) {
            if (model::activityMatches(activity[i], state.activityFilter, activityQuery))
                out.activityRows.push_back(i);
        }
        // Records exist but the filter or the search hides every one of them: the body says
        // so instead of claiming the run has nothing to report.
        out.activityFilteredEmpty = out.activityRows.empty() && !activity.empty();
        if (out.activityFilteredEmpty)
            out.emptyText = kActivityNoMatches;
        out.bodyRows = out.activityRows.empty() ? 1 : out.activityRows.size();
    }
    return out;
}

void renderInspector(InspectorState& state, const std::vector<model::AgentLane>& lanes,
                     const ui::Rect& region, const ui::Metrics& m, bool acceptKeys,
                     const std::deque<model::LogRecord>& activity) {
    // Fail closed: a closed panel, a zero rectangle, an unmetered profile or a rectangle in
    // the other profile's unit paints nothing rather than a plausible panel in the wrong
    // coordinate space.
    if (!state.open || region.w <= 0.0f || region.h <= 0.0f || m.gu <= 0.0f || m.row <= 0.0f ||
        !ui::rectMatchesProfile(region, m))
        return;

    state.drawnRows = 0;

    // The floating shape gets a window of its own.
    //
    // Not a detail of ImGui: a child window's draw list is rendered *after* its parent's, so
    // anything the composer's multiline field (itself a child window) draws would survive over
    // a panel painted into the root window's list, whatever the call order. A window created
    // after it is the only surface that can cover it - the same reason the shell's old region
    // stubs drew their panels in a child. The docked column overlaps nothing and stays in the
    // root window, where its text is cell-exact with the thread beside it.
    if (state.overlay) {
        ImGui::SetCursorScreenPos(ImVec2(region.x, region.y));
        ImGui::BeginChild("##inspector_panel", ImVec2(region.w, region.h), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawPanelBody(state, lanes, region, m, acceptKeys, activity);
        ImGui::EndChild();
        return;
    }
    drawPanelBody(state, lanes, region, m, acceptKeys, activity);
}

} // namespace llmfun::tui::views
