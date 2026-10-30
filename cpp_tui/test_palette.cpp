// test_palette.cpp
//
// The palette's contract, driven through the real frame pipeline in both profiles:
//
//   * text grid (default): the same headless harness test_session_filter_smoke
//     uses - the ncurses backend replaced by an equivalent event injector, the
//     real ImTui text backend rasterising into a TScreen cell grid at 80x24.
//     Keys go through the 1.87+ event queue, so a chord is a down/release pair.
//   * `--gui`: the real GLFW/OpenGL3 backend under xvfb, with the framebuffer
//     read back through Backend::captureNextFramePpm (the task-8 seam).
//
// What is pinned:
//   1. `Ctrl+K` opens, `Esc` closes, and the overlay never moves what is under
//      it: every cell outside the overlay rect is byte-identical with the
//      palette open, and the whole grid is identical again after it closes.
//   2. Filtering re-uses session_fuzzy.h (the same matcher the rail uses) and
//      the matched bytes are highlighted - over actions, sessions and commands,
//      with the free-text row pinned last.
//   3. `Ctrl+N`/`Ctrl+P` (the grid's measured selection chords, plan/key_gate.md
//      §4c), `Enter`, `Tab` and `Shift+Tab` work - `Up`/`Down` are the pixel
//      profile's chords and are exercised in `--gui`.
//   4. The list draws viewport + one row of overscan: 5 result rows at 80x24 and
//      12 in the GUI, whichever the match count - never one row per match.
//   5. Every row does what it says (quit included: the flag is set, the frame
//      after it reports "user terminated" - the process is not left), and the
//      registry walk proves no row is a dead entry and no state-changing action
//      is missing from the table.
//   6. The `/` entry point carries the slash into the query: typing `continue`
//      then Enter submits `/continue`, not `continue`.
//   7. The goldens: unfiltered, filtered and the SESSIONS section.
//
// Exit codes: 0 = all checks pass; 1 = a check failed (the grid is dumped on
// stderr); 2 = setup error (no display, bad arguments).
//
//   ./test_palette [-v] [--update] [--print]
//   xvfb-run -s "-screen 0 1280x820x24" ./test_palette --gui --out DIR
//
// `--update` rewrites the golden fixtures; do that only for a deliberate rendering change, and
// commit the diff with it.

#include "imtui/imtui.h"

#include "imgui/imgui_internal.h"

#include "model/keymap.h"
#include "model/ui_state.h"
#include "tui.h"
#include "tui_backend.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "views/palette.h"
#include "views/thread.h" // threadWindow() - the paging arithmetic the row must agree with

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

using namespace llmfun::tui;

int g_failures = 0;
int g_frames = 0;
const char* g_phase = "startup";

// ---------------------------------------------------------------------------
// Shared assertions
// ---------------------------------------------------------------------------

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

[[noreturn]] void setupError(const std::string& what) {
    std::fprintf(stderr, "SETUP [%s]: %s\n", g_phase, what.c_str());
    std::exit(2);
}

// ---------------------------------------------------------------------------
// Text-grid harness
// ---------------------------------------------------------------------------

ImTui::TScreen g_screen;
TuiState g_state;
std::string g_clipboard;

struct Grid {
    int nx = 0, ny = 0;
    std::vector<int> ch;
    /// Cell background: the selection fill is a background-only change in the
    /// text grid, so the highlight can only be asserted from here.
    std::vector<unsigned> bg;
};

std::string utf8Of(int cp) {
    std::string s;
    if (cp < 0x80) {
        s.push_back(char(cp));
    } else if (cp < 0x800) {
        s.push_back(char(0xC0 | (cp >> 6)));
        s.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(char(0xE0 | (cp >> 12)));
        s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(char(0x80 | (cp & 0x3F)));
    }
    return s;
}

Grid grid() {
    Grid g;
    g.nx = g_screen.nx;
    g.ny = g_screen.ny;
    g.ch.resize(static_cast<std::size_t>(g.nx) * g.ny);
    g.bg.resize(static_cast<std::size_t>(g.nx) * g.ny);
    for (int i = 0; i < g.nx * g.ny; ++i) {
        g.ch[static_cast<std::size_t>(i)] = g_screen.data[i].ch;
        g.bg[static_cast<std::size_t>(i)] = g_screen.data[i].bg;
    }
    return g;
}

std::string rowText(const Grid& g, int y) {
    if (y < 0 || y >= g.ny)
        return "";
    std::string s;
    for (int x = 0; x < g.nx; ++x) {
        const int c = g.ch[static_cast<std::size_t>(y) * g.nx + x];
        s += (c == 0) ? " " : utf8Of(c);
    }
    return s;
}

void dumpGrid(const Grid& g) {
    std::fprintf(stderr, "grid (%dx%d):\n", g.nx, g.ny);
    for (int y = 0; y < g.ny; ++y) {
        std::string line;
        for (int x = 0; x < g.nx; ++x) {
            const int c = g.ch[static_cast<std::size_t>(y) * g.nx + x];
            line.push_back((c >= 32 && c < 127) ? char(c) : '.');
        }
        std::fprintf(stderr, "%2d |%s|\n", y, line.c_str());
    }
}

// check() for anything grid-related: a failure also dumps what was on screen.
void checkGrid(bool ok, const std::string& what) {
    check(ok, what);
    if (!ok)
        dumpGrid(grid());
}

void frame(const std::function<void(ImGuiIO&)>& inject = nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    if (inject)
        inject(io);
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    // The quit row is asserted through the flag, never by letting the harness
    // stop: a `false` here would mean an earlier scenario left the flag set.
    if (!llmfun::tui::tuiRender(g_state)) {
        std::fprintf(stderr, "FAIL [%s]: tuiRender reported 'user terminated'\n", g_phase);
        std::exit(1);
    }
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    ++g_frames;
}

void idle(int n = 1) {
    for (int i = 0; i < n; ++i)
        frame();
}

void pressKey(ImGuiKey key) {
    frame([&](ImGuiIO& io) { io.AddKeyEvent(key, true); });
    frame([&](ImGuiIO& io) { io.AddKeyEvent(key, false); });
}

// A chord with the modifier already held: one frame per edge, so the trickled
// event queue cannot split the pair across frames.
void pressCtrl(ImGuiKey key) {
    frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, true); });
    frame([&](ImGuiIO& io) { io.AddKeyEvent(key, true); });
    frame([&](ImGuiIO& io) { io.AddKeyEvent(key, false); });
    frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, false); });
}

void pressShiftTab() {
    frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Shift, true); });
    frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Tab, true); });
    frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Tab, false); });
    frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Shift, false); });
}

void typeText(const std::string& s) {
    for (char c : s) {
        frame([&](ImGuiIO& io) { io.AddInputCharacter(static_cast<unsigned int>(c)); });
        // The palette re-ranks at the top of the next frame (its geometry and the
        // list it shows must agree), so one idle frame is what makes a keystroke
        // visible - the same one-frame lag the D loop hides by rendering again
        // immediately after input.
        idle(1);
    }
}

void click(int x, int y) {
    frame([&](ImGuiIO& io) {
        io.MousePos = ImVec2(static_cast<float>(x), static_cast<float>(y));
        io.MouseDown[0] = true;
    });
    frame([&](ImGuiIO& io) {
        io.MousePos = ImVec2(static_cast<float>(x), static_cast<float>(y));
        io.MouseDown[0] = false;
    });
}

std::string input() { return std::string(g_state.userQuery.inputBuf.c_str()); }

std::string paletteQuery() {
    return std::string(g_state.palette.query.data(),
                       ::strnlen(g_state.palette.query.data(), g_state.palette.query.size() - 1));
}

/// The overlay window while it is live, nullptr once it is not drawn.
ImGuiWindow* paletteWindow() {
    ImGuiContext& ctx = *ImGui::GetCurrentContext();
    for (int i = 0; i < ctx.Windows.Size; ++i) {
        ImGuiWindow* w = ctx.Windows[i];
        if (std::strcmp(w->Name, "##palette") == 0)
            return w->LastFrameActive == ctx.FrameCount ? w : nullptr;
    }
    return nullptr;
}

ImGuiWindow* paletteListWindow() {
    ImGuiContext& ctx = *ImGui::GetCurrentContext();
    for (int i = 0; i < ctx.Windows.Size; ++i) {
        ImGuiWindow* w = ctx.Windows[i];
        if (std::string(w->Name).find("/##palette_list") != std::string::npos)
            return w->LastFrameActive == ctx.FrameCount ? w : nullptr;
    }
    return nullptr;
}

/// The overlay's cell rect [x0, x1) x [y0, y1) while it is open.
struct CellRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

/// Like findRow, but only cells inside `rect` count: the rail and the thread keep
/// their own text to the left/right of a centred overlay, and a palette
/// assertion must not read the shell behind it.
int findRowIn(const Grid& g, const std::string& needle, const CellRect& rect) {
    for (int y = std::max(0, rect.y0); y < std::min(g.ny, rect.y1); ++y) {
        std::string text;
        for (int x = std::max(0, rect.x0); x < std::min(g.nx, rect.x1); ++x) {
            const int c = g.ch[static_cast<std::size_t>(y) * g.nx + x];
            text += (c == 0) ? " " : utf8Of(c);
        }
        if (text.find(needle) != std::string::npos)
            return y;
    }
    return -1;
}

CellRect paletteRect() {
    ImGuiWindow* w = paletteWindow();
    if (w == nullptr)
        setupError("the palette window is not on screen");
    CellRect r;
    r.x0 = static_cast<int>(std::lround(w->Pos.x));
    r.y0 = static_cast<int>(std::lround(w->Pos.y));
    r.x1 = static_cast<int>(std::lround(w->Pos.x + w->Size.x));
    r.y1 = static_cast<int>(std::lround(w->Pos.y + w->Size.y));
    return r;
}

/// The whole cell grid as a rect: for assertions about the *shell* (the rail),
/// which is exactly what an overlay must leave alone.
CellRect wholeGrid() {
    CellRect r;
    r.x1 = g_screen.nx;
    r.y1 = g_screen.ny;
    return r;
}

/// True when every cell outside `rect` is identical between the two frames.
bool equalOutside(const Grid& a, const Grid& b, const CellRect& rect) {
    if (a.nx != b.nx || a.ny != b.ny)
        return false;
    for (int y = 0; y < a.ny; ++y)
        for (int x = 0; x < a.nx; ++x) {
            if (rect.contains(x, y))
                continue;
            const std::size_t i = static_cast<std::size_t>(y) * a.nx + x;
            if (a.ch[i] != b.ch[i])
                return false;
        }
    return true;
}

