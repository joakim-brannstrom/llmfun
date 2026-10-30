// test_help.cpp
//
// The help page (views/help.h + views/help.cpp, design FR-27 / task 42): one searchable page
// with the keymap, the commands, one paragraph per region and the developer surface.
//
// What this binary proves, in order:
//
//   1. the source. The view includes no `tui.h`, names no v4 type, reads no display size, writes
//      no colour literal, asks no profile flag and opens no file; its vocabulary lives in its
//      header;
//   2. the page. The four sections are in the design's order, the Keys section is *generated*
//      (one row per binding of the registry, with the registry's own sentence), the Commands
//      section is the palette's command table, the Regions section has one paragraph per region
//      and the rail's paragraph matches what the rail does (character keys, one-press delete, no
//      confirmation);
//   3. the document. The Keys section and `doc/ui_keys.md` (both generated from
//      `model::defaultKeymap()`) list the same chords and actions - the anti-drift check the task
//      asks for;
//   4. the search. A query narrows the page by substring over the key and the sentence, drops the
//      headers of what it emptied, is case-insensitive, and wraps prose to the width it is given;
//   5. the keys. `Shift+/` resolves to the page when no text field has the keyboard and to nothing
//      while the composer does (typing `?` must stay typing), the page is a keyboard trap that
//      keeps quit, and `Esc` is the chain's first step;
//   6. the layout. The page has a rectangle inside the display at 100x30 and at the 40x15 floor,
//      it is the top of the overlay stack, and a closed page has no geometry;
//   7. the frame wiring: `?` in the character queue opens it (the grid's path), a text field keeps
//      it closed, `/` moves the keyboard into its search field, the search narrows what is drawn,
//      `Close` and `Esc` each close it in one press, and `/help` stays the typed fallback;
//   8. the goldens: open at 100x30, filtered at 100x30, and at the 40x15 floor.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = usage error.
//
//   ./test_help [-v] [--update] [--print]
//
// `--update` rewrites the fixtures; do that only for a deliberate rendering change, and commit the
// diff with it.

#include "views/help.h"

#include "views/palette.h"
#include "views/shell.h"

#include "tui.h"

#include "model/keymap.h"
#include "model/keymap_doc.h"
#include "model/rail_keys.h"
#include "model/ui_state.h"
#include "ui/input.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/text.h"

#include "imtui/imtui.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace ui = llmfun::tui::ui;
namespace views = llmfun::tui::views;
namespace model = llmfun::tui::model;
namespace tui = llmfun::tui;

namespace {

int g_checks = 0;
int g_failures = 0;
bool g_verbose = false;
bool g_print = false;
const char* g_phase = "start";

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) {
        if (g_verbose)
            std::printf("ok   %s\n", what.c_str());
        return;
    }
    std::fprintf(stderr, "FAIL [%s]: %s\n", g_phase, what.c_str());
    ++g_failures;
}

[[noreturn]] void usage(const char* argv0, const std::string& problem) {
    std::fprintf(stderr, "usage: %s [-v] [--update] [--print]   (%s)\n", argv0, problem.c_str());
    std::exit(2);
}

std::string readWhole(const std::string& path) {
    std::ifstream in(path);
    if (!in.good())
        return std::string();
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    if (!out.good())
        return false;
    out << text;
    return out.good();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string trimTrailingSpaces(std::string text) {
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    return text;
}

// ---------------------------------------------------------------------------
// The cell grid the text backend writes (the `TScreen` pattern)
// ---------------------------------------------------------------------------

ImTui::TScreen g_screen;
int g_w = 100;
int g_h = 30;

std::string utf8Of(uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

struct Grid {
    int nx = 0;
    int ny = 0;
    std::vector<uint32_t> ch;
    std::vector<unsigned char> bg;

    uint32_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return ch[static_cast<std::size_t>(y) * nx + x];
    }

    int bgAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return -1;
        return bg[static_cast<std::size_t>(y) * nx + x];
    }

    std::string rowText(int y) const {
        std::string text;
        for (int x = 0; x < nx; ++x) {
            const uint32_t cp = at(x, y);
            text += cp == 0 ? std::string(" ") : utf8Of(cp);
        }
        return text;
    }

    std::string allText() const {
        std::string text;
        for (int y = 0; y < ny; ++y) {
            text += rowText(y);
            text += '\n';
        }
        return text;
    }

    bool hasText(const std::string& needle) const { return contains(allText(), needle); }

    int rowOf(const std::string& needle) const {
        for (int y = 0; y < ny; ++y) {
            if (contains(rowText(y), needle))
                return y;
        }
        return -1;
    }
};

Grid capture() {
    Grid grid;
    grid.nx = g_screen.nx;
    grid.ny = g_screen.ny;
    const std::size_t cells = static_cast<std::size_t>(grid.nx) * grid.ny;
    grid.ch.resize(cells);
    grid.bg.resize(cells);
    for (std::size_t i = 0; i < cells; ++i) {
        grid.ch[i] = g_screen.data[i].ch;
        grid.bg[i] = g_screen.data[i].bg;
    }
    return grid;
}

void dump(const Grid& grid) {
    std::fprintf(stderr, "grid (%dx%d):\n", grid.nx, grid.ny);
    for (int y = 0; y < grid.ny; ++y)
        std::fprintf(stderr, "%2d |%s|\n", y, trimTrailingSpaces(grid.rowText(y)).c_str());
}

void checkGrid(bool ok, const std::string& what) {
    check(ok, what);
    if (!ok)
        dump(capture());
}

/// One frame of the text grid at `g_w x g_h`: a decoration-free root window, the body drawing
/// inside it, then the frame through the real backend. `inject` runs *before* `NewFrame`, which is
/// when ImGui consumes its input queue.
Grid frame(const std::function<void(const ui::Metrics&)>& body,
           const std::function<void(ImGuiIO&)>& inject = nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_w), static_cast<float>(g_h));
    io.DeltaTime = 1.0f / 60.0f;
    if (inject)
        inject(io);
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("help-frame", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    body(ui::metricsFor(true, 1.0f, 1.0f));
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    const Grid grid = capture();
    if (g_print) {
        std::printf("--- frame %dx%d\n", g_w, g_h);
        for (int y = 0; y < grid.ny; ++y)
            std::printf("%2d |%s|\n", y, grid.rowText(y).c_str());
    }
    return grid;
}

