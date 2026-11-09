/// The Raw Log surface.
///
/// Structure of this file, in the order the frame meets it:
///
///   1. the two text helpers every view carries (a measure and a clipped line) and the ink
///      table for the five severities;
///   2. the filter index: the cache that keeps 5000 records off the render path;
///   3. the anatomy: `rawLogParts()` answers every rectangle and every fixed string once, and
///      `renderRawLog()` draws from those numbers;
///   4. the keys the surface owns (only while it is modal) and the controls.

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
#include <cstdint>
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

/// `text`'s width in the profile's own unit: cells on the grid (a wide glyph counts two) and
/// pixels in the pixel profile (the atlas).
float textUnits(const ui::Metrics& m, std::string_view text) {
    if (text.empty())
        return 0.0f;
    if (m.textGrid)
        return static_cast<float>(ui::displayWidth(text));
    if (ImGui::GetCurrentContext() == nullptr || ImGui::GetFont() == nullptr)
        return 0.0f;
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

/// The message, mid-ellipsized: whole while it fits, then the head that fits beside the
/// ellipsis marker and then the marker. Cut between codepoints, never inside one. `headLen`, when
/// asked for, is the byte length of the drawn head: the whole message when it fits, otherwise cut.
std::string ellipsizedMessage(const ui::Metrics& m, std::string_view message, float boxW,
                              std::size_t* headLen = nullptr) {
    if (boxW <= 0.0f) {
        if (headLen != nullptr)
            *headLen = 0;
        return std::string();
    }
    if (textUnits(m, message) <= boxW) {
        if (headLen != nullptr)
            *headLen = message.size();
        return std::string(message);
    }
    const std::string_view ellipsis = ui::asciiFallback(ui::Icon::Ellipsis);
    const float ellipsisW = textUnits(m, ellipsis);
    if (ellipsisW > boxW) {
        if (headLen != nullptr)
            *headLen = 0;
        return std::string();
    }
    float used = 0.0f;
    std::size_t keep = 0;
    while (keep < message.size()) {
        // One codepoint: its lead byte plus any continuation bytes behind it.
        std::size_t len = 1;
        while (keep + len < message.size() &&
               (static_cast<unsigned char>(message[keep + len]) & 0xC0) == 0x80)
            ++len;
        const float w = textUnits(m, message.substr(keep, len));
        if (used + w > boxW - ellipsisW)
            break;
        used += w;
        keep += len;
    }
    if (headLen != nullptr)
        *headLen = keep;
    return std::string(message.substr(0, keep)) + std::string(ellipsis);
}

/// The first case-insensitive occurrence of `filter` inside `text`, as a byte
/// offset into the original, or `npos`. ASCII-lower on both sides - the same
/// comparison the filter matcher applies - so the offsets land on the same
/// bytes of the original string.
std::size_t caseInsensitiveFind(std::string_view text, std::string_view filter) {
    if (filter.empty() || filter.size() > text.size())
        return std::string_view::npos;
    return model::rawLogAsciiLower(text).find(model::rawLogAsciiLower(filter));
}

// The record row's segment widths, in cells. `rawRowPlan()` keys its rung table
// on these, so a row's geometry stays a whole number of cells on the text grid
// and the same proportion in pixels.
constexpr int kRawLogGapCells = 2; // between adjacent segments
constexpr int kRawLogMarkerCells = 1;
constexpr int kRawLogClockCells = 12;     // "HH:MM:SS.mmm"
constexpr int kRawLogShortClockCells = 8; // "HH:MM:SS"
constexpr int kRawLogLevelCells = 5;      // rawLogLevelField()'s width
constexpr int kRawLogLocationCells = 36;  // the fixed location column
/// Wide mode's cap on the location column: the measured widest
/// location clamps here instead of owning the surface. The residue a longer
/// location leaves unreachable by scroll is accepted.
constexpr int kRawLogMaxLocationCells = 72;
/// The untruncated location: `rawLogLocationText()` asked for a column wider than
/// any location can fill, so the answer is the full `module.function file:line`
/// join - the wide plan's cap, not the measurement, bounds the column.
constexpr int kRawLogLocationUnlimitedCells = 1 << 20;
constexpr int kRawLogMinMessageCells = 12;
constexpr int kRawLogTagGapCells = 2;
constexpr int kRawLogTagMinCells = 8; // "action N", the shortest possible tag

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
void refreshExpansion(RawLogState& state, const model::RawLogRing& ring, const ui::Metrics& m) {
    state.expandedAt = std::string::npos;
    state.expandedLines.clear();
    state.builtExpanded = state.expandedKey;
    // The expanded record's own contribution to the wide measurement: it resets with
    // the expansion (a closed record draws no detail) and is re-measured from the
    // lines this call resolved. The frame runs this whenever the index rebuild ran,
    // so the extent is never staler than the maxima the walk fills.
    state.maxDetailW = 0.0f;
    if (state.expandedKey.empty())
        return;
    for (std::size_t i = 0; i < state.matching.size(); ++i) {
        const model::RawLogRecord* record = ring.at(state.matching[i]);
        if (record == nullptr || rawLogKey(*record) != state.expandedKey)
            continue;
        state.expandedAt = i;
        state.expandedLines = model::rawLogDetailLines(*record);
        for (const std::string& line : state.expandedLines)
            state.maxDetailW = std::max(state.maxDetailW, textUnits(m, line));
        return;
    }
}

/// Rebuild `state.matching` when the filter, the ring, the floor or the font identity
/// changed, and answer whether it was rebuilt.
///
/// The ring is 5000 records and §6.4 forbids a per-record string on the render path, so the
/// filtered index list is a cache with the palette's trigger: one string compare and three
/// integers per frame, and one pass only when the question changed. Four facts form the key:
/// the filter, the ring's *arrival counter* (a deque at capacity keeps its size while evicting, and
/// a front timestamp is ambiguous when a burst stamps several records with the same millisecond -
/// the accept counter changes on every arrival, so a rotated ring can never look unchanged), the
/// floor, and the profile's `gu`: the wide measurement below is cells or px depending on the
/// profile, so a font-scale or profile change must re-measure rather than reuse stale maxima
/// (the expansion is not part of it; `refreshExpansion()` owns that). The same pass also
/// measures the widest drawn message and location; a hidden record is never measured.
bool rebuildIndex(RawLogState& state, const model::RawLogRing& ring, const ui::Metrics& m) {
    const bool changed = std::strcmp(state.filter.data(), state.builtFilter.c_str()) != 0 ||
                         ring.size() != state.builtSize || ring.arrivals != state.builtArrivals ||
                         state.minLevel != state.builtMinLevel || state.builtGu != m.gu;
    state.matchingRecords = state.matching.size();
    if (!changed)
        return false;

    state.matching.clear();
    // The maxima are this walk's answer, not an incremental max over arrivals: the
    // walk re-measures everything the list it just built draws.
    state.maxMessageW = 0.0f;
    state.maxLocationW = 0.0f;
    if (state.matching.capacity() == 0)
        state.matching.reserve(std::min<std::size_t>(ring.size(), 512));
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const model::RawLogRecord* record = ring.at(i);
        if (record == nullptr || !model::rawLogShowsLevel(record->level, state.minLevel) ||
            !model::rawLogMatches(*record, state.filter.data()))
            continue;
        state.matching.push_back(i);
        state.maxMessageW = std::max(state.maxMessageW, textUnits(m, record->message));
        state.maxLocationW =
            std::max(state.maxLocationW,
                     textUnits(m, rawLogLocationText(*record, kRawLogLocationUnlimitedCells)));
    }
    state.builtFilter = state.filter.data();
    state.builtSize = ring.size();
    state.builtArrivals = ring.arrivals;
    state.builtMinLevel = state.minLevel;
    state.builtGu = m.gu;
    state.matchingRecords = state.matching.size();
    return true;
}

