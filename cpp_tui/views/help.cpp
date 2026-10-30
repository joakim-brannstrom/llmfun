/// The help page (views/help.h owns the contract and the anatomy).
///
/// Structure of this file, in the order the page is built:
///
///   1. the two text helpers every view carries (a measure and a clipped line) and a
///      lower-case/contains pair for the search;
///   2. the generated key rows: one per binding of `model::defaultKeymap()`, spelled with
///      `model::chordLabel()` - the same functions `doc/ui_keys.md` is generated from - plus the
///      rail's row keys from `model::kRailRowKeys`;
///   3. the prose: one paragraph per region, the Raw Log's, and the sentence that says where the
///      command list comes from;
///   4. `helpPage()`: the four sections, in the design's order;
///   5. the rebuild (filter + wrap) and the renderer, which draws the rows it is given.
///
/// Layering note (`tools/gate_ratchet.txt`): no colour literal, no display size, no v4 type, no
/// `tui.h` and no profile branch beyond the metrics it was handed, so this file needs no ledger
/// row.
#include "views/help.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "model/keymap.h"
#include "model/rail_keys.h"
#include "model/status_chips.h"
#include "ui/input.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/text.h"
#include "ui/tokens.h"
#include "views/palette.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace llmfun::tui::views {

namespace {

using ui::ColorToken;

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

/// The search's comparison form: ASCII lower case, in place. The page's own text is ASCII, so a
/// table is not needed and a locale cannot change what a query matches.
std::string lowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

/// The group a scope is listed under in the Keys section: the registry's `Focus` in the words the
/// page uses. Not `keymap_doc.cpp`'s `scopeLabel()` because that one is the *document's*
/// vocabulary ("palette/overlay"); the page speaks to a user.
const char* scopeGroup(const model::Scope& scope) {
    if (scope.isAnywhere())
        return "anywhere";
    switch (scope.region()) {
    case model::Focus::Composer:
        return "composer";
    case model::Focus::Rail:
        return "rail";
    case model::Focus::Thread:
        return "thread";
    case model::Focus::Overlay:
        return "overlays";
    case model::Focus::RawLog:
        return "raw log";
    case model::Focus::Settings:
        return "settings sheet";
    case model::Focus::Help:
        return "help page";
    case model::Focus::None:
    case model::Focus::Count:
        break;
    }
    return "anywhere";
}

// ---------------------------------------------------------------------------
// The prose
// ---------------------------------------------------------------------------

/// One region's paragraph: the group header it is listed under and the sentence(s) it makes.
struct RegionNote {
    const char* group;
    std::string text;
};

/// The now bar's four chips in a reader's words, built from `model::kStatusChipNames` so the
/// paragraph cannot name a chip the struct does not carry (it said "tokens" and left `model` out
/// once - the row the reader sees is `ctx 0/0   -   none   Ready`).
std::string chipNamesPhrase() {
    std::string phrase = "(";
    for (std::size_t i = 0; i < model::kStatusChipNameCount; ++i) {
        if (i != 0)
            phrase += ", ";
        phrase += model::kStatusChipNames[i];
    }
    phrase += ")";
    return phrase;
}

/// The six tiled regions, in the layout's own order (top to bottom, left to right), each with one
/// paragraph. These are the only hand-written part of the page apart from the Raw Log's, and they
/// describe the shipped behaviour - the rail's entry names the character keys the rail actually
/// reads (`model::kRailRowKeys`) and says that delete is one press, because it is.
std::vector<RegionNote> regionNotes() {
    std::vector<RegionNote> notes;
    notes.push_back(
        {"top bar",
         "One row above the thread: the wordmark, the session title (click it to rename), the "
         "palette hint, the run pill, the RAW badge while the log is up, and the Settings entry. "
         "Below the width breakpoint the bar folds away into the now bar; the mark, the run pill "
         "and the badge never drop."});
    notes.push_back(
        {"rail",
         "The session list. / focuses the search field, and on a display that collapsed the rail "
         "it "
         "widens the column and hands the width back when the search is over. The row keys are "
         "characters, not chords: m opens the row menu, r renames, d deletes and p pins, on the "
         "row "
         "the pointer is over or the row that holds the keyboard focus. Delete is immediate - d, "
         "or "
         "the row menu's Delete, in one press. Ctrl+B hides or shows the rail."});
    notes.push_back(
        {"thread",
         "The transcript, newest at the bottom: one group per query, its blocks labelled by role "
         "(you, agent, thinking, tool call, tool result) with the answer attached. A table is "
         "drawn "
         "as a grid; with a block focused, y copies it and s shows a table's markdown source. The "
         "wheel or the scrollbar takes the view, End follows the newest again, and Load earlier "
         "reveals the groups older than the window. Focus mode hides the rail, the inspector and "
         "the top bar and draws the transcript in a centred column - F11, Ctrl+Shift+M in the "
         "pixel profile, or the palette's Focus mode row in the terminal - and Esc leaves it."});
    notes.push_back(
        {"inspector",
         "The right-hand panel, with two tabs. Agents lists one lane per agent (click a row to "
         "drill in; right click, or m, opens the row menu for the row under the pointer); Activity "
         "is the UI's own record list, with a filter over the level and the text. [ and ] walk the "
         "tabs, Ctrl+I hides or shows the panel, and Ctrl+5 / Ctrl+6 pick a tab in the pixel "
         "profile - the terminal cannot deliver those chords."});
    notes.push_back(
        {"composer",
         "The query field and its one action: Send while idle, Stop while a run is in flight. "
         "Enter "
         "inserts a newline, because the terminal delivers Shift+Enter and Ctrl+Enter as the same "
         "byte, so the control is what submits. Up and Down on the draft's first and last line "
         "recall the history. Esc stops a run, then clears the draft, then releases the keyboard, "
         "and in focus mode it leaves the mode first. "
         "A "
         "paste of more than 5000 characters asks before it is inserted."});
    notes.push_back(
        {"now bar",
         "The bottom row: the state rule, the phase word, the current detail, "
         "the run's clock, the contextual key hints and the four chips " +
             chipNamesPhrase() +
             ". In compact mode it also carries the top bar's badge, and focus mode - "
             "where that bar is hidden - keeps the badge here too and names its exit "
             "(Esc leave focus). Esc stop appears exactly while that press would stop "
             "the run. An action's answer (a copy, a saved log) replaces the hints for a "
             "moment; a warning or an error stays until x clears it, because it waits "
             "for an answer. In the pixel profile those answers are cards in the corner "
             "instead, with their own Dismiss control."});
    return notes;
}

/// The developer surface's paragraph (its own section: the design keeps it out of the normal
/// actions, and a reader who never opens it should not have to read about it here).
///
/// `Esc` closes the *overlay* - the grid's full-screen shape, which is modal. The GUI's drawer is
/// part of the tiling (`RawLogState::ownsKeyboard()` requires the overlay shape, and
/// `escStepClosesOverlay()` the same), so there `Esc` belongs to the run; the drawer closes from
/// its `Close` control. A page that says "Esc closes it" without that distinction
/// teaches a GUI user to cancel their agent.
constexpr const char* kRawLogNote =
    "An opt-in developer stream: the UI's own records, with level, time, source and message. It "
    "opens with /rawlog, the palette's Developer row, --debug-ui, or Ctrl+Shift+L in the pixel "
    "profile (the terminal loses the Shift and delivers Ctrl+L). On the grid it is a full-screen "
    "overlay and Esc closes it; in the GUI it is a drawer above the composer, resizable by its "
    "grip, and it closes from its Close control (Esc there belongs to the run). "
    "Keys: f follows the newest, p pauses, / filters and y copies the selected record. The ring "
    "keeps the last 5000 records or 2 MB and counts whatever it had to drop, and its open state is "
    "never saved.";

/// The Commands section's preamble: where the list lives and where the typed fallback goes. The
/// fallback is `/help`, which is D's own slash registry - it prints the full command list into
/// the thread and is untouched by this page (design FR-27: "`/help` in the thread remains for
/// scripts").
constexpr const char* kCommandsNote =
    "Typed into the composer, or run from the palette's command list. /help still prints the "
    "agent's full command list into the thread.";

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

/// The page's anatomy inside the box `ui::computeShell()` answered: the title row, the search
/// field, the list between them and the hint row at the bottom. Pure arithmetic - this file never
/// reads the display size and never writes the layout.
struct HelpGeometry {
    float frame = 0.0f;
    float pad = 0.0f;   // the surface's inner padding
    float gap = 0.0f;   // vertical spacing between blocks
    float lineH = 1.0f; // one text line (the title and the hint row)
    float fieldH = 1.0f;
    float titleY = 0.0f;
    float fieldY = 0.0f;
    float listTop = 0.0f;
    float listH = 0.0f;
    float hintY = 0.0f;
    float innerW = 0.0f;
    float x0 = 0.0f;
};

HelpGeometry helpGeometry(const ui::Rect& region, const ui::Metrics& m) {
    HelpGeometry g;
    g.frame = m.border;
    g.pad = m.textGrid ? m.pad1 : m.pad2;
    g.gap = m.textGrid ? 0.0f : ImGui::GetStyle().ItemSpacing.y;
    g.lineH = m.textGrid ? m.row : ImGui::GetTextLineHeight();
    g.fieldH = m.textGrid ? m.row : ImGui::GetFrameHeight();
    g.x0 = region.x + g.frame + g.pad;
    g.innerW = std::max(0.0f, region.w - 2.0f * (g.frame + g.pad));
    g.titleY = region.y + g.frame + g.pad;
    g.fieldY = g.titleY + g.lineH + g.gap;
    g.listTop = g.fieldY + g.fieldH + g.gap;
    g.hintY = region.bottom() - g.frame - g.pad - g.lineH;
    g.listH = std::max(0.0f, g.hintY - g.gap - g.listTop);
    return g;
}

/// How many rows the list can show, at least one (the "no matches" line lives there too).
int viewportRowsFor(const HelpGeometry& g, const ui::Metrics& m) {
    if (m.row <= 0.0f)
        return 1;
    return std::max(1, static_cast<int>(std::floor(g.listH / m.row)));
}

/// The order the Keys section lists its groups in: the widest scope first (`anywhere`), then the
/// regions a user can be in. A group with nothing in it draws nothing, and `./test_help` checks
/// that the page's order is this one - so a scope `scopeGroup()` can answer has to be listed here,
/// or its rows would rank last and break the "each group once, in order" rule.
constexpr const char* kKeyGroupOrder[] = {
    "anywhere", "composer",       "overlays",  "rail",
    "raw log",  "settings sheet", "help page", "rail rows",
};

int keyGroupRank(const std::string& group) {
    for (std::size_t i = 0; i < sizeof(kKeyGroupOrder) / sizeof(kKeyGroupOrder[0]); ++i) {
        if (group == kKeyGroupOrder[i])
            return static_cast<int>(i);
    }
    return static_cast<int>(sizeof(kKeyGroupOrder) / sizeof(kKeyGroupOrder[0]));
}

/// The page's key rows: the registry's rows, folded to one line per (chord, action) and grouped.
///
/// The registry repeats a row for every region a chord has to survive in - `Ctrl+Q` is bound five
/// times, once per modal surface, because a modal surface is a keyboard trap (keymap.h). That is
/// right in the table (each row is what the resolver looks up) and noise on a page, so the repeats
/// collapse: one line per chord and action, under the widest scope it has. A *different* action on
/// the same chord stays a separate line (`Ctrl+K` opens the palette *and* closes the topmost
/// overlay, and both are worth knowing).
std::vector<HelpKeyRow> helpPageKeyRows() {
    std::vector<HelpKeyRow> rows = helpKeyRows();
    std::vector<HelpKeyRow> folded;
    folded.reserve(rows.size());
    for (HelpKeyRow& row : rows) {
        bool seen = false;
        for (const HelpKeyRow& kept : folded) {
            if (kept.chord == row.chord && kept.action == row.action) {
                seen = true;
                break;
            }
        }
        if (!seen)
            folded.push_back(std::move(row));
    }
    std::stable_sort(folded.begin(), folded.end(), [](const HelpKeyRow& a, const HelpKeyRow& b) {
        return keyGroupRank(a.group) < keyGroupRank(b.group);
    });
    return folded;
}

/// The longest hint that fits `width` whole - the now bar's rule, applied to one line.
std::string hintFor(float width, const ui::Metrics& m) {
    for (std::size_t i = 0; i < kHelpHintCount; ++i) {
        if (textUnits(m, kHelpHints[i]) <= width)
            return kHelpHints[i];
    }
    return std::string();
}

} // namespace