/// equalOutside, but listing the cells that moved (a failure here means the
/// overlay changed something it does not own, and *which* cell says what).
void checkEqualOutside(const Grid& a, const Grid& b, const CellRect& rect,
                       const std::string& what) {
    const bool ok = equalOutside(a, b, rect);
    check(ok, what);
    if (ok)
        return;
    if (a.nx != b.nx || a.ny != b.ny) {
        std::fprintf(stderr, "  grid size changed: %dx%d -> %dx%d\n", a.nx, a.ny, b.nx, b.ny);
        return;
    }
    int shown = 0;
    for (int y = 0; y < a.ny && shown < 12; ++y)
        for (int x = 0; x < a.nx && shown < 12; ++x) {
            if (rect.contains(x, y))
                continue;
            const std::size_t i = static_cast<std::size_t>(y) * a.nx + x;
            if (a.ch[i] == b.ch[i])
                continue;
            std::fprintf(stderr, "  cell (%2d,%2d): '%s' -> '%s'\n", x, y,
                         (a.ch[i] == 0 ? " " : utf8Of(a.ch[i]).c_str()),
                         (b.ch[i] == 0 ? " " : utf8Of(b.ch[i]).c_str()));
            ++shown;
        }
    dumpGrid(a);
    dumpGrid(b);
}

bool equalInside(const Grid& a, const Grid& b, const CellRect& rect) {
    for (int y = rect.y0; y < rect.y1; ++y)
        for (int x = rect.x0; x < rect.x1; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * a.nx + x;
            if (a.ch[i] != b.ch[i])
                return true; // some cell changed
        }
    return false;
}

bool equalGrids(const Grid& a, const Grid& b) {
    return a.nx == b.nx && a.ny == b.ny && a.ch == b.ch;
}

/// The text of one row inside `rect`, trimmed of trailing spaces.
std::string rowTextIn(const Grid& g, int y, const CellRect& rect) {
    std::string t;
    for (int x = std::max(0, rect.x0); x < std::min(g.nx, rect.x1); ++x) {
        const int c = g.ch[static_cast<std::size_t>(y) * g.nx + x];
        t += (c == 0) ? " " : utf8Of(c);
    }
    while (!t.empty() && t.back() == ' ')
        t.pop_back();
    return t;
}

/// A result row is the one that ends in a section tag; chrome rows (title, query,
/// hint) carry none.
bool isResultRowText(const std::string& text) {
    for (const char* tag : {"ACTIONS", "SESSIONS", "COMMANDS", "DEVELOPER"}) {
        const std::size_t len = std::strlen(tag);
        if (text.size() >= len && text.compare(text.size() - len, len, tag) == 0)
            return true;
    }
    return text.size() >= 3 && text.compare(text.size() - 3, 3, "ASK") == 0;
}

/// Result rows inside the overlay's rect (the rail's own rows sit outside it).
int countResultRows(const Grid& g, const CellRect& rect) {
    int n = 0;
    for (int y = std::max(0, rect.y0); y < std::min(g.ny, rect.y1); ++y)
        if (isResultRowText(rowTextIn(g, y, rect)))
            ++n;
    return n;
}

/// The drawn selection must follow `selected`: exactly one result row carries a
/// fill, and it is the row Enter would run. This is the regression guard for the
/// slot-vs-row-index defect the first revision shipped - the highlight was drawn
/// on `selected - firstVisible`, so after the list scrolled it marked a
/// different row than the one activation would run (and after `Ctrl+P` it marked
/// the row *after* it).
void checkSelectionHighlight(const CellRect& rect, const std::string& what) {
    const Grid g = grid();
    const views::PaletteState& p = g_state.palette;
    if (p.visible.empty()) {
        check(false, what + " (nothing to select)");
        return;
    }
    const views::PaletteItem& sel =
        p.items[static_cast<std::size_t>(p.visible[static_cast<std::size_t>(p.selected)])];

    struct Row {
        int y;
        unsigned bg;
    };
    std::vector<Row> rows;
    for (int y = std::max(0, rect.y0); y < std::min(g.ny, rect.y1); ++y) {
        if (!isResultRowText(rowTextIn(g, y, rect)))
            continue;
        const int x = std::min(g.nx - 1, std::max(0, rect.x0) + 2);
        rows.push_back({y, g.bg[static_cast<std::size_t>(y) * g.nx + x]});
    }
    if (rows.empty()) {
        check(false, what + " (no result rows drawn)");
        return;
    }
    // The surface of the unselected rows is whatever most of them carry.
    unsigned plain = rows.front().bg;
    int best = 0;
    for (const Row& a : rows) {
        int n = 0;
        for (const Row& b : rows)
            if (b.bg == a.bg)
                ++n;
        if (n > best) {
            best = n;
            plain = a.bg;
        }
    }
    int highlighted = 0;
    bool onSelectedRow = false;
    for (const Row& r : rows) {
        if (r.bg == plain)
            continue;
        ++highlighted;
        if (rowTextIn(g, r.y, rect).find(sel.label.substr(0, 10)) != std::string::npos)
            onSelectedRow = true;
    }
    check(highlighted == 1 && onSelectedRow,
          what + " (rows with a selection fill: " + std::to_string(highlighted) +
              ", on the selected row: " + (onSelectedRow ? "yes" : "no") + ")");
}

SessionEntry mkSession(const char* id, const char* title, const char* preview, int count,
                       bool active = false) {
    SessionEntry e;
    e.id = id;
    e.title = title;
    e.preview = preview;
    e.messageCount = static_cast<std::size_t>(count);
    e.isActive = active;
    return e;
}

void seedThread() {
    g_state.chat.outputLines.clear();
    g_state.chat.outputLineOpen.clear();
    g_state.chat.expansionDefaultThroughId = 0;
    g_state.chat.outputLineNextId = 1;
    // A stream from a previous scenario is state too: it is the *newest* answer as far as the
    // copy row is concerned, so a scenario that starts "with no answer" has to lose it.
    g_state.chat.hasStreamMsg = false;
    g_state.chat.streamMsg = ChatMessage{};
    g_state.chat.outputLines.push_back(ChatMessage{"[1] show me the palette", "show me the palette",
                                                   "", ChatMessageType::User,
                                                   g_state.chat.outputLineNextId++});
    g_state.chat.outputLines.push_back(ChatMessage{"[2] the palette is open", "final answer body",
                                                   "", ChatMessageType::FinalAnswer,
                                                   g_state.chat.outputLineNextId++});
}

/// Fresh state for one scenario: three sessions, one answer, composer empty.
void resetClean() {
    g_state.palette = views::PaletteState{};
    g_state.ui.rail = model::RailState{};
    g_state.sessionPanel.actions.clear();
    g_state.sessionPanel.activeId = "s001-0";
    g_state.sessionPanel.sessions = {
        mkSession("s001-0", "Alpha refactor", "", 3, true),
        mkSession("s002-1", "Beta deploy", "deploy the beta build", 5),
        mkSession("s003-2", "Gamma debug", "", 1),
    };
    g_state.userQuery = UserQueryState{};
    g_state.readyStatus = true;
    g_state.cancelPending = false;
    g_state.cancelRequested = false;
    g_state.quitRequested = false;
    g_state.quitConfirmArmed = false;
    g_state.escapeConsumed = false;
    g_state.popupOpenLastFrame = false;
    g_state.offline = false;
    g_state.autoScroll = true;
    g_clipboard.clear();
    seedThread();
    // The scenarios type into the composer, so it has to own the keyboard: a state reset is not a
    // frame, and the previous scenario may have left the keyboard in an overlay (or nowhere).
    g_state.composer.focusRequested = true;
    idle(2);
}

void openPalette() {
    pressCtrl(ImGuiKey_K);
    if (!g_state.palette.open)
        setupError("Ctrl+K did not open the palette");
    idle(1); // let the query field take the keyboard
}

/// One row of the open palette by its stable id.
const views::PaletteItem* paletteItem(const char* id) {
    for (const auto& item : g_state.palette.items)
        if (std::strcmp(item.id, id) == 0)
            return &item;
    return nullptr;
}
/// Groups the thread's window hides for this state, by the thread's *own* arithmetic.
std::size_t threadHidden(const TuiState& state) {
    return views::threadWindow(state.threadAdapter.storedGroupCount(), state.ui.thread.pageGroups,
                               state.ui.earlierGroups)
        .hidden;
}

/// Replace the thread with `count` user groups (one group each) and let the frame settle.
void seedGroups(int count) {
    g_state.chat.outputLines.clear();
    g_state.chat.outputLineOpen.clear();
    g_state.chat.openWorkGroups.clear();
    g_state.chat.expansionDefaultThroughId = 0;
    g_state.chat.outputLineNextId = 1;
    g_state.chat.hasStreamMsg = false;
    g_state.chat.streamMsg = ChatMessage{};
    for (int i = 0; i < count; ++i) {
        const std::string text = "question " + std::to_string(i);
        tuiAddOutputLine(g_state,
                         ChatMessage{"[" + std::to_string(i) + "] " + text, text, "",
                                     ChatMessageType::User, g_state.chat.outputLineNextId++});
    }
    idle(2);
}

/// A live answer: what a stream looks like to the adapter (the store's flag and the message).
void startStream() {
    g_state.chat.streamMsg =
        ChatMessage{"", "the answer is arriving", "", ChatMessageType::Assistant, 0};
    g_state.chat.hasStreamMsg = true;
    idle(2);
}

// S17: the `Load earlier` row pages the *stored* groups (task 38, review finding 1).
//
// The thread draws the live answer after the bounded window and budgets for
// `storedGroupCount()`, so the palette's row has to use the same number. With `groupCount()` a
// live answer counted as a pageable group: exactly one window of stored groups (nothing hidden)
// plus a stream made the row enabled and pressing it paged nothing.
void scenarioLoadEarlierWindow() {
    g_phase = "S17 load earlier window";
    resetClean();
    // The thread only pages what the layout left out of the window (`threadHiddenGroups()`
    // answers what is hidden, and the palette's row reads the same number).
    idle(2);
    const int window = std::max(1, g_state.ui.thread.pageGroups);
    check(g_state.threadAdapter.groupCount() == 2, "S17: the seeded thread is two groups");

    // Exactly one window of stored groups: nothing older to show.
    seedGroups(window);
    openPalette();
    const views::PaletteItem* row = paletteItem("actions.loadEarlier");
    check(row != nullptr, "S17: the palette has a `Load earlier` row");
    check(window == g_state.ui.thread.pageGroups, "S17: the window is stable across the reseed");
    check(threadHidden(g_state) == 0, "S17: one window of groups hides nothing");

    startStream();
    check(g_state.threadAdapter.groupCount() == static_cast<std::size_t>(window) + 1,
          "S17: the live answer is a group of its own");
    check(g_state.threadAdapter.storedGroupCount() == static_cast<std::size_t>(window),
          "S17: but it is not a stored group");
    check(threadHidden(g_state) == 0, "S17: and the thread still hides nothing");
    row = paletteItem("actions.loadEarlier");
    check(row != nullptr && !row->enabled,
          "S17: so the row is disabled while an answer streams over a full window");

    // More than a window of stored groups, still streaming: the number is the stored groups'.
    seedGroups(window + 5);
    startStream();
    const std::size_t stored = g_state.threadAdapter.storedGroupCount();
    const std::size_t hidden = threadHidden(g_state);
    check(stored == static_cast<std::size_t>(window) + 5,
          "S17: five stored groups more than the window (" + std::to_string(stored) + ")");
    check(hidden == 5,
          "S17: the thread hides exactly those five (got " + std::to_string(hidden) + ")");
    row = paletteItem("actions.loadEarlier");
    check(row != nullptr && row->enabled,
          "S17: and the row is enabled, matching the thread's own window (stored " +
              std::to_string(stored) + ", hidden " + std::to_string(hidden) + ", open " +
              std::to_string(g_state.palette.open ? 1 : 0) + ", pageGroups " +
              std::to_string(g_state.ui.thread.pageGroups) + ", enabled " +
              std::to_string(row != nullptr ? (row->enabled ? 1 : 0) : -1) + ")");
    pressKey(ImGuiKey_Escape);
}