/// The body's line map, rebuilt each frame: one line per record, plus its
/// preview line (the record has detail and is not the expanded record), OR its
/// expanded continuation lines.
struct RawLogLineMap {
    std::vector<std::size_t> bases; // per matching record: the record's first body line
    std::size_t total = 0;          // the body's line count, head row excluded
};

RawLogLineMap rawLogLineMap(const RawLogState& state, const model::RawLogRing& ring) {
    RawLogLineMap map;
    map.bases.reserve(state.matching.size());
    std::size_t line = 0;
    const bool expandedInList = state.expandedAt < state.matching.size();
    for (std::size_t i = 0; i < state.matching.size(); ++i) {
        map.bases.push_back(line);
        if (expandedInList && i == state.expandedAt) {
            // The expanded record: its row plus its continuation lines - the
            // same lines `refreshExpansion` cached this frame.
            line += 1 + state.expandedLines.size();
        } else {
            // Everyone else: one row, plus the preview line the detail earns.
            const model::RawLogRecord* record = ring.at(state.matching[i]);
            line += record != nullptr && !record->detail.empty() ? 2 : 1;
        }
    }
    map.total = line;
    return map;
}

/// The body line at `line` in the map's space.
const model::RawLogRecord* recordAtLine(const RawLogState& state, const model::RawLogRing& ring,
                                        const RawLogLineMap& map, std::size_t line,
                                        std::size_t& recordIndex, bool* isPreview = nullptr) {
    if (isPreview != nullptr)
        *isPreview = false;
    const auto slot = std::upper_bound(map.bases.begin(), map.bases.end(), line);
    if (slot == map.bases.begin())
        return nullptr;
    const std::size_t index = static_cast<std::size_t>(slot - map.bases.begin()) - 1;
    const std::size_t offset = line - map.bases[index];
    const model::RawLogRecord* record = ring.at(state.matching[index]);
    if (offset == 0) {
        recordIndex = state.matching[index];
        return record;
    }
    if (state.expandedAt < state.matching.size() && index == state.expandedAt)
        return nullptr; // a continuation line of the expanded record
    if (isPreview != nullptr && record != nullptr && !record->detail.empty() && offset == 1)
        *isPreview = true;
    recordIndex = state.matching[index];
    return record;
}