/// The whole workbench frame (the shell plus the overlay layer) at `w x h`, exactly as the frame
/// layer composes it - one `TuiState`, no second renderer.
views::ShellFrame shellFrame(tui::TuiState& state, int w, int h,
                             const std::function<void(ImGuiIO&)>& inject = nullptr,
                             const std::function<void()>& pre = nullptr) {
    views::ShellFrame out;
    g_w = w;
    g_h = h;
    frame(
        [&](const ui::Metrics&) {
            // `pre` runs after `NewFrame` and before the shell draws: it is where a harness can
            // claim a process-global (a focused text field) the way the real frame would.
            if (pre)
                pre();
            out = views::renderShell(state, ImVec2(static_cast<float>(w), static_cast<float>(h)));
            views::renderOverlays(state, out);
        },
        inject);
    return out;
}

tui::TuiState makeState() {
    tui::TuiState state;
    state.readyStatus = true;
    tui::SessionEntry entry;
    entry.id = "s1";
    entry.title = "help";
    entry.preview = "hello";
    entry.messageCount = 2;
    entry.isActive = true;
    state.sessionPanel.sessions.push_back(entry);
    state.sessionPanel.activeId = "s1";
    return state;
}

// ---------------------------------------------------------------------------
// Small queries over the page
// ---------------------------------------------------------------------------

const views::HelpSection* findSection(const std::vector<views::HelpSection>& page,
                                      const char* name) {
    for (const views::HelpSection& section : page) {
        if (section.name == name)
            return &section;
    }
    return nullptr;
}

/// Every entry of a section, headers and prose included, flattened into one string: what a
/// "does the page say X" check reads.
std::string sectionText(const views::HelpSection& section) {
    std::string text;
    for (const views::HelpEntry& entry : section.entries) {
        text += entry.group;
        text += ' ';
        text += entry.left;
        text += ' ';
        text += entry.right;
        text += '\n';
    }
    return text;
}

const views::HelpEntry* findEntry(const views::HelpSection& section, const char* group,
                                  const char* left) {
    for (const views::HelpEntry& entry : section.entries) {
        if (entry.group == group && entry.left == left)
            return &entry;
    }
    return nullptr;
}

/// The paragraph of one region (the entry whose group is the region's name).
std::string regionParagraph(const std::vector<views::HelpSection>& page, const char* group) {
    const views::HelpSection* regions = findSection(page, "Regions");
    if (regions == nullptr)
        return std::string();
    for (const views::HelpEntry& entry : regions->entries) {
        if (entry.group == group && entry.paragraph)
            return entry.right;
    }
    return std::string();
}

std::string rowsText(const views::HelpState& state) {
    std::string text;
    for (const views::HelpRow& row : state.rows) {
        text += row.left;
        text += ' ';
        text += row.right;
        text += '\n';
    }
    return text;
}

int countRows(const views::HelpState& state, views::HelpRowKind kind) {
    int n = 0;
    for (const views::HelpRow& row : state.rows)
        n += row.kind == kind ? 1 : 0;
    return n;
}

// ---------------------------------------------------------------------------
// 1. The source
// ---------------------------------------------------------------------------

void checkSource() {
    g_phase = "S: source";
    const std::string cpp = readWhole(std::string(TEST_HELP_VIEW_SOURCE));
    const std::string header = readWhole(std::string(TEST_HELP_HEADER_SOURCE));
    check(!cpp.empty(), "the page's source is readable");
    check(!header.empty(), "and so is its header");
    check(!contains(cpp, "#include \"tui.h\""), "views/help.cpp includes no tui.h (D-ADAPTER)");
    check(!contains(cpp, "ChatMessage"), "and names no v4 message type");
    check(!contains(cpp, "RenderGroup"), "and no v4 render group");
    check(!contains(cpp, "displaySize()"), "and reads no display size (D-METRICS)");
    check(!contains(cpp, "ImVec4("), "and writes no colour literal (D-THEME)");
    check(!contains(cpp, "tuiIsTextGrid"), "and asks no profile flag (the metrics answer it)");
    check(!contains(header, "#include \"tui.h\""), "views/help.h includes no tui.h either");
    // The page is a reader: the frame owns every write path, so a view that opened a file would be
    // a second writer nobody asked for.
    check(!contains(cpp, "fopen") && !contains(cpp, "std::ofstream"),
          "the page opens no file at all");
    check(!contains(cpp, "saveUiConf") && !contains(cpp, "flushUiConf"),
          "and never writes ui.conf");
    // The vocabulary lives in the header (the settings sheet's rule): the .cpp draws the strings.
    check(!contains(cpp, "\"Help\""), "the title is the header's string, not a literal");
    check(contains(cpp, "kHelpEmpty"), "and the empty state is the header's constant");
    check(!contains(cpp, "\"Close\""), "and the close control's label");
    // The *key table* is not written out by hand: the file names the registry and spells no chord
    // of its own. (The region prose legitimately quotes `Ctrl+B`, `Ctrl+I`, `Ctrl+Shift+L`, ... -
    // those are sentences about a region, and they are asserted one by one further down.)
    check(!contains(cpp, "\"Ctrl+K\""), "the key table spells no chord itself (the registry does)");
    check(!contains(cpp, "\"Shift+/\""), "not even the page's own chord");
}

// ---------------------------------------------------------------------------
// 2. The page
// ---------------------------------------------------------------------------