/// The visible row (`p.visible` index) of the item with this id, -1 when the query hides it.
int visibleRowById(const char* id) {
    const views::PaletteState& p = g_state.palette;
    for (int row = 0; row < static_cast<int>(p.visible.size()); ++row) {
        const views::PaletteItem& item =
            p.items[static_cast<std::size_t>(p.visible[static_cast<std::size_t>(row)])];
        if (std::strcmp(item.id, id) == 0)
            return row;
    }
    return -1;
}

/// Move the selection onto the row with this id, with the grid's measured chords.
///
/// A scenario names a row instead of counting `Ctrl+N` presses: the table grew in task 35 (the
/// rail, the two inspector tabs, settings and the command lines), and an offset-based test would
/// have to be re-derived every time a row is added - which is the kind of test that ends up
/// asserting the wrong row.
void selectRowById(const char* id) {
    const int target = visibleRowById(id);
    if (target < 0)
        setupError(std::string("row '") + id + "' is not in the visible list");
    while (g_state.palette.selected < target)
        pressCtrl(ImGuiKey_N);
    while (g_state.palette.selected > target)
        pressCtrl(ImGuiKey_P);
    if (g_state.palette.selected != target)
        setupError(std::string("could not select row '") + id + "'");
}

int queuedActions() { return static_cast<int>(g_state.sessionPanel.actions.size()); }

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

// S1: open/close, the overlay's contents, and "it never moves what is under it".
void scenarioOpenClose() {
    g_phase = "S1 open/close";
    resetClean();
    const Grid before = grid();
    openPalette();

    const Grid open = grid();
    check(paletteWindow() != nullptr, "S1: the overlay window is on screen");
    const CellRect rect = paletteRect();
    checkGrid(findRowIn(open, "Search", rect) >= 0, "S1: the overlay is titled Search");
    // The first five rows of the ACTIONS group (the table's confidence order): the viewport shows
    // five at 80x24, and the rows below it are reachable with the selection chords or by typing.
    for (const char* label : {"New session", "Focus composer", "Toggle rail", "Toggle inspector"})
        checkGrid(findRowIn(open, label, rect) >= 0,
                  std::string("S1: action row '") + label + "' is drawn");
    checkGrid(findRowIn(open, "ALL 24", rect) >= 0,
              "S1: the title row counts every row of every section (13 actions + 7 command lines "
              "+ the Raw Log + 3 sessions)");
    checkGrid(findRowIn(open, "Esc close", rect) >= 0, "S1: the hint row lists the keys");
    checkGrid(countResultRows(open, rect) == 5, "S1: the viewport shows 5 result rows at 80x24");

    check(rect.x0 == 8 && rect.x1 == 72, "S1: the overlay is 80% of the 80 columns, centred");
    checkEqualOutside(before, open, rect, "S1: no cell outside the overlay moved when it opened");
    check(equalInside(before, open, rect), "S1: the overlay painted over the thread");

    pressKey(ImGuiKey_Escape);
    idle(2);
    check(!g_state.palette.open, "S1: Esc closes the overlay");
    checkGrid(equalGrids(before, grid()), "S1: closing restores the grid byte for byte");
}

// S2: bounded rendering - viewport + one row of overscan, whatever the matches.
void scenarioBoundedRows() {
    g_phase = "S2 bounded rows";
    resetClean();
    std::vector<SessionEntry> many;
    for (int i = 0; i < 13; ++i) {
        char id[32], title[64];
        std::snprintf(id, sizeof id, "many%02d", i);
        std::snprintf(title, sizeof title, "Many session %02d", i);
        many.push_back(mkSession(id, title, "", i, i == 0));
    }
    g_state.sessionPanel.sessions = many;
    idle(2);
    openPalette();

    const CellRect rect = paletteRect();
    checkGrid(countResultRows(grid(), rect) == 5, "S2: 25 matches still draw 5 rows at 80x24");
    ImGuiWindow* list = paletteListWindow();
    check(list != nullptr, "S2: the result list is its own child window");
    if (list != nullptr) {
        // One cell per row in the grid: 5 visible + the 1-row overscan.
        check(list->ContentSize.y >= 5.0f && list->ContentSize.y <= 6.0f,
              "S2: the list draws the viewport plus one row of overscan (" +
                  std::to_string(list->ContentSize.y) + " rows for 25 matches)");
    }
    checkGrid(findRowIn(grid(), "New session", rect) >= 0, "S2: the first row is the first action");
    checkGrid(findRowIn(grid(), "Many session 00", rect) < 0, "S2: the 6th match is not on screen");

    // Walk the selection to a session row that started below the fold. The row is named, not
    // counted: the table's length is not the contract, the window's bound is.
    selectRowById("many00");
    checkGrid(findRowIn(grid(), "New session", rect) < 0, "S2: the list scrolled - row 0 is gone");
    checkGrid(findRowIn(grid(), "Many session 00", rect) >= 0,
              "S2: the selection's row is visible");
    checkGrid(countResultRows(grid(), rect) == 5, "S2: the window still shows 5 rows");
    // The fill must follow the selection, not the drawn slot (review F1: the
    // first revision highlighted `selected - firstVisible`, i.e. the row after
    // the one Enter runs once the list has scrolled - which hid it entirely at
    // the bottom of the window).
    checkSelectionHighlight(rect, "S2: the highlight follows the selection once scrolled");
    const int sessionRow = g_state.palette.selected;
    pressCtrl(ImGuiKey_P);
    check(g_state.palette.selected == sessionRow - 1, "S2: Ctrl+P moves the selection back");
    checkSelectionHighlight(rect, "S2: the highlight follows the selection after Ctrl+P");
    check(g_state.palette.selected >= g_state.palette.firstVisible &&
              g_state.palette.selected < g_state.palette.firstVisible + 5,
          "S2: the selection always stays inside the visible window");
}

// S3: fuzzy filtering through session_fuzzy.h, with the matched bytes highlighted.
void scenarioFilter() {
    g_phase = "S3 filter";
    resetClean();
    openPalette();
    typeText("a");
    check(paletteQuery() == "a", "S3: 'a' reaches the palette's query field");
    typeText("l");
    check(paletteQuery() == "al", "S3: 'l' reaches the palette's query field too");
    typeText("p");
    check(paletteQuery() == "alp", "S3: 'p' reaches the palette's query field too");
    check(input().empty(), "S3: nothing was typed into the composer behind the overlay");

    const Grid g = grid();
    const CellRect rect = paletteRect();
    checkGrid(findRowIn(g, "Alpha refactor", rect) >= 0,
              "S3: the session matching 'alp' is listed");
    checkGrid(findRowIn(g, "Beta deploy", rect) < 0, "S3: a non-matching session is filtered out");
    checkGrid(findRowIn(g, "New session", rect) < 0, "S3: a non-matching action is filtered out");
    checkGrid(findRowIn(g, "Send \"alp\"", rect) >= 0, "S3: the free-text row is offered last");
    checkGrid(countResultRows(g, rect) == 2, "S3: two rows match (the session and the ASK row)");

    // The highlight: the matched word is over-drawn in the filter's match colour,
    // so its cells differ from the same label's cells in a non-matching row.
    const int ySession = findRowIn(g, "Alpha refactor", rect);
    const int yAsk = findRowIn(g, "Send \"alp\"", rect);
    check(ySession >= 0 && yAsk >= 0, "S3: both rows are drawn");
    if (ySession >= 0 && yAsk >= 0) {
        const int xSession = rowText(g, ySession).find("Alpha");
        const int xAsk = rowText(g, yAsk).find("Send");
        check(xSession >= 0 && xAsk >= 0, "S3: the labels start where the text says");
        if (xSession >= 0 && xAsk >= 0) {
            const unsigned char matched = g_screen.data[ySession * g.nx + xSession].fg;
            const unsigned char plain = g_screen.data[yAsk * g.nx + xAsk].fg;
            check(matched != plain, "S3: the matched bytes are highlighted (fg differs)");
        }
    }

    // Ranking puts the ASK row last whatever its score.
    check(g_state.palette.visible.size() == 2, "S3: the ranked view holds the two matches");
    if (g_state.palette.visible.size() == 2) {
        const views::PaletteItem& last =
            g_state.palette.items[static_cast<std::size_t>(g_state.palette.visible[1])];
        check(std::strcmp(last.id, "ask.freeText") == 0, "S3: the free-text row is ranked last");
    }

    // The same matcher over the command lines, from a fresh open: `/co` narrows to the commands
    // that contain it, and the free-text row keeps following them (last).
    pressKey(ImGuiKey_Escape);
    idle(1);
    openPalette();
    typeText("/co");
    check(paletteQuery() == "/co", "S3: the command query reached the field");
    check(visibleRowById("commands.continue") >= 0 && visibleRowById("commands.compact") >= 0,
          "S3: the command lines matched by '/co' are listed");
    check(visibleRowById("actions.quit") < 0, "S3: a non-matching action is filtered out");
    const int lastRow = static_cast<int>(g_state.palette.visible.size()) - 1;
    check(lastRow >= 0 &&
              std::strcmp(g_state.palette
                              .items[static_cast<std::size_t>(
                                  g_state.palette.visible[static_cast<std::size_t>(lastRow)])]
                              .id,
                          "ask.freeText") == 0,
          "S3: the free-text row follows the commands (last)");
    pressKey(ImGuiKey_Escape);
    idle(1);
}

// S11: the registry. Every row has a handler, every state-changing action has a row, and every
// action of the model's registry has a decided entry point - the three halves of D-PALETTE that a
// table can be wrong about (a dead entry, a missing entry, an action nobody can reach).