/// The head-of-ring placeholder row: when the ring has wrapped, the stamps prove
/// `oldest seq - 1` records were evicted from the front, and the surface says so in one
/// row of its own, like the interior placeholders the accept path inserts. Drawn only
/// over at least one real row: over an all-miss filter the row would read as a result,
/// not a warning. An unstamped oldest survivor (seq 0 or 1) draws no row.
bool hasHeadRow(const RawLogState& state, const model::RawLogRing& ring) {
    if (ring.dropped == 0 || state.matching.empty())
        return false;
    const model::RawLogRecord* front = ring.at(0);
    return front != nullptr && front->seq > 1;
}

/// The head row's text: `N older records were evicted before this point`.
std::string headRowText(const model::RawLogRing& ring) {
    return std::to_string(model::rawLogHeadGap(*ring.records, ring.dropped)) + kRawLogHeadGapSuffix;
}

// The anatomy

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

// The per-frame column plan

RowPlan rawRowPlan(float bodyW, const ui::Metrics& m) {
    RowPlan plan{};
    const int cells = bodyW > 0.0f ? static_cast<int>(std::floor(bodyW / m.gu)) : 0;

    // The ladder: the widest rung whose segments leave the message its minimum
    // (12 cells at rungs 0-3). The rung reads the frame width alone, so a row's
    // geometry never depends on its content.
    int clockCells = 0;
    int locCells = 0;
    int tagCells = 0;
    if (cells >= 84) {
        plan.rung = 0;
        clockCells = kRawLogClockCells;
        locCells = kRawLogLocationCells;
        tagCells = kRawLogTagGapCells + kRawLogTagMinCells;
    } else if (cells >= 74) {
        plan.rung = 1;
        clockCells = kRawLogClockCells;
        locCells = kRawLogLocationCells;
    } else if (cells >= 70) {
        plan.rung = 2;
        clockCells = kRawLogShortClockCells;
        locCells = kRawLogLocationCells;
    } else if (cells >= 32) {
        plan.rung = 3;
        clockCells = kRawLogShortClockCells;
    } else {
        plan.rung = 4;
    }

    // Present segments each take one gap before them; an absent segment's gaps
    // collapse, so the message always follows the last drawn segment by one gap.
    const float gap = static_cast<float>(kRawLogGapCells) * m.gu;
    float x = 0.0f;
    plan.markerX = x;
    plan.markerW = static_cast<float>(kRawLogMarkerCells) * m.gu;
    x += plan.markerW + gap;
    plan.clockX = x;
    plan.clockW = static_cast<float>(clockCells) * m.gu;
    if (clockCells > 0)
        x += plan.clockW + gap;
    plan.levelX = x;
    plan.levelW = static_cast<float>(kRawLogLevelCells) * m.gu;
    x += plan.levelW + gap;
    plan.locX = x;
    plan.locW = static_cast<float>(locCells) * m.gu;
    if (locCells > 0)
        x += plan.locW + gap;
    plan.msgX = x;
    plan.msgW = std::max(0.0f, bodyW - plan.msgX);
    plan.tagW = static_cast<float>(tagCells) * m.gu;
    plan.locationCells = locCells;
    return plan;
}