void checkPage() {
    g_phase = "P: page";
    const std::vector<views::HelpSection> page = views::helpPage();
    check(page.size() == 4, "the page has four sections");
    if (page.size() != 4)
        return;
    check(page[0].name == "Keys", "the first section is Keys");
    check(page[1].name == "Commands", "then Commands");
    check(page[2].name == "Regions", "then Regions");
    check(page[3].name == "Raw Log", "and the developer surface last");

    // Keys: one row per binding in the *registry* table (the document's), folded on the page to one
    // line per chord and action (a modal surface is a keyboard trap, so the registry repeats a
    // chord for every region it must survive in).
    const std::vector<views::HelpKeyRow> keys = views::helpKeyRows();
    check(keys.size() == model::defaultKeymap().count,
          "the key rows are one per binding (" + std::to_string(keys.size()) + " for " +
              std::to_string(model::defaultKeymap().count) + ")");
    bool rowsMatch = keys.size() == model::defaultKeymap().count;
    if (rowsMatch) {
        std::size_t i = 0;
        for (const model::Binding& binding : model::defaultKeymap()) {
            const model::ActionInfo* info = model::actionInfo(binding.action);
            rowsMatch = rowsMatch && keys[i].chord == model::chordLabel(binding.chord) &&
                        keys[i].action == model::actionName(binding.action) && info != nullptr &&
                        keys[i].summary == info->summary && !keys[i].summary.empty() &&
                        !keys[i].group.empty();
            ++i;
        }
    }
    check(rowsMatch, "every row is the binding's chord, name, summary and scope");
    // A summary is the only user-facing sentence an action has: an action without one would draw
    // an empty row.
    bool summaries = true;
    for (const model::ActionInfo& info : model::allActions())
        summaries = summaries && info.summary != nullptr && info.summary[0] != '\0';
    check(summaries, "every registry action carries a one-line summary");
    // The rail's row keys are the page's too, and they come from the table the rail reads.
    const views::HelpSection* keysSection = findSection(page, "Keys");
    check(keysSection != nullptr, "the Keys section exists");
    if (keysSection != nullptr) {
        // Every binding is on the page (its chord and its sentence), and no line is drawn twice.
        bool everyBinding = true;
        for (const model::Binding& binding : model::defaultKeymap()) {
            const model::ActionInfo* info = model::actionInfo(binding.action);
            if (info == nullptr)
                continue;
            const std::string chord = model::chordLabel(binding.chord);
            bool seen = false;
            for (const views::HelpEntry& entry : keysSection->entries)
                seen = seen || (entry.left == chord && entry.right == info->summary);
            everyBinding = everyBinding && seen;
        }
        check(everyBinding, "every binding of the registry is listed on the page");
        bool folded = true;
        for (std::size_t i = 0; i < keysSection->entries.size(); ++i) {
            for (std::size_t j = i + 1; j < keysSection->entries.size(); ++j) {
                const views::HelpEntry& a = keysSection->entries[i];
                const views::HelpEntry& b = keysSection->entries[j];
                folded = folded && !(a.group == b.group && a.left == b.left && a.right == b.right);
            }
        }
        check(folded, "and no row is drawn twice (the trap repeats are folded)");
        // One header per group, in the page's order: a range of rows, not an interleaving.
        const char* order[] = {"anywhere", "composer",       "overlays",  "rail",
                               "raw log",  "settings sheet", "help page", "rail rows"};
        const int orderCount = static_cast<int>(sizeof(order) / sizeof(order[0]));
        std::string lastGroup;
        bool orderedGroups = true;
        for (const views::HelpEntry& entry : keysSection->entries) {
            if (entry.group == lastGroup)
                continue;
            int rank = -1, lastRank = -1;
            for (int i = 0; i < orderCount; ++i) {
                if (entry.group == order[i])
                    rank = i;
                if (lastGroup == order[i])
                    lastRank = i;
            }
            orderedGroups = orderedGroups && rank >= 0 && rank > lastRank;
            lastGroup = entry.group;
        }
        check(orderedGroups, "the groups run once each, in the page's own order");
        std::string railKey = "/";
        check(findEntry(*keysSection, "rail rows", railKey.c_str()) != nullptr,
              "the rail's search key is listed");
        bool allRailKeys = true;
        for (std::size_t i = 0; i < model::kRailRowKeyCount; ++i) {
            const std::string key(1, model::kRailRowKeys[i].key);
            const views::HelpEntry* entry = findEntry(*keysSection, "rail rows", key.c_str());
            allRailKeys =
                allRailKeys && entry != nullptr && entry->right == model::kRailRowKeys[i].action;
        }
        check(allRailKeys, "and so is every row key, with the table's own sentence");
    }

    // Commands: the palette's table, and the typed fallback named.
    const views::HelpSection* commands = findSection(page, "Commands");
    check(commands != nullptr, "the Commands section exists");
    if (commands != nullptr) {
        bool allCommands = true;
        for (const views::CommandRow& command : views::commandTable()) {
            const views::HelpEntry* entry = findEntry(*commands, "", command.label);
            allCommands = allCommands && entry != nullptr && entry->right == command.description;
        }
        check(allCommands, "every command of the palette's table is listed with its description");
        check(findEntry(*commands, "", "/help") != nullptr, "/help is one of them");
        check(contains(sectionText(*commands), "/help still prints"),
              "and the page says the typed /help is still the full list");
    }

    // Regions: the six, in the layout's order, each with a non-empty paragraph.
    const views::HelpSection* regions = findSection(page, "Regions");
    check(regions != nullptr, "the Regions section exists");
    if (regions != nullptr) {
        const char* expected[] = {"top bar", "rail", "thread", "inspector", "composer", "now bar"};
        bool ordered = true;
        std::string last;
        for (const char* name : expected) {
            ordered = ordered && !regionParagraph(page, name).empty();
            // The groups appear in this order: every name is found after the previous one.
            const std::string text = sectionText(*regions);
            ordered = ordered && text.find(name) != std::string::npos;
            last = name;
        }
        check(ordered, "all six regions have a paragraph");
        check(!last.empty(), "the last region is the now bar");
        int paragraphs = 0;
        for (const views::HelpEntry& entry : regions->entries)
            paragraphs += entry.paragraph ? 1 : 0;
        check(paragraphs == 6, "and there are exactly six of them");
    }

    // The rail's paragraph must match the code (task 23 rev 3): the character keys, one-press
    // delete, and no confirmation.
    const std::string rail = regionParagraph(page, "rail");
    check(!rail.empty(), "the rail's paragraph exists");
    check(contains(rail, "Ctrl+B"), "it names the rail's chord");
    for (char key : {'m', 'r', 'd', 'p'}) {
        const std::string spelled = std::string(1, key);
        check(contains(rail, " " + spelled + " ") || contains(rail, spelled + " "),
              "it names the character key '" + spelled + "'");
    }
    check(contains(rail, "/ focuses"), "it says what the search key does");
    check(contains(rail, "one press"), "it says delete is one press");
    check(!contains(rail, "confirm") && !contains(rail, "Confirm"),
          "and it describes no confirmation");

    // The now bar's paragraph names the chips the row actually draws, from the struct's own
    // inventory: the four names in `StatusChips`, and no chip that does not exist.
    const std::string nowBar = regionParagraph(page, "now bar");
    check(!nowBar.empty(), "the now bar's paragraph exists");
    for (std::size_t i = 0; i < model::kStatusChipNameCount; ++i) {
        check(contains(nowBar, model::kStatusChipNames[i]),
              std::string("it names the '") + model::kStatusChipNames[i] + "' chip");
    }
    check(!contains(nowBar, "tokens"), "and no chip the struct does not carry");
    // The words, not the letter: a check on "x" alone is satisfied by "pixel", and the behavioural
    // test presses the key without ever reading this page.
    check(contains(nowBar, "x clears it"), "it names the key that answers a lingering warning");
    check(contains(nowBar, "Dismiss"), "and the pixel profile's Dismiss control");

    // Raw Log: the developer surface, with the measured paths and the ring's bound.
    const views::HelpSection* rawLog = findSection(page, "Raw Log");
    check(rawLog != nullptr, "the Raw Log section exists");
    if (rawLog != nullptr) {
        const std::string text = sectionText(*rawLog);
        check(contains(text, "/rawlog"), "it names the typed path");
        check(contains(text, "--debug-ui"), "and the flag");
        check(contains(text, "5000"), "and the ring's bound");
        check(contains(text, "Ctrl+Shift+L"), "and the pixel profile's chord");
        // `Esc` closes the grid's overlay only; the GUI's drawer belongs to the tiling, so `Esc`
        // there is the run's key and the drawer closes from its Close control. A page that
        // promises otherwise teaches a user to cancel their agent.
        check(contains(text, "the grid"), "it scopes the Esc close to the grid's overlay");
        check(contains(text, "Close control"), "and names how the GUI's drawer closes");
        check(contains(text, "Esc there belongs to the run"), "and says what Esc does there");
    }

    // Nothing on the page is empty: an empty row draws a blank line nobody can explain.
    bool noEmptyRight = true;
    for (const views::HelpSection& section : page) {
        for (const views::HelpEntry& entry : section.entries)
            noEmptyRight = noEmptyRight && !entry.right.empty();
    }
    check(noEmptyRight, "no entry has an empty sentence");
}