/// Undo everything a palette row can do, so the registry walk can perform them all in one state
/// without one row's effect deciding the next row's answer.
void resetRowEffects() {
    g_state.quitRequested = false;
    g_state.quitConfirmArmed = false;
    g_state.cancelPending = false;
    g_state.cancelRequested = false;
    g_state.sessionPanel.actions.clear();
    g_state.ui.rail.selectId.clear();
    g_state.ui.rail.pendingId.clear();
    g_state.ui.prefs = model::ShellPrefs{};
    g_state.ui.rawLogOpen = false;
    g_state.ui.earlierGroups = 0;
    g_state.rawLog = views::RawLogState{};
    g_state.inspector.tabRequestPending = false;
    g_state.inspector.reservedPressed = false;
    // Focus mode (task 43) is a shell mode with no surface: toggling it is the row's effect, so it
    // is reset like the preferences are.
    g_state.ui.focusMode = false;
    g_state.settingsSheet = views::SettingsState{};
    // A row can open a modal surface of its own (the help page, task 42): this walk is about the
    // handler answering, so the surface stack starts clean for the next row.
    g_state.help = views::HelpState{};
    g_state.userQuery.submitReady = false;
    g_state.userQuery.submitQuery.clear();
    g_state.userQuery.inputHistory.clear();
    g_state.userQuery.inputBuf.clear();
    g_clipboard.clear();
}

void scenarioRegistry() {
    g_phase = "S11 registry";
    resetClean();
    idle(2);

    // 1. One registry row per action, with a name and a decided reach.
    const int actionCount = static_cast<int>(model::Action::Count);
    check(static_cast<int>(model::allActions().count) == actionCount - 1,
          "S11: the action registry has one row per action (" +
              std::to_string(model::allActions().count) + " for " +
              std::to_string(actionCount - 1) + ")");
    for (int i = 1; i < actionCount; ++i) {
        const auto action = static_cast<model::Action>(i);
        const model::ActionInfo* info = model::actionInfo(action);
        check(info != nullptr && info->action == action && info->name != nullptr &&
                  info->name[0] != '\0',
              std::string("S11: action '") + model::actionName(action) + "' is in the registry");
    }

    // 2. A "Palette" reach claim is a row; a "Chord" reach claim is a binding; an "Esc step" is
    //    what `resolveEscape` answers. A new action has to pick one, and a claim that the build
    //    does not honour fails here instead of shipping as a dead key.
    //
    //    The table the walk reads is the one this build ships (every surface row enabled).
    openPalette();
    for (const model::ActionInfo& info : model::allActions()) {
        const std::string name = model::actionName(info.action);
        if (info.reach == model::ActionReach::Palette) {
            bool row = false;
            for (const views::PaletteItem& item : g_state.palette.items)
                row = row || item.action == info.action;
            check(row, "S11: the palette lists '" + name + "'");
        } else if (info.reach == model::ActionReach::Chord) {
            bool bound = false;
            for (const model::Binding& binding : model::defaultKeymap())
                bound = bound || binding.action == info.action;
            check(bound, "S11: '" + name + "' is a chord the table binds");
        } else {
            bool step = false;
            for (const bool overlay : {false, true})
                for (const bool run : {false, true})
                    for (const bool input : {false, true})
                        for (const bool focus : {false, true})
                            step = step || model::resolveEscape({overlay, run, input, focus}) ==
                                               info.action;
            check(step, "S11: '" + name + "' is a step of the Esc chain");
        }
    }

    // 3. No dead entry: every row of the table answers a handler in a state where it is enabled.
    //    Two passes, because one row's state excludes another's (the free-text row is disabled
    //    while a run is active, `Stop run` is disabled while it is not).
    for (const bool running : {false, true}) {
        g_state.palette = views::PaletteState{}; // closed, so `Ctrl+K` opens it
        g_state.readyStatus = !running;
        idle(1);
        openPalette();
        typeText("z"); // any query: the free-text row exists exactly then
        int enabled = 0, handled = 0;
        for (const views::PaletteItem& item : g_state.palette.items) {
            if (!item.enabled)
                continue;
            ++enabled;
            const bool ok = views::palettePerformRow(item, g_state.palette, g_state);
            handled += ok ? 1 : 0;
            if (!ok)
                check(false, std::string("S11: row '") + item.id + "' has no handler");
            // Undo what a row did, so the next row starts from the same state.
            resetRowEffects();
        }
        check(enabled > 0 && handled == enabled,
              "S11: every enabled row answered a handler (" + std::to_string(handled) + "/" +
                  std::to_string(enabled) + (running ? ", run active)" : ", idle)"));
        g_state.palette = views::PaletteState{};
        g_state.readyStatus = true;
        idle(1);
    }
    idle(2);
}

// S4: Tab / Shift+Tab cycle the group filter.
void scenarioGroups() {
    g_phase = "S4 groups";
    resetClean();
    openPalette();
    CellRect rect = paletteRect();
    checkGrid(findRowIn(grid(), "ALL 24", rect) >= 0,
              "S4: the title row starts on ALL with every row (13 actions + 7 commands + raw log + "
              "3 sessions)");

    pressKey(ImGuiKey_Tab);
    check(g_state.palette.groupIndex == 1, "S4: Tab moved to the ACTIONS group");
    rect = paletteRect();
    checkGrid(findRowIn(grid(), "ACTIONS 13", rect) >= 0, "S4: the title row shows ACTIONS 13");
    checkGrid(findRowIn(grid(), "Alpha refactor", rect) < 0, "S4: session rows are filtered out");

    pressKey(ImGuiKey_Tab);
    check(g_state.palette.groupIndex == 2, "S4: Tab moved to the SESSIONS group");
    rect = paletteRect();
    checkGrid(findRowIn(grid(), "SESSIONS 3", rect) >= 0, "S4: the title row shows SESSIONS 3");
    checkGrid(findRowIn(grid(), "New session", rect) < 0, "S4: action rows are filtered out");
    checkGrid(findRowIn(grid(), "Alpha refactor", rect) >= 0, "S4: the sessions are listed");

    pressKey(ImGuiKey_Tab);
    check(g_state.palette.groupIndex == 3, "S4: Tab moved to the COMMANDS group");
    rect = paletteRect();
    checkGrid(findRowIn(grid(), "COMMANDS 7", rect) >= 0, "S4: the title row shows COMMANDS 7");
    checkGrid(findRowIn(grid(), "/continue", rect) >= 0, "S4: the command lines are listed");
    checkGrid(findRowIn(grid(), "Alpha refactor", rect) < 0, "S4: no session rows here");

    pressKey(ImGuiKey_Tab);
    check(g_state.palette.groupIndex == 4, "S4: Tab moved to the DEVELOPER group");
    rect = paletteRect();
    checkGrid(findRowIn(grid(), "DEVELOPER 1", rect) >= 0, "S4: the title row shows DEVELOPER 1");
    checkGrid(findRowIn(grid(), "Toggle inspector", rect) < 0,
              "S4: the developer group holds no action rows");

    // ASK has no rows without a query, so the cycle wraps from DEVELOPER to ALL.
    pressKey(ImGuiKey_Tab);
    check(g_state.palette.groupIndex == 0, "S4: the cycle wraps to ALL after the last group");
    rect = paletteRect();
    checkGrid(findRowIn(grid(), "ALL 24", rect) >= 0, "S4: the title row is back on ALL");

    pressShiftTab();
    check(g_state.palette.groupIndex == 4, "S4: Shift+Tab steps back to DEVELOPER");
    rect = paletteRect();
    checkGrid(findRowIn(grid(), "DEVELOPER 1", rect) >= 0, "S4: the title row follows Shift+Tab");
    pressShiftTab();
    check(g_state.palette.groupIndex == 3, "S4: and once more to COMMANDS");
    rect = paletteRect();
    checkGrid(findRowIn(grid(), "COMMANDS 7", rect) >= 0, "S4: the title row follows Shift+Tab");
    pressShiftTab();
    check(g_state.palette.groupIndex == 2, "S4: and once more to SESSIONS");
}

// S5: every row does what it says. Quit included - the flag is asserted and then
// cleared, so the harness itself never stops.
void scenarioActivate() {
    g_phase = "S5 activate new session";
    resetClean();
    openPalette();
    pressKey(ImGuiKey_Enter);
    check(!g_state.palette.open, "S5a: Enter closes the palette");
    check(queuedActions() == 1 &&
              g_state.sessionPanel.actions.front().type == SessionActionType::New,
          "S5a: 'New session' queues one New action");
    g_state.sessionPanel.actions.clear();

    g_phase = "S5 activate focus composer";
    resetClean();
    openPalette();
    selectRowById("actions.focusComposer");
    pressKey(ImGuiKey_Enter);
    idle(1);
    typeText("q");
    check(input() == "q", "S5b: 'Focus composer' returns the keyboard to the composer");
    check(paletteQuery().empty(), "S5b: the palette's own field kept nothing");

    g_phase = "S5 activate copy answer";
    resetClean();
    openPalette();
    selectRowById("actions.copyLastAnswer");
    pressKey(ImGuiKey_Enter);
    check(g_clipboard == "final answer body",
          "S5c: 'Copy last answer' copies the newest answer (got '" + g_clipboard + "')");

    g_phase = "S5 activate stop run";
    resetClean();
    g_state.readyStatus = false; // a run is active
    idle(1);
    openPalette();
    selectRowById("actions.stopRun");
    pressKey(ImGuiKey_Enter);
    check(tuiTakeCancel(g_state), "S5d: 'Stop run' requests the cancel the D loop consumes");
    check(g_state.cancelRequested, "S5d: the Stopping latch is set for the composer");
    g_state.cancelRequested = false;

    g_phase = "S5 activate session switch";
    resetClean();
    openPalette();
    typeText("beta");
    pressKey(ImGuiKey_Enter);
    const bool selectQueued =
        queuedActions() == 1 &&
        g_state.sessionPanel.actions.front().type == SessionActionType::Select &&
        g_state.sessionPanel.actions.front().sessionId == "s002-1";
    check(selectQueued, "S5e: a SESSIONS row queues Select with that session's id");
    g_state.sessionPanel.actions.clear();

    g_phase = "S5 activate session switch while busy";
    resetClean();
    g_state.readyStatus = false;
    idle(1);
    openPalette();
    typeText("beta");
    pressKey(ImGuiKey_Enter);
    check(queuedActions() == 0 && g_state.ui.rail.pendingId == "s002-1",
          "S5f: while a run is active the switch parks in the rail's one slot (rail rule)");
    g_state.ui.rail.pendingId.clear();

    g_phase = "S5 activate free text";
    resetClean();
    openPalette();
    typeText("hello agent");
    selectRowById("ask.freeText");
    pressKey(ImGuiKey_Enter);
    check(!g_state.palette.open, "S5g: the free-text row closes the palette");
    check(g_state.userQuery.submitReady && g_state.userQuery.submitQuery == "hello agent",
          "S5g: the free-text row submits the query");
    check(g_state.userQuery.inputHistory.size() == 1 &&
              g_state.userQuery.inputHistory.back() == "hello agent",
          "S5g: the submitted query joins the composer's history");
    tuiResetSubmit(g_state);

    g_phase = "S5 activate quit";
    resetClean();
    openPalette();
    selectRowById("actions.quit");
    // Enter's *down* edge is the activation frame: the row sets the quit flag and
    // the frame after it is the loop's exit handshake, so the release is sent
    // once that has been consumed (frame() would fail on it otherwise).
    frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Enter, true); });
    check(g_state.quitRequested, "S5h: 'Quit' requests the exit");
    check(!g_state.palette.open, "S5h: 'Quit' closes the overlay");
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        ImTui_ImplText_NewFrame();
        ImGui::NewFrame();
        const bool running = llmfun::tui::tuiRender(g_state);
        ImGui::Render();
        ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
        ++g_frames;
        check(!running, "S5h: the frame after a quit reports 'user terminated'");
    }
    g_state.quitRequested = false;
    frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Enter, false); });
    idle(1);
    g_state.chat.outputLines.clear();
}