// ---------------------------------------------------------------------------
// The generated key rows
// ---------------------------------------------------------------------------

std::vector<HelpKeyRow> helpKeyRows() {
    std::vector<HelpKeyRow> rows;
    rows.reserve(model::defaultKeymap().count);
    for (const model::Binding& binding : model::defaultKeymap()) {
        HelpKeyRow row;
        row.chord = model::chordLabel(binding.chord);
        row.action = model::actionName(binding.action);
        const model::ActionInfo* info = model::actionInfo(binding.action);
        if (info != nullptr && info->summary != nullptr)
            row.summary = info->summary;
        row.group = scopeGroup(binding.when);
        rows.push_back(std::move(row));
    }
    return rows;
}

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

std::vector<HelpSection> helpPage() {
    std::vector<HelpSection> page;

    // Keys: the registry's own rows, then the rail's character keys - the one table of them that
    // exists (`model/rail_keys.h`), so a key that changes cannot be advertised here as the old
    // one.
    HelpSection keys;
    keys.name = "Keys";
    for (const HelpKeyRow& row : helpPageKeyRows())
        keys.entries.push_back({row.group, row.chord, row.summary, /*paragraph=*/false});
    {
        std::string search;
        search.push_back(model::kRailSearchKey);
        keys.entries.push_back({"rail rows", search,
                                "focus the rail's search field (and widen a collapsed rail)",
                                /*paragraph=*/false});
        for (std::size_t i = 0; i < model::kRailRowKeyCount; ++i) {
            std::string key(1, model::kRailRowKeys[i].key);
            keys.entries.push_back(
                {"rail rows", key, model::kRailRowKeys[i].action, /*paragraph=*/false});
        }
    }
    page.push_back(std::move(keys));

    // Commands: the palette's table, one row per command (`views::commandTable()`), preceded by
    // the sentence that says where the list is and that `/help` still prints the full registry.
    HelpSection commands;
    commands.name = "Commands";
    commands.entries.push_back({"", "", kCommandsNote, /*paragraph=*/true});
    for (const CommandRow& command : commandTable())
        commands.entries.push_back({"", command.label, command.description, /*paragraph=*/false});
    page.push_back(std::move(commands));

    // Regions: one paragraph per region, each under its own group header (the region's name).
    HelpSection regions;
    regions.name = "Regions";
    for (const RegionNote& note : regionNotes())
        regions.entries.push_back({note.group, "", note.text, /*paragraph=*/true});
    page.push_back(std::move(regions));

    // Raw Log: the developer surface, kept apart from the six regions because it is a mode.
    HelpSection rawLog;
    rawLog.name = "Raw Log";
    rawLog.entries.push_back({"developer", "", kRawLogNote, /*paragraph=*/true});
    page.push_back(std::move(rawLog));

    return page;
}

