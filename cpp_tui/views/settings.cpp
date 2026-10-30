/// The settings sheet (views/settings.h owns the contract, the vocabulary and the anatomy).
///
/// One window, five fixed rows of chrome and a body of section headers plus settings rows. Every
/// value is drawn from the same words the file stores (`model::sendKeyWord` / `model::uiLogWord`),
/// so what the sheet shows is what this build is running with - never the raw text of a line that
/// failed to parse.
///
/// This file draws, edits the two preference structs and answers; it writes no file and reads no
/// display (D-METRICS). The frame owns both write paths: it saves `imgui2.conf` once when
/// `fontScaleChanged` says a commit happened, and `ui.conf` reaches the disk through the debounce
/// the frame already runs - so a sheet that is closed a second after a change still leaves both
/// files consistent with what the user chose.
#include "views/settings.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "tui_settings.h"

#include "ui/icons.h"
#include "ui/input.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/text.h"
#include "ui/tokens.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace llmfun::tui::views {

namespace {

using ui::ColorToken;

/// The row ids (declared in the header next to the labels, so a test names a row without
/// re-spelling a literal).
constexpr const char* kIdFontScale = kSettingsRowFontScale;
constexpr const char* kIdSendKey = kSettingsRowSendKey;
constexpr const char* kIdRawLog = kSettingsRowRawLog;
constexpr const char* kIdBackend = kSettingsRowBackend;
constexpr const char* kIdUiConf = kSettingsRowUiConf;
constexpr const char* kIdIniConf = kSettingsRowIniConf;
constexpr const char* kIdLogLevel = kSettingsRowLogLevel;
constexpr const char* kIdCopyInfo = kSettingsRowCopyInfo;

/// How long the copy answer stays in the note row. Long enough to read, short enough that a
/// stale "copied" is gone before the next action.
constexpr double kCopyNoticeSeconds = 4.0;

// ---------------------------------------------------------------------------
// Text helpers (the two every view carries)
// ---------------------------------------------------------------------------

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

/// One line of text, vertically centred in `box` and clipped to it.
void lineAt(const ui::Metrics& m, const ui::Rect& box, float x, std::string_view text,
            ColorToken ink) {
    if (box.w <= 0.0f || box.h <= 0.0f || text.empty() || x >= box.right())
        return;
    const float lineH = ImGui::GetCurrentContext() != nullptr ? ImGui::GetTextLineHeight() : m.row;
    const ImVec2 pen(x, box.y + std::floor((std::min(box.h, m.row) - lineH) * 0.5f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ui::color(ink));
    dl->PushClipRect(ImVec2(box.x, box.y), ImVec2(box.right(), box.bottom()), true);
    dl->AddText(pen, col, text.data(), text.data() + text.size());
    dl->PopClipRect();
}

/// `text` cut to `budget` units, with the profile's ellipsis when it does not fit.
///
/// The grid cuts in cells through `ui::truncate()`; the pixel profile keeps the string whole and
/// lets the clip rect cut it (its ellipsis would need a second measure), which is the same split
/// the palette makes between its two profiles.
std::string fitText(const ui::Metrics& m, std::string_view text, float budget) {
    if (budget <= 0.0f)
        return std::string();
    if (m.textGrid)
        return ui::truncate(text, static_cast<int>(budget));
    return std::string(text);
}

// ---------------------------------------------------------------------------
// The row table
// ---------------------------------------------------------------------------

bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view trim(std::string_view text) {
    std::size_t first = 0;
    while (first < text.size() && isBlank(text[first]))
        ++first;
    std::size_t last = text.size();
    while (last > first && isBlank(text[last - 1]))
        --last;
    return text.substr(first, last - first);
}

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool containsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty())
        return true;
    if (needle.size() > haystack.size())
        return false;
    for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
        std::size_t i = 0;
        while (i < needle.size() && lowerAscii(haystack[start + i]) == lowerAscii(needle[i]))
            ++i;
        if (i == needle.size())
            return true;
    }
    return false;
}

/// Does `row` pass the search? The label and the section name only - a note is prose, and
/// matching prose makes a one-letter query hit every row (the palette's argument for its own
/// descriptions).
bool matchesRow(const SettingsRow& row, std::string_view query) {
    if (query.empty())
        return true;
    return containsNoCase(row.label, query) || containsNoCase(row.section, query);
}

/// The percentage strings the font-scale row prints. One owner, so the value and its note cannot
/// disagree about the range.
std::string percentText(int percent) { return std::to_string(percent) + " %"; }

/// The row a section starts with: a header, not a setting - no id, so the cursor skips it and
/// `Enter` cannot act on it.
SettingsRow sectionHeader(const char* section) {
    SettingsRow header;
    header.section = section;
    header.label = section;
    header.kind = SettingsRowKind::ReadOnly;
    return header;
}