// S5i: the palette's Quit row shares the chord's confirmation policy. With a
// run active the first activation arms and closes the overlay; the second quits.
void scenarioQuitArmed() {
    g_phase = "S5 activate quit while running";
    resetClean();
    g_state.readyStatus = false; // a run is active
    idle(1);
    openPalette();
    selectRowById("actions.quit");
    frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Enter, true); });
    check(!g_state.quitRequested, "S5i: Quit while running does not quit on the first press");
    check(g_state.quitConfirmArmed, "S5i: it arms the shared confirmation");
    check(!g_state.palette.open, "S5i: it closes the palette");
    frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Enter, false); });
    idle(1);
    check(g_state.quitConfirmArmed, "S5i: the arming survives the palette close");
    check(!g_state.quitRequested, "S5i: arming wrote no quit request");

    openPalette();
    selectRowById("actions.quit");
    frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Enter, true); });
    check(g_state.quitRequested, "S5i: the second activation quits");
    check(!g_state.quitConfirmArmed, "S5i: quitting clears the arming");
    g_state.quitRequested = false;
    frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Enter, false); });
    idle(1);
}

// S9: an overlay owns the keyboard while it is open. The shell's frame-global
// handlers (the rail's Esc, the End-follow) must stand down, or one key press
// would act twice - inside the palette and on the region behind it.
void scenarioKeyboardOwnership() {
    g_phase = "S9 keyboard ownership";
    // The rail's search row is part of the column anatomy: at 80x24 the shell gives the rail the
    // collapsed strip, whose own policy clears a query it cannot show. Widen for the scenario.
    ImGui::GetIO().DisplaySize = ImVec2(120.0f, 36.0f);
    g_state.ui.prefs.railOpen = 1;
    resetClean();
    std::snprintf(g_state.ui.rail.filterBuf.data(), g_state.ui.rail.filterBuf.size(), "beta");
    idle(2);
    // The active session's title is also in the top bar, so the filter's evidence is the
    // rail's own list: one matching row drawn, and it is the matching one.
    checkGrid(g_state.ui.rail.drawnRows == 1 && findRowIn(grid(), "Beta deploy", wholeGrid()) >= 0,
              "S9: the rail filter is live (only the matching session is listed)");

    g_state.autoScroll = false;
    openPalette();
    pressKey(ImGuiKey_End);
    check(!g_state.autoScroll, "S9: End inside the palette does not jump the thread behind it");

    pressKey(ImGuiKey_Escape);
    idle(1);
    check(!g_state.palette.open, "S9: Esc closes the palette");
    check(std::string(g_state.ui.rail.filterBuf.data()) == "beta",
          "S9: that Esc did not also clear the rail's search (one step per press)");
    checkGrid(findRowIn(grid(), "Beta deploy", wholeGrid()) >= 0,
              "S9: the rail still filters after the overlay closed");
    // Clear the filter again so the next scenario starts from a clean rail.
    g_state.ui.rail.filterBuf.fill('\0');
    idle(1);
    ImGui::GetIO().DisplaySize = ImVec2(80.0f, 24.0f);
    g_state.ui.prefs.railOpen = -1;
    idle(1);
}

// S10: the row flags track the live state, and the free-text row refuses to
// submit while a run is active (review F3/F4).
void scenarioLiveFlags() {
    g_phase = "S10 live flags";
    resetClean();
    // Start without an answer: 'Copy last answer' must be born disabled...
    g_state.chat.outputLines.pop_back();
    idle(1);
    openPalette();
    const views::PaletteItem* copy = paletteItem("actions.copyLastAnswer");
    check(copy != nullptr && !copy->enabled,
          "S10: 'Copy last answer' is disabled while the thread has no answer");
    // ...and must become usable when one arrives *while the panel stays open*,
    // without reopening it (a snapshot-only table would stay disabled).
    tuiAddOutputLine(g_state,
                     ChatMessage{"[3] late", "a late answer", "", ChatMessageType::FinalAnswer,
                                 g_state.chat.outputLineNextId++});
    idle(1);
    check(copy != nullptr && copy->enabled,
          "S10: 'Copy last answer' turns enabled when an answer arrives");

    typeText("hi");
    const views::PaletteItem* ask = paletteItem("ask.freeText");
    check(ask != nullptr && ask->enabled, "S10: the free-text row is usable while idle");

    // A run starts: the row goes disabled, and the activation is refused even if
    // it is reached (the composer's guard, mirrored).
    g_state.readyStatus = false;
    idle(1);
    check(ask != nullptr && !ask->enabled,
          "S10: the free-text row is disabled while a run is active");
    const views::PaletteItem* stop = paletteItem("actions.stopRun");
    check(stop != nullptr && stop->enabled, "S10: 'Stop run' is enabled while a run is active");

    const int lastRow = static_cast<int>(g_state.palette.visible.size()) - 1;
    while (g_state.palette.selected < lastRow)
        pressCtrl(ImGuiKey_N);
    pressKey(ImGuiKey_Enter);
    check(!g_state.userQuery.submitReady,
          "S10: Enter on the disabled free-text row submits nothing during a run");
    check(g_state.palette.open, "S10: the palette stays open on a refused activation");

    g_state.readyStatus = true;
    g_state.cancelRequested = false;
    idle(1);
    pressKey(ImGuiKey_Escape); // the palette is still open from the run check
    idle(1);

    // Focus mode (task 43) hides the panel both tab rows act on: they go grey with their siblings,
    // and the row refuses the activation instead of closing the palette on a request the frame
    // would drop.
    g_state.ui.focusMode = true;
    openPalette();
    const views::PaletteItem* agentsRow = paletteItem("actions.inspectorAgents");
    const views::PaletteItem* activityRow = paletteItem("actions.inspectorActivity");
    check(agentsRow != nullptr && !agentsRow->enabled && activityRow != nullptr &&
              !activityRow->enabled,
          "S10: the inspector tab rows are disabled while focus mode hides the panel");
    g_state.ui.prefs.tab = model::InspectorTab::Activity;
    check(agentsRow != nullptr && !views::palettePerformRow(*agentsRow, g_state.palette, g_state) &&
              g_state.ui.prefs.tab == model::InspectorTab::Activity,
          "S10: ...and the row refuses the activation, so the tab is untouched");
    pressKey(ImGuiKey_Escape);
    idle(1);
    g_state.ui.focusMode = false;
    openPalette();
    const views::PaletteItem* agentsBack = paletteItem("actions.inspectorAgents");
    check(agentsBack != nullptr && agentsBack->enabled,
          "S10: ...and the row is enabled again once the mode is left");
    pressKey(ImGuiKey_Escape);
    idle(1);
}

// S9: there is no v4 panel to flush a pending switch. The palette answers through the
// rail, and the shell applies the answer on the next frame (the overlay layer is drawn
// after `applyRailAnswers()`), so the pick both happens and shows: the target row
// carries the pending mark and the thread shows the loading card.
void scenarioWorkbenchRail() {
    g_phase = "S9 workbench select through the rail";
    resetClean();
    idle(2);

    openPalette();
    typeText("beta");
    pressKey(ImGuiKey_Enter);
    idle(1); // the answer is applied on the frame after the overlay closed
    check(queuedActions() == 1 &&
              g_state.sessionPanel.actions.front().type == SessionActionType::Select &&
              g_state.sessionPanel.actions.front().sessionId == "s002-1",
          "S9w: the workbench queues the palette's pick through the rail");
    g_state.sessionPanel.actions.clear();

    g_phase = "S9 workbench select while busy";
    resetClean();
    g_state.readyStatus = false;
    idle(2);
    openPalette();
    typeText("beta");
    pressKey(ImGuiKey_Enter);
    idle(1);
    check(queuedActions() == 0 && g_state.ui.rail.pendingId == "s002-1",
          "S9w: while a run is active the pick parks in the rail's one slot");
    g_state.ui.rail.pendingId.clear();

    idle(2);
}

// S8: the 40x15 floor. The palette degrades to fewer rows and a narrower box,
// and it still never touches what is outside it.
// S8: the 40x15 floor. The palette degrades to fewer rows and a narrower box,
// and it still never touches what is outside it.
void scenarioFloor() {
    g_phase = "S8 40x15 floor";
    resetClean();
    const ImVec2 harnessSize = ImGui::GetIO().DisplaySize;
    ImGui::GetIO().DisplaySize = ImVec2(40.0f, 15.0f);
    // Child sizes are one frame behind the display size: let the squeezed layout
    // settle before taking the reference grid, or the comparison below measures
    // the settle instead of the overlay.
    idle(3);
    const Grid before = grid();

    pressCtrl(ImGuiKey_K);
    idle(1);
    check(g_state.palette.open, "S8: the palette opens on a 40x15 terminal");
    const CellRect rect = paletteRect();
    check(rect.x0 >= 0 && rect.y0 >= 0 && rect.x1 <= 40 && rect.y1 <= 15,
          "S8: the overlay stays inside the 40x15 display (" + std::to_string(rect.x0) + "," +
              std::to_string(rect.y0) + " - " + std::to_string(rect.x1) + "," +
              std::to_string(rect.y1) + ")");
    checkGrid(findRowIn(grid(), "Search", rect) >= 0, "S8: the title row survives at 40x15");
    const int rows = countResultRows(grid(), rect);
    checkGrid(rows >= 3 && rows <= 4,
              "S8: the list degrades to the rows that fit (" + std::to_string(rows) + ")");
    checkEqualOutside(before, grid(), rect, "S8: nothing outside the overlay moved at 40x15");

    pressKey(ImGuiKey_Escape);
    idle(1);
    check(!g_state.palette.open, "S8: Esc closes it at 40x15 too");

    ImGui::GetIO().DisplaySize = harnessSize;
    idle(2);
}