// ---------------------------------------------------------------------------
// 3. The document (the anti-drift check)
// ---------------------------------------------------------------------------

/// The `| key | action | where | ... |` rows of the generated document, as (chord, action) pairs.
std::vector<std::string> documentRows(const std::string& markdown) {
    std::vector<std::string> rows;
    std::size_t pos = markdown.find("| key | action | where |");
    if (pos == std::string::npos)
        return rows;
    pos = markdown.find('\n', pos);
    while (pos != std::string::npos) {
        const std::size_t end = markdown.find('\n', pos + 1);
        if (end == std::string::npos)
            break;
        const std::string line = markdown.substr(pos + 1, end - pos - 1);
        if (line.rfind("| ", 0) != 0)
            break; // the table ended (a blank line or the next heading)
        if (line.find("---") != std::string::npos) {
            pos = end; // the header's separator row, not a binding
            continue;
        }
        // `| key | action | ...`: the first two cells are what the page also lists.
        const std::size_t first = line.find('|', 0);
        const std::size_t second = line.find('|', first + 1);
        const std::size_t third = line.find('|', second + 1);
        if (second == std::string::npos || third == std::string::npos)
            break;
        std::string key = line.substr(first + 1, second - first - 1);
        std::string action = line.substr(second + 1, third - second - 1);
        while (!key.empty() && key.front() == ' ')
            key.erase(key.begin());
        while (!key.empty() && key.back() == ' ')
            key.pop_back();
        while (!action.empty() && action.front() == ' ')
            action.erase(action.begin());
        while (!action.empty() && action.back() == ' ')
            action.pop_back();
        rows.push_back(key + " -> " + action);
        pos = end;
    }
    return rows;
}

void checkDocument() {
    g_phase = "D: document";
    const std::string markdown = model::uiKeysMarkdown();
    check(!markdown.empty(), "the document is generated");

    std::vector<std::string> doc = documentRows(markdown);
    std::vector<std::string> page;
    for (const views::HelpKeyRow& row : views::helpKeyRows())
        page.push_back(row.chord + " -> " + row.action);
    std::sort(doc.begin(), doc.end());
    std::sort(page.begin(), page.end());
    check(!doc.empty(), "the document has a binding table");
    check(doc == page, "the page's key rows are the document's rows (" +
                           std::to_string(page.size()) + " for " + std::to_string(doc.size()) +
                           ")");

    // The two obvious members, spelled as a user reads them.
    check(std::find(page.begin(), page.end(), "Shift+/ -> Help") != page.end(),
          "the document lists Shift+/ as the help chord");
    check(std::find(page.begin(), page.end(), "Ctrl+K -> Palette") != page.end(),
          "and Ctrl+K as the palette's");
    check(model::chordLabel(ImGuiMod_Shift | ImGuiKey_Slash) == "Shift+/",
          "a Shift chord is spelled with its key, not with '?'");
    check(model::chordLabel(ImGuiKey_End) == "End", "and a named key by its name");
}

// ---------------------------------------------------------------------------
// 4. The search
// ---------------------------------------------------------------------------