/// Every row of the sheet, in display order and unfiltered. The values are the *effective* ones:
/// the catalogue's words for the three preferences, and the frame's derived facts for the rest.
std::vector<SettingsRow> allRows(const SettingsState& state, const model::ShellPrefs& prefs,
                                 const TuiSettings& settings, const ui::Metrics& m) {
    std::vector<SettingsRow> rows;

    // --- Appearance --------------------------------------------------------
    {
        SettingsRow row;
        row.id = kIdFontScale;
        row.section = kSettingsSectionAppearance;
        row.label = "Text scale";
        if (m.textGrid) {
            // A terminal cell is the terminal's size: there is no scale to set, and a row that
            // offered one would be a control that does nothing. The row stays so the section is
            // not empty and the fact is stated where a GUI user would look for the slider.
            row.value = "1 cell";
            row.note = "a terminal cell is the terminal's size (GUI only)";
            row.kind = SettingsRowKind::ReadOnly;
            row.enabled = false;
        } else {
            // The *drawable* percentage: a hand-edited `FontScaleMain=1.02` in `imgui2.conf` loads
            // (the loader accepts 0.5..3.0), but no step can produce 102 % - so the sheet prints
            // what `clampFontScale()` answers, which is also where the first `Enter` starts from.
            row.value =
                percentText(model::fontScalePercent(model::clampFontScale(settings.fontScaleMain)));
            row.note = "GUI text scale, " +
                       percentText(static_cast<int>(model::kFontScaleMin * 100)) + " to " +
                       percentText(static_cast<int>(model::kFontScaleMax * 100)) + ", " +
                       std::to_string(model::kFontScaleStepPercent) + " % steps";
            row.kind = SettingsRowKind::Choice;
            row.enabled = true;
        }
        rows.push_back(std::move(row));
    }

    // --- Behaviour ---------------------------------------------------------
    {
        // The key gate's answer, and *only* that. A hand-edited `SendKey=enter` is a legal line,
        // so the effective value is shown - in the warning ink, with the reason, because a value
        // this build cannot honour is a fact the user has to see (plan/key_gate.md §3).
        SettingsRow row;
        row.id = kIdSendKey;
        row.section = kSettingsSectionBehaviour;
        row.label = "Send key";
        row.value = model::sendKeyWord(prefs.sendKey);
        if (model::sendKeyProven(prefs.sendKey)) {
            row.note = "Enter breaks the line; the Send button sends";
        } else {
            row.note = "not delivered by this build's key gate - Send is the only way";
        }
        // Nothing to cycle to: the gate proves one value, so this row reports rather than edits.
        row.kind = SettingsRowKind::ReadOnly;
        row.enabled = false;
        rows.push_back(std::move(row));
    }
    {
        SettingsRow row;
        row.id = kIdRawLog;
        row.section = kSettingsSectionBehaviour;
        row.label = "Raw log level";
        row.value = model::uiLogWord(prefs.rawLog);
        row.note = "what the Raw Log shows when it is opened (off = only what a normal run logs)";
        row.kind = SettingsRowKind::Choice;
        row.enabled = true;
        rows.push_back(std::move(row));
    }

    // --- Advanced ----------------------------------------------------------
    {
        SettingsRow row;
        row.id = kIdBackend;
        row.section = kSettingsSectionAdvanced;
        row.label = "Backend";
        row.value = m.textGrid ? "text grid (terminal)" : "gui (window)";
        row.note = kSettingsBackendNote;
        rows.push_back(std::move(row));
    }
    {
        SettingsRow row;
        row.id = kIdUiConf;
        row.section = kSettingsSectionAdvanced;
        row.label = "Workbench settings";
        row.value = state.uiConfPath.empty() ? std::string(kSettingsNoProfile) : state.uiConfPath;
        row.note = "ui.conf: send key, raw log level, rail/inspector state, pins";
        rows.push_back(std::move(row));
    }
    {
        SettingsRow row;
        row.id = kIdIniConf;
        row.section = kSettingsSectionAdvanced;
        row.label = "Window settings";
        row.value = state.iniConfPath.empty() ? std::string(kSettingsNoProfile) : state.iniConfPath;
        row.note = "imgui2.conf: window size and text scale";
        rows.push_back(std::move(row));
    }
    {
        SettingsRow row;
        row.id = kIdLogLevel;
        row.section = kSettingsSectionAdvanced;
        row.label = "Log level";
        row.value = state.logArchive ? kSettingsLogLevelOn : kSettingsLogLevelOff;
        row.note = kSettingsLogLevelNote;
        rows.push_back(std::move(row));
    }
    {
        SettingsRow row;
        row.id = kIdCopyInfo;
        row.section = kSettingsSectionAdvanced;
        row.label = kSettingsCopyLabel;
        row.kind = SettingsRowKind::Action;
        row.enabled = true;
        row.note = kSettingsCopyNote;
        rows.push_back(std::move(row));
    }
    return rows;
}

/// The index of the next row the cursor may stand on, from `from` in `direction`, or `from` when
/// there is none (no wrap: a settings list that jumps from the last row to the first hides how
/// long it is). Section headers - the rows with no id - are stepped over.
int stepSelection(const std::vector<SettingsRow>& rows, int from, int direction) {
    const int count = static_cast<int>(rows.size());
    int index = from + direction;
    while (index >= 0 && index < count) {
        if (rows[static_cast<std::size_t>(index)].id[0] != '\0')
            return index;
        index += direction;
    }
    return from;
}

