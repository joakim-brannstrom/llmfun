/// The Raw Log surface (views/rawlog.h owns the contract and the anatomy).
///
/// Structure of this file, in the order the frame meets it:
///
///   1. the two text helpers every view carries (a measure and a clipped line) and the ink
///      table for the five severities;
///   2. the filter index: the cache that keeps 5000 records off the render path;
///   3. the anatomy: `rawLogParts()` answers every rectangle and every fixed string once, and
///      `renderRawLog()` draws from those numbers;
///   4. the keys the surface owns (only while it is modal) and the controls.
///
/// Layering note (`tools/gate_ratchet.txt`): no colour literal, no display size, no v4 type and
/// no per-widget profile branch beyond the metrics it was handed, so this file needs no ledger
/// row.
#include "views/rawlog.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "ui/icons.h"
#include "ui/input.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/text.h"
#include "ui/tokens.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace llmfun::tui::views {

namespace {

using ui::ColorToken;

/// The follower's cadence: one pin per 100 ms at most, i.e. 10 fps (design §4.9: "a developer
/// stream must not force the UI to 30 fps indefinitely"). The pin is additionally content-
/// driven (it runs only while something sits below the fold), so a quiet stream costs nothing.
constexpr double kFollowIntervalSeconds = 0.1;
/// Rows drawn beyond the viewport, so a partially visible row at the bottom edge still paints.
constexpr float kOverscanRows = 1.0f;

// ---------------------------------------------------------------------------
// Text helpers (the two every view carries)
// ---------------------------------------------------------------------------

/// `text`'s width in the profile's own unit: cells on the grid (a wide glyph counts two) and
/// pixels in the pixel profile (the atlas). The pixel half needs `ImGui::CalcTextSize`, which
/// `ui/text.h` deliberately does not reach for.
float textUnits(const ui::Metrics& m, std::string_view text) {
    if (text.empty())
        return 0.0f;
    if (m.textGrid)
        return static_cast<float>(ui::displayWidth(text));
    if (ImGui::GetCurrentContext() == nullptr || ImGui::GetFont() == nullptr)
        return 0.0f;
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

/// The record row as the surface can show it: the full model row when it fits, and the same row
/// without its trailing action segment when only that segment would be cut. The action remains in
/// the filter, the copy payload and the saved file; a drawn fragment ("actio") advertises nothing.
std::string rowTextForWidth(const model::RawLogRecord& record, float width, const ui::Metrics& m) {
    std::string text = model::rawLogRowText(record);
    if (width <= 0.0f || record.action == 0 || textUnits(m, text) <= width)
        return text;
    const std::size_t at = text.rfind(" action ");
    if (at != std::string::npos)
        text.erase(at);
    return text;
}

/// One line of text left-aligned at `x`, vertically centred in `box` and clipped to it.
void lineAt(const ui::Metrics& m, const ui::Rect& box, float x, float y, std::string_view text,
            ColorToken ink) {
    if (box.w <= 0.0f || box.h <= 0.0f || text.empty() || x >= box.right())
        return;
    const float lineH = ImGui::GetCurrentContext() != nullptr ? ImGui::GetTextLineHeight() : m.row;
    const ImVec2 pen(x, y + std::floor((std::min(box.h, m.row) - lineH) * 0.5f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ui::color(ink));
    dl->PushClipRect(ImVec2(box.x, box.y), ImVec2(box.right(), box.bottom()), true);
    dl->AddText(pen, col, text.data(), text.data() + text.size());
    dl->PopClipRect();
}

/// A fill is a *background* in a cell renderer, so the glyphs of the frame before would show
/// through it. One run of spaces per row is what an opaque fill means on the text grid.
void blankCells(const ui::Metrics& m, const ui::Rect& r, ColorToken fill) {
    if (!m.textGrid || m.row <= 0.0f || r.w <= 0.0f)
        return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::string blank(static_cast<std::size_t>(std::max(0.0f, r.w)), ' ');
    const ImU32 col = ImGui::GetColorU32(ui::color(fill));
    for (float row = r.y; row < r.bottom(); row += m.row)
        dl->AddText(ImVec2(r.x, row), col, blank.c_str());
}

/// The ink a severity is drawn in. Trace and debug are developer detail and stay in the
/// secondary ink: they are not warnings, and painting them like ones would make a trace flood
/// look like a wall of errors (D-BLOCK: the word carries the meaning, the colour only ranks it).
ColorToken levelInk(int level) {
    switch (level) {
    case model::kRawLogLevelWarn:
        return ColorToken::stateWarn;
    case model::kRawLogLevelError:
        return ColorToken::stateDanger;
    case model::kRawLogLevelTrace:
    case model::kRawLogLevelDebug:
        return ColorToken::textMid;
    default:
        return ColorToken::textHi;
    }
}

/// The dropped counter's row text: `N records dropped` (NFR-17).
std::string droppedText(std::size_t dropped) {
    return std::to_string(dropped) + kRawLogDroppedSuffix;
}

/// The count the title row shows: `matching/ring` while the filter narrows the list, and the
/// ring's size alone when it does not (a `12/12` beside an untouched filter is noise).
std::string countText(const RawLogState& state, const model::RawLogRing& ring) {
    if (state.matchingRecords == ring.size())
        return std::to_string(ring.size());
    return std::to_string(state.matchingRecords) + "/" + std::to_string(ring.size());
}

// ---------------------------------------------------------------------------
// The filter index
// ---------------------------------------------------------------------------

/// Find the expanded record in the current `matching` list and cache its detail lines.
///
/// Separate from `rebuildIndex()` because the two questions change at different times: a click
/// changes only the expansion (the index is untouched), while an arrival or a keystroke changes
/// only the index. Folding them together would re-filter 5000 records - with a `localtime_r` and
/// two lower-case copies per record - every time a row is clicked.
void refreshExpansion(RawLogState& state, const model::RawLogRing& ring) {
    state.expandedAt = std::string::npos;
    state.expandedLines.clear();
    state.builtExpanded = state.expandedKey;
    if (state.expandedKey.empty())
        return;
    for (std::size_t i = 0; i < state.matching.size(); ++i) {
        const model::RawLogRecord* record = ring.at(state.matching[i]);
        if (record == nullptr || rawLogKey(*record) != state.expandedKey)
            continue;
        state.expandedAt = i;
        state.expandedLines = model::rawLogDetailLines(*record);
        return;
    }
}

/// Rebuild `state.matching` when the filter, the ring or the floor changed, and answer whether it
/// was rebuilt.
///
/// The ring is 5000 records and §6.4 forbids a per-record string on the render path, so the
/// filtered index list is a cache with the palette's trigger: one string compare and three
/// integers per frame, and one pass only when the question changed. Three facts form the key:
/// the filter, the ring's *arrival counter* (a deque at capacity keeps its size while evicting, and
/// a front timestamp is ambiguous when a burst stamps several records with the same millisecond -
/// the accept counter changes on every arrival, so a rotated ring can never look unchanged), and
/// the floor. The expansion is *not* part of it: `refreshExpansion()` owns that.
bool rebuildIndex(RawLogState& state, const model::RawLogRing& ring) {
    const bool changed = std::strcmp(state.filter.data(), state.builtFilter.c_str()) != 0 ||
                         ring.size() != state.builtSize || ring.arrivals != state.builtArrivals ||
                         state.minLevel != state.builtMinLevel;
    state.matchingRecords = state.matching.size();
    if (!changed)
        return false;

    state.matching.clear();
    if (state.matching.capacity() == 0)
        state.matching.reserve(std::min<std::size_t>(ring.size(), 512));
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const model::RawLogRecord* record = ring.at(i);
        if (record == nullptr || !model::rawLogShowsLevel(record->level, state.minLevel) ||
            !model::rawLogMatches(*record, state.filter.data()))
            continue;
        state.matching.push_back(i);
    }
    state.builtFilter = state.filter.data();
    state.builtSize = ring.size();
    state.builtArrivals = ring.arrivals;
    state.builtMinLevel = state.minLevel;
    state.matchingRecords = state.matching.size();
    return true;
}

/// The record row at body line `line`, or nullptr for a line that belongs to the expanded
/// record's detail block (or to nothing at all).
const model::RawLogRecord* recordAtLine(const RawLogState& state, const model::RawLogRing& ring,
                                        std::size_t line, std::size_t& recordIndex) {
    const std::size_t detailCount = state.expandedLines.size();
    const bool hasDetail = detailCount > 0 && state.expandedAt != std::string::npos;
    std::size_t index = line;
    if (hasDetail && line > state.expandedAt) {
        if (line <= state.expandedAt + detailCount)
            return nullptr;
        index = line - detailCount;
    }
    if (index >= state.matching.size())
        return nullptr;
    recordIndex = state.matching[index];
    return ring.at(recordIndex);
}

// ---------------------------------------------------------------------------
// The anatomy
// ---------------------------------------------------------------------------

/// Every rectangle and every fixed string the surface draws, answered once.
///
/// The rows are a fixed ladder from the top (title, filter) and one from the bottom (controls),
/// and the body takes what is left - so a 4-row drawer and a 30-row overlay share one anatomy
/// and the body is the only part that grows.
struct RawLogParts {
    ui::Rect title;
    ui::Rect chips;
    ui::Rect filter;
    ui::Rect body;
    ui::Rect status;
    ui::Rect footer;
    ui::Rect copyButton;
    ui::Rect saveButton;
    ui::Rect closeButton;
};

RawLogParts rawLogParts(const ui::Rect& region, const ui::Metrics& m, bool withStatus) {
    RawLogParts parts;
    const float rowH = m.row;
    const float padX = m.pad2;
    const float padY = m.textGrid ? 0.0f : m.pad1;
    const float x = region.x + padX;
    const float w = region.w - 2.0f * padX;
    const float top = region.y + padY;
    const float bottom = region.bottom() - padY;
    if (w <= 0.0f || rowH <= 0.0f || bottom - top < rowH)
        return parts;

    // The title row, the filter row, then the controls at the bottom. A surface shorter than
    // four rows folds the filter away before it folds the body: a body with one row still shows
    // the stream, an empty filter field shows nothing.
    const int availRows = static_cast<int>((bottom - top) / rowH);
    const bool withFilter = availRows >= 4;
    parts.title = ui::Rect{x, top, w, rowH, m.textGrid};
    float y = top + rowH;
    if (withFilter) {
        parts.filter = ui::Rect{x, y, w, rowH, m.textGrid};
        y += rowH;
    }
    parts.footer = ui::Rect{x, bottom - rowH, w, rowH, m.textGrid};
    // The status row (a save answer, the dropped counter) sits above the controls and is reserved
    // only when `withStatus` says it has something to say - a blank row above the controls is a
    // row the body lost for nothing (review M3).
    float bodyBottom = parts.footer.y;
    if (withStatus && availRows >= 3) {
        parts.status = ui::Rect{x, bodyBottom - rowH, w, rowH, m.textGrid};
        bodyBottom = parts.status.y;
    }
    parts.body = ui::Rect{x, y, w, std::max(0.0f, bodyBottom - y), m.textGrid};

    // The title row's three groups: the name, the mode chips beside it, the count at the right
    // (measured by the caller once the string is known).
    const float nameW = textUnits(m, kRawLogTitle);
    parts.chips = ui::Rect{x + nameW + 2.0f * m.gu, top, std::max(0.0f, w - nameW - 2.0f * m.gu),
                           rowH, m.textGrid};

    // The controls: three buttons from the left of the footer row. The hints take the rest and
    // are dropped whole when they cannot be drawn whole (a hint cut mid-word advertises
    // nothing). The pixel profile has no keyboard path to these keys, so it draws no hints.
    float buttonX = x;
    const auto placeButton = [&](const char* label, ui::Rect& out) {
        const ui::ButtonSpec spec{label};
        const ui::Size size = ui::measureButton(m, spec);
        out = ui::Rect{buttonX, parts.footer.y, size.w, std::min(size.h, rowH), m.textGrid};
        buttonX += size.w + m.gu;
    };
    placeButton(kRawLogCopyLabel, parts.copyButton);
    placeButton(kRawLogSaveLabel, parts.saveButton);
    placeButton(kRawLogCloseLabel, parts.closeButton);
    return parts;
}

/// The right-aligned count's rectangle: measured after the string is known, so the row can
/// drop it when it does not fit beside the chips.
ui::Rect countsRect(const RawLogParts& parts, const ui::Metrics& m, const std::string& text) {
    const float w = textUnits(m, text);
    if (w <= 0.0f || w + m.gu >= parts.title.w)
        return ui::Rect{0.0f, 0.0f, 0.0f, 0.0f, m.textGrid};
    return ui::Rect{parts.title.right() - w, parts.title.y, w, parts.title.h, m.textGrid};
}

// ---------------------------------------------------------------------------
// The body's interaction
// ---------------------------------------------------------------------------

/// The scrollbar's strip, in screen coordinates: the child's last `ScrollbarSize` columns.
bool overScrollbar() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 pos = ImGui::GetWindowPos();
    const float right = pos.x + ImGui::GetWindowWidth();
    return ImGui::GetIO().MousePos.x >= right - style.ScrollbarSize;
}

/// One body row as a hit box (the rail's `rowItem`): clicked = toggle the detail, hovered =
/// the tooltip gate for a cut message. The item claims no flow space.
struct RowHit {
    bool clicked = false;
    bool hovered = false;
};

RowHit submitRow(const ui::Rect& row, const char* id) {
    RowHit hit;
    if (row.w <= 0.0f || row.h <= 0.0f || ImGui::GetCurrentWindow() == nullptr)
        return hit;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 extent = window->DC.CursorMaxPos;
    ImGui::SetCursorScreenPos(ImVec2(row.x, row.y));
    ImGui::InvisibleButton(id, ImVec2(row.w, row.h));
    hit.clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    hit.hovered = ImGui::IsItemHovered();
    window->DC.CursorMaxPos = extent;
    return hit;
}

/// The keys the surface owns. Only while it is modal: the pixel profile's drawer leaves the
/// keyboard to the composer, and a plain `f` typed there belongs to the draft.
///
/// `ui::takeCharacter()` refuses while a text field owns the keyboard, which is the other half
/// of the rule: a `p` typed into the filter field stays typing.
void takeSurfaceKeys(RawLogState& state) {
    if (!state.overlayShape)
        return;
    if (ui::takeCharacter('f'))
        rawLogFollowNewest(state);
    else if (ui::takeCharacter('p'))
        state.pauseRequested = true;
    else if (ui::takeCharacter('/'))
        state.filterFocus = true;
    else if (ui::takeCharacter('y'))
        state.copiedNow = true;
}

} // namespace

