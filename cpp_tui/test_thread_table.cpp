// test_thread_table.cpp
//
// The table bridge in the thread (task 37, design section 6.8 / D-TABLE): a message body's
// GFM tables (a table nested in a list item included) are drawn by the text-grid table renderer
// in the TUI, and the GUI's markdown never sees the fence. The bridge's pure half
// (`ui::bridgeTables`) is pinned by `./test_table` without a frame; what is pinned *here* is
// the rendering:
//
//   1. the sources     `ui/markdown.cpp` begins no window on the table path (no child window,
//                      no `Begin(`), and the profile question is asked once
//                      (`ui/table.cpp`'s `tableBridgeEnabled()`) - the two halves that make
//                      "the TUI draws a grid, the GUI does not" one decision.
//   2. the grid        a message whose body holds a table renders as a grid: the header's cells
//                      on one row inside borders, the body's cells aligned, and no markdown
//                      source row anywhere.
//   3. no table        a body without a table renders exactly as before - no border glyph on any
//                      row, both paragraphs present.
//   5. a fenced table  a table inside a code fence is *not* rewritten: the fence draws its code
//                      (pipes and all) and no grid appears.
//   6. the source      `s` on the focused block shows the markdown (`| Tool | ...`) instead of
//      toggle          the grid, and `s` again brings the grid back - the state is the block's
//                      `UiState` flag, and what the renderer sees is the body's text.
//   7. never blank     a `llmfun-table` fence whose content is not a table (a model that wrote
//                      the magic language itself) draws its source instead of nothing.
//   8. the declines   a verbatim TOOL row and an indented code block offer no `s`.
//   9. the nested     a table four columns under a list item is a real table: the grid is drawn,
//                      the paragraph after it stays prose (the fence is written at the item's
//                      column), and `s` shows its markdown.
//  10. the goldens    the three scenes above as text+inks screens, one per size the case uses.
//
// The grid is the real text backend (the `TScreen` pattern of `test_session_filter_smoke.cpp`)
// driven through the real `tuiRender()`, so a golden is what the terminal would show.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = usage error.
//
//   ./test_thread_table [-v] [--update] [--dump]

#include "imtui/imtui.h" // imgui + ImTui_ImplText_*

#include "tui.h" // llmfun::tui: TuiState, tuiRender, the markdown text-mode lifecycle

#include "model/block.h"
#include "model/ui_state.h"
#include "store/thread_store.h"
#include "ui/markdown.h"
#include "ui/table.h"

#include "imgui/imgui.h"

#include <array>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tui = llmfun::tui;
namespace ui = llmfun::tui::ui;
namespace model = llmfun::tui::model;
namespace store = llmfun::tui::store;