// S6: the palette is opt-in - with it closed the composer still gets the keys.
void scenarioNoRegression() {
    g_phase = "S6 keystroke regression";
    resetClean();
    typeText("ab");
    check(input() == "ab", "S6: typing goes to the composer while the palette is closed");
    check(!g_state.palette.open, "S6: nothing opened the palette by itself");
    g_state.userQuery.inputBuf.clear();
}

// S7: a click into the composer closes the overlay instead of leaving a palette
// that no longer owns the keyboard on screen.
void scenarioClickAway() {
    g_phase = "S7 click away";
    resetClean();
    openPalette();
    check(g_state.palette.open, "S7: the overlay is open");
    // The composer's field sits below the overlay; its row is the status row + 2.
    click(40, 24 - 3);
    idle(2);
    check(!g_state.palette.open, "S7: clicking outside the overlay closes it");
}

// S12: the `/` entry point. The composer takes the character on an empty line and hands it to the
// palette as a *seed*, so a typed slash command survives: `/` + `continue` + Enter submits
// `/continue`, not `continue`. This is the regression guard for the character being swallowed
// (the palette used to open empty, and only a pasted `/continue` could be sent).
void scenarioSlashSeed() {
    g_phase = "S12 slash seed";
    resetClean();
    // The `/` gate is the composer's character filter (views/composer.cpp), not a registry row,
    // so this scenario drives the real field.
    idle(2);

    typeText("/");
    check(g_state.palette.open,
          "S12: `/` on an empty composer line opens the palette (input='" + input() +
              "', focused=" + std::to_string(static_cast<int>(g_state.composer.focused)) +
              ", wantText=" + std::to_string(static_cast<int>(ImGui::GetIO().WantTextInput)) + ")");
    check(paletteQuery() == "/",
          "S12: the slash is carried into the palette's query (got '" + paletteQuery() + "')");
    check(g_state.palette.groupIndex == 3, "S12: it opens on the COMMANDS section");
    check(input().empty(), "S12: the composer kept no slash of its own (got '" + input() + "')");
    checkGrid(findRowIn(grid(), "/continue", paletteRect()) >= 0,
              "S12: the command lines are what the section shows");

    // The seed is a prefix: typing continues the line instead of replacing it (ImGui selects the
    // whole buffer when a keyboard activation focuses the field - measured).
    typeText("continue");
    check(paletteQuery() == "/continue",
          "S12: typing after the seed appends (got '" + paletteQuery() + "')");
    check(visibleRowById("commands.continue") >= 0, "S12: the `/continue` row matches the query");
    check(paletteItem("ask.freeText") != nullptr, "S12: the free-text fallback is offered as well");

    pressKey(ImGuiKey_Enter);
    check(!g_state.palette.open, "S12: Enter closes the palette");
    check(g_state.userQuery.submitReady && g_state.userQuery.submitQuery == "/continue",
          "S12: the submitted line is the command line (got '" + g_state.userQuery.submitQuery +
              "')");
    tuiResetSubmit(g_state);

    // `closePalette()` clears the seed with the rest of the query: the next open is empty again.
    check(g_state.palette.query[0] == '\0', "S12: closing cleared the query");
    check(!g_state.palette.seedCaretPending, "S12: and the one-shot caret request");
    g_state.userQuery.inputBuf.clear();
    idle(1);
    openPalette();
    check(paletteQuery().empty(), "S12: `Ctrl+K` still opens with an empty query");
    pressKey(ImGuiKey_Escape);
    idle(1);
    idle(2);
}

// S13: the command rows submit what they show, and the UI's own command stays the UI's own: the
// `/rawlog` row toggles the surface instead of sending the word to the model.
void scenarioCommandRows() {
    g_phase = "S13 command rows";
    resetClean();
    idle(2);
    typeText("/");
    check(g_state.palette.open, "S13: the palette is open on the COMMANDS section");

    for (const int index : g_state.palette.visible) {
        const views::PaletteItem& item = g_state.palette.items[static_cast<std::size_t>(index)];
        if (std::strcmp(item.id, "ask.freeText") == 0)
            continue; // the pinned fallback follows the section
        check(std::strcmp(item.group, "COMMANDS") == 0 && item.label.compare(0, 1, "/") == 0,
              std::string("S13: the row '") + item.label + "' is a command line");
    }

    selectRowById("commands.rawlog");
    pressKey(ImGuiKey_Enter);
    check(g_state.ui.rawLogOpen, "S13: the `/rawlog` row opens the developer log");
    check(!g_state.userQuery.submitReady, "S13: a UI command is never submitted as a query");
    // Put the surface back; the next scenario starts from a closed Raw Log.
    g_state.ui.rawLogOpen = false;
    g_state.rawLog = views::RawLogState{};
    idle(2);
    idle(2);
}

// S14: "Copy last answer" names a *block*. A code-only answer is one (its kind is rewritten to
// `Code` and its role word still says ANSWER), and the live streamed body is one before the store
// has the message - the two cases a message-store walk cannot get right.
void scenarioCopyBlock() {
    g_phase = "S14 copy last answer";
    resetClean();
    g_state.chat.outputLines.push_back(ChatMessage{"[3] code", "```\nint x = 1;\n```", "",
                                                   ChatMessageType::FinalAnswer,
                                                   g_state.chat.outputLineNextId++});
    idle(2);
    openPalette();
    selectRowById("actions.copyLastAnswer");
    pressKey(ImGuiKey_Enter);
    check(g_clipboard.find("int x = 1;") != std::string::npos,
          "S14: a code-only answer is copied (got '" + g_clipboard + "')");
    check(g_clipboard.find("ANSWER") == std::string::npos,
          "S14: the copy is the body, not the row's header");

    g_phase = "S14 copy the streaming body";
    resetClean();
    g_state.readyStatus = false;
    g_state.chat.hasStreamMsg = true;
    g_state.chat.streamMsg = ChatMessage{"", "partial answer", "", ChatMessageType::Assistant, 0};
    idle(2);
    openPalette();
    selectRowById("actions.copyLastAnswer");
    pressKey(ImGuiKey_Enter);
    check(g_clipboard == "partial answer",
          "S14: the live streamed body is the newest answer (got '" + g_clipboard + "')");

    g_phase = "S14 no answer";
    resetClean();
    g_state.chat.outputLines.pop_back(); // the user query is all that is left
    idle(2);
    openPalette();
    const views::PaletteItem* copy = paletteItem("actions.copyLastAnswer");
    check(copy != nullptr && !copy->enabled, "S14: with no answer the row is disabled");
    pressKey(ImGuiKey_Escape);
    idle(1);
    g_state.chat.hasStreamMsg = false;
}

// S16: the review's two Important findings and the behaviour changes the Minor ones asked for.
// Each check names the defect it pins: they are all "the palette did more - or less - than the
// control it points at", which is the divergence D-PALETTE exists to prevent.
void scenarioRowParity() {
    g_phase = "S16 submit handshake";
    resetClean();
    idle(1);
    // (a) The history cap and the draft: the palette's line is submitted through the composer's
    // handshake, so it cannot grow the history past `MAX_HISTORY` (review I1) and it cannot eat a
    // draft the user is still writing. The draft is *typed* through the field: a write to the
    // buffer while the widget owns the keyboard is discarded by ImGui (the composer's documented
    // trap), so the harness must not fake it.
    for (int i = 0; i < static_cast<int>(UserQueryState::MAX_HISTORY); ++i)
        g_state.userQuery.inputHistory.push_back("old " + std::to_string(i));
    typeText("a draft the user is still writing");
    check(input() == "a draft the user is still writing", "S16: the draft reached the field");
    openPalette();
    selectRowById("commands.help");
    pressKey(ImGuiKey_Enter);
    check(static_cast<int>(g_state.userQuery.inputHistory.size()) ==
              static_cast<int>(UserQueryState::MAX_HISTORY),
          "S16: the palette's submit keeps the history at MAX_HISTORY (" +
              std::to_string(g_state.userQuery.inputHistory.size()) + ")");
    check(!g_state.userQuery.inputHistory.empty() &&
              g_state.userQuery.inputHistory.back() == "/help",
          "S16: the submitted command line is the newest history entry");
    check(input() == "a draft the user is still writing",
          "S16: a palette submit leaves an unsent draft alone (got '" + input() + "')");
    tuiResetSubmit(g_state);

    // (b) The composer's draft is never the palette's to edit - not even when the palette happens
    // to submit the same text: the field re-asserts its own copy when it takes the keyboard back
    // (ImGui's active-buffer rule), so clearing here would be a fight the palette loses, and the
    // safer half of the case is to keep what the user wrote.
    resetClean();
    idle(1);
    typeText("hello");
    openPalette();
    typeText("hello"); // the same line, this time in the palette
    selectRowById("ask.freeText");
    pressKey(ImGuiKey_Enter);
    check(g_state.userQuery.submitQuery == "hello", "S16: the ASK row submitted the draft's line");
    check(input() == "hello", "S16: and the draft is still the user's (got '" + input() + "')");

    // (c) `New session` refuses during a run, exactly like the rail's `+` and the panel's `New`
    // (review I2): the row is drawn disabled and the activation is refused.
    g_phase = "S16 new session while busy";
    resetClean();
    g_state.readyStatus = false;
    idle(1);
    openPalette();
    const views::PaletteItem* newRow = paletteItem("actions.newSession");
    check(newRow != nullptr && !newRow->enabled, "S16: 'New session' is disabled during a run");
    check(newRow != nullptr && !views::palettePerformRow(*newRow, g_state.palette, g_state),
          "S16: and the activation is refused, like the panel's own New");
    check(g_state.sessionPanel.actions.empty() && !g_state.ui.rail.newRequested,
          "S16: so nothing was queued mid-run");
    pressKey(ImGuiKey_Escape);
    idle(1);

    // The workbench's answer is the rail's request - one implementation, not a second one.
    g_phase = "S16 new session through the rail";
    resetClean();
    idle(2);
    openPalette();
    const views::PaletteItem* newRowWorkbench = paletteItem("actions.newSession");
    check(newRowWorkbench != nullptr &&
              views::palettePerformRow(*newRowWorkbench, g_state.palette, g_state) &&
              g_state.ui.rail.newRequested,
          "S16: the workbench row asks the rail to queue the New action");
    idle(1); // `applyRailAnswers()` runs at the top of the frame
    check(queuedActions() == 1 &&
              g_state.sessionPanel.actions.front().type == SessionActionType::New,
          "S16: and the shell turns it into one New action");
    g_state.sessionPanel.actions.clear();
    g_state.ui.rail.newRequested = false;
    g_state.palette = views::PaletteState{};
    idle(2);

    // (d) `Focus composer` cannot promise the composer while a settings sheet is still under the
    // palette: closing would hand the keyboard back to the sheet (review M6).
    g_phase = "S16 focus composer under a sheet";
    resetClean();
    idle(2);
    views::openSettings(g_state.settingsSheet);
    idle(1);
    openPalette();
    const views::PaletteItem* focusRow = paletteItem("actions.focusComposer");
    check(focusRow != nullptr && !focusRow->enabled,
          "S16: 'Focus composer' is disabled while a sheet is under the palette");
    check(focusRow != nullptr && !views::palettePerformRow(*focusRow, g_state.palette, g_state),
          "S16: and it refuses the activation instead of re-arming the sheet");
    pressKey(ImGuiKey_Escape); // the palette is the topmost surface
    idle(1);
    g_state.settingsSheet = views::SettingsState{};
    idle(2);

    // (e) A new query is a new list: the selection goes back to its best row (review M2).
    g_phase = "S16 selection after a query";
    resetClean();
    idle(1);
    openPalette();
    selectRowById("actions.quit");
    check(g_state.palette.selected > 0, "S16: the selection was walked before the query");
    typeText("co");
    check(g_state.palette.selected == 0, "S16: a new query starts at its best match");

    // (f) An out-of-range section filter is ALL, not an out-of-bounds read (`groupIndex` is public
    // state, so a harness or a future caller can set anything). A fresh palette, so the query is
    // the one this check types.
    pressKey(ImGuiKey_Escape);
    idle(1);
    openPalette();
    g_state.palette.groupIndex = 99;
    typeText("q");
    check(visibleRowById("actions.quit") >= 0 && visibleRowById("commands.quit") >= 0,
          "S16: an out-of-range section filter falls back to ALL (rows of two sections listed)");
    checkGrid(findRowIn(grid(), "ALL ", paletteRect()) >= 0, "S16: and the title row says ALL");
    pressKey(ImGuiKey_Escape);
    idle(1);
}