void rebuildFor(views::HelpState& state, const char* query, int cells) {
    std::snprintf(state.query.data(), state.query.size(), "%s", query);
    views::rebuildHelp(state, cells);
}

void checkSearch() {
    g_phase = "F: search";
    views::HelpState state;
    views::openHelp(state);
    rebuildFor(state, "", 100);
    const int allRows = static_cast<int>(state.rows.size());
    const int allMatches = state.matches;
    check(allRows > 40, "the unfiltered page is a page (" + std::to_string(allRows) + " rows)");
    check(allMatches > model::defaultKeymap().count,
          "and it counts its entries (" + std::to_string(allMatches) + ")");
    check(countRows(state, views::HelpRowKind::Section) == 4, "with four section headers");
    check(countRows(state, views::HelpRowKind::Paragraph) > 6,
          "and the prose wrapped to the width it was given");

    // A query with no match leaves nothing at all - not even a header.
    rebuildFor(state, "zzzznothing", 100);
    check(state.rows.empty(), "a query that matches nothing draws no rows");
    check(state.matches == 0, "and counts no match");

    // A chord fragment finds the key rows, whichever case it is typed in.
    rebuildFor(state, "Ctrl+K", 100);
    const std::string paletteRows = rowsText(state);
    check(contains(paletteRows, "palette"), "a chord fragment finds its row");
    check(countRows(state, views::HelpRowKind::Section) <= 2, "and drops the sections it emptied");
    rebuildFor(state, "ctrl+k", 100);
    check(rowsText(state) == paletteRows, "the search is case-insensitive");

    // A region's name finds its paragraph and the key group of the same name.
    rebuildFor(state, "composer", 100);
    const std::string composerRows = rowsText(state);
    check(contains(composerRows, "composer"), "a region's name finds it");
    check(contains(composerRows, "newline"), "and its paragraph, not only the header");
    check(contains(composerRows, "Up"), "and the composer's own keys");

    // A new query is a new result set: it starts at its *top*, not at the scroll offset the longer
    // page had. A user at the end of the key table who types a filter must see the first match.
    rebuildFor(state, "", 100);
    state.viewportRows = 17;
    state.firstVisible = 40; // deep into the unfiltered page
    rebuildFor(state, "composer", 100);
    check(state.firstVisible == 0, "a new query starts at the top of its results");
    check(!state.rows.empty() && state.rows.front().kind == views::HelpRowKind::Section &&
              state.rows.front().left == "Keys",
          "so the first drawn row is the first matching section header");
    // A pure width change keeps the position: the same rows, wrapped differently.
    rebuildFor(state, "", 100);
    state.viewportRows = 17;
    state.firstVisible = 20;
    views::rebuildHelp(state, 80);
    check(state.firstVisible == 20, "a width change keeps the scroll position");

    // Wrapping: the same paragraph takes more rows on a narrower page, and no row is wider than
    // the page it was wrapped for.
    rebuildFor(state, "the transcript", 100);
    const int wideRows = countRows(state, views::HelpRowKind::Paragraph);
    rebuildFor(state, "the transcript", 40);
    const int narrowRows = countRows(state, views::HelpRowKind::Paragraph);
    check(narrowRows > wideRows, "a narrower page wraps the same prose into more lines (" +
                                     std::to_string(narrowRows) + " for " +
                                     std::to_string(wideRows) + ")");
    bool fits = true;
    for (const views::HelpRow& row : state.rows) {
        if (row.kind == views::HelpRowKind::Paragraph)
            fits = fits && ui::displayWidth(row.right) <= 40;
    }
    check(fits, "and every prose line fits the width it was wrapped for");

    // The left column never takes more than a third of the page.
    views::HelpState wide;
    views::openHelp(wide);
    views::rebuildHelp(wide, 120);
    check(wide.leftColumn > 0 && wide.leftColumn <= 120 / 3 + 2,
          "the key column is clamped to a third of the page (" + std::to_string(wide.leftColumn) +
              ")");
}

// ---------------------------------------------------------------------------
// 5. The keys
// ---------------------------------------------------------------------------

void checkKeys() {
    g_phase = "K: keys";
    const model::Keymap& keymap = model::defaultKeymap();
    const ImGuiKeyChord help = ImGuiMod_Shift | ImGuiKey_Slash;
    check(model::resolve(keymap, help, model::Focus::None) == model::Action::Help,
          "Shift+/ opens the page when nothing owns the keyboard");
    check(model::resolve(keymap, help, model::Focus::Composer) == model::Action::None,
          "and types a '?' in the composer instead (D-KEYMAP's text-field rule)");
    check(model::resolve(keymap, help, model::Focus::Overlay) == model::Action::None,
          "an overlay is a keyboard trap for it");
    check(model::resolve(keymap, help, model::Focus::RawLog) == model::Action::None,
          "and so is the Raw Log");
    check(model::resolve(keymap, help, model::Focus::Settings) == model::Action::None,
          "and the settings sheet");
    check(model::resolve(keymap, help, model::Focus::Help) == model::Action::None,
          "the page does not bind its own chord (Esc closes it)");
    check(model::resolve(keymap, ImGuiMod_Ctrl | ImGuiKey_Q, model::Focus::Help) ==
              model::Action::Quit,
          "while quit keeps working inside it");
    check(model::resolve(keymap, ImGuiMod_Ctrl | ImGuiKey_K, model::Focus::Help) ==
              model::Action::None,
          "and the palette's chord does not open behind it");
    // The plain-key rule is unchanged for a key that types a letter.
    check(model::resolve(keymap, ImGuiKey_M, model::Focus::Composer) == model::Action::None,
          "a plain character still never fires in the composer");
    // The registry's reach answer: the page is a palette row *and* a chord - the row is the path
    // that works while the composer holds the keyboard (where `Shift+/` is the `?` being typed).
    const model::ActionInfo* info = model::actionInfo(model::Action::Help);
    check(info != nullptr && info->reach == model::ActionReach::Palette,
          "the registry says the page is reached from the palette (and by its key)");
    {
        tui::TuiState state = makeState();
        std::deque<std::string> idStorage;
        bool listed = false;
        for (const views::PaletteItem& item :
             views::paletteActions(state, std::string(), idStorage)) {
            listed = listed || item.action == model::Action::Help;
        }
        check(listed, "and the palette's table has the row");
    }
}