namespace {

int g_checks = 0;
int g_failures = 0;
bool g_verbose = false;
bool g_update = false;
bool g_dump = false;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) {
        if (g_verbose)
            std::printf("ok   %s\n", what.c_str());
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

[[noreturn]] void usage(const char* argv0, const char* problem) {
    std::fprintf(stderr, "usage: %s [-v] [--update] [--dump]   (%s)\n", argv0, problem);
    std::exit(2);
}

// ---------------------------------------------------------------------------
// The cell grid the text backend writes
// ---------------------------------------------------------------------------

ImTui::TScreen g_screen;

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

// The box-drawing glyphs the table renderer draws with (ui/table.cpp's table), as code points.
constexpr uint32_t kTopLeft = 0x250C;     // ┌
constexpr uint32_t kTopRight = 0x2510;    // ┐
constexpr uint32_t kBottomLeft = 0x2514;  // └
constexpr uint32_t kBottomRight = 0x2518; // ┘
constexpr uint32_t kHeaderLeft = 0x251C;  // ├
constexpr uint32_t kVertical = 0x2502;    // │

struct Grid {
    int nx = 0;
    int ny = 0;
    std::vector<uint32_t> ch;
    std::vector<uint8_t> fg;
    std::vector<uint8_t> bg;

    uint32_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return ch[static_cast<std::size_t>(y) * nx + x];
    }
    uint8_t fgAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return fg[static_cast<std::size_t>(y) * nx + x];
    }
    uint8_t bgAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return bg[static_cast<std::size_t>(y) * nx + x];
    }
    bool untouched(int x, int y) const { return at(x, y) == 0 && bgAt(x, y) == 0; }

    std::string rowText(int y) const {
        std::string text;
        for (int x = 0; x < nx; ++x) {
            const uint32_t cp = at(x, y);
            text += cp == 0 ? std::string(" ") : utf8Of(cp);
        }
        return text;
    }

    bool rowContains(int y, const std::string& needle) const {
        return rowText(y).find(needle) != std::string::npos;
    }
    /// Any row contains `needle`.
    bool contains(const std::string& needle) const {
        for (int y = 0; y < ny; ++y)
            if (rowContains(y, needle))
                return true;
        return false;
    }
    /// How many cells hold `cp`.
    int countCell(uint32_t cp) const {
        int count = 0;
        for (uint32_t cell : ch)
            count += cell == cp ? 1 : 0;
        return count;
    }
    /// The rows that hold text (for a failure dump).
    std::string dump() const {
        std::string text;
        for (int y = 0; y < ny; ++y) {
            std::string row;
            for (int x = 0; x < nx; ++x) {
                const uint32_t cp = at(x, y);
                row += (cp >= 32 && cp < 127) ? std::string(1, static_cast<char>(cp))
                                              : (cp == 0 ? std::string(" ") : std::string("."));
            }
            while (!row.empty() && row.back() == ' ')
                row.pop_back();
            text += row + "\n";
        }
        return text;
    }
};

Grid capture() {
    Grid grid;
    grid.nx = g_screen.nx;
    grid.ny = g_screen.ny;
    const std::size_t cells = static_cast<std::size_t>(grid.nx) * grid.ny;
    grid.ch.resize(cells);
    grid.fg.resize(cells);
    grid.bg.resize(cells);
    for (std::size_t i = 0; i < cells; ++i) {
        grid.ch[i] = g_screen.data[i].ch;
        grid.fg[i] = g_screen.data[i].fg;
        grid.bg[i] = g_screen.data[i].bg;
    }
    return grid;
}

/// One frame of the real app on the text backend, with an optional character queue.
Grid frame(tui::TuiState& state, const std::function<void(ImGuiIO&)>& inject = nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    if (inject)
        inject(io);
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    if (!tui::tuiRender(state)) {
        std::fprintf(stderr, "FAIL: tuiRender returned false (quit)\n");
        std::exit(1);
    }
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    return capture();
}

// ---------------------------------------------------------------------------
// Harness init/shutdown: the real app's order (markdown text mode BEFORE the backend init,
// which locks the font atlas), then the theme.
// ---------------------------------------------------------------------------

void harnessInit(tui::TuiState& state) {
    std::setlocale(LC_ALL, "");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    tui::initMarkdownTextMode(state);
    ImTui_ImplText_Init();
    tui::applyUiTheme();
    tui::finishMarkdownTextMode(state);
    ImGui::GetIO().DisplaySize = ImVec2(100.0f, 30.0f);
}

void harnessShutdown(tui::TuiState& state) {
    tui::destroyMarkdown(state);
    ImTui_ImplText_Shutdown();
    ImGui::DestroyContext();
}

/// A fresh thread with one message on a 100x30 grid.
void setup(tui::TuiState& state, const char* body,
           tui::ChatMessageType type = tui::ChatMessageType::FinalAnswer) {
    tui::tuiClearOutput(state);
    ImGui::GetIO().DisplaySize = ImVec2(100.0f, 30.0f);
    tui::ChatMessage m;
    m.summary = "Answer";
    m.text = body;
    m.type = type;
    // Monotonic id exactly like the C API: the render-group cache is keyed by message ids with
    // a (0, 0) sentinel, so an id-0 message would be "unchanged" and never grouped.
    m.id = state.chat.outputLineNextId++;
    tui::tuiAddOutputLine(state, m);
}