/// The nearest selectable row at or after `from`, else at or before it, else -1.
int snapSelection(const std::vector<SettingsRow>& rows, int from) {
    const int count = static_cast<int>(rows.size());
    int index = std::clamp(from, 0, std::max(0, count - 1));
    while (index < count) {
        if (rows[static_cast<std::size_t>(index)].id[0] != '\0')
            return index;
        ++index;
    }
    index = std::min(from, count - 1);
    while (index >= 0) {
        if (rows[static_cast<std::size_t>(index)].id[0] != '\0')
            return index;
        --index;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

/// Where the sheet's parts sit inside the box the layout engine answered.
struct SheetGeometry {
    ImVec2 pos = ImVec2(0.0f, 0.0f);
    float width = 0.0f;
    float height = 0.0f;
    float frameT = 1.0f; ///< the sheet's own edge (one unit, like the palette's)
    float pad = 0.0f;    ///< inner padding
    float lineH = 1.0f;  ///< one text line
    float rowH = 1.0f;   ///< one list row
    float gap = 0.0f;    ///< vertical spacing between blocks
    float searchH = 1.0f;
    ui::Rect title, search, strip, body, note, hints;
    int bodyRows = 0;
};

/// The sheet *fills* the box it is given: the layout engine already sized it (60 % x 50 % on the
/// grid, at most 520 x 600 px, never below the surface minimum), so a second "shrink to content"
/// step would only make the drawn surface disagree with the geometry every other consumer reads.
///
/// The vertical anatomy is three fixed rows (title, search, section strip), the body, and two
/// footer rows: the selected row's note and the key hints. The footer gives way first - a sheet
/// tall enough for its chrome and nothing else is a surface that cannot do its job:
///
///     * the note and the hints are reserved only while the body keeps `3 * rowH` **beside them**,
///       so a 100x30 sheet gets `title+search+strip (3) + body (8) + note (1) + hints (1)`;
///     * at the 40x15 floor (a 24x7 sheet) neither footer row fits that test, so both drop and the
///       body keeps **two** rows - enough for the section header and the cursor's row, and never a
///       zero-height body with nothing to act on.
///
/// Nothing is drawn for a zero-height part, so the parts' rectangles are what both the draw and a
/// harness test read.
SheetGeometry sheetGeometry(const ui::Rect& box, const ui::Metrics& m) {
    SheetGeometry g;
    g.frameT = std::max(1.0f, m.border);
    g.pad = m.textGrid ? 0.0f : ImGui::GetStyle().WindowPadding.x;
    g.lineH = m.textGrid ? 1.0f : std::max(1.0f, ImGui::GetTextLineHeight());
    g.gap = m.textGrid ? 0.0f : std::max(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f);
    g.rowH = m.textGrid ? 1.0f : std::max(m.row, ImGui::GetFrameHeight());
    g.searchH = m.textGrid ? 1.0f : std::max(g.lineH, ImGui::GetFrameHeight());
    g.width = box.w;
    g.height = box.h;
    g.pos = ImVec2(box.x, box.y);

    const float x0 = box.x + g.frameT + g.pad;
    const float innerW = std::max(0.0f, g.width - 2.0f * (g.frameT + g.pad));
    const float top = box.y + g.frameT + g.pad;
    const float bottom = box.y + g.height - g.frameT - g.pad;

    g.title = ui::Rect{x0, top, innerW, g.lineH, m.textGrid};
    g.search = ui::Rect{x0, g.title.bottom() + g.gap, innerW, g.searchH, m.textGrid};
    g.strip = ui::Rect{x0, g.search.bottom() + g.gap, innerW, g.lineH, m.textGrid};
    const float bodyTop = g.strip.bottom() + g.gap;
    float room = std::max(0.0f, bottom - bodyTop);

    // The footer rows are reserved only while the body keeps three rows without them.
    const float footerRow = g.lineH + g.gap;
    const float minimumBody = 3.0f * g.rowH;
    const bool withHints = room - footerRow >= minimumBody;
    if (withHints)
        room -= footerRow;
    const bool withNote = room - footerRow >= minimumBody;
    if (withNote)
        room -= footerRow;

    g.body = ui::Rect{x0, bodyTop, innerW, room, m.textGrid};
    g.bodyRows = room >= g.rowH ? static_cast<int>(std::floor(room / g.rowH)) : 0;
    float footerY = bodyTop + room + g.gap;
    if (withNote) {
        g.note = ui::Rect{x0, footerY, innerW, g.lineH, m.textGrid};
        footerY += footerRow;
    }
    if (withHints)
        g.hints = ui::Rect{x0, footerY, innerW, g.lineH, m.textGrid};
    return g;
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

/// One row's hit box: submitted once, so a click cannot be read by two items that disagree about
/// where the row is. The item claims no flow space (the caller's extent is restored).
bool rowClicked(const ui::Rect& row, const char* id) {
    if (row.w <= 0.0f || row.h <= 0.0f || ImGui::GetCurrentWindow() == nullptr)
        return false;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 extent = window->DC.CursorMaxPos;
    ImGui::SetCursorScreenPos(ImVec2(row.x, row.y));
    ImGui::InvisibleButton(id, ImVec2(row.w, row.h));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    window->DC.CursorMaxPos = extent;
    return clicked;
}

/// A fill is a *background* in a cell renderer, so the glyphs of the frame before would show
/// through it. One run of spaces per row is what an opaque fill means on the text grid (the
/// Raw Log's rule, and the reason an overlay cannot be a draw-list fill alone).
void blankCells(const ui::Metrics& m, const ui::Rect& r, ColorToken fill) {
    if (!m.textGrid || m.row <= 0.0f || r.w <= 0.0f)
        return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::string blank(static_cast<std::size_t>(std::max(0.0f, r.w)), ' ');
    const ImU32 col = ImGui::GetColorU32(ui::color(fill));
    for (float row = r.y; row < r.bottom(); row += m.row)
        dl->AddText(ImVec2(r.x, row), col, blank.c_str());
}

/// The keys the sheet owns, read *before* the frame is drawn so the drawn frame shows their
/// effect (the palette's rule).
///
/// `Esc` is deliberately absent: closing the sheet is the first step of the D-ESC chain and the
/// frame-level keymap performs it (`views::escStepClosesOverlay()` / `closeTopOverlay()`), so one
/// press can never both close the sheet and act on the region behind it.
///
/// `↑`/`↓` are the pixel profile's binding; the text backend delivers no bare arrow as a key
/// event (plan/key_gate.md §4b), so `Ctrl+P`/`Ctrl+N` (§4c) are the pair both profiles hear.
/// `Tab`/`Shift+Tab` walk the sections - the strip's four entries always exist, so there is
/// nothing to skip (the palette's cycle has to skip its empty groups; this one does not).
///
/// These are the surface's *list* keys, not registry chords: the keymap holds the global bindings
/// (`Ctrl+K`, `Ctrl+Q`, `Ctrl+B`, …) and a modal list's cursor keys are the surface's own - the
/// palette's `handleNavKeys()` and the inspector's `[`/`]` are the same shape (D-KEYMAP's rule is
/// about global chords, which is why `Esc` - a global step - is *not* read here).
///
/// A cursor key or a `Tab` also takes the keyboard off the search field (`fieldReleased`): with the
/// field focused, `Enter` must mean "done typing" and not "change a persisted preference" - and a
/// key that moved the list has already answered the question the field was being typed into (the
/// same reason the field never *runs* a row). The release is a latch, not a one-shot blur, because
/// ImGui's Tab-nav hands the keyboard back to the window's only text field a frame later (review
/// round 2).
///
/// Returns true when `Enter` asked for the selected row to be activated *and* the list owns the
/// keyboard; `renderSettings()` runs it once it has this frame's row list.
bool takeKeys(SettingsState& state, bool ownsKeyboard) {
    if (!ownsKeyboard)
        return false;
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        state.section =
            (state.section + (io.KeyShift ? kSettingsSectionCount - 1 : 1)) % kSettingsSectionCount;
        state.fieldReleased = true;
        state.searchFocus = false;
    }
    const bool previous = ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
                          (ImGui::IsKeyPressed(ImGuiKey_P, false) && io.KeyCtrl);
    const bool next = ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
                      (ImGui::IsKeyPressed(ImGuiKey_N, false) && io.KeyCtrl);
    const int direction = (next ? 1 : 0) - (previous ? 1 : 0);
    if (direction != 0) {
        state.selected = stepSelection(state.rows, state.selected, direction);
        // A cursor key means "into the list": it also cancels a focus request that has not been
        // answered yet, so a field that never took the keyboard cannot come back and swallow the
        // `Enter` that follows.
        state.fieldReleased = true;
        state.searchFocus = false;
    }
    // `/` focuses the field again after a click or a cursor key moved the keyboard off it.
    //
    // While the field is *released* the guard inside `takeCharacter()` describes that very field's
    // stale re-activation (ImGui keeps its process-global "some text field wants the keyboard" flag
    // set for a couple of frames after its nav takes the field back), so the unguarded form is what
    // keeps the advertised way back working on the frame right after a `Tab` - with the guarded one
    // the press was neither typed nor handled (review M7). Any other time the guard is right: a `/`
    // typed into a search stays text.
    const bool slash =
        state.fieldReleased ? ui::takeCharacterAfterRelease('/') : ui::takeCharacter('/');
    if (slash) {
        state.searchFocus = true;
        state.fieldReleased = false;
    }
    // `Enter` edits a row only while the *list* owns the keyboard. `searchFocused` is the previous
    // frame's observation and `searchFocus` the armed request, which is exactly what `takeKeys()`
    // needs: it runs before the field is drawn. With the field focused, `Enter` is the field's
    // ("done typing") - which in ImGui's single-line field does nothing at all, so a press can
    // never become a silent `ui.conf` write through the search box.
    //
    // The gate is the sheet's own state, never ImGui's process-global "a text field somewhere wants
    // the keyboard" flag (see the header): that one is a frame stale, so a composer field that was
    // active when the sheet opened would make the first press inert with nothing on screen to
    // explain it (review round 2, N2).
    const bool fieldOwns = !state.fieldReleased && (state.searchFocused || state.searchFocus);
    if (fieldOwns)
        return false;
    return ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
           ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
}

/// Run the selected row. `Choice` moves to the next legal value, `Action` performs its one action,
/// `ReadOnly` does nothing (the row's `enabled` is false, so this is only reached through Enter on
/// a row the sheet drew as live).
void activate(SettingsState& state, model::ShellPrefs& prefs, TuiSettings& settings,
              const ui::Metrics& m, const SettingsRow& row) {
    if (!row.enabled || row.kind == SettingsRowKind::ReadOnly)
        return;
    if (std::strcmp(row.id, kIdFontScale) == 0) {
        // A scale is a pixel-profile fact; the grid row is read-only and draws that.
        if (m.textGrid)
            return;
        const float next = model::stepFontScale(settings.fontScaleMain, 1);
        if (next != settings.fontScaleMain) {
            settings.fontScaleMain = next;
            // The frame writes `imgui2.conf` once, in the frame that committed the change: the
            // view knows *that* it changed, only the frame knows the file. `metricsFor()` reads
            // the new scale on the next frame (`tui.cpp`), which is what "applies next frame"
            // means for the one value that changes the metrics themselves.
            state.fontScaleChanged = true;
        }
        return;
    }
    if (std::strcmp(row.id, kIdRawLog) == 0) {
        // The four levels, in the order `uiLogWord()` names them, wrapping at the end: a
        // one-press cycle, and the value written is one the writer can spell (`uiConfText`).
        const int current = std::clamp(prefs.rawLog, model::kRawLogOff, model::kRawLogTrace);
        prefs.rawLog = (current + 1) % model::kUiLogChoiceCount;
        return;
    }
    if (std::strcmp(row.id, kIdCopyInfo) == 0) {
        const std::string text = systemInfoText(state, m);
        if (ImGui::GetCurrentContext() != nullptr)
            ImGui::SetClipboardText(text.c_str());
        state.copiedUntil = ImGui::GetTime() + kCopyNoticeSeconds;
        return;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// The contract's small answers
// ---------------------------------------------------------------------------

const char* settingsSectionName(int section) {
    switch (section) {
    case 0:
        return kSettingsSectionAll;
    case 1:
        return kSettingsSectionAppearance;
    case 2:
        return kSettingsSectionBehaviour;
    case 3:
        return kSettingsSectionAdvanced;
    default:
        break;
    }
    return kSettingsSectionAll;
}

std::vector<SettingsRow> settingsRows(const SettingsState& state, const model::ShellPrefs& prefs,
                                      const TuiSettings& settings, const ui::Metrics& m) {
    const std::vector<SettingsRow> all = allRows(state, prefs, settings, m);
    const std::string_view query = trim(std::string_view(state.search.data()));
    const std::string_view wanted = settingsSectionName(state.section);
    const bool allSections = state.section <= 0;

    std::vector<SettingsRow> rows;
    rows.reserve(all.size() + kSettingsSectionCount);
    std::string_view openSection;
    for (const SettingsRow& row : all) {
        if (!allSections && wanted != row.section)
            continue;
        if (!matchesRow(row, query))
            continue;
        if (openSection.empty() || openSection != row.section) {
            // A header per section the filtered list actually shows - so a search that keeps two
            // rows from two sections still says which section each one is in.
            rows.push_back(sectionHeader(row.section));
            openSection = row.section;
        }
        rows.push_back(row);
    }
    return rows;
}

std::string systemInfoText(const SettingsState& state, const ui::Metrics& m) {
    // The six documented fields (task 34, design §4.10 "copy system info"). Nothing else is in
    // the payload: no message text, no session title, no path - a diagnostics string that carried
    // a user's transcript would be a copy that leaks every time someone pastes it into a bug
    // report, and the paths are already on screen in the Advanced rows.
    std::string text;
    text += "llmfun workbench\n";
    text += "backend: " + std::string(m.textGrid ? "text grid (terminal)" : "gui (window)") + "\n";
    text += "display: " + std::to_string(state.displayW) + "x" + std::to_string(state.displayH) +
            (m.textGrid ? " cells" : " px") + "\n";
    text += "profile: " + std::string(m.textGrid ? "text grid" : "pixel") + "\n";
    // The renderer is not a setting any more (task 41): one exists, so the field says so
    // rather than echoing a choice nobody can make.
    text += "generation: workbench\n";
    text += "flags: " + state.flags + "\n";
    text += "versions: " + state.versions + "\n";
    return text;
}

void closeSettings(SettingsState& state) {
    state.open = false;
    state.search.fill(0);
    ++state.searchSeq; // the next open re-reads the empty buffer, not ImGui's stale edit state
    state.searchFocus = false;
    state.searchFocused = false;
    state.fieldReleased = false;
    state.selected = 0;
    state.firstVisible = 0;
    state.rows.clear();
    state.rowsBuilt = 0;
    state.rowsDrawn = 0;
    state.closeRequested = false;
    state.fontScaleChanged = false;
    state.copiedUntil = 0.0;
}

void openSettings(SettingsState& state) {
    state.open = true;
    state.search.fill(0);
    ++state.searchSeq;
    state.searchFocus = true;
    state.fieldReleased = false;
    state.selected = 0;
    state.firstVisible = 0;
    state.closeRequested = false;
    state.fontScaleChanged = false;
    state.copiedUntil = 0.0;
}

// ---------------------------------------------------------------------------
// The sheet
// ---------------------------------------------------------------------------

void renderSettings(SettingsState& state, const ui::Rect& region, const ui::Metrics& m,
                    model::ShellPrefs& prefs, TuiSettings& settings, bool ownsKeyboard) {
    if (!state.open || region.w <= 0.0f || region.h <= 0.0f)
        return;
    if (!ui::rectMatchesProfile(region, m))
        return; // fail closed: a rectangle in the wrong unit is worse than no sheet

    // The rows first: the keys act on them and the frame draws from them.
    //
    // Built per frame, unlike the palette's ranked table: this list is eleven entries of short
    // strings and two of its values (the run's flags, the archive switch) are facts about *this*
    // frame, so a cache would have to compare exactly what it saved (design §4.12's "matching runs
    // over the model, not per draw" is about the palette's fuzzy match over the session store).
    state.rows = settingsRows(state, prefs, settings, m);
    state.rowsBuilt = state.rows.size();
    state.rowsDrawn = 0;
    {
        const int snapped = snapSelection(state.rows, state.selected);
        state.selected = snapped < 0 ? 0 : snapped;
    }

    // `takeKeys()` reads `searchFocused` as the *previous* frame's observation - it runs before the
    // field is drawn, so this frame's answer does not exist yet - and the draw below resets it.
    const bool activatePressed = takeKeys(state, ownsKeyboard);
    state.searchFocused = false;
    if (activatePressed && state.selected >= 0 &&
        state.selected < static_cast<int>(state.rows.size())) {
        activate(state, prefs, settings, m, state.rows[static_cast<std::size_t>(state.selected)]);
    }

    const SheetGeometry g = sheetGeometry(region, m);

    // The overlay is its own window and not a draw-list fill: a fill recolours the cells but keeps
    // the glyphs already drawn in the frame, so the thread would show through the sheet (the
    // palette's and the Raw Log's lesson). It is also what makes the surface opaque in the grid.
    ImGui::SetNextWindowPos(g.pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(g.width, g.height), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_ChildBg));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##settings", nullptr, flags);
    // The sheet is modal while it is the topmost surface: a composer that still held ImGui's
    // active id would keep eating the plain keys (`/`) before the sheet reads them from the
    // character queue. Focusing it on every frame it does not have the focus is what makes that
    // hold on a re-open too (task 33's finding); the search field inside the window keeps the
    // keyboard as usual, because the focus belongs to *this* window.
    if (ownsKeyboard &&
        (ImGui::IsWindowAppearing() || !ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)))
        ImGui::SetWindowFocus();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    // The surface. The window's own background is not enough in a cell renderer: a fill recolours
    // the cells but keeps the glyphs already drawn in this frame, so the thread would show through
    // the sheet (the Raw Log's finding). One panel fill plus one run of spaces per row is what an
    // opaque surface means here; the edge is `panel()`'s hairline drawn *inside* the rect (a stroke
    // would straddle the boundary and paint two cells in the grid).
    ui::panel(ImGui::GetWindowDrawList(), m, region, ColorToken::surface, /*hairline=*/true);
    blankCells(m, region, ColorToken::surface);

    // Title row: the sheet's name on the left; on the right the `Close` control and, when the row
    // can still hold it, the active section with the row count. The name and the control are the
    // two that never drop - a title row without its name or its way out is not a title row - so the
    // summary is what gives way on a 24-cell sheet (the 40x15 golden pins it).
    lineAt(m, g.title, g.title.x, kSettingsTitle, ColorToken::textHi);
    {
        const float titleEnd = g.title.x + textUnits(m, kSettingsTitle) + 2.0f * m.gu;
        float right = g.title.right();
        const ui::ButtonSpec closeSpec{kSettingsCloseLabel};
        const float closeW = ui::measureButton(m, closeSpec).w;
        const ui::Rect close{right - closeW, g.title.y, closeW, std::min(g.title.h, m.row),
                             m.textGrid};
        if (close.w > 0.0f && close.x >= titleEnd) {
            ui::button(closeSpec, m, close);
            if (ui::buttonPressed(closeSpec, m, close))
                state.closeRequested = true;
            right = close.x - m.pad2;
        }
        std::string summary = settingsSectionName(state.section);
        summary += "  ";
        // The rows a user can *act on*: section headers are structure, not settings, so counting
        // them would make `ALL 11` a lie about how many settings there are (and `BEHAVIOUR 3` for
        // two rows reads as a bug). Named for what it counts - the enclosing `settings` is the
        // `TuiSettings` this view edits.
        std::size_t settingCount = 0;
        for (const SettingsRow& row : state.rows) {
            if (row.id[0] != '\0')
                ++settingCount;
        }
        summary += std::to_string(settingCount);
        const float w = textUnits(m, summary);
        if (w + m.gu <= right - titleEnd)
            lineAt(m, g.title, right - w, summary, ColorToken::textLow);
    }

    // The search field. Live: the row list is ten entries and a substring test, so every keystroke
    // can answer immediately (an `Enter` is not needed and would fight the row's `Enter`).
    const float fieldW = std::max(0.0f, g.search.w - m.pad2);
    if (g.search.w > 0.0f && fieldW > m.pad1) {
        ImGui::SetCursorScreenPos(ImVec2(g.search.x, g.search.y));
        // The request stays armed until the field really owns the keyboard: ImGui applies
        // `SetKeyboardFocusHere()` on the item's next Begin, which can be a frame later.
        if (state.searchFocus)
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(fieldW);
        const std::string id = "##settings_search" + std::to_string(state.searchSeq);
        ImGui::InputTextWithHint(id.c_str(), kSettingsSearchHint, state.search.data(),
                                 state.search.size());
        state.searchFocused = ImGui::IsItemActive();
        // A click in the field is a user asking to type there, so it ends a `Tab`/cursor release.
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            state.fieldReleased = false;
        if (state.searchFocus && state.searchFocused) {
            state.searchFocus = false;
            state.fieldReleased = false;
        }
        // A cursor key or a `Tab` moved the list, so the field lets go of the keyboard and stays
        // released: without this the next `Enter` would be swallowed by a field nobody is typing
        // into (and, before `takeKeys()` gated it, would have edited a row). ImGui's Tab-nav
        // re-activates the window's only text field on the frame *after* the Tab, so a one-shot
        // clear would be undone a frame later (review round 2, N1). Only a focus request the user
        // made - `/`, the keyboard hand-back when an overlay above closes - or a click in the field
        // lifts the latch (the two checks above); `openSettings()` starts clear.
        if (state.fieldReleased && state.searchFocused) {
            ImGui::ClearActiveID();
            state.searchFocused = false;
        }
        // ImGui paints the nav ring around a focused widget; on the grid that is a full-cell
        // rectangle that outlives the frame in the terminal. The caret marks the focus, so
        // the ring is hidden in the text grid only (the composer's and palette's rule).
        if (m.textGrid) {
            ImGuiContext& ctx = *ImGui::GetCurrentContext();
            const ImGuiID fieldId = ImGui::GetID(id.c_str());
            if (ctx.ActiveId == fieldId || ctx.NavId == fieldId)
                ctx.NavCursorVisible = false;
        }
    } else {
        // A sheet too narrow for the field cannot focus it: the list takes the keyboard instead of
        // an armed request that would refuse every `Enter` with nothing on screen to explain why
        // (review round 2, N2).
        state.searchFocus = false;
        state.fieldReleased = true;
    }

    // The section strip: one chip per section, the active one inverted on the grid and underlined
    // in the pixel profile (the inspector's tab rule, so a "this one" marker reads the same way in
    // both surfaces).
    {
        float x = g.strip.x;
        for (int section = 0; section < kSettingsSectionCount; ++section) {
            const std::string name = settingsSectionName(section);
            const float w = textUnits(m, name) + 2.0f * m.gu;
            if (x + w > g.strip.right())
                break; // a strip that does not fit is cut, never wrapped over the body
            const ui::Rect chip{x, g.strip.y, w, g.strip.h, m.textGrid};
            const bool active = state.section == section;
            ColorToken ink = active ? ColorToken::textHi : ColorToken::textLow;
            if (active && m.textGrid) {
                ui::rule(m, chip, ColorToken::accent);
                ink = ColorToken::canvas;
            }
            lineAt(m, chip, chip.x + m.gu, name, ink);
            if (!m.textGrid) {
                ui::Rect underline{chip.x, chip.bottom() - m.border, chip.w, m.border, m.textGrid};
                ui::rule(m, underline, active ? ColorToken::accent : ColorToken::lineSoft);
            }
            const std::string id = "##settings_section" + std::to_string(section);
            if (rowClicked(chip, id.c_str()))
                state.section = section;
            x += w + 2.0f * m.gu;
        }
    }

    // The body: the visible window of rows, the selection kept inside it. A click selects a row
    // and a second click on the *selected* row changes it (Enter does the same), so a stray click
    // never edits a persisted preference.
    //
    // A search that hides everything says so: a blank body under a `0` in the title reads as a
    // broken sheet, and "nothing matches" is a different fact from "there are no settings".
    if (state.rows.empty()) {
        lineAt(m, g.body, g.body.x + 2.0f * m.gu, kSettingsNoMatches, ColorToken::textLow);
    } else if (g.bodyRows > 0) {
        // The pointer scrolls the list by moving the cursor one row per notch - the window follows
        // the selection, so there is one scroll position to reason about and the row Enter acts on
        // is always the row that is on screen.
        if (ownsKeyboard && ImGui::GetIO().MouseWheel != 0.0f &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            if (mouse.y >= g.body.y && mouse.y < g.body.bottom()) {
                const int step = ImGui::GetIO().MouseWheel > 0.0f ? -1 : 1;
                state.selected = stepSelection(state.rows, state.selected, step);
            }
        }
        const int count = static_cast<int>(state.rows.size());
        int first = state.firstVisible;
        if (state.selected < first)
            first = state.selected;
        if (state.selected >= first + g.bodyRows)
            first = state.selected - g.bodyRows + 1;
        first = std::clamp(first, 0, std::max(0, count - g.bodyRows));
        state.firstVisible = first;

        const int lastDrawn = std::min(count, first + g.bodyRows + 1);
        for (int index = first; index < lastDrawn; ++index) {
            const SettingsRow& row = state.rows[static_cast<std::size_t>(index)];
            const float rowY = g.body.y + static_cast<float>(index - first) * g.rowH;
            const ui::Rect box{g.body.x, rowY, g.body.w, g.rowH, m.textGrid};
            if (box.y >= g.body.bottom())
                break;
            ++state.rowsDrawn;

            const bool isHeader = row.id[0] == '\0';
            const bool selected = !isHeader && index == state.selected;
            if (selected) {
                // The selection fill is the *first* carrier, and in a cell renderer it is nearly
                // invisible: `selection` (accent at 14 %) quantizes onto the surface at the depths
                // a terminal advertises. So the mark is the rail's, for the same reason: a rule in
                // the row's first unit - one cell of the rule glyph on the grid (a background cell
                // would read as a fill, not as a mark) and a 2 px bar in the pixel profile.
                ui::rule(m, box, ColorToken::selection);
                if (m.textGrid) {
                    ui::iconMark(m, ui::Rect{box.x, box.y, m.gu, box.h, m.textGrid}, ui::Icon::Rule,
                                 ColorToken::accent);
                } else {
                    ui::rule(m, ui::Rect{box.x, box.y, 2.0f, box.h, false}, ColorToken::accent);
                }
            }

            float textX = box.x + 2.0f * m.gu;
            if (isHeader) {
                lineAt(m, box, textX, row.label, ColorToken::textLow);
                continue;
            }

            // The value is right-aligned and the label gets what is left, so a long path never
            // pushes a value off the row (the value is the part a user is looking for). The label
            // is *clipped* to its share, not just cut: in the pixel profile a 60-character home
            // path would otherwise be painted under the right-aligned value (the grid cuts with
            // `ui::truncate()`, the pixel profile with the clip rect). An action row has no value:
            // its label *is* the control, so it carries the interactive ink.
            const float valueW = textUnits(m, row.value);
            const float valueX = box.right() - m.gu - valueW;
            const float labelRight = row.value.empty() ? box.right() - m.gu : valueX - 2.0f * m.gu;
            const ui::Rect labelBox{box.x, box.y, std::max(0.0f, labelRight - box.x), box.h,
                                    m.textGrid};
            ColorToken labelInk = selected ? ColorToken::textHi : ColorToken::textMid;
            if (row.kind == SettingsRowKind::Action && row.enabled)
                labelInk = ColorToken::accent;
            lineAt(m, labelBox, textX, fitText(m, row.label, labelRight - textX), labelInk);
            if (!row.value.empty()) {
                ColorToken ink = ColorToken::textMid;
                if (row.kind == SettingsRowKind::Choice)
                    ink = ColorToken::accent;
                else if (!row.enabled)
                    ink = ColorToken::textLow;
                // The send key is the one row whose value can be a value this build cannot
                // honour (a hand-edited `SendKey=`): it says so in the warning ink rather than
                // pretending the setting works.
                if (std::strcmp(row.id, kIdSendKey) == 0 && !model::sendKeyProven(prefs.sendKey))
                    ink = ColorToken::stateWarn;
                lineAt(m, box, valueX, row.value, ink);
            }

            const std::string id = "##settings_row" + std::to_string(index);
            if (rowClicked(box, id.c_str())) {
                if (state.selected == index)
                    activate(state, prefs, settings, m, row);
                else
                    state.selected = index;
            }
        }

        // The body is a window on a longer list and the sheet draws no ImGui child, so nothing else
        // would say "there is more": a proportional thumb in the right gutter, the rail's shape.
        // Drawn *after* the rows so a selected row's fill cannot cover it, and inside the unit the
        // values already leave free (`valueX` and the label both stop one `gu` short of the edge),
        // so no row loses width to it (review M1).
        if (count > g.bodyRows) {
            const float trackH = static_cast<float>(g.bodyRows) * g.rowH;
            const float thumbH = std::min(
                trackH, std::max(g.rowH, std::floor(trackH * static_cast<float>(g.bodyRows) /
                                                    static_cast<float>(count))));
            const float travel = std::max(0.0f, trackH - thumbH);
            const int lastFirst = std::max(1, count - g.bodyRows);
            const float thumbY = g.body.y + std::floor(travel * static_cast<float>(first) /
                                                       static_cast<float>(lastFirst));
            const ui::Rect thumb{g.body.right() - m.gu, thumbY, m.gu, thumbH, m.textGrid};
            if (m.textGrid) {
                // A cell renderer states a fill at low colour depth poorly (the selection's
                // lesson), so the grid gets the mark the selected row uses - one rule glyph per
                // covered cell - and the pixel profile gets the bar.
                for (float y = thumb.y; y < thumb.bottom(); y += g.rowH)
                    ui::iconMark(
                        m,
                        ui::Rect{thumb.x, y, thumb.w, std::min(g.rowH, thumb.bottom() - y), true},
                        ui::Icon::Rule, ColorToken::textDim);
            } else {
                ui::rule(m, thumb, ColorToken::textDim);
            }
        }
    }

    // The note row: the selected row's explanation, or the answer to the copy action. A note that
    // is cut is better than a note that pushes the hints out of the sheet, and the golden pins
    // what a 40x15 display can hold.
    {
        const bool copied = state.copiedUntil > ImGui::GetTime();
        std::string note;
        ColorToken ink = ColorToken::textLow;
        if (copied) {
            note = kSettingsCopied;
            ink = ColorToken::accent;
        } else if (state.selected >= 0 && state.selected < static_cast<int>(state.rows.size())) {
            note = state.rows[static_cast<std::size_t>(state.selected)].note;
        }
        lineAt(m, g.note, g.note.x, fitText(m, note, g.note.w), ink);
    }

    // The key hints, in the backend's own vocabulary. Dropped whole when the row cannot hold it -
    // a hint cut mid-word advertises nothing.
    {
        const char* hints = m.textGrid ? kSettingsHints : kSettingsHintsGui;
        if (textUnits(m, hints) <= g.hints.w)
            lineAt(m, g.hints, g.hints.x, hints, ColorToken::textLow);
    }

    ImGui::End();
}

} // namespace llmfun::tui::views