// ---------------------------------------------------------------------------
// The contract's small answers
// ---------------------------------------------------------------------------

bool isRawLogCommand(std::string_view query) {
    const std::string_view command = kRawLogCommand;
    while (!query.empty() && (query.back() == ' ' || query.back() == '\t'))
        query.remove_suffix(1);
    return query == command;
}

std::string rawLogKey(const model::RawLogRecord& record) {
    // The stamp and the three coordinates. Two records that agree on all four read the same on
    // screen, so one identity for both is honest - and a collision only means "the other one
    // expands too", never a lost record.
    return std::to_string(record.timestampMs) + "|" + record.module + "|" + record.function + "|" +
           record.message;
}

void rawLogFollowNewest(RawLogState& state) {
    state.followRequested = true;
    // The catch-up is owed *now*: the clock gate opens on the next frame, and the pin itself
    // still waits for the content to sit below the fold (D-FOLLOW: content-driven, never a
    // per-frame pin).
    state.nextPinAt = 0.0;
}

std::string rawLogVisibleText(const RawLogState& state, const model::RawLogRing& ring,
                              std::size_t first, std::size_t windowRows) {
    std::string out;
    const std::size_t total = state.matching.size() + state.expandedLines.size();
    const std::size_t end = std::min(total, first + windowRows);
    for (std::size_t line = first; line < end; ++line) {
        std::size_t recordIndex = 0;
        const model::RawLogRecord* record = recordAtLine(state, ring, line, recordIndex);
        if (record != nullptr)
            out += model::rawLogRowText(*record);
        else if (line > state.expandedAt && line - state.expandedAt <= state.expandedLines.size())
            out += state.expandedLines[line - state.expandedAt - 1];
        else
            continue;
        out += '\n';
    }
    return out;
}