/// The block the one message of `setup()` is drawn as (its stable `BlockId`), so the `s` case
/// can name the focus the way a click would instead of guessing ids.
model::BlockId firstBlock(tui::TuiState& state) {
    const store::GroupView group = state.threadAdapter.groupAt(0);
    const model::BlockDesc block = state.threadAdapter.blockAt(0, 0);
    model::BlockId id = block.id();
    if (!id.valid())
        id.group = group.id;
    return id;
}

/// The `b`-th block of group `k` (a work group's message is block 1: block 0 is its header).
model::BlockId blockIdAt(tui::TuiState& state, std::size_t k, std::size_t b) {
    const model::BlockDesc block = state.threadAdapter.blockAt(k, b);
    return block.id();
}

// ---------------------------------------------------------------------------
// The goldens: the same text+inks format test_table and test_primitives write
// ---------------------------------------------------------------------------

std::string goldenText(const char* name, const Grid& grid) {
    std::string text = "name: ";
    text += name;
    text += "\nscreen: ";
    text += std::to_string(grid.nx);
    text += "x";
    text += std::to_string(grid.ny);
    text += "\ntext:\n";
    std::vector<std::string> rows(static_cast<std::size_t>(grid.ny));
    for (int y = 0; y < grid.ny; ++y) {
        std::string row = grid.rowText(y);
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
        rows[static_cast<std::size_t>(y)] = row;
    }
    // Trailing empty rows are not part of what a screen says.
    int kept = grid.ny;
    while (kept > 0 && rows[static_cast<std::size_t>(kept - 1)].empty())
        --kept;
    for (int y = 0; y < kept; ++y) {
        text += rows[static_cast<std::size_t>(y)];
        text += '\n';
    }
    text += "inks:\n";
    for (int y = 0; y < kept; ++y) {
        std::string runs;
        int start = -1;
        int index = -1;
        for (int x = 0; x <= grid.nx; ++x) {
            const int value =
                x < grid.nx && grid.at(x, y) != 0 ? static_cast<int>(grid.fgAt(x, y)) : -1;
            const bool same = start >= 0 && value == index;
            if (start >= 0 && !same) {
                runs += (runs.empty() ? "" : ", ") + std::to_string(start) + "-" +
                        std::to_string(x - 1) + "=" + std::to_string(index);
                start = -1;
            }
            if (value >= 0 && start < 0) {
                start = x;
                index = value;
            }
        }
        text += runs + "\n";
    }
    return text;
}