// ---------------------------------------------------------------------------
// Open, close, rebuild
// ---------------------------------------------------------------------------

void openHelp(HelpState& state) {
    state.open = true;
    state.query.fill('\0');
    state.rows.clear();
    state.builtQuery.clear();
    state.builtWidth = -1;
    state.matches = 0;
    state.leftColumn = 0;
    state.firstVisible = 0;
    state.viewportRows = 0;
    state.drawnRows = 0;
    state.searchFocus = false;
    state.searchFocused = false;
    state.closeRequested = false;
    // The field's buffer is re-read on the next frame: a reopened page must not inherit ImGui's
    // stale edit state for the same id (the settings sheet's sequence trick).
    ++state.searchSeq;
}

void closeHelp(HelpState& state) {
    state.open = false;
    state.searchFocus = false;
    state.searchFocused = false;
    state.closeRequested = false;
    state.query.fill('\0');
    state.firstVisible = 0;
}

void rebuildHelp(HelpState& state, int innerCells) {
    const std::string nextQuery = state.query.data();
    // A *new result set* starts at its top: a page scrolled to the bottom of the key table would
    // otherwise open the filtered list in the middle of its matches, with the section header and
    // the rows that matched above the fold. A width change keeps the position (the same rows, a
    // longer or shorter wrap).
    const bool queryChanged = nextQuery != state.builtQuery;
    state.builtQuery = nextQuery;
    state.builtWidth = innerCells;
    state.rows.clear();
    state.matches = 0;
    state.leftColumn = 0;
    if (queryChanged)
        state.firstVisible = 0;

    const std::string needle = lowerAscii(state.builtQuery);
    const bool filtering = !needle.empty();
    const int wrapCells = std::max(8, innerCells);

    // The left column first: the widest key of what survives, clamped to a third of the page so a
    // long chord cannot push every sentence off the row.
    const std::vector<HelpSection> page = helpPage();
    int widest = 0;
    for (const HelpSection& section : page) {
        for (const HelpEntry& entry : section.entries) {
            if (entry.paragraph || entry.left.empty())
                continue;
            if (filtering &&
                lowerAscii(entry.left + " " + entry.right + " " + entry.group).find(needle) ==
                    std::string::npos)
                continue;
            widest = std::max(widest, ui::displayWidth(entry.left));
        }
    }
    state.leftColumn = std::min(widest + 2, std::max(8, innerCells / 3));

    for (const HelpSection& section : page) {
        // The section header is emitted only for a section that kept something: a query that
        // matches nothing in `Raw Log` must not leave its title standing over an empty body.
        const std::size_t before = state.rows.size();
        std::string lastGroup;
        for (const HelpEntry& entry : section.entries) {
            const std::string haystack =
                lowerAscii(entry.left + " " + entry.right + " " + entry.group);
            if (filtering && haystack.find(needle) == std::string::npos)
                continue;
            ++state.matches;
            if (!entry.group.empty() && entry.group != lastGroup) {
                state.rows.push_back({HelpRowKind::Group, entry.group, std::string()});
                lastGroup = entry.group;
            }
            if (!entry.paragraph) {
                state.rows.push_back({HelpRowKind::Entry, entry.left, entry.right});
                continue;
            }
            // Prose: wrapped to the page's width, one row per line. A paragraph that fits is one
            // row, and an empty one draws nothing at all.
            const int bodyCells = std::max(8, wrapCells - 2);
            for (std::string_view line : ui::wrap(entry.right, bodyCells))
                state.rows.push_back({HelpRowKind::Paragraph, std::string(), std::string(line)});
        }
        if (state.rows.size() == before)
            continue;
        state.rows.insert(state.rows.begin() + static_cast<std::ptrdiff_t>(before),
                          {HelpRowKind::Section, section.name, std::string()});
    }

    // A scroll position from a longer page (the filter changed) must not outlive the page it was
    // measured on.
    const int rows = static_cast<int>(state.rows.size());
    state.firstVisible =
        std::clamp(state.firstVisible, 0, std::max(0, rows - std::max(1, state.viewportRows)));
}