// ---------------------------------------------------------------------------
// The surface
// ---------------------------------------------------------------------------

void renderRawLog(RawLogState& state, const ui::Rect& region, const ui::Metrics& m,
                  const model::RawLogRing& ring, bool ownsKeyboard) {
    if (!state.open || region.w <= 0.0f || region.h <= 0.0f)
        return;
    if (!ui::rectMatchesProfile(region, m))
        return; // fail closed: a rectangle in the wrong unit is worse than no surface

    // The index first: the parts need the counts, and the counts are the cache's answer.
    const bool indexRebuilt = rebuildIndex(state, ring);
    // The expansion is a second, cheaper question (a click, not an arrival) - but a rebuild moves
    // the indices it is expressed in, so both triggers refresh it. The index just built is the one
    // it walks, so the two can never disagree about which record is open.
    if (indexRebuilt || state.builtExpanded != state.expandedKey)
        refreshExpansion(state, ring);
    state.dropped = ring.dropped;
    state.drawnRows = 0;
    state.filterFocused = false;

    // The surface. The floating shape paints an opaque panel with its own edge (it covers the
    // transcript); the drawer is part of the tiling but still needs a surface of its own, or the
    // thread's glyphs would show through in a cell renderer.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ui::panel(dl, m, region, ColorToken::surface, /*hairline=*/true);
    blankCells(m, region, ColorToken::surface);

    // The drawer's grip (design §4.9: the drawer is resizable). One row at the top of the drawer,
    // outside the content rect, so it cannot be mistaken for the title row it sits above: a drag
    // answers the height it wants and the shell writes `UiState::rawLogH`, which the *layout*
    // clamps into its band next frame - a view never re-tiles by itself (D-METRICS).
    ui::Rect content = region;
    if (!state.overlayShape && region.h >= 2.0f * m.row) {
        const ui::Rect grip{region.x, region.y, region.w, m.row, m.textGrid};
        ui::Rect line = grip;
        line.h = m.border;
        ui::rule(m, line, ColorToken::lineSoft);
        const std::string label = kRawLogResizeLabel;
        const float labelW = textUnits(m, label);
        if (labelW + 4.0f * m.gu <= grip.w)
            lineAt(m, grip, grip.right() - m.pad2 - labelW, grip.y, label, ColorToken::textDim);
        if (ImGui::GetCurrentWindow() != nullptr) {
            const ImVec2 extent = ImGui::GetCurrentWindow()->DC.CursorMaxPos;
            ImGui::SetCursorScreenPos(ImVec2(grip.x, grip.y));
            ImGui::InvisibleButton("##rawlog_grip", ImVec2(grip.w, grip.h));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsItemActive()) {
                // Dragging up grows the drawer: the pointer moves -y and the height must grow.
                const float wanted = region.h - ImGui::GetIO().MouseDelta.y;
                state.resizeTo = std::max(kRawLogMinResizeUnits * m.row, wanted);
            }
            ImGui::GetCurrentWindow()->DC.CursorMaxPos = extent;
        }
        content.y += m.row;
        content.h -= m.row;
    }

    const bool hasStatus = state.dropped > 0 || !state.savePath.empty() || !state.saveError.empty();
    const RawLogParts parts = rawLogParts(content, m, hasStatus);
    if (parts.title.w <= 0.0f)
        return;

    if (ownsKeyboard)
        takeSurfaceKeys(state);

    // The title row: the name in the primary ink, the mode chips beside it, the counts at the
    // right. The chips are the design's `follow`/`paused` words - a mode the user cannot see is
    // a mode they will not trust.
    lineAt(m, parts.title, parts.title.x, parts.title.y, kRawLogTitle, ColorToken::textHi);
    float chipX = parts.chips.x;
    if (state.follow) {
        const char* word = kRawLogChipFollow;
        const float w = textUnits(m, word);
        if (chipX + w <= parts.title.right()) {
            lineAt(m, parts.title, chipX, parts.title.y, word, ColorToken::accent);
            chipX += w + 2.0f * m.gu;
        }
    }
    if (state.paused && chipX < parts.title.right()) {
        lineAt(m, parts.title, chipX, parts.title.y, kRawLogChipPaused, ColorToken::stateWarn);
    }
    const std::string counts = countText(state, ring);
    const ui::Rect countsBox = countsRect(parts, m, counts);
    if (countsBox.w > 0.0f)
        lineAt(m, countsBox, countsBox.x, countsBox.y, counts, ColorToken::textMid);

    // The filter field. Live (no Enter): the list is an in-memory ring, so every keystroke can
    // answer immediately. The id carries the sequence so a programmatic clear re-reads the
    // buffer instead of ImGui's stale edit state.
    if (parts.filter.w > 0.0f) {
        const float fieldW = std::max(0.0f, parts.filter.w - m.pad2);
        if (fieldW > m.pad1) {
            ImGui::SetCursorScreenPos(ImVec2(parts.filter.x, parts.filter.y));
            // The `/` request stays armed until the field really owns the keyboard: ImGui applies
            // `SetKeyboardFocusHere()` on the item's next Begin, which can be a frame later (and
            // is dropped entirely on a frame where the window itself is being focused). Clearing
            // the latch on the *call* would lose the press in exactly those frames.
            if (state.filterFocus)
                ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(fieldW);
            const std::string id = "##rawlog_filter" + std::to_string(state.filterSeq);
            ImGui::InputTextWithHint(id.c_str(), kRawLogFilterHint, state.filter.data(),
                                     state.filter.size());
            state.filterFocused = ImGui::IsItemActive();
            if (state.filterFocus && state.filterFocused)
                state.filterFocus = false;
        }
    }

    // The body. Its own child, so the stream scrolls under a title and a control row that stay
    // put - and so `Save`/`Close` never scroll out of reach of the rows they act on.
    const float rowH = m.row;
    const std::size_t totalLines = state.matching.size() + state.expandedLines.size();
    if (parts.body.h >= rowH) {
        ImGui::SetCursorScreenPos(ImVec2(parts.body.x, parts.body.y));
        ImGui::BeginChild("##rawlog_body", ImVec2(parts.body.w, parts.body.h),
                          ImGuiChildFlags_None);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float rowW = std::max(0.0f, ImGui::GetContentRegionAvail().x);
        ImGui::Dummy(ImVec2(1.0f, std::max(rowH * static_cast<float>(totalLines), parts.body.h)));

        // D-FOLLOW, the Raw Log's half: follow is pointer-owned and content-driven. A wheel
        // notch or a scrollbar press is the user taking the view, so the flag drops; the pin
        // runs only while something sits below the fold, and at most once per 100 ms.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            if (ImGui::GetIO().MouseWheel != 0.0f)
                state.followCleared = true;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && overScrollbar())
                state.followCleared = true;
        }
        if (state.follow && !state.paused) {
            const double now = ImGui::GetTime();
            const float maxY = ImGui::GetScrollMaxY();
            if (now >= state.nextPinAt && ImGui::GetScrollY() < maxY - 0.5f) {
                ImGui::SetScrollHereY(1.0f);
                state.nextPinAt = now + kFollowIntervalSeconds;
                ++state.followPins;
            }
        }

        // The visible window plus one row of overscan (NFR-18): the ring may hold 5000 records
        // and the surface draws the rows it shows, never the list. `first` is clamped to the last
        // window a list this size can show: a child keeps its scroll across frames and ids, so a
        // surface that got shorter (a smaller drawer, a filter that hides everything) would
        // otherwise paint a blank body until ImGui clamped the offset on a later frame.
        //
        // Two row counts, on purpose: `viewportRows` is what a user can *see* and is what `y`
        // copies; `drawRows` adds the one overscan row that exists only so a partially visible row
        // at the bottom edge still paints. Copying the overscan row put a line on the clipboard
        // that was never on screen (review I1).
        const float scrollY = ImGui::GetScrollY();
        const std::size_t rowCount = totalLines == 0 ? 1 : totalLines;
        const std::size_t viewportRows =
            static_cast<std::size_t>(std::max(0.0f, std::ceil(parts.body.h / rowH)));
        const std::size_t drawRows = viewportRows + 1;
        const std::size_t maxFirst = rowCount > viewportRows ? rowCount - viewportRows : 0;
        const std::size_t first = std::min(
            static_cast<std::size_t>(std::max(0.0f, std::floor(scrollY / rowH))), maxFirst);
        state.firstVisible = first;
        state.windowRows = viewportRows;
        {
            const ui::TypeScope bodyType(ui::TypeStep::body, m.textGrid);
            for (std::size_t i = 0; i < drawRows && first + i < rowCount; ++i) {
                const std::size_t line = first + i;
                const ui::Rect row{origin.x, origin.y + rowH * static_cast<float>(line), rowW, rowH,
                                   m.textGrid};
                if (row.y >= parts.body.bottom() || row.bottom() <= parts.body.y)
                    continue;
                ++state.drawnRows;

                if (totalLines == 0) {
                    lineAt(m, row, row.x, row.y, ring.size() == 0 ? kRawLogEmpty : kRawLogNoMatches,
                           ColorToken::textMid);
                    continue;
                }
                std::size_t recordIndex = 0;
                const model::RawLogRecord* record = recordAtLine(state, ring, line, recordIndex);
                if (record == nullptr) {
                    // A continuation line of the expanded record.
                    const std::size_t detailLine = line - state.expandedAt - 1;
                    const char* text = detailLine < state.expandedLines.size()
                                           ? state.expandedLines[detailLine].c_str()
                                           : "";
                    lineAt(m, row, row.x, row.y, text, ColorToken::textMid);
                    continue;
                }
                const std::string text = rowTextForWidth(*record, rowW, m);
                lineAt(m, row, row.x, row.y, text, levelInk(record->level));
                const std::string id = "##rawlog_row" + std::to_string(line);
                const RowHit hit = submitRow(row, id.c_str());
                if (hit.clicked) {
                    const std::string key = rawLogKey(*record);
                    state.expandedKey = state.expandedKey == key ? std::string() : key;
                }
            }
        }
        ImGui::EndChild();
    } else {
        // A folded body has no window to copy: clear the last frame's geometry so Copy cannot
        // hand the clipboard rows the user cannot see (review M10).
        state.firstVisible = 0;
        state.windowRows = 0;
    }

    // The dropped counter: its own row, and only when the ring lost something (NFR-17: a bound
    // that silently drops data is a bug the user cannot report).
    if (parts.status.h > 0.0f) {
        if (state.dropped > 0)
            lineAt(m, parts.status, parts.status.x, parts.status.y, droppedText(state.dropped),
                   ColorToken::stateWarn);
        // The save answer is drawn where the action happened as well as in the now bar: a user
        // who pressed `Save` is looking at the surface, not at the bar.
        const std::string answer =
            !state.saveError.empty()
                ? std::string(kRawLogSaveFailedPrefix) + state.saveError
                : (!state.savePath.empty() ? std::string(kRawLogSavedPrefix) + state.savePath
                                           : std::string());
        if (!answer.empty()) {
            const bool droppedRow = state.dropped > 0;
            const ui::Rect row = droppedRow ? ui::Rect{parts.status.x, parts.status.y,
                                                       parts.status.w, parts.status.h, m.textGrid}
                                            : parts.status;
            const float x =
                droppedRow ? row.x + textUnits(m, droppedText(state.dropped)) + 2.0f * m.gu : row.x;
            lineAt(m, row, x, row.y, answer,
                   state.saveError.empty() ? ColorToken::accent : ColorToken::stateDanger);
        }
    }

    // The controls, and the grid's key hints beside them.
    if (parts.footer.h > 0.0f) {
        const auto drawButton = [&](const ui::Rect& box, const char* label, bool enabled = true) {
            if (box.w <= 0.0f || box.h <= 0.0f)
                return false;
            ui::ButtonSpec spec{label};
            spec.enabled = enabled;
            ui::button(spec, m, box);
            return ui::buttonPressed(spec, m, box);
        };
        const bool canCopy = totalLines > 0 && state.windowRows > 0;
        if (drawButton(parts.copyButton, kRawLogCopyLabel, canCopy))
            state.copiedNow = true;
        if (drawButton(parts.saveButton, kRawLogSaveLabel))
            state.saveRequested = true;
        if (drawButton(parts.closeButton, kRawLogCloseLabel))
            state.closeRequested = true;
        if (state.overlayShape) {
            const float hintsX = parts.closeButton.right() + 2.0f * m.gu;
            const float room = parts.footer.right() - hintsX;
            if (room > 0.0f && textUnits(m, kRawLogHints) <= room)
                lineAt(m, parts.footer, hintsX, parts.footer.y, kRawLogHints, ColorToken::textDim);
        }
    }

    // The two actions that leave the surface: the clipboard is ImGui's, the file is the model's
    // pure writer - both performed here, in the frame that drew the control, so the answer the
    // user sees belongs to the press they made.
    if (state.copiedNow) {
        state.copiedNow = false;
        // The payload replaces the previous one even when it is empty: a stale `copiedRows` would
        // claim a copy the user did not get (review M10), and a `y` on an empty window must clear
        // the clipboard instead of leaving a line the user did not just copy.
        state.copiedRows = rawLogVisibleText(state, ring, state.firstVisible, state.windowRows);
        ImGui::SetClipboardText(state.copiedRows.c_str());
        // A copy that took nothing says nothing: "Copied 0 rows" would be a claim about a press
        // that put an empty string on the clipboard, which is the state it just replaced.
        if (!state.copiedRows.empty()) {
            const std::size_t rows =
                1 + static_cast<std::size_t>(
                        std::count(state.copiedRows.begin(), state.copiedRows.end(), '\n'));
            state.notice = std::string(kRawLogCopiedPrefix) + std::to_string(rows) +
                           (rows == 1 ? " row" : " rows");
            state.noticeKind = model::ToastKind::Success;
        }
    }
    if (state.saveRequested) {
        state.saveRequested = false;
        const std::string filter = state.filter.data();
        std::string path;
        std::string error;
        const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
        if (model::rawLogSave(ring, filter, nowMs, path, error)) {
            state.savePath = path;
            state.saveError.clear();
            state.notice = std::string(kRawLogSavedPrefix) + path;
            state.noticeKind = model::ToastKind::Success;
        } else {
            state.savePath.clear();
            state.saveError = error;
            state.notice = std::string(kRawLogSaveFailedPrefix) + error;
            // A write that failed is an *error*: it persists until the user answers it, in both
            // profiles, because the file they asked for is not there.
            state.noticeKind = model::ToastKind::Error;
        }
    }
}

} // namespace llmfun::tui::views