/// The wide variant: the message draws at its full natural width - no ellipsis - and the
/// location column widens to the measured widest location, capped so one pathological
/// location cannot own the surface. A zero `maxLocationW` or degenerate `maxDrawnW` keeps
/// the narrow plan. A rung that dropped its location keeps it dropped, the tag is dropped
/// whole, and the marker, clock and level rungs keep their narrow geometry.
RowPlan rawRowPlanWide(float rowW, const ui::Metrics& m, float maxLocationW, float maxDrawnW) {
    RowPlan plan = rawRowPlan(rowW, m);
    if (maxDrawnW <= 0.0f)
        return plan;
    const float gap = static_cast<float>(kRawLogGapCells) * m.gu;
    // A zero `maxLocationW` keeps the narrow column: a surface whose records all have
    // empty locations must not slide its message start left when wide mode engages.
    if (plan.locW > 0.0f && maxLocationW > 0.0f) {
        plan.locW = std::min(maxLocationW, static_cast<float>(kRawLogMaxLocationCells) * m.gu);
        plan.msgX = plan.locX + plan.locW + gap;
        plan.locationCells = maxLocationW <= static_cast<float>(kRawLogMaxLocationCells) * m.gu
                                 ? kRawLogLocationUnlimitedCells
                                 : kRawLogMaxLocationCells;
    }
    plan.msgW = maxDrawnW;
    plan.tagW = 0.0f;
    return plan;
}

/// The wide surface's content width: the wide plan's message start, the widest drawn extent
/// (the message and the expanded detail lines share one starting column, so one max covers
/// both) and one trailing gap. Wide mode is engaged exactly while this exceeds the body
/// width - below it the frame draws the narrow plan and no scrollbar exists.
float rawLogContentW(const RowPlan& wide, float maxDrawnW, const ui::Metrics& m) {
    return wide.msgX + maxDrawnW + static_cast<float>(kRawLogGapCells) * m.gu;
}