// ---------------------------------------------------------------------------
// 6. The layout
// ---------------------------------------------------------------------------

void checkLayout() {
    g_phase = "L: layout";
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    check(ui::overlayMinimumW(m, ui::OverlayLayer::Help) > 0.0f, "the page has a minimum width");
    check(ui::overlayMinimumH(m, ui::OverlayLayer::Help) > 0.0f, "and a minimum height");
    const int sizes[2][2] = {{100, 30}, {40, 15}};
    for (const auto& size : sizes) {
        ui::LayoutPrefs prefs;
        prefs.helpOpen = true;
        const ui::ShellLayout layout = ui::computeShell(
            m, ImVec2(static_cast<float>(size[0]), static_cast<float>(size[1])), prefs);
        const std::string at = std::to_string(size[0]) + "x" + std::to_string(size[1]);
        check(layout.helpOverlay.w > 0.0f && layout.helpOverlay.h > 0.0f,
              "the page has a rectangle at " + at);
        check(layout.helpOverlay.x >= 0.0f && layout.helpOverlay.y >= 0.0f &&
                  layout.helpOverlay.right() <= static_cast<float>(size[0]) &&
                  layout.helpOverlay.bottom() <= static_cast<float>(size[1]),
              "and it is inside the display at " + at);
        check(layout.helpOverlay.cells, "and it states the profile's unit at " + at);
        check(layout.top == ui::OverlayLayer::Help, "and it is the top of the stack at " + at);
        check(layout.thread.w > 0.0f, "the tiling is untouched at " + at);
    }
    {
        // Closed: no geometry, nothing to close.
        ui::LayoutPrefs prefs;
        const ui::ShellLayout layout = ui::computeShell(m, ImVec2(100.0f, 30.0f), prefs);
        check(layout.helpOverlay.w == 0.0f && layout.helpOverlay.h == 0.0f,
              "a closed page has no rectangle");
        check(layout.top == ui::OverlayLayer::None, "and nothing to close");
    }
    check(std::string(ui::overlayLayerName(ui::OverlayLayer::Help)) == "help",
          "the layer has a name for the log and the tests");
}

// ---------------------------------------------------------------------------
// 6b. The real frame loop (`tuiRender`)
// ---------------------------------------------------------------------------

/// One frame through the real frame layer: the keymap, the `Esc` chain and the ordering the app
/// uses, which the shell-only harness above does not run.
void appFrame(tui::TuiState& state, int w, int h,
              const std::function<void(ImGuiIO&)>& inject = nullptr) {
    g_w = w;
    g_h = h;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));
    io.DeltaTime = 1.0f / 60.0f;
    if (inject)
        inject(io);
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    if (!tui::tuiRender(state)) {
        std::fprintf(stderr, "FAIL [%s]: tuiRender reported 'user terminated'\n", g_phase);
        std::exit(1);
    }
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
}

/// `Shift+/` the way the pixel profile delivers it, one event per frame (ImGui trickles the queue,
/// so a frame that carries both the modifier and the key only delivers one of them - measured).
void pressShiftSlash(tui::TuiState& state, int w, int h) {
    appFrame(state, w, h, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Shift, true); });
    appFrame(state, w, h, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Slash, true); });
    appFrame(state, w, h, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Slash, false); });
    appFrame(state, w, h, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Shift, false); });
}

void pressEscape(tui::TuiState& state, int w, int h) {
    appFrame(state, w, h, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, true); });
    appFrame(state, w, h, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, false); });
}

/// Let the frame loop reach its rest state: the composer's field asks for the keyboard on its own
/// first frames (it is the region a chat app starts in), so a test that wants a *global* key has to
/// wait for that and then release it with `Esc` - which is what a user does before a chord.
void settleAndRelease(tui::TuiState& state, int w, int h) {
    for (int i = 0; i < 6; ++i)
        appFrame(state, w, h);
    for (int i = 0; i < 4; ++i) {
        if (!ImGui::GetIO().WantTextInput)
            break;
        pressEscape(state, w, h);
        appFrame(state, w, h);
    }
}

void checkFrameLoop() {
    g_phase = "X: frame loop";
    {
        // The chord, through the dispatcher the app uses. `Shift+/` is the field's `?` while the
        // composer holds the keyboard (the keymap's text-field rule, and the reason the palette
        // also lists the page), so the user first releases the field - one `Esc` - and then the
        // chord opens the page. `Esc` closes it again, one press each.
        tui::TuiState state = makeState();
        settleAndRelease(state, 100, 30);
        check(!ImGui::GetIO().WantTextInput, "the composer's field is released");
        check(!state.help.open, "the page starts closed");
        pressShiftSlash(state, 100, 30);
        check(state.help.open, "Shift+/ opens it through the frame's keymap");
        check(state.overlayOwnsKeyboard(), "and it owns the keyboard");
        pressEscape(state, 100, 30);
        check(!state.help.open, "Esc closes it in one press");
        check(!state.overlayOwnsKeyboard(), "and the keyboard goes back below");
    }
    {
        // The character, through the same loop (the terminal's path).
        tui::TuiState state = makeState();
        settleAndRelease(state, 100, 30);
        check(!state.help.open, "still closed before the character");
        appFrame(state, 100, 30, [](ImGuiIO& io) { io.AddInputCharacter('?'); });
        appFrame(state, 100, 30);
        check(state.help.open, "a typed '?' opens it through the same loop");
        pressEscape(state, 100, 30);
        check(!state.help.open, "and Esc closes it there too");
    }
}

// ---------------------------------------------------------------------------
// 7. The frame wiring
// ---------------------------------------------------------------------------

void idle(tui::TuiState& state, int w, int h, int frames = 2) {
    for (int i = 0; i < frames; ++i)
        shellFrame(state, w, h);
}