void checkGolden(const char* name, const std::string& text, const Grid& grid) {
    const std::string path = std::string(TEST_THREAD_TABLE_GOLDEN_DIR) + "/" + name + ".txt";
    if (g_update) {
        std::ofstream out(path);
        out << text;
        out.close();
        check(out.good(), std::string("golden written: ") + path);
        return;
    }
    std::ifstream in(path);
    check(in.good(), std::string("golden is readable: ") + path + " (run with --update)");
    if (!in.good())
        return;
    const std::string want((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(want == text, std::string("the ") + name + " golden matches");
    if (want != text && g_verbose)
        std::printf("--- %s ---\n%s\n", name, grid.dump().c_str());
}

// ---------------------------------------------------------------------------
// 1. The sources: no child window on the table path, one profile question in the bridge
// ---------------------------------------------------------------------------

std::string readWhole(const char* path) {
    std::ifstream in(path);
    if (!in.good())
        return std::string();
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void checkSources() {
    const std::string markdown = readWhole(TEST_THREAD_TABLE_MARKDOWN_SOURCE);
    check(!markdown.empty(), "ui/markdown.cpp is readable (design 6.8's renderer lives there)");
    // The fenced renderer draws into the current window's draw list and advances the flow by
    // hand. A child window is what rich_md's own code block uses, and what the table must not:
    // it carries a 4-row minimum and a filled background, neither of which a grid table has.
    check(markdown.find("BeginChild") == std::string::npos,
          "ui/markdown.cpp begins no child window (the table is not one)");
    check(markdown.find("RegisterFencedBlockRenderer") != std::string::npos,
          "ui/markdown.cpp registers the fenced block renderers");
    check(markdown.find("kTableFenceLanguage") != std::string::npos &&
              markdown.find("kTableSourceFenceLanguage") != std::string::npos,
          "both bridge languages are registered from the one constant (no orphaned fence)");
    check(markdown.find("renderTable") != std::string::npos,
          "the grid is drawn by ui/table.* (never a second renderer)");

    const std::string store = readWhole(TEST_THREAD_TABLE_STORE_SOURCE);
    check(!store.empty(), "store/thread_store.cpp is readable");
    // The store applies the bridge in its one body renderer, through the seam that takes the
    // profile it already measured - never the pure transform, so the render and the `s`
    // predicate cannot ask a different question.
    check(store.find("ui::bridgeBodyForProfile") != std::string::npos &&
              store.find("ui::bridgeTables") == std::string::npos,
          "the body renderer goes through the bridge's seam, never the pure transform");
    const std::string table = readWhole(TEST_THREAD_TABLE_UI_SOURCE);
    check(!table.empty(), "ui/table.cpp is readable");
    check(table.find("metricsFor") == std::string::npos,
          "the bridge reads no profile itself: ui/ may not ask which backend is live");
}

// ---------------------------------------------------------------------------
// 2. The grid, in the thread's block rows
// ---------------------------------------------------------------------------

const char* kTableBody = "Here is what the run did.\n"
                         "\n"
                         "| Tool | Calls | Time |\n"
                         "|------|------:|-----:|\n"
                         "| read_file | 3 | 240ms |\n"
                         "| executeCommand | 1 | 9.1s |\n"
                         "\n"
                         "That is all.\n";

/// The scene's own body text (not the shell's): every table row and every paragraph.
const char* kTableNotesBody = "Here is what the run did.\n"
                              "\n"
                              "Nothing tabular in this one.\n"
                              "\n"
                              "That is all.\n";

void checkWorkbenchGrid() {
    tui::TuiState state;
    state.iniFilename = "";
    setup(state, kTableBody);
    Grid grid;
    for (int i = 0; i < 3; ++i)
        grid = frame(state);

    if (g_dump)
        std::printf("--- workbench grid ---\n%s", grid.dump().c_str());
    check(grid.countCell(kTopLeft) >= 1, "the table's top-left corner is drawn");
    check(grid.countCell(kBottomRight) >= 1, "the table's bottom-right corner is drawn");
    check(grid.countCell(kVertical) >= 4, "the column borders are drawn");
    // The header's three cells on one row, and a body row's numbers right-aligned.
    bool headerRow = false;
    bool bodyRow = false;
    for (int y = 0; y < grid.ny; ++y) {
        if (grid.rowContains(y, "Tool") && grid.rowContains(y, "Calls") &&
            grid.rowContains(y, "Time"))
            headerRow = true;
        if (grid.rowContains(y, "read_file") && grid.rowContains(y, "240ms"))
            bodyRow = true;
    }
    check(headerRow, "the header cells render on one row (the grid, not the source)");
    check(bodyRow, "a body row renders its cells");
    check(!grid.contains("| Tool"), "the markdown source row is not drawn");
    check(!grid.contains("|------|"), "the delimiter row is not drawn");

    const std::string golden = goldenText("thread-table-grid-100x30", grid);
    checkGolden("thread-table-grid-100x30", golden, grid);
}

// ---------------------------------------------------------------------------
// 3. A body without a table: nothing changes (no border glyph, both paragraphs)
// ---------------------------------------------------------------------------

void checkNoTable() {
    tui::TuiState state;
    state.iniFilename = "";
    setup(state, kTableNotesBody);
    Grid grid;
    for (int i = 0; i < 3; ++i)
        grid = frame(state);
    if (g_dump)
        std::printf("--- notes grid ---\n%s", grid.dump().c_str());
    check(grid.countCell(kTopLeft) == 0, "a body with no table draws no border");
    check(grid.countCell(kVertical) == 0, "a body with no table draws no column border");
    check(grid.contains("Here is what the run did."), "the first paragraph renders");
    check(grid.contains("Nothing tabular in this one."), "the middle paragraph renders");
    check(grid.contains("That is all."), "the last paragraph renders");

    const std::string golden = goldenText("thread-table-notes-100x30", grid);
    checkGolden("thread-table-notes-100x30", golden, grid);
}

// ---------------------------------------------------------------------------
// 4. A table inside a code fence is code, not a table
// ---------------------------------------------------------------------------

void checkFencedTable() {
    tui::TuiState state;
    state.iniFilename = "";
    const char* body = "An example:\n"
                       "\n"
                       "```\n"
                       "| a | b |\n"
                       "|---|---|\n"
                       "| 1 | 2 |\n"
                       "```\n";
    setup(state, body);
    Grid grid;
    for (int i = 0; i < 3; ++i)
        grid = frame(state);
    if (g_dump)
        std::printf("--- fenced grid ---\n%s", grid.dump().c_str());
    check(grid.countCell(kTopLeft) == 0, "a fenced table is not bridged into a grid");
    check(grid.countCell(kVertical) == 0, "no column border for a fenced table");
    check(grid.contains("| a | b |"), "the fence's own code is drawn verbatim");
    check(grid.contains("|---|---|"), "the delimiter line is drawn verbatim");
}

// ---------------------------------------------------------------------------
// 5. `s` shows the source, `s` again shows the grid
// ---------------------------------------------------------------------------

void checkSourceToggle() {
    tui::TuiState state;
    state.iniFilename = "";
    setup(state, kTableBody);
    Grid grid;
    for (int i = 0; i < 2; ++i)
        grid = frame(state);
    check(grid.countCell(kTopLeft) >= 1, "the grid is up before the toggle");

    // The focus is one BlockId, and this is the one the single message drew as: the same
    // identity a click on the row would set.
    const model::BlockId id = firstBlock(state);
    check(id.valid(), "the message drew exactly one block to focus");
    state.ui.thread.focus = id;

    // The composer holds the keyboard at startup, and `ui::takeCharacter` refuses while a text
    // field owns it (the plain-key rule: an `s` typed into a message must stay typing). `Esc`
    // on an empty draft with nothing running is the chain's blur step (D-ESC), which is the
    // path a user takes to the thread's keys - so the harness presses it rather than reaching
    // around the guard.
    frame(state, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, true); });
    frame(state, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, false); });
    for (int i = 0; i < 3 && ImGui::GetIO().WantTextInput; ++i)
        frame(state);
    check(!ImGui::GetIO().WantTextInput, "premise: the composer released the keyboard");

    // `s`, the printable key: a character on the grid (plan/key_gate.md 4f), so the harness
    // queues the character the backend would deliver.
    grid = frame(state, [](ImGuiIO& io) { io.AddInputCharactersUTF8("s"); });
    grid = frame(state);
    if (g_dump)
        std::printf("--- source grid ---\n%s", grid.dump().c_str());
    check(grid.countCell(kTopLeft) == 0, "`s` replaces the grid with its markdown");
    check(grid.contains("| Tool | Calls | Time |"), "the source row is the markdown's own");
    check(grid.contains("| read_file | 3 | 240ms |"), "a source body row is drawn verbatim");
    check(state.ui.thread.tableSource.count(id) == 1, "the block's source flag is set");

    const std::string golden = goldenText("thread-table-source-100x30", grid);
    checkGolden("thread-table-source-100x30", golden, grid);

    grid = frame(state, [](ImGuiIO& io) { io.AddInputCharactersUTF8("s"); });
    grid = frame(state);
    check(grid.countCell(kTopLeft) >= 1, "`s` again brings the grid back");
    check(state.ui.thread.tableSource.count(id) == 0, "and clears the flag");
}