// S15: `/` has two owners and they do not fight. The rail reads it for its search field whenever
// no text field holds the keyboard (task 23, design §4.3 "from anywhere except the composer"), and
// the composer reads it for the palette. A `/` typed with the rail focused must therefore focus the
// rail's search and *not* open the palette - which is also why the palette never fires on a key a
// field consumed.
void scenarioRailSlash() {
    g_phase = "S15 rail slash";
    resetClean();
    // The rail and its `/` are the workbench shell's. `resetClean()` focuses the composer (the
    // other scenarios type there), so the release is an explicit step here: `/` from anywhere
    // *except* the composer is the design's rule.
    pressKey(ImGuiKey_Escape);
    idle(2);
    check(!ImGui::GetIO().WantTextInput, "S15: the composer field is not holding the keyboard");
    click(3, 4); // somewhere on the rail, away from the search row
    idle(2);
    check(!ImGui::GetIO().WantTextInput, "S15: the click released the composer field");

    g_state.ui.rail.filterFocus = false;
    g_state.ui.rail.focusSearch = false;
    // One frame carries the character; the token the rail raises (`filterFocus`) is answered by
    // the frame layer, which widens the collapsed strip so there is a field to focus.
    frame([](ImGuiIO& io) { io.AddInputCharacter(static_cast<unsigned int>('/')); });
    check(g_state.ui.rail.filterFocus, "S15: the rail's `/` asks for its own search field");
    idle(2);
    check(!g_state.palette.open, "S15: the rail's `/` does not open the palette");
    check(g_state.ui.rail.searchWide,
          "S15: the frame answered the request by widening the rail's search row");
    check(std::string(g_state.ui.rail.filterBuf.data()).empty(),
          "S15: the search field is focused, not filled with the slash");

    // `Ctrl+K` from the rail is still the palette, with an empty query (no seed to leak in).
    g_state.ui.rail.filterFocus = false;
    g_state.ui.rail.focusSearch = false;
    idle(1);
    openPalette();
    check(paletteQuery().empty(), "S15: `Ctrl+K` from the rail opens an empty query");
    pressKey(ImGuiKey_Escape);
    idle(1);
    idle(2);
}

// ---------------------------------------------------------------------------
// The goldens
// ---------------------------------------------------------------------------

/// One golden scene: a name, the query the palette opens with, and the section it shows ("" = ALL).
struct PaletteScene {
    const char* name;
    const char* query;
    const char* section;
};

std::vector<PaletteScene> paletteScenes() {
    return {
        {"palette-all", "", ""},
        {"palette-filtered", "co", ""},
        {"palette-sessions", "", "SESSIONS"},
    };
}

/// Draw the palette *alone* at 80x24 and return the screen as text.
///
/// The sheet is driven directly, the way `test_rawlog` drives its surface: the golden then pins
/// the palette - its title, its sections, its rows and their tags, its hint row - and not the
/// shell behind it, which the shell's own goldens pin. Two frames, because the window appears and
/// takes the keyboard on the first one and the caret fix (a seeded query) settles on the second.
std::string paletteGoldenText(const PaletteScene& scene) {
    TuiState state;
    state.sessionPanel.sessions = {
        mkSession("s001-0", "Alpha refactor", "", 3, true),
        mkSession("s002-1", "Beta deploy", "deploy the beta build", 5),
        mkSession("s003-2", "Gamma debug", "", 1),
    };
    state.sessionPanel.activeId = "s001-0";
    state.chat.outputLines.push_back(
        ChatMessage{"[1] q", "a", "", ChatMessageType::FinalAnswer, 1});
    state.chat.outputLineNextId = 2;
    state.threadAdapter.bind(state);

    views::PaletteState p;
    views::openPaletteForState(p, state, scene.query);
    if (scene.section[0] != '\0')
        views::selectGroup(p, scene.section);

    // The palette's query field keeps ImGui's per-id edit state across frames, and an earlier
    // scenario left it active with its own text: an *active* InputText ignores the buffer it is
    // handed and writes its own copy back (the composer's documented trap). Dropping the active id
    // makes the field re-read the seeded buffer, which is the state a golden must capture.
    ImGui::ClearActiveID();

    ui::LayoutPrefs prefs;
    prefs.paletteOpen = true;
    const ui::ShellLayout layout = ui::computeShell(ui::metricsFor(), ui::displaySize(), prefs);
    for (int i = 0; i < 2; ++i) {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        ImTui_ImplText_NewFrame();
        ImGui::NewFrame();
        views::renderPalette(p, state, layout.paletteOverlay);
        ImGui::Render();
        ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
        ++g_frames;
    }

    const Grid g = grid();
    std::string text = "name: ";
    text += scene.name;
    text += "\nscreen: " + std::to_string(g.nx) + "x" + std::to_string(g.ny);
    text += "\nstate: section=" + std::to_string(p.groupIndex) +
            " rows=" + std::to_string(p.visible.size()) +
            " selected=" + std::to_string(p.selected) + " query='" + std::string(p.query.data()) +
            "'";
    text += "\ntext:\n";
    for (int y = 0; y < g.ny; ++y) {
        std::string row = rowText(g, y);
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
        text += row;
        text += '\n';
    }
    return text;
}

bool writeGolden(const std::string& path, const std::string& text) {
    FILE* out = std::fopen(path.c_str(), "wb");
    if (out == nullptr)
        return false;
    const std::size_t written = std::fwrite(text.data(), 1, text.size(), out);
    std::fclose(out);
    return written == text.size();
}

std::string readGolden(const std::string& path) {
    FILE* in = std::fopen(path.c_str(), "rb");
    if (in == nullptr)
        return {};
    std::string text;
    char buffer[4096];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof buffer, in)) > 0)
        text.append(buffer, n);
    std::fclose(in);
    return text;
}