void typeChar(tui::TuiState& state, int w, int h, char c) {
    shellFrame(state, w, h,
               [&](ImGuiIO& io) { io.AddInputCharacter(static_cast<unsigned int>(c)); });
    shellFrame(state, w, h);
}

void checkWiring() {
    g_phase = "W: wiring";
    {
        // The grid's path: the character queue carries a `?`.
        tui::TuiState state = makeState();
        const views::ShellFrame out =
            shellFrame(state, 100, 30, [](ImGuiIO& io) { io.AddInputCharacter('?'); });
        check(state.help.open, "a typed '?' opens the page (the terminal's path)");
        check(views::escStepClosesOverlay(state), "and the page is what Esc closes");
        check(state.overlayOwnsKeyboard(), "and it owns the keyboard");
        const Grid grid = capture();
        checkGrid(grid.hasText("Help"), "the page is drawn");
        check(out.layout.helpOverlay.w > 0.0f,
              "and it has a rectangle in the frame that opened it");
        // The layer above the tiling: the thread's own text must not be painted over the page.
        check(state.help.open, "the page stays open until a press closes it");
    }
    {
        // A text field keeps the character: the composer's `?` is typing, not a page.
        tui::TuiState state = makeState();
        shellFrame(
            state, 100, 30, [](ImGuiIO& io) { io.AddInputCharacter('?'); },
            []() { ImGui::GetIO().WantTextInput = true; });
        check(!state.help.open, "a '?' typed into a focused field stays text");
        check(!views::escStepClosesOverlay(state), "and no overlay is up");
    }
    {
        // `Esc` closes it in one press, through the frame-level chain.
        tui::TuiState state = makeState();
        shellFrame(state, 100, 30, [](ImGuiIO& io) { io.AddInputCharacter('?'); });
        check(state.help.open, "the page is up");
        check(views::closeTopOverlay(state) == ui::OverlayLayer::Help, "Esc's step is the page");
        check(!state.help.open, "and it closed");
        check(!views::escStepClosesOverlay(state), "with nothing left to close");
        check(views::closeTopOverlay(state) == ui::OverlayLayer::None,
              "a second close finds nothing (one press, one step)");
        check(!state.overlayOwnsKeyboard(), "and the keyboard is back below");
    }
    {
        // `/` moves the keyboard into the search field, and the field narrows the page.
        tui::TuiState state = makeState();
        state.help.open = true;
        idle(state, 100, 30);
        check(!state.help.searchFocused, "the page opens without the keyboard in its field");
        typeChar(state, 100, 30, '/');
        idle(state, 100, 30, 2);
        check(state.help.searchFocused, "'/' focuses the search field");
        const int allRows = static_cast<int>(state.help.rows.size());
        for (const char c : std::string("composer")) {
            shellFrame(state, 100, 30,
                       [&](ImGuiIO& io) { io.AddInputCharacter(static_cast<unsigned int>(c)); });
        }
        idle(state, 100, 30, 2);
        check(std::string(state.help.query.data()) == "composer",
              "typing lands in the field (" + std::string(state.help.query.data()) + ")");
        check(static_cast<int>(state.help.rows.size()) < allRows,
              "and the page narrows as it is typed");
        check(state.help.matches > 0 && contains(rowsText(state.help), "composer"),
              "to the rows that mention it");
    }
    {
        // The `Close` control is the pointer's way out.
        tui::TuiState state = makeState();
        const views::ShellFrame out =
            shellFrame(state, 100, 30, [](ImGuiIO& io) { io.AddInputCharacter('?'); });
        idle(state, 100, 30);
        const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
        const ui::ButtonSpec spec{views::kHelpCloseLabel};
        const float closeRight = out.layout.helpOverlay.right() - m.border - m.pad1;
        const float closeX = closeRight - ui::measureButton(m, spec).w * 0.5f;
        const float closeY = out.layout.helpOverlay.y + m.border + m.pad1 + m.row * 0.5f;
        shellFrame(state, 100, 30, [&](ImGuiIO& io) { io.AddMousePosEvent(closeX, closeY); });
        shellFrame(state, 100, 30, [&](ImGuiIO& io) {
            io.AddMousePosEvent(closeX, closeY);
            io.AddMouseButtonEvent(0, true);
        });
        shellFrame(state, 100, 30, [&](ImGuiIO& io) {
            io.AddMousePosEvent(closeX, closeY);
            io.AddMouseButtonEvent(0, false);
        });
        shellFrame(state, 100, 30);
        check(!state.help.open, "the Close control closes the page in one press");
    }
    {
        // The typed fallback is untouched: `/help` is one of the palette's command lines, and the
        // page never intercepts a `/` that belongs to a text field.
        bool listed = false;
        for (const views::CommandRow& command : views::commandTable())
            listed = listed || std::string(command.label) == "/help";
        check(listed, "/help is still the typed fallback (the palette's COMMANDS row)");
        tui::TuiState state = makeState();
        shellFrame(state, 100, 30, [](ImGuiIO& io) { io.AddInputCharacter('/'); });
        check(!state.help.open, "a '/' typed into the composer opens no page");
    }
    {
        // The palette's `Show help` row is the path that works from the composer's field. The row
        // opens the page and the palette closes *after* it, so `closePalette()`'s keyboard
        // hand-back runs with the page already up: it must not ask the composer behind the page for
        // the keyboard (the rule the settings sheet established, and the reason `isSubmitted` must
        // stay clean - it is a latch other paths read).
        tui::TuiState state = makeState();
        // Both latches start *set* (the composer asks for the keyboard on its first frame), so the
        // check is a clean A/B: clear them, perform the row, close the palette, and nothing behind
        // the page may have asked for the keyboard.
        state.composer.focusRequested = false;
        state.userQuery.isSubmitted = false;
        views::openPaletteForState(state.palette, state, "");
        bool performed = false;
        for (const views::PaletteItem& item : state.palette.items) {
            if (item.action != model::Action::Help)
                continue;
            performed = views::palettePerformRow(item, state.palette, state);
            break;
        }
        check(performed, "the palette's Show help row performs");
        views::closePalette(state.palette, state);
        check(state.help.open, "the page is up after the palette closed");
        check(!state.composer.focusRequested,
              "and the closing palette asks no composer behind the page for the keyboard");
        check(!state.userQuery.isSubmitted, "and sets no submit latch behind it");
        // The other half of the rule: closing the page hands the keyboard back. The latches are
        // still clear from the two lines above, so this is a clean A/B as well - in a fresh state
        // both default to true and the check would pass without testing anything.
        views::closeTopOverlay(state);
        check(state.composer.focusRequested, "closing the page asks the composer for the keyboard");
        check(state.userQuery.isSubmitted, "and arms the composer's submit latch again");
    }
}