std::string rawLogLocationText(const model::RawLogRecord& record, int cells) {
    if (cells <= 0)
        return std::string();
    std::string modulePart = record.module;
    std::string filePart;
    if (!record.file.empty()) {
        filePart = record.file;
        if (record.line > 0)
            filePart += ':' + std::to_string(record.line);
    }
    // The join `rawLogRowText()` builds, re-derived from the record's own fields
    // after every trim pass.
    const auto nameOf = [&record, &modulePart] {
        std::string name = modulePart;
        if (!record.function.empty()) {
            if (!name.empty())
                name += '.';
            name += record.function;
        }
        return name;
    };
    const auto joined = [&] {
        std::string where = nameOf();
        if (!filePart.empty()) {
            if (!where.empty())
                where += ' ';
            where += filePart;
        }
        return where;
    };

    std::string text = joined();
    bool firstPass = true;
    while (ui::displayWidth(text) > cells && !modulePart.empty()) {
        // One pass: the module's leftmost dot-component goes, and the first pass
        // also shortens the file to its basename with the line kept.
        const std::size_t dot = modulePart.find('.');
        modulePart = dot == std::string::npos ? std::string() : modulePart.substr(dot + 1);
        if (firstPass) {
            firstPass = false;
            const std::size_t slash = filePart.find_last_of('/');
            if (slash != std::string::npos)
                filePart.erase(0, slash + 1);
        }
        text = joined();
    }
    if (ui::displayWidth(text) <= cells)
        return text;

    // Mid-ellipsis: the elided middle of the name goes, its tail and the whole
    // file part stay. Without a name there is no middle to elide.
    const std::string name = nameOf();
    if (name.empty())
        return std::string();
    const int fileOverhead =
        filePart.empty() ? 0 : 1 + static_cast<int>(ui::displayWidth(filePart));
    const int available = cells - fileOverhead;
    if (available < 3)
        return std::string();
    std::string tail;
    std::size_t at = name.size();
    while (at > 0) {
        std::size_t start = at - 1;
        while (start > 0 && (static_cast<unsigned char>(name[start]) & 0xC0) == 0x80)
            --start;
        const std::string candidate = name.substr(start);
        if (3 + ui::displayWidth(candidate) > available)
            break;
        tail = candidate;
        at = start;
    }
    return "..." + tail + (filePart.empty() ? std::string() : " " + filePart);
}

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
    // The same map the body draws from, built here so the payload and the
    // screen can never disagree about which line is which.
    const RawLogLineMap map = rawLogLineMap(state, ring);
    const bool head = hasHeadRow(state, ring);
    std::string out;
    const std::size_t total = (head ? 1u : 0u) + map.total;
    const std::size_t end = std::min(total, first + windowRows);
    for (std::size_t line = first; line < end; ++line) {
        if (head && line == 0) {
            out += headRowText(ring);
            out += '\n';
            continue;
        }
        const std::size_t bodyLine = head ? line - 1 : line;
        std::size_t recordIndex = 0;
        bool isPreview = false;
        const model::RawLogRecord* record =
            recordAtLine(state, ring, map, bodyLine, recordIndex, &isPreview);
        if (record != nullptr && !isPreview) {
            out += model::rawLogRowText(*record);
        } else if (record != nullptr) {
            // The preview line: it takes a screen row but copies nothing -
            // Copy takes the canonical one-line form, never a detail preview.
            continue;
        } else {
            // A continuation line of the expanded record, if this line is one.
            if (state.expandedAt < state.matching.size()) {
                const std::size_t base = map.bases[state.expandedAt];
                if (bodyLine > base && bodyLine - base <= state.expandedLines.size()) {
                    out += state.expandedLines[bodyLine - base - 1];
                    out += '\n';
                }
            }
            continue;
        }
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
    const bool indexRebuilt = rebuildIndex(state, ring, m);
    // The expansion is a second, cheaper question (a click, not an arrival) - but a rebuild moves
    // the indices it is expressed in, so both triggers refresh it. The index just built is the one
    // it walks, so the two can never disagree about which record is open.
    if (indexRebuilt || state.builtExpanded != state.expandedKey)
        refreshExpansion(state, ring, m);
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
    const bool headRow = hasHeadRow(state, ring);
    const RawLogLineMap lineMap = rawLogLineMap(state, ring);
    const std::size_t totalLines = (headRow ? 1u : 0u) + lineMap.total;
    if (parts.body.h >= rowH) {
        ImGui::SetCursorScreenPos(ImVec2(parts.body.x, parts.body.y));
        ImGui::BeginChild("##rawlog_body", ImVec2(parts.body.w, parts.body.h), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float rowW = std::max(0.0f, ImGui::GetContentRegionAvail().x);
        // The body's live vertical extent: the inner height this frame actually offers,
        // with whatever scrollbars the previous frame reserved already accounted for.
        const float availY = std::max(0.0f, ImGui::GetContentRegionAvail().y);
        // The frame's column plan: every record row draws from these offsets, so the
        // message column cannot move between rows. Loss rows and the placeholder row
        // are full-width lines outside the plan.
        const RowPlan narrow = rawRowPlan(rowW, m);
        // Wide mode: engagement is about the drawn text, not the location column - when the
        // widest message or expanded detail line does not fit the narrow row's message room, the
        // wide variant engages and the filler below grows to the content width, which drives the
        // child's horizontal scrollbar. A stretched location alone never engages it, and the
        // ultra-narrow guard below still reads the ladder the rungs fix, so it keeps the narrow
        // plan.
        const float maxDrawnW = std::max(state.maxMessageW, state.maxDetailW);
        RowPlan plan = narrow;
        float contentW = rowW;
        const bool wideEngaged = maxDrawnW > narrow.msgW;
        if (wideEngaged) {
            const RowPlan wide = rawRowPlanWide(rowW, m, state.maxLocationW, maxDrawnW);
            plan = wide;
            contentW = rawLogContentW(wide, maxDrawnW, m);
        }
        state.wideMode = wideEngaged;
        state.contentW = contentW;
        // The filler's width is the surface's horizontal extent, and the scrollbar's only input.
        // Its height: when the content is going to scroll horizontally the scrollbar will consume
        // the inner height's last row, so the filler reserves that row now - otherwise the filler
        // sized for the taller inner would read as a phantom scroll row, and the follower's pin
        // would park the view past a fold the new geometry does not have.
        const float innerY = contentW > rowW ? std::max(0.0f, availY - rowH) : availY;
        ImGui::Dummy(ImVec2(contentW, std::max(rowH * static_cast<float>(totalLines), innerY)));

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
            // The fold is measured against this frame's own content, not the child's
            // carried-over scroll maximum: right after a resize the carried value still
            // describes the pre-resize geometry, and a pin that trusted it would jump the
            // view rows past a fold the new geometry does not have.
            const float maxY = std::max(0.0f, rowH * static_cast<float>(totalLines) - availY);
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
        // The horizontal scrollbar consumes the body's last row, so in wide mode the viewport
        // the copy spans is the row count of the height that is left - copying the full body
        // height would hand the clipboard a top line scrolled off the view and a bottom line
        // sitting under the scrollbar. Narrow mode keeps the full body.
        const float visibleH = contentW > rowW ? std::max(0.0f, parts.body.h - rowH) : parts.body.h;
        const std::size_t viewportRows =
            static_cast<std::size_t>(std::max(0.0f, std::ceil(visibleH / rowH)));
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
                const ui::Rect row{origin.x, origin.y + rowH * static_cast<float>(line), contentW,
                                   rowH, m.textGrid};
                if (row.y >= parts.body.bottom() || row.bottom() <= parts.body.y)
                    continue;
                // The ultra-narrow rung: below kRawLogMarkerCells + kRawLogGapCells +
                // kRawLogLevelCells
                // + kRawLogGapCells = 10 cells the plan's own segments exceed the body, so no
                // record row can be drawn at all - not even partially, because a markerless
                // fragment would pretend to be a row. The empty state and the filter field stay
                // usable, and an undrawn line must not count as a drawn one for Copy. The
                // narrow plan answers this even in wide mode: wide mode widens the location
                // column, which would blank a narrow body that the ladder itself still fits.
                if (totalLines > 0 && narrow.msgX > rowW)
                    continue;
                ++state.drawnRows;

                if (totalLines == 0) {
                    lineAt(m, row, row.x, row.y, ring.size() == 0 ? kRawLogEmpty : kRawLogNoMatches,
                           ColorToken::textMid);
                    continue;
                }
                // The head-of-ring placeholder: the seqs evicted before the oldest
                // survivor. A row of its own in the dim ink, not a record - clicking it does
                // not expand anything and it copies like any other visible row.
                if (headRow && line == 0) {
                    lineAt(m, row, row.x, row.y, headRowText(ring), ColorToken::textDim);
                    continue;
                }
                const std::size_t bodyLine = headRow ? line - 1 : line;
                std::size_t recordIndex = 0;
                bool isPreview = false;
                const model::RawLogRecord* record =
                    recordAtLine(state, ring, lineMap, bodyLine, recordIndex, &isPreview);
                if (record != nullptr && isPreview) {
                    // The detail preview: the first detail line of a closed record with
                    // detail, in the dim ink at the message column. It takes a screen row
                    // - the scroll model accounts for it - but it is not the record's row:
                    // no marker, no clock, no level. It copies nothing, like every other
                    // preview line, but it does toggle its record on a click.
                    const std::vector<std::string> preview = model::rawLogDetailLines(*record);
                    lineAt(m, row, row.x + plan.msgX, row.y,
                           preview.empty() ? "" : preview.front().c_str(), ColorToken::textLow);
                    // The preview is its own hit target, and its click toggles the
                    // same record the row toggles: the id carries the
                    // record's key so the two rows can never collide.
                    const std::string key = rawLogKey(*record);
                    const std::string previewId = "##rawlog_preview" + key;
                    const RowHit hit = submitRow(row, previewId.c_str());
                    if (hit.clicked)
                        state.expandedKey = state.expandedKey == key ? std::string() : key;
                    continue;
                }
                if (record == nullptr) {
                    // A continuation line of the expanded record: aligned under the
                    // message segment like the preview lines, in the metadata ink.
                    // The copy path is unaffected - it copies the text
                    // verbatim, whatever the screen indent.
                    const std::size_t detailLine = bodyLine - lineMap.bases[state.expandedAt] - 1;
                    const char* text = detailLine < state.expandedLines.size()
                                           ? state.expandedLines[detailLine].c_str()
                                           : "";
                    lineAt(m, row, row.x + plan.msgX, row.y, text, ColorToken::textLow);
                    continue;
                }
                // The record row: each segment drawn straight from the record's own fields,
                // at the plan's offsets. Nothing between the segments is assembled or
                // sliced; a segment that does not fit is dropped whole, and a loss row is
                // not a log line - its message alone, in the dim ink, the same treatment
                // the detail continuations get.
                if (record->loss) {
                    lineAt(m, row, row.x, row.y, record->message, ColorToken::textDim);
                } else {
                    const bool isOpen = state.expandedAt < state.matching.size() &&
                                        recordIndex == state.matching[state.expandedAt];
                    if (!record->detail.empty())
                        lineAt(m, row, row.x + plan.markerX, row.y,
                               isOpen ? kRawLogOpenGlyph : kRawLogOpenableGlyph,
                               ColorToken::textLow);
                    if (plan.clockW > 0.0f) {
                        std::string clock = model::rawLogClockText(record->timestampMs);
                        const std::size_t cells = static_cast<std::size_t>(
                            std::max(0, static_cast<int>(plan.clockW / m.gu)));
                        if (clock.size() > cells)
                            clock.resize(cells);
                        lineAt(m, row, row.x + plan.clockX, row.y, clock, ColorToken::textLow);
                    }
                    lineAt(m, row, row.x + plan.levelX, row.y,
                           model::rawLogLevelField(record->level), levelInk(record->level));
                    if (plan.locW > 0.0f) {
                        const std::string where = rawLogLocationText(*record, plan.locationCells);
                        lineAt(m, row, row.x + plan.locX, row.y, where, ColorToken::textLow);
                    }
                    float messageW = plan.msgW;
                    if (plan.tagW > 0.0f && record->action != 0) {
                        // Only the plan's widest rung reserves tag room, and the wide plan
                        // reserves none at all (its tagW is 0): a tag right-aligns to a row
                        // edge a scrolled surface carries off-screen. There the tag rides
                        // the body's right edge and takes its room from the message: a row
                        // that cannot fit both keeps the message whole and drops the tag
                        // whole, because a cut tag lies about the action.
                        std::string tag = "action " + std::to_string(record->action);
                        if (!record->actionPayload.empty()) {
                            tag.push_back(' ');
                            tag += record->actionPayload;
                        }
                        const float tagW = textUnits(m, tag);
                        if (tagW + kRawLogTagGapCells * m.gu <=
                            plan.msgW - kRawLogMinMessageCells * m.gu) {
                            lineAt(m, row, row.x + row.w - tagW, row.y, tag, ColorToken::accent);
                            messageW = plan.msgW - tagW - kRawLogTagGapCells * m.gu;
                        }
                    }
                    const ui::Rect messageBox{row.x + plan.msgX, row.y, messageW, row.h,
                                              m.textGrid};
                    std::size_t headLen = 0;
                    const std::string shown =
                        ellipsizedMessage(m, record->message, messageW, &headLen);
                    // The highlight: the first case-insensitive filter occurrence inside the drawn
                    // slice. The search runs over the drawn head's bytes - the head is the
                    // message's own prefix, so a match there is the matcher's first match in the
                    // visible part, and a match surviving only in the clipped tail stays plain. A
                    // blanks-only filter is match-all, and the highlight agrees by painting
                    // nothing.
                    std::size_t spanStart = 0;
                    std::size_t spanEnd = 0;
                    const std::string_view filter = state.filter.data();
                    if (!model::rawLogFilterBlank(filter) && headLen > 0) {
                        const std::size_t at =
                            caseInsensitiveFind(record->message.substr(0, headLen), filter);
                        if (at != std::string_view::npos) {
                            spanStart = at;
                            spanEnd = std::min(at + filter.size(), headLen);
                        }
                    }
                    if (spanEnd == spanStart) {
                        lineAt(m, messageBox, messageBox.x, messageBox.y, shown,
                               levelInk(record->level));
                    } else {
                        // Three draws of three slices: the span carries the accent, its neighbors
                        // keep the level ink. Each slice is drawn from the pen position the single
                        // draw would have used, so the glyphs land where they always did, and a
                        // slice that touches an edge is empty and lineAt drops it. The slices never
                        // draw past their own box - text left of a clip rect would smear onto the
                        // clip's first cell in the grid renderer.
                        const float preW = textUnits(m, shown.substr(0, spanStart));
                        const float spanW =
                            textUnits(m, shown.substr(spanStart, spanEnd - spanStart));
                        const ui::Rect preBox{messageBox.x, messageBox.y, preW, messageBox.h,
                                              m.textGrid};
                        const ui::Rect spanBox{messageBox.x + preW, messageBox.y, spanW,
                                               messageBox.h, m.textGrid};
                        const ui::Rect postBox{messageBox.x + preW + spanW, messageBox.y,
                                               messageW - preW - spanW, messageBox.h, m.textGrid};
                        lineAt(m, preBox, messageBox.x, messageBox.y, shown.substr(0, spanStart),
                               levelInk(record->level));
                        lineAt(m, spanBox, messageBox.x + preW, messageBox.y,
                               shown.substr(spanStart, spanEnd - spanStart), ColorToken::accent);
                        lineAt(m, postBox, messageBox.x + preW + spanW, messageBox.y,
                               shown.substr(spanEnd), levelInk(record->level));
                    }
                }
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
        // The wide answers go stale with the geometry: a folded body has no wide layout to
        // describe, so the next frame's measurement starts from the same ground the scrolled
        // window reset above gives firstVisible/windowRows.
        state.wideMode = false;
        state.contentW = 0.0f;
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
        const bool canCopy = state.drawnRows > 0 && state.windowRows > 0;
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
        state.copiedRows = state.drawnRows > 0 ? rawLogVisibleText(state, ring, state.firstVisible,
                                                                   state.windowRows)
                                               : std::string();
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
        const std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
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