void checkGoldens(bool update) {
    const std::string dir = TEST_PALETTE_GOLDEN_DIR;
    for (const PaletteScene& scene : paletteScenes()) {
        const std::string path =
            dir + "/" + scene.name + "-" + std::to_string(80) + "x" + std::to_string(24) + ".txt";
        const std::string text = paletteGoldenText(scene);
        if (update) {
            check(writeGolden(path, text), "golden written: " + path);
            continue;
        }
        const std::string expected = readGolden(path);
        check(!expected.empty(),
              "golden is readable: " + path + " (run with --update to create it)");
        if (expected == text) {
            check(true, std::string(scene.name) + " 80x24 golden matches");
            continue;
        }
        std::size_t line = 1;
        std::size_t at = 0;
        while (at < expected.size() && at < text.size() && expected[at] == text[at]) {
            if (expected[at] == '\n')
                ++line;
            ++at;
        }
        check(false, std::string(scene.name) + " 80x24 golden matches (first difference on line " +
                         std::to_string(line) + ": " + path + ")");
    }
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

void harnessInit() {
    std::setlocale(LC_ALL, "");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    llmfun::tui::applyUiTheme();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigInputTextCursorBlink = false; // whole-grid comparisons across frames
    io.KeyRepeatDelay = 0.050f;
    io.KeyRepeatRate = 0.050f;
    io.SetClipboardTextFn = [](void*, const char* text) {
        g_clipboard = text != nullptr ? text : "";
    };
    ImTui_ImplText_Init();
    io.DisplaySize = ImVec2(80.0f, 24.0f);
}

void harnessShutdown() {
    ImTui_ImplText_Shutdown();
    ImGui::DestroyContext();
}

int runText(bool update) {
    harnessInit();
    idle(1);
    if (g_screen.nx != 80 || g_screen.ny != 24)
        setupError("unexpected display size " + std::to_string(g_screen.nx) + "x" +
                   std::to_string(g_screen.ny) + " (want 80x24)");

    scenarioOpenClose();
    scenarioBoundedRows();
    scenarioFilter();
    scenarioGroups();
    scenarioActivate();
    scenarioQuitArmed();
    scenarioNoRegression();
    scenarioClickAway();
    scenarioKeyboardOwnership();
    scenarioLiveFlags();
    scenarioLoadEarlierWindow();
    scenarioWorkbenchRail();
    scenarioRegistry();
    scenarioSlashSeed();
    scenarioCommandRows();
    scenarioCopyBlock();
    scenarioRowParity();
    // S15 leaves the keyboard with the rail, so it runs after the composer-driven scenarios.
    scenarioRailSlash();
    // Last: S8 owns the display size for its duration and restores it.
    scenarioFloor();
    // And the goldens last of all: they draw the sheet on its own, so no earlier state can leak
    // into them.
    checkGoldens(update);

    harnessShutdown();
    std::printf("%s: %d checks failed (%d frames)\n", g_failures == 0 ? "OK" : "FAILED", g_failures,
                g_frames);
    return g_failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// GUI mode: the same palette through the real pixel backend
// ---------------------------------------------------------------------------

struct Ppm {
    int width = 0, height = 0;
    std::vector<unsigned char> rgb;
};

bool readPpm(const std::string& path, Ppm& out) {
    FILE* in = std::fopen(path.c_str(), "rb");
    if (in == nullptr)
        return false;
    char magic[3] = {0, 0, 0};
    int width = 0, height = 0, maxValue = 0;
    const bool header = std::fscanf(in, "%2s %d %d %d", magic, &width, &height, &maxValue) == 4;
    const int separator = header ? std::fgetc(in) : EOF;
    if (!header || std::strcmp(magic, "P6") != 0 || width <= 0 || height <= 0 || maxValue != 255 ||
        separator == EOF) {
        std::fclose(in);
        return false;
    }
    out.width = width;
    out.height = height;
    out.rgb.resize(static_cast<std::size_t>(width) * height * 3);
    const std::size_t read = std::fread(out.rgb.data(), 1, out.rgb.size(), in);
    std::fclose(in);
    return read == out.rgb.size();
}

/// How many pixels of `y` in [x0, x1) differ between two captures.
int countRowDiff(const Ppm& a, const Ppm& b, int y, int x0, int x1) {
    if (a.width != b.width || a.height != b.height || y < 0 || y >= a.height)
        return -1;
    int n = 0;
    for (int x = std::max(0, x0); x < std::min(a.width, x1); ++x) {
        const std::size_t i = (static_cast<std::size_t>(y) * a.width + x) * 3;
        if (a.rgb[i] != b.rgb[i] || a.rgb[i + 1] != b.rgb[i + 1] || a.rgb[i + 2] != b.rgb[i + 2])
            ++n;
    }
    return n;
}

int runGui(const std::string& outDir) {
    g_phase = "GUI";
    std::setlocale(LC_ALL, "");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // no imgui2.ini churn in the capture dir
    TuiState state;
    state.sessionPanel.sessions = {
        mkSession("s001-0", "Alpha refactor", "", 3, true),
        mkSession("s002-1", "Beta deploy", "deploy the beta build", 5),
    };
    state.sessionPanel.activeId = "s001-0";
    state.chat.outputLines.push_back(
        ChatMessage{"[1] q", "a", "", ChatMessageType::FinalAnswer, 1});
    state.chat.outputLineNextId = 2;

    const std::string iniPath = outDir + "/palette-imgui2.ini";
    std::remove(iniPath.c_str());
    std::remove(settingsPath(iniPath).c_str());
    std::string error;
    auto backend = makeGuiBackend(iniPath, state.settings);
    if (!backend->init(error))
        setupError("GUI backend init failed (no display?): " + error);
    state.backend = std::move(backend);
    llmfun::tui::initMarkdownGuiMode(state);
    llmfun::tui::applyUiTheme();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigInputTextCursorBlink = false;

    const auto frames = [&state](int count) {
        for (int i = 0; i < count; ++i) {
            state.backend->newFrame();
            if (!llmfun::tui::tuiRender(state))
                setupError("tuiRender reported 'user terminated'");
            state.backend->renderFrame();
            ++g_frames;
        }
    };
    const auto capture = [&](const std::string& name, Ppm& out) {
        const std::string path = outDir + "/" + name;
        if (!state.backend->captureNextFramePpm(path))
            setupError("the GUI backend refused the capture request");
        frames(1);
        if (!readPpm(path, out))
            setupError("the capture is not a readable P6 PPM: " + path);
    };

    frames(2);
    // 14 sessions so the result list can reach its 12-row cap.
    std::vector<SessionEntry> many;
    for (int i = 0; i < 14; ++i) {
        char id[32], title[64];
        std::snprintf(id, sizeof id, "gui%02d", i);
        std::snprintf(title, sizeof title, "Gui session %02d", i);
        many.push_back(mkSession(id, title, "", i, i == 0));
    }
    state.sessionPanel.sessions = many;
    frames(2);

    Ppm baseline;
    capture("palette-closed.ppm", baseline);

    // Ctrl+K through the IO boundary (the same event pair the GLFW backend
    // produces for a held modifier).
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    frames(1);
    io.AddKeyEvent(ImGuiKey_K, true);
    frames(1);
    io.AddKeyEvent(ImGuiKey_K, false);
    frames(1);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frames(1);
    check(state.palette.open, "GUI: Ctrl+K opens the overlay");

    ImGuiWindow* window = nullptr;
    {
        ImGuiContext& ctx = *ImGui::GetCurrentContext();
        for (int i = 0; i < ctx.Windows.Size; ++i)
            if (std::strcmp(ctx.Windows[i]->Name, "##palette") == 0 &&
                ctx.Windows[i]->LastFrameActive == ctx.FrameCount)
                window = ctx.Windows[i];
    }
    check(window != nullptr, "GUI: the overlay is its own window");
    if (window == nullptr) {
        std::printf("FAILED: %d checks failed\n", g_failures);
        return 1;
    }
    check(std::lround(window->Size.x) == 640, "GUI: the overlay is 640 px wide (got " +
                                                  std::to_string(std::lround(window->Size.x)) +
                                                  ")");
    check(std::lround(window->Pos.x) ==
              std::lround((ImGui::GetIO().DisplaySize.x - window->Size.x) * 0.5f),
          "GUI: the overlay is centred horizontally");

    // 12 result rows + the one-row overscan, measured on the list child itself.
    ImGuiWindow* list = nullptr;
    {
        ImGuiContext& ctx = *ImGui::GetCurrentContext();
        for (int i = 0; i < ctx.Windows.Size; ++i)
            if (std::string(ctx.Windows[i]->Name).find("/##palette_list") != std::string::npos &&
                ctx.Windows[i]->LastFrameActive == ctx.FrameCount)
                list = ctx.Windows[i];
    }
    check(list != nullptr, "GUI: the result list is a child window");
    if (list != nullptr) {
        const float rowH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
        const float rows = list->ContentSize.y / rowH;
        check(rows >= 12.0f && rows <= 13.0f,
              "GUI: the list draws 12 rows plus one overscan (got " + std::to_string(rows) + ")");
    }

    Ppm open;
    capture("palette-open.ppm", open);
    // The overlay painted over the thread: its top frame row is a solid line the
    // baseline does not have there. A single pixel would be a coin flip (the
    // thread's own surface and the palette's are the same token), so the whole
    // row is compared.
    const int frameY = static_cast<int>(std::lround(window->Pos.y));
    const int frameX0 = static_cast<int>(std::lround(window->Pos.x)) + 4;
    const int frameX1 = static_cast<int>(std::lround(window->Pos.x + window->Size.x)) - 4;
    const int painted = countRowDiff(baseline, open, frameY, frameX0, frameX1);
    check(painted > (frameX1 - frameX0) / 2,
          "GUI: the overlay paints over what is under it (" + std::to_string(painted) + " of " +
              std::to_string(frameX1 - frameX0) + " pixels on its top edge changed)");

    // Up/Down: the pixel profile's selection chords (the grid uses Ctrl+N/P).
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    io.AddKeyEvent(ImGuiKey_DownArrow, true);
    frames(1);
    io.AddKeyEvent(ImGuiKey_DownArrow, false);
    frames(1);
    check(state.palette.selected == 1,
          "GUI: Down moves the selection (got " + std::to_string(state.palette.selected) + ")");
    io.AddKeyEvent(ImGuiKey_UpArrow, true);
    frames(1);
    io.AddKeyEvent(ImGuiKey_UpArrow, false);
    frames(1);
    check(state.palette.selected == 0, "GUI: Up moves the selection back");

    io.AddKeyEvent(ImGuiKey_Escape, true);
    frames(1);
    io.AddKeyEvent(ImGuiKey_Escape, false);
    frames(2);
    check(!state.palette.open, "GUI: Esc closes the overlay");

    Ppm closed;
    capture("palette-closed-again.ppm", closed);
    const int repainted = countRowDiff(baseline, closed, frameY, frameX0, frameX1);
    check(repainted == 0, "GUI: the row the overlay painted is the shell's again (" +
                              std::to_string(repainted) + " pixels still differ)");

    // GUI history: Ctrl+Up must actually put the recalled text in the field.
    // The field is active, so the hand-off must drop the edit state and come
    // back through the one-shot focus request; writing inputBuf while the
    // InputText is active is ignored by ImGui.
    state.userQuery.inputHistory = {"first query", "second query"};
    state.userQuery.historyPos = -1;
    state.userQuery.inputBuf.clear();
    state.userQuery.newInputBufString.clear();
    state.userQuery.isSubmitted = true; // focus the composer on the next frame
    frames(2);
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    frames(1);
    io.AddKeyEvent(ImGuiKey_UpArrow, true);
    frames(1);
    io.AddKeyEvent(ImGuiKey_UpArrow, false);
    frames(1);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frames(2);
    check(state.userQuery.inputBuf == "second query",
          "GUI: Ctrl+Up recalls the newest history entry (got '" + state.userQuery.inputBuf + "')");
    // The latch is the composer's (`ComposerState::focusedLastFrame`, the value the view writes at
    // the end of its own frame); `UserQueryState::focusedLastFrame` was the pre-task-27 field and
    // nothing has written it since the composer took its state over.
    check(state.composer.focusedLastFrame, "GUI: the recalled field keeps the keyboard");

    // Ctrl+Down past the oldest entry restores the (empty) draft; an empty
    // hand-off is the case the non-empty inputBuf check used to skip.
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    frames(1);
    io.AddKeyEvent(ImGuiKey_DownArrow, true);
    frames(1);
    io.AddKeyEvent(ImGuiKey_DownArrow, false);
    frames(1);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frames(2);
    check(state.userQuery.inputBuf.empty(), "GUI: Ctrl+Down returns to the empty draft");

    state.backend->shutdown();
    state.backend.reset();
    ImGui::DestroyContext();
    std::printf("%s: %d checks failed (%d frames)\n", g_failures == 0 ? "OK" : "FAILED", g_failures,
                g_frames);
    return g_failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    bool gui = false;
    bool update = false;
    std::string outDir = ".";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--gui") {
            gui = true;
        } else if (arg == "--out" && i + 1 < argc) {
            outDir = argv[++i];
        } else if (arg == "--update") {
            update = true;
        } else {
            std::fprintf(stderr, "usage: %s [--gui [--out DIR]] [--update]\n", argv[0]);
            return 2;
        }
    }
    if (update && gui) {
        std::fprintf(stderr, "usage: %s --update is the text grid's (the GUI has no goldens)\n",
                     argv[0]);
        return 2;
    }
    return gui ? runGui(outDir) : runText(update);
}