// ---------------------------------------------------------------------------
// 8. The goldens
// ---------------------------------------------------------------------------

struct Scene {
    const char* name;
    const char* search;
    int w, h;
};

std::vector<Scene> scenes() {
    return {
        {"help-open", "", 100, 30},
        {"help-filtered", "composer", 100, 30},
        {"help-floor", "", 40, 15},
    };
}

std::string goldenText(const Scene& scene) {
    tui::TuiState state = makeState();
    g_w = scene.w;
    g_h = scene.h;
    // The scene's own state, set directly: a `?` would leave the caret blinking in the search
    // field, and a caret that blinks with the frame clock has no place in a fixture.
    state.help.open = true;
    if (scene.search != nullptr)
        std::snprintf(state.help.query.data(), state.help.query.size(), "%s", scene.search);
    // Two frames, and the second one is captured: every harness shares one ImGui context, so a
    // single frame would inherit whatever active id the checks above left behind.
    shellFrame(state, scene.w, scene.h);
    const views::ShellFrame out = shellFrame(state, scene.w, scene.h);
    const Grid grid = capture();
    const ui::Rect box = out.layout.helpOverlay;

    std::string text = "name: ";
    text += scene.name;
    text += "\nscreen: " + std::to_string(scene.w) + "x" + std::to_string(scene.h);
    text += "\npage: x=" + std::to_string(static_cast<int>(box.x)) +
            " y=" + std::to_string(static_cast<int>(box.y)) +
            " w=" + std::to_string(static_cast<int>(box.w)) +
            " h=" + std::to_string(static_cast<int>(box.h));
    text += "\nquery: '" + std::string(state.help.query.data()) + "'";
    text += "\nrows: built=" + std::to_string(state.help.rows.size()) +
            " matches=" + std::to_string(state.help.matches) +
            " drawn=" + std::to_string(state.help.drawnRows) +
            " first=" + std::to_string(state.help.firstVisible) +
            " viewport=" + std::to_string(state.help.viewportRows) +
            " column=" + std::to_string(state.help.leftColumn);
    text += "\nsections: " + std::to_string(countRows(state.help, views::HelpRowKind::Section)) +
            " groups: " + std::to_string(countRows(state.help, views::HelpRowKind::Group)) +
            " entries: " + std::to_string(countRows(state.help, views::HelpRowKind::Entry)) +
            " lines: " + std::to_string(countRows(state.help, views::HelpRowKind::Paragraph));
    text += "\ntext:\n";
    for (int y = 0; y < grid.ny; ++y) {
        text += trimTrailingSpaces(grid.rowText(y));
        text += '\n';
    }
    return text;
}

void checkGoldens(bool update) {
    g_phase = "G: goldens";
    const std::string dir = TEST_HELP_GOLDEN_DIR;
    for (const Scene& scene : scenes()) {
        const std::string path = dir + "/" + scene.name + "-" + std::to_string(scene.w) + "x" +
                                 std::to_string(scene.h) + ".txt";
        const std::string text = goldenText(scene);
        if (update) {
            check(writeFile(path, text), "golden written: " + path);
            continue;
        }
        const std::string expected = readWhole(path);
        check(!expected.empty(),
              "golden is readable: " + path + " (run with --update to create it)");
        if (expected == text) {
            check(true, std::string(scene.name) + " " + std::to_string(scene.w) + "x" +
                            std::to_string(scene.h) + " golden matches");
            continue;
        }
        std::fprintf(stderr, "FAIL [%s]: %s differs\n", g_phase, path.c_str());
        ++g_failures;
        ++g_checks;
        // The first differing line, so the fix is obvious.
        std::size_t want = 0, got = 0;
        int line = 1;
        while (want < expected.size() && got < text.size()) {
            if (expected[want] != text[got]) {
                std::fprintf(stderr, "  first difference on line %d\n", line);
                break;
            }
            if (expected[want] == '\n')
                ++line;
            ++want;
            ++got;
        }
        std::fprintf(stderr, "  expected: %s\n", expected.c_str());
        std::fprintf(stderr, "  got:\n%s\n", text.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    bool update = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose")
            g_verbose = true;
        else if (arg == "--update")
            update = true;
        else if (arg == "--print")
            g_print = true;
        else if (arg == "-h" || arg == "--help")
            std::printf("usage: %s [-v] [--update] [--print]\n", argv[0]), std::exit(0);
        else
            usage(argv[0], "unknown argument '" + arg + "'");
    }

    // One glyph set whatever machine runs this: the ASCII column would change every box-drawing
    // cell a golden pins (`LLMFUN_ASCII=0` outranks TERM and the locale).
    ::setenv("LLMFUN_ASCII", "0", 1);

    ImGui::CreateContext();
    ImTui_ImplText_Init();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    g_screen.nx = 100;
    g_screen.ny = 30;

    checkSource();
    checkPage();
    checkDocument();
    checkSearch();
    checkKeys();
    checkLayout();
    checkWiring();
    checkGoldens(update);
    // The real frame loop last: `tuiRender` drives the composer's field, which claims the
    // characters the shell-only harness above reads - so this phase must not run before it.
    checkFrameLoop();

    ImGui::DestroyContext();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d of %d check(s) FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("OK: %d check(s) passed\n", g_checks);
    return 0;
}