// ---------------------------------------------------------------------------
// 6. A fence that does not parse draws its source (never blank)
// ---------------------------------------------------------------------------

void checkBogusFence() {
    tui::TuiState state;
    state.iniFilename = "";
    // A model that writes the bridge's own language: the transform skips fenced code, so this
    // reaches the renderer as a `llmfun-table` fence whose content is not a table.
    const char* body = "Hand-written:\n"
                       "\n"
                       "```llmfun-table\n"
                       "not a table at all\n"
                       "```\n";
    setup(state, body);
    Grid grid;
    for (int i = 0; i < 3; ++i)
        grid = frame(state);
    if (g_dump)
        std::printf("--- bogus grid ---\n%s", grid.dump().c_str());
    check(grid.countCell(kTopLeft) == 0, "an unparseable fence draws no grid");
    check(grid.contains("not a table at all"),
          "an unparseable fence draws its source, never blank");
}

// ---------------------------------------------------------------------------
// 7. What `s` must *not* offer: a verbatim row, and an indented code block
// ---------------------------------------------------------------------------
//
// Both are cases where a table-like body exists but nothing would change on screen, so the
// toggle must not set a flag no renderer reads: `blockHasTable()` asks the bridge itself.

void checkSourceToggleDeclines() {
    // A tool transcript is drawn verbatim (never markdown), so a fence in it would be printed
    // literally - the bridge must not touch it, and `s` must not claim it can show it.
    {
        tui::TuiState state;
        state.iniFilename = "";
        setup(state, kTableBody, tui::ChatMessageType::ToolCall);
        Grid grid;
        for (int i = 0; i < 2; ++i)
            grid = frame(state);
        if (g_dump)
            std::printf("--- tool row ---\n%s", grid.dump().c_str());
        check(grid.countCell(kTopLeft) == 0, "a tool transcript is not bridged into a grid");
        check(grid.contains("| Tool | Calls | Time |"), "the transcript is drawn verbatim");

        // The tool row is the work group's second block (block 0 is the group's header).
        const model::BlockId id = blockIdAt(state, 0, 1);
        check(id.valid() && id.kind == model::BlockKind::ToolCall,
              "premise: the focused block is the tool row");
        state.ui.thread.focus = id;
        check(!state.threadAdapter.blockHasTable(0, 1),
              "a verbatim row has no table to show (the adapter says so)");

        // The composer holds the keyboard at startup: `Esc` is the chain's blur step.
        frame(state, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, true); });
        frame(state, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, false); });
        for (int i = 0; i < 3 && ImGui::GetIO().WantTextInput; ++i)
            frame(state);
        frame(state, [](ImGuiIO& io) { io.AddInputCharactersUTF8("s"); });
        grid = frame(state);
        check(!ImGui::GetIO().WantTextInput, "premise: the composer released the keyboard");
        check(state.ui.thread.tableSource.empty(),
              "`s` on a verbatim row sets no flag (the press is a no-op, not a silent bit)");
        check(grid.contains("| Tool | Calls | Time |"), "and the row still draws its transcript");
    }

    // GFM's indented code block: four columns of indent is code, so the bridge leaves it alone
    // and rich_md draws it as the code block it always drew.
    {
        tui::TuiState state;
        state.iniFilename = "";
        const char* body = "A sample:\n"
                           "\n"
                           "    | Tool | Calls |\n"
                           "    |------|------:|\n"
                           "    | read_file | 3 |\n";
        setup(state, body);
        Grid grid;
        for (int i = 0; i < 3; ++i)
            grid = frame(state);
        if (g_dump)
            std::printf("--- indented table ---\n%s", grid.dump().c_str());
        check(grid.countCell(kTopLeft) == 0, "an indented table stays an indented code block");
        check(grid.contains("| read_file | 3 |"), "and its lines are drawn verbatim");
        check(!state.threadAdapter.blockHasTable(0, 0),
              "`s` has nothing to offer on an indented code block either");
    }
}