// ---------------------------------------------------------------------------
// The renderer
// ---------------------------------------------------------------------------

void renderHelp(HelpState& state, const ui::Rect& region, const ui::Metrics& m, bool ownsKeyboard) {
    // `drawnRows` is what the frame drew *this* time: a frame that draws nothing (closed, a box in
    // the wrong unit, a box too small for one row) must not report the previous frame's count - it
    // is a measurement a golden and a harness read.
    state.drawnRows = 0;
    if (!state.open || region.w <= 0.0f || region.h <= 0.0f)
        return;
    if (!ui::rectMatchesProfile(region, m))
        return; // fail closed: a rectangle in the wrong unit is worse than no page

    const HelpGeometry g = helpGeometry(region, m);
    if (g.innerW <= 0.0f || g.listH < m.row)
        return;
    // A cell budget in both profiles: the grid counts cells, the pixel profile divides the box by
    // its unit. Prose wraps to it and is clipped by the row it is drawn in, so an approximation
    // can never overflow the page.
    const int innerCells = m.textGrid
                               ? static_cast<int>(g.innerW)
                               : static_cast<int>(std::floor(g.innerW / std::max(1.0f, m.gu)));
    if (std::strcmp(state.query.data(), state.builtQuery.c_str()) != 0 ||
        innerCells != state.builtWidth)
        rebuildHelp(state, innerCells);

    // The scroll keys, before the rows are clamped: one viewport for PgUp/PgDn, one row for the
    // arrows and their `Ctrl` pair (the palette's measured keys). They read `IsKeyPressed`, which
    // the search field does not claim for these keys, and the arrows only while the field is
    // *not* editing - there they belong to the caret.
    if (ownsKeyboard) {
        const ImGuiIO& io = ImGui::GetIO();
        const int viewport = viewportRowsFor(g, m);
        int delta = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false))
            delta -= viewport;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false))
            delta += viewport;
        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
                (ImGui::IsKeyPressed(ImGuiKey_P, false) && io.KeyCtrl))
                delta -= 1;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
                (ImGui::IsKeyPressed(ImGuiKey_N, false) && io.KeyCtrl))
                delta += 1;
        }
        state.firstVisible += delta;
        // `/` focuses the search field. It is a character, not a chord (plan/key_gate.md §10):
        // the terminal delivers no key event for it, and `ui::takeCharacter()` refuses while a
        // text field owns the keyboard, which is what keeps a `/command` typed into the composer
        // the composer's.
        if (ui::takeCharacter('/'))
            state.searchFocus = true;
    }

    const int viewport = viewportRowsFor(g, m);
    state.viewportRows = viewport;
    const int total = static_cast<int>(state.rows.size());
    state.firstVisible = std::clamp(state.firstVisible, 0, std::max(0, total - viewport));

    // The page is its own window and not a draw-list fill: a fill recolours the cells but keeps
    // the glyphs already drawn in this frame, so the thread would show through the page (the
    // palette's and the settings sheet's lesson). Taking the window focus is what makes the page a
    // keyboard trap in fact and not only in the keymap: a composer field that still held ImGui's
    // active id would keep eating the plain keys before the page reads them.
    ImGui::SetNextWindowPos(ImVec2(region.x, region.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(region.w, region.h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_ChildBg));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##help", nullptr, flags);
    if (ownsKeyboard &&
        (ImGui::IsWindowAppearing() || !ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)))
        ImGui::SetWindowFocus();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    // The surface: an opaque panel with its own edge (drawn inside the rect - a stroke would
    // straddle the boundary and paint two cells in the grid), plus one run of spaces per row for
    // the cell renderer.
    ui::panel(ImGui::GetWindowDrawList(), m, region, ColorToken::surface, /*hairline=*/true);
    blankCells(m, region, ColorToken::surface);

    // The title row: the page's name on the left; on the right the `Close` control and, when the
    // row can still hold it, how many entries the query left. The name and the control never
    // drop - a title row without its name or its way out is not a title row.
    const ui::Rect title{g.x0, g.titleY, g.innerW, g.lineH, m.textGrid};
    lineAt(m, title, g.x0, g.titleY, kHelpTitle, ColorToken::textHi);
    {
        const float titleEnd = g.x0 + textUnits(m, kHelpTitle) + 2.0f * m.gu;
        float right = title.right();
        const ui::ButtonSpec closeSpec{kHelpCloseLabel};
        const float closeW = ui::measureButton(m, closeSpec).w;
        const ui::Rect close{right - closeW, g.titleY, closeW, std::min(g.lineH, m.row),
                             m.textGrid};
        if (close.w > 0.0f && close.x >= titleEnd) {
            ui::button(closeSpec, m, close);
            if (ui::buttonPressed(closeSpec, m, close))
                state.closeRequested = true;
            right = close.x - m.pad2;
        }
        const std::string summary =
            state.query.data()[0] != '\0'
                ? std::to_string(state.matches) + " match" + (state.matches == 1 ? "" : "es")
                : std::string();
        const float w = textUnits(m, summary);
        if (!summary.empty() && w + m.gu <= right - titleEnd)
            lineAt(m, title, right - w, g.titleY, summary, ColorToken::textLow);
    }

    // The search field. Live: the page is a few hundred strings and a substring test, so every
    // keystroke can answer immediately (an `Enter` would fight the page's `Enter`-free key set).
    if (g.innerW > m.pad1) {
        ImGui::SetCursorScreenPos(ImVec2(g.x0, g.fieldY));
        if (state.searchFocus)
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(g.innerW);
        const std::string id = "##help_search" + std::to_string(state.searchSeq);
        ImGui::InputTextWithHint(id.c_str(), kHelpSearchHint, state.query.data(),
                                 state.query.size());
        state.searchFocused = ImGui::IsItemActive();
        if (state.searchFocus && state.searchFocused)
            state.searchFocus = false;
        // ImGui paints the nav cursor around the focused widget; on the 1-cell grid that is a
        // full-cell ring that outlives the frame in the terminal. The caret marks the focus.
        if (m.textGrid) {
            ImGuiContext& ctx = *ImGui::GetCurrentContext();
            const ImGuiID fieldId = ImGui::GetID(id.c_str());
            if (ctx.ActiveId == fieldId || ctx.NavId == fieldId)
                ctx.NavCursorVisible = false;
        }
    }

    // The rows. One line per row, drawn from `firstVisible`; a section title is the page's own
    // structure (bright, unindented), a group header is one step down and indented, an entry has
    // the left column and the sentence beside it, and prose spans the inner width.
    const float indent = m.pad2;
    {
        const ui::TypeScope bodyType(ui::TypeStep::body, m.textGrid);
        // Whole rows only: the list's height is a whole number of rows on the grid and a
        // fractional one in the pixel profile, and a row drawn past the list's bottom edge would
        // spill over the hint row (or the page's own frame).
        const int lastDrawn = std::min(total, state.firstVisible + viewport);
        for (int i = state.firstVisible; i < lastDrawn; ++i) {
            const HelpRow& row = state.rows[static_cast<std::size_t>(i)];
            const float y = g.listTop + m.row * static_cast<float>(i - state.firstVisible);
            ++state.drawnRows;
            const ui::Rect line{g.x0, y, g.innerW, m.row, m.textGrid};
            switch (row.kind) {
            case HelpRowKind::Section:
                // The title is the accent ink and nothing else: a rule behind it would paint the
                // whole row (a cell renderer draws a rule as a colour band), and the sections are
                // already separated by their titles and the group headers under them.
                lineAt(m, line, g.x0, y, row.left, ColorToken::accent);
                break;
            case HelpRowKind::Group:
                lineAt(m, line, g.x0 + indent, y, row.left, ColorToken::textLow);
                break;
            case HelpRowKind::Entry: {
                const float columnX = g.x0 + indent;
                const float rightX = columnX + static_cast<float>(state.leftColumn) * m.gu;
                std::string left = row.left;
                if (m.textGrid && ui::displayWidth(left) > state.leftColumn)
                    left = ui::truncate(left, state.leftColumn);
                lineAt(m, line, columnX, y, left, ColorToken::textHi);
                if (rightX < line.right()) {
                    // Cells, not pixels: a sentence that does not fit is cut with the profile's
                    // ellipsis instead of running under the page's edge, where a clipped word
                    // advertises nothing (the palette's rule).
                    std::string right = row.right;
                    if (m.textGrid)
                        right = ui::truncate(right, static_cast<int>(line.right() - rightX));
                    lineAt(m, line, rightX, y, right, ColorToken::textMid);
                }
                break;
            }
            case HelpRowKind::Paragraph:
                lineAt(m, line, g.x0 + indent, y, row.right, ColorToken::textMid);
                break;
            }
        }
        if (total == 0)
            lineAt(m, {g.x0, g.listTop, g.innerW, m.row, m.textGrid}, g.x0 + indent, g.listTop,
                   kHelpEmpty, ColorToken::textMid);
    }

    // The hint row: the keys the page answers to, the longest form that fits whole.
    const std::string hint = hintFor(g.innerW, m);
    if (!hint.empty())
        lineAt(m, {g.x0, g.hintY, g.innerW, g.lineH, m.textGrid}, g.x0, g.hintY, hint,
               ColorToken::textDim);

    ImGui::End();
}

} // namespace llmfun::tui::views