// ---------------------------------------------------------------------------
// 8. A table nested in a list item
// ---------------------------------------------------------------------------
//
// GFM's indented code block is counted from the container's content column, so a table four
// columns under a list item is a table. It must draw the grid, the fence the bridge writes must
// keep the item alive (a paragraph after the table stays prose), and `s` must be offered.

void checkNestedTable() {
    tui::TuiState state;
    state.iniFilename = "";
    const char* body = "1. Results:\n"
                       "\n"
                       "    | Tool | Calls |\n"
                       "    |------|------:|\n"
                       "    | read_file | 3 |\n"
                       "\n"
                       "    After the table.\n";
    setup(state, body);
    Grid grid;
    for (int i = 0; i < 3; ++i)
        grid = frame(state);
    if (g_dump)
        std::printf("--- nested table ---\n%s", grid.dump().c_str());
    check(grid.countCell(kTopLeft) >= 1, "a table four columns under a list item draws the grid");
    check(grid.contains("After the table."), "the paragraph after the table is still drawn");

    const model::BlockId id = firstBlock(state);
    check(id.valid(), "the nested table drew a block to focus");
    state.ui.thread.focus = id;
    check(state.threadAdapter.blockHasTable(0, 0), "`s` is offered on the nested table");

    // The composer holds the keyboard at startup: `Esc` is the chain's blur step.
    frame(state, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, true); });
    frame(state, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, false); });
    for (int i = 0; i < 3 && ImGui::GetIO().WantTextInput; ++i)
        frame(state);
    check(!ImGui::GetIO().WantTextInput, "premise: the composer released the keyboard");
    grid = frame(state, [](ImGuiIO& io) { io.AddInputCharactersUTF8("s"); });
    grid = frame(state);
    check(grid.countCell(kTopLeft) == 0, "`s` replaces the nested grid with its markdown");
    check(grid.contains("| Tool | Calls |"), "the nested table's source is drawn");
    grid = frame(state, [](ImGuiIO& io) { io.AddInputCharactersUTF8("s"); });
    grid = frame(state);
    check(grid.countCell(kTopLeft) >= 1, "`s` again brings the nested grid back");
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0)
            g_verbose = true;
        else if (std::strcmp(argv[i], "--update") == 0)
            g_update = true;
        else if (std::strcmp(argv[i], "--dump") == 0)
            g_dump = true;
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
            usage(argv[0], "print every check with -v, rewrite the goldens with --update");
        else
            usage(argv[0], "unknown argument");
    }
    // One glyph set and one colour depth for the run, whatever terminal it starts on.
    ::setenv("LLMFUN_ASCII", "0", 1);
    ::setenv("COLORTERM", "truecolor", 1);
    tui::TuiState state;
    state.iniFilename = "";
    harnessInit(state);
    frame(state); // warm-up: the text backend builds its atlas on the first frame

    checkSources();
    checkWorkbenchGrid();
    checkNoTable();
    checkFencedTable();
    checkSourceToggle();
    checkBogusFence();
    checkSourceToggleDeclines();
    checkNestedTable();

    harnessShutdown(state);

    if (g_failures == 0) {
        std::printf("test_thread_table: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "test_thread_table: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
