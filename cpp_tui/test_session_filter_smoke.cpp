// test_session_filter_smoke.cpp
//
// Panel-level headless smoke harness for the session filter (Phase 4, Task 8,
// A29) and the filter nav-focus hardening (Phase 4, Task 11, A31).
//
// It drives the REAL TuiState through the same frame pipeline as main.cpp,
// with the ncurses backend replaced by an equivalent per-frame io reset
// (mirror of ImTui_ImplNcurses_NewFrame's input handling, imtui-impl-
// ncurses.cpp:208-317, without a terminal):
//
//     [io reset: KeysDown / KeyCtrl / KeyShift / mouse / DeltaTime]
//     [inject: keys / chars / mouse]
//     ImTui_ImplText_NewFrame()
//     ImGui::NewFrame()
//     llmfun::tui::tuiRender(state)  // app render (owns ImGui::EndFrame)
//     ImGui::Render()
//     ImTui_ImplText_RenderDrawData(drawData, screen)  // into TScreen grid
//
// Same pattern as plan/task10/render_check.cpp (text backend only, no PTY).
// The KeyMap table copied from ImTui_ImplNcurses_Init keeps pressKey()
// semantics identical to the real app. Fixed 80x24 DisplaySize = the PTY
// winsize the real app gets in an 80x24 terminal.
//
// A click is two frames: MouseDown on frame N, MouseUp on frame N+1 at the
// same position (ImGui 1.81 buttons act on the release edge).
//
// Assertions are made against:
//   - the C++ TuiState: filterBuf/filterSeq, pendingSelectId, the actions
//     queue, renameActive/renameBuf, panelOpen, readyStatus, userQuery,
//     and ImGui nav/active ids (imgui_internal.h) for focus stability;
//   - the TScreen cell grid: row text (labels, "no matches", order), UTF-8
//     validity of every row, and relative fg colors for the highlight.
//
// Exit codes: 0 = all scenarios pass; 1 = first assertion failure (the full
// grid is dumped on stderr); 2 = environment error (init/display size).
//
// No terminal required (text backend only) — run from the build dir:
//   ./test_session_filter_smoke
//   TERM=xterm-256color COLUMNS=80 LINES=24 timeout 300 ./test_session_filter_smoke < /dev/null

#include "imtui/imtui.h"

#include "imgui/imgui_internal.h"

#include "tui.h"

#include <algorithm>
#include <cctype>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace llmfun::tui;

namespace llmfun::tui {
// Theme setup defined in tui.cpp, not declared in tui.h (same pattern as
// plan/task10/render_check.cpp).
void applyTheme();
} // namespace llmfun::tui

namespace {

ImTui::TScreen g_screen;
TuiState g_state;
int g_frames = 0;
const char* g_phase = "startup";

void fail(const std::string& what) {
    std::fprintf(stderr, "FAIL [%s @ frame %d]: %s\n", g_phase, g_frames, what.c_str());
    // Full grid dump (ch 0 printed as '.')
    std::fprintf(stderr, "grid (%dx%d):\n", g_screen.nx, g_screen.ny);
    for (int y = 0; y < g_screen.ny; ++y) {
        std::string line;
        for (int x = 0; x < g_screen.nx; ++x) {
            unsigned c = g_screen.data[y * g_screen.nx + x].ch;
            line.push_back((c >= 32 && c < 127) ? char(c) : '.');
        }
        std::fprintf(stderr, "%2d |%s|\n", y, line.c_str());
    }
    std::exit(1);
}

// ---------------------------------------------------------------- grid tools

struct Grid {
    int nx = 0, ny = 0;
    std::vector<int> ch;
    std::vector<unsigned char> fg;
};

Grid grid() {
    Grid g;
    g.nx = g_screen.nx;
    g.ny = g_screen.ny;
    g.ch.resize(size_t(g.nx) * g.ny);
    g.fg.resize(size_t(g.nx) * g.ny);
    for (int i = 0; i < g.nx * g.ny; ++i) {
        g.ch[i] = g_screen.data[i].ch;
        g.fg[i] = g_screen.data[i].fg;
    }
    return g;
}

std::string utf8Of(int cp) {
    std::string s;
    if (cp < 0x80)
        s.push_back(char(cp));
    else if (cp < 0x800) {
        s.push_back(char(0xC0 | (cp >> 6)));
        s.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(char(0xE0 | (cp >> 12)));
        s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(char(0x80 | (cp & 0x3F)));
    }
    return s;
}

std::string rowText(const Grid& g, int y, int x0 = 0, int x1 = -1) {
    if (y < 0 || y >= g.ny)
        return "";
    if (x1 < 0)
        x1 = g.nx;
    std::string s;
    for (int x = x0; x < x1 && x < g.nx; ++x) {
        int c = g.ch[y * g.nx + x];
        s += (c == 0) ? " " : utf8Of(c);
    }
    return s;
}

// Find first row (>= 0) whose text contains needle; -1 if absent.
int findRow(const Grid& g, const std::string& needle, int x0 = 0, int x1 = -1) {
    for (int y = 0; y < g.ny; ++y)
        if (rowText(g, y, x0, x1).find(needle) != std::string::npos)
            return y;
    return -1;
}

int firstNonSpace(const Grid& g, int y, int x0 = 0) {
    for (int x = x0; x < g.nx; ++x) {
        int c = g.ch[y * g.nx + x];
        if (c != 0)
            return x;
    }
    return -1;
}

unsigned char fgAt(const Grid& g, int x, int y) { return g.fg[y * g.nx + x]; }

bool validUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        int need = 0;
        if (c < 0x80)
            need = 0;
        else if (c >= 0xC2 && c <= 0xDF)
            need = 1;
        else if (c >= 0xE0 && c <= 0xEF)
            need = 2;
        else if (c >= 0xF0 && c <= 0xF4)
            need = 3;
        else
            return false;
        if (i + 1 + (size_t)need > s.size())
            return false;
        for (int k = 1; k <= need; ++k)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80)
                return false;
        i += 1 + (size_t)need;
    }
    return true;
}

bool allRowsValidUtf8(const Grid& g, const char* tag) {
    for (int y = 0; y < g.ny; ++y)
        if (!validUtf8(rowText(g, y)))
            fail(std::string("row not valid UTF-8 (") + tag + ", row " + std::to_string(y) +
                 "): '" + rowText(g, y) + "'");
    return true;
}

// ------------------------------------------------------------- frame driver

void frame(const std::function<void(ImGuiIO&)>& inject = nullptr) {
    g_phase = "frame driver";
    // Per-frame input state: mirror ImTui_ImplNcurses_NewFrame (imtui-impl-
    // ncurses.cpp:208-317) without a terminal — clear the 512-key state,
    // drop modifiers, mouse at (0,0) with no buttons, 60fps active delta.
    ImGuiIO& io = ImGui::GetIO();
    std::fill(io.KeysDown, io.KeysDown + 512, 0);
    io.KeyCtrl = false;
    io.KeyShift = false;
    io.MousePos = ImVec2(0.0f, 0.0f);
    io.MouseDown[0] = false;
    io.MouseDown[1] = false;
    io.DeltaTime = 1.0f / 60.0f;
    if (inject)
        inject(io);
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    if (!llmfun::tui::tuiRender(g_state))
        fail("tuiRender returned false (quit)");
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    ++g_frames;
}

void idle(int n = 1) {
    for (int i = 0; i < n; ++i)
        frame();
}

void pressKey(ImGuiKey k) {
    frame([&](ImGuiIO& io) { io.KeysDown[io.KeyMap[k]] = true; });
    // Release frame: a physical key press is down for ~1 frame, then up.
    // Without the up frame, io.KeysDownDuration keeps accumulating across
    // consecutive presses (NewFrame only resets it to -1 when it observes
    // the key up), so IsKeyPressed's press-edge (duration == 0) would fire
    // for the first press only — the real ncurses backend naturally
    // produces the down->up sequence (getch sees the key absent on the
    // following frame, imtui-impl-ncurses.cpp:208-317).
    frame();
}

void typeUtf8(const std::string& s) {
    frame([&](ImGuiIO& io) { io.AddInputCharactersUTF8(s.c_str()); });
}

void typeAll(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            unsigned short uc = c;
            frame([&](ImGuiIO& io) {
                // Mirror the ncurses backend: space also sets KeysDown[32]
                // (imtui-impl-ncurses.cpp:294-301) alongside the character.
                if (uc == 32)
                    io.KeysDown[32] = true;
                io.AddInputCharacter(uc);
            });
            ++i;
        } else {
            // Multi-byte UTF-8: decode the whole character and inject it as
            // one code point, mirroring the ncurses backend
            // (wchar -> UTF-8 string -> AddInputCharactersUTF8,
            // imtui-impl-ncurses.cpp:296-301). A raw leading or
            // continuation byte is not valid UTF-8 on its own, so injecting
            // it single-byte-wise would mangle the character (S13).
            unsigned cp = 0;
            int len = 0;
            if ((c & 0xE0) == 0xC0) {
                cp = c & 0x1F;
                len = 2;
            } else if ((c & 0xF0) == 0xE0) {
                cp = c & 0x0F;
                len = 3;
            } else if ((c & 0xF8) == 0xF0) {
                cp = c & 0x07;
                len = 4;
            }
            if (len == 0 || i + static_cast<std::size_t>(len) > s.size()) {
                ++i; // invalid or truncated sequence: skip (test inputs are valid)
                continue;
            }
            for (int k = 1; k < len; ++k) {
                cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
            }
            const unsigned short ucp = static_cast<unsigned short>(cp);
            frame([&](ImGuiIO& io) { io.AddInputCharacter(ucp); });
            i += static_cast<std::size_t>(len);
        }
    }
}

void click(int x, int y) {
    frame([&](ImGuiIO& io) {
        io.MousePos = ImVec2(x, y);
        io.MouseDown[0] = true;
    });
    frame([&](ImGuiIO& io) {
        io.MousePos = ImVec2(x, y);
        io.MouseDown[0] = false;
    });
}

// ------------------------------------------------------------ state helpers

std::string filter() {
    const char* b = g_state.sessionPanel.filterBuf.data();
    return std::string(b, ::strnlen(b, 63));
}

// inputBuf is a std::string sized to include ImGui's trailing NUL (the
// CallbackResize resize), so compare it as a C string.
std::string input() { return std::string(g_state.userQuery.inputBuf.c_str()); }

bool filterIsEmpty() {
    // The app reads filterBuf as a C string (std::string(filterBuf.data())),
    // so "empty" means the C string is whitespace-only. Do NOT scan the raw
    // bytes: after an Escape that cancels an edit back to an empty initial
    // value, the vendored imgui 1.81 writes only the NUL terminator into the
    // user buffer (imgui_widgets.cpp:4256-4260 + ImStrncpy with length 1,
    // line 4396) and leaves the previous characters after it — the real app
    // shows an empty filter in that state.
    const std::string s = filter();
    for (char c : s)
        if (!std::isspace((unsigned char)c))
            return false;
    return true;
}

void drainActions() {
    while (!g_state.sessionPanel.actions.empty())
        g_state.sessionPanel.actions.pop_front();
}

int actionCount() { return (int)g_state.sessionPanel.actions.size(); }

bool checkAction(SessionActionType wantType, const std::string& wantId, const char* tag) {
    if (actionCount() != 1) {
        fail(std::string(tag) + ": expected exactly 1 queued action, got " +
             std::to_string(actionCount()));
    }
    const SessionAction& a = g_state.sessionPanel.actions.front();
    if (a.type != wantType)
        fail(std::string(tag) + ": action type != Select (got " + std::to_string((int)a.type) +
             ")");
    if (a.sessionId != wantId)
        fail(std::string(tag) + ": action id '" + a.sessionId + "' != expected '" + wantId + "'");
    drainActions();
    return true;
}

std::string idOfTitle(const std::string& title) {
    for (const auto& e : g_state.sessionPanel.sessions)
        if (e.title == title)
            return e.id;
    fail("title not found in snapshot: " + title);
    return "";
}

SessionEntry mk(const char* id, const char* title, const char* preview, int count,
                bool active = false) {
    SessionEntry e;
    e.id = id;
    e.title = title;
    e.preview = preview;
    e.messageCount = (size_t)count;
    e.isActive = active;
    return e;
}

std::string longTitle() {
    // A27: 186 chars; fuzzy "abz" matches a@0, b@94, z@185 -> raw score
    // 100 + 40 + 100 + 25 + 100 + 25 - 94 - 185 = -89 (deeply negative).
    // Must be clamped to 0 (still shown, last) and truncated by A19.
    std::string t = "aaaa";
    t += std::string(90, 'x');
    t += "b";
    t += std::string(90, 'x');
    t += "z";
    return t;
}

std::vector<SessionEntry> seedSessions() {
    // 13 distinguishable titles: 8 title-only rows, 4 title+preview rows
    // (2 grid rows each), 1 long-title row; covers greek letters, accented
    // multi-byte title, preview-only matches and the weak-match clamp case.
    return {
        mk("s001-0", "Alpha refactor", "", 3, true),
        mk("s002-1", "Beta deploy", "", 5),
        mk("s003-2", "Gamma debug", "", 1),
        mk("s004-3", "Delta release", "", 2),
        mk("s005-4", "Epsilon test", "", 0),
        mk("s006-5", "Zeta backup", "", 4),
        mk("s007-6", "Eta migrate", "", 7),
        mk("s008-7", "Theta profile", "", 2),
        mk("s009-8", "Iota rollback", "rollback the iota change", 3),
        mk("s010-9", "Kappa monitor", "set up kappa monitoring", 1),
        mk("s011-10", "Caf\u00E9 au lait", "lambda caf\u00E9 index", 2),
        mk("s012-11", "Mu cleanup", "cleanup unused mu code", 6),
        mk("s013-12", longTitle().c_str(), "", 1),
    };
}

void setSnapshot(std::vector<SessionEntry> v) { g_state.sessionPanel.sessions = std::move(v); }

// --------------------------------------------------------- layout positions

// The "Sessions ---" separator row, and derived positions:
//   +1 = filter input row, +2 = first session row (the rows child has no
//   visible border row; the active session's Rename button occupies the
//   row directly below it).
int sepRowY() {
    int y = findRow(grid(), "Sessions");
    if (y < 0)
        fail("separator row 'Sessions' not found");
    return y;
}

int statusRowY() {
    int y = findRow(grid(), "Context:");
    if (y < 0)
        fail("status row 'Context:' not found");
    return y;
}

void clickFilter() { click(5, sepRowY() + 1); }

// The main query InputTextMultiline shares its bottom row with the Send
// field (SameLine after the multi-line input); Send/Prev/Next stack below
// it, so the input's clickable row is three above the status row.
void clickMainInput() { click(40, statusRowY() - 3); }

// Click the row whose title label starts at the first non-space cell.
void clickRowByTitle(const std::string& needle) {
    int y = findRow(grid(), needle);
    if (y < 0)
        fail("row not found for click: " + needle);
    int x = firstNonSpace(grid(), y);
    if (x < 0)
        fail("empty row for click: " + needle);
    click(x + 1, y);
}

int navId() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    return (int)g.NavId;
}

int activeId() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    return (int)g.ActiveId;
}

// Reset to a known-clean panel state: no filter, no rename box, no queued
// actions, panel open, ready, no agents, mouse parked.
void resetClean() {
    for (int i = 0; i < 8; ++i) {
        auto& p = g_state.sessionPanel;
        if (p.renameActive) {
            pressKey(ImGuiKey_Escape); // closes rename first (R21)
            idle(1);
            continue;
        }
        if (!filterIsEmpty()) {
            pressKey(ImGuiKey_Escape); // clears filter
            idle(1);
            continue;
        }
        if (!p.panelOpen) {
            int y = findRow(grid(), "Open");
            if (y >= 0) {
                int x = firstNonSpace(grid(), y);
                click(x + 1, y);
            }
            idle(1);
            continue;
        }
        break;
    }
    if (g_state.sessionPanel.renameActive)
        fail("resetClean: rename box still active");
    if (!filterIsEmpty())
        fail("resetClean: filter still set");
    if (!g_state.sessionPanel.panelOpen)
        fail("resetClean: panel not open");
    drainActions();
    g_state.readyStatus = true;
    g_state.left.agents.clear();
    setSnapshot(seedSessions());
    g_state.sessionPanel.pendingSelectId.clear();
    g_state.sessionPanel.pendingDeleteId.clear();
    // Park the mouse so no tooltip/hover artifacts linger.
    frame([](ImGuiIO& io) {
        io.MousePos = ImVec2(0, 0);
        io.MouseDown[0] = false;
    });
    idle(1);
}

// ================================================================= scenarios

void scenario0_calibration() {
    g_phase = "S0 calibration";
    idle(3);
    Grid g = grid();
    allRowsValidUtf8(g, "S0");

    int yClose = findRow(g, "Close");
    if (yClose < 0)
        fail("S0: 'Close' button row not found");
    if (findRow(g, "New") != yClose)
        fail("S0: 'New' not on the Close row");

    int ySep = sepRowY();
    if (ySep <= yClose)
        fail("S0: separator not below Close/New row");

    // Filter row: directly under the separator, empty (frame only).
    std::string frow = rowText(g, ySep + 1);
    for (char c : frow)
        if (c != ' ')
            fail("S0: filter row not empty: '" + frow + "'");

    // First session row: directly under the filter row (the rows child has
    // no visible border row). The active session's "Rename" button occupies
    // the row below the active title row.
    int yFirst = findRow(g, "Alpha refactor [3]");
    if (yFirst < 0)
        fail("S0: first session row 'Alpha refactor [3]' not found");
    if (yFirst != ySep + 2)
        fail("S0: first session row at " + std::to_string(yFirst) + ", expected " +
             std::to_string(ySep + 2));

    if (statusRowY() < 0)
        fail("S0: status row not found");

    // At startup the main query input auto-focuses (isSubmitted initial),
    // so nav must be initialized on it.
    if (navId() == 0)
        fail("S0: nav id not initialized after startup (main input auto-focus)");
    if (activeId() == 0)
        fail("S0: no active widget after startup");
}

// C11: typing at startup goes to the main query input; the filter never
// steals focus; clicking the filter moves focus there and back.
void scenario16_focus() {
    g_phase = "S16 focus (C11)";
    typeAll("hello"); // main input is focused from frame 1
    if (input() != "hello")
        fail("S16: startup typing did not reach the main query input ('" + input() + "')");
    if (!filterIsEmpty())
        fail("S16: filter changed without interaction (focus stolen at startup)");

    clickFilter();
    int n1 = navId();
    typeAll("eta");
    if (filter() != "eta")
        fail("S16: typing after filter click did not reach the filter ('" + filter() + "')");
    if (input() != "hello")
        fail("S16: main query input lost its text while filter was focused");
    // Nav stays parked on the filter across the typing frames.
    for (int i = 0; i < 3; ++i) {
        idle(1);
        if (navId() != n1)
            fail("S16: nav id drifted while filter focused (no arrow pressed)");
    }

    clickMainInput();
    frame([](ImGuiIO& io) { io.AddInputCharacter((unsigned short)'X'); });
    if (input() != "helloX")
        fail("S16: click-back on main input failed ('" + input() + "')");
    {
        Grid g = grid();
        int y = findRow(g, "Eta migrate [7]");
        if (y < 0)
            fail("S16: filter list not rendered after focus round-trip");
    }

    // A23: Esc clears the filter. Park focus on the filter first so the
    // main input is not the active widget when Esc lands: a focused text
    // input cancels its own edit session on Escape (imgui 1.81 InputText,
    // vendored imgui_widgets.cpp:4180), which would revert the in-progress
    // "X" to the value captured when focus was gained. Deactivating the
    // main input commits "helloX" to the buffer, so it must survive as-is.
    clickFilter();
    idle(1);
    pressKey(ImGuiKey_Escape);
    idle(1);
    if (!filterIsEmpty())
        fail("S16: Esc did not clear the filter");
    if (input() != "helloX")
        fail("S16: Esc touched the main query input ('" + input() + "')");
}

// (a) A fuzzy query narrows the list and ranks by score.
void scenario1_filter_ranks() {
    g_phase = "S1 (a) filter narrows + ranks";
    resetClean();
    clickFilter();
    typeAll("eta");
    idle(1);
    if (filter() != "eta")
        fail("S1: filter != 'eta'");

    Grid g = grid();
    allRowsValidUtf8(g, "S1");
    int y1 = findRow(g, "Eta migrate [7]");   // 390
    int y2 = findRow(g, "Beta deploy [5]");   // 349
    int y3 = findRow(g, "Zeta backup [4]");   // 349 (stable: snapshot order)
    int y4 = findRow(g, "Theta profile [2]"); // 348
    int y5 = findRow(g, "Delta release [2]"); // 321
    int y6 = findRow(g, "Kappa monitor [1]"); // 154 (preview only)
    int y7 = findRow(g, "Iota rollback [3]"); // 152 (preview only)
    if (y1 < 0 || y1 >= y2 || y2 >= y3 || y3 >= y4 || y4 >= y5 || y5 >= y6 || y6 >= y7)
        fail("S1: ranked order wrong (y1..y7=" + std::to_string(y1) + "," + std::to_string(y2) +
             "," + std::to_string(y3) + "," + std::to_string(y4) + "," + std::to_string(y5) + "," +
             std::to_string(y6) + "," + std::to_string(y7) + ")");
    // Preview rows sit right below their session rows. At the default panel
    // width the row budget is 22 cells, so longer previews render truncated
    // with an ellipsis (previewRowLabel, A17/M1): the 23-char Kappa preview
    // becomes "set up kappa monito...", the 24-char Iota preview becomes
    // "rollback the iota c...".
    if (findRow(g, "set up kappa monito...") < 0)
        fail("S1: Kappa preview row missing");
    if (findRow(g, "rollback the iota c...") < 0)
        fail("S1: Iota preview row missing");
    if (findRow(g, "set up kappa monito...") <= y6 || findRow(g, "set up kappa monito...") >= y7)
        fail("S1: Kappa preview row not directly below its title row");
    if (findRow(g, "rollback the iota c...") <= y7)
        fail("S1: Iota preview row not below its title row");
    // Non-matches hidden.
    for (const char* hidden : {"Alpha refactor", "Gamma debug", "Epsilon test", "Caf\u00E9 au lait",
                               "Mu cleanup", "aaaa"})
        if (findRow(g, hidden) >= 0)
            fail(std::string("S1: non-matching row still visible: ") + hidden);

    // No flicker: two idle frames render the identical grid.
    Grid a = grid();
    idle(2);
    Grid b = grid();
    if (a.ch != b.ch)
        fail("S1: grid not stable across idle frames (flicker)");
}

// (b) No match shows "no matches"; backspace-to-empty restores the list.
void scenario2_no_match() {
    g_phase = "S2 (b) no matches + backspace";
    pressKey(ImGuiKey_Backspace);
    pressKey(ImGuiKey_Backspace);
    pressKey(ImGuiKey_Backspace);
    idle(1);
    if (!filterIsEmpty())
        fail("S2: backspace did not empty the filter");
    {
        Grid g = grid();
        if (findRow(g, "Alpha refactor [3]") < 0)
            fail("S2: full list not restored after emptying the filter");
    }

    typeAll("zzq");
    idle(1);
    {
        Grid g = grid();
        if (findRow(g, "no matches") < 0)
            fail("S2: 'no matches' row missing for empty result");
        for (const char* hidden : {"Eta migrate", "Alpha refactor", "Kappa monitor"})
            if (findRow(g, hidden) >= 0)
                fail(std::string("S2: row visible with no-match filter: ") + hidden);
    }
    // Backspace-to-empty restores the full list again.
    pressKey(ImGuiKey_Backspace);
    pressKey(ImGuiKey_Backspace);
    pressKey(ImGuiKey_Backspace);
    idle(1);
    {
        Grid g = grid();
        if (findRow(g, "Alpha refactor [3]") < 0 || findRow(g, "no matches") >= 0)
            fail("S2: full list not restored after clearing 'zzq'");
    }
}

// (c) Escape clears the filter; the list reverts.
void scenario3_esc_clears() {
    g_phase = "S3 (c) Esc clears";
    typeAll("alp"); // only s001 "Alpha refactor" matches
    idle(1);
    {
        Grid g = grid();
        if (findRow(g, "Alpha refactor [3]") < 0)
            fail("S3: 'alp' should show Alpha refactor");
        for (const char* hidden : {"Beta deploy", "Kappa monitor", "Mu cleanup"})
            if (findRow(g, hidden) >= 0)
                fail(std::string("S3: extra row for 'alp': ") + hidden);
    }
    pressKey(ImGuiKey_Escape);
    idle(1);
    if (!filterIsEmpty())
        fail("S3: filter not empty after Esc");
    {
        Grid g = grid();
        if (findRow(g, "Alpha refactor [3]") < 0 || findRow(g, "Beta deploy [5]") < 0)
            fail("S3: full list not restored after Esc");
    }
}

// (d) Enter selects the top match even when it is not the active session.
void scenario4_enter_selects_top() {
    g_phase = "S4 (d) Enter selects top";
    clickFilter();
    typeAll("eta");
    idle(1);
    int seqBefore = g_state.sessionPanel.filterSeq;
    pressKey(ImGuiKey_Enter);
    idle(1);
    checkAction(SessionActionType::Select, "s007-6", "S4");
    if (!filterIsEmpty())
        fail("S4: filter not cleared after Enter-select");
    if (g_state.sessionPanel.filterSeq != seqBefore + 1)
        fail("S4: filterSeq not bumped by the programmatic clear (C10)");
    {
        Grid g = grid();
        if (findRow(g, "Alpha refactor [3]") < 0)
            fail("S4: full list not restored after Enter-select");
    }
}

// (e) Clicking a filtered row selects that row.
void scenario5_click_row() {
    g_phase = "S5 (e) click filtered row";
    clickFilter();
    typeAll("eta");
    idle(1);
    clickRowByTitle("Beta deploy");
    idle(1);
    checkAction(SessionActionType::Select, "s002-1", "S5");
    if (!filterIsEmpty())
        fail("S5: filter not cleared after row click");
}

// (f) Enter while busy queues pendingSelect; it flushes on the first ready
//     frame as an ordinary Select.
void scenario6_enter_busy() {
    g_phase = "S6 (f) Enter while busy";
    clickFilter();
    typeAll("eta");
    idle(1);
    g_state.readyStatus = false;
    idle(1);
    pressKey(ImGuiKey_Enter);
    idle(1);
    if (actionCount() != 0)
        fail("S6: action queued while busy (should be deferred)");
    if (g_state.sessionPanel.pendingSelectId != "s007-6")
        fail("S6: pendingSelectId != top match ('" + g_state.sessionPanel.pendingSelectId + "')");
    if (!filterIsEmpty())
        fail("S6 (A24): filter not cleared immediately while busy");
    g_state.readyStatus = true;
    idle(2);
    checkAction(SessionActionType::Select, "s007-6", "S6 flush");
    if (!g_state.sessionPanel.pendingSelectId.empty())
        fail("S6: pendingSelectId not cleared after flush");
}

// (f2) Row clicks while busy: the LAST click wins (single slot).
void scenario7_click_busy_last_wins() {
    g_phase = "S7 (f2) clicks while busy, last wins";
    clickFilter();
    typeAll("eta");
    idle(1);
    g_state.readyStatus = false;
    idle(1);
    clickRowByTitle("Zeta backup");
    clickRowByTitle("Delta release");
    idle(1);
    if (g_state.sessionPanel.pendingSelectId != "s004-3")
        fail("S7: pendingSelectId != last clicked ('" + g_state.sessionPanel.pendingSelectId +
             "')");
    if (actionCount() != 0)
        fail("S7: action queued while busy");
    if (!filterIsEmpty())
        fail("S7 (A24): filter not cleared immediately while busy");
    g_state.readyStatus = true;
    idle(2);
    checkAction(SessionActionType::Select, "s004-3", "S7 flush");
}

// (g) Snapshot updates with an active filter: the filter re-applies to the
//     new snapshot every frame; results, order and "no matches" all follow.
void scenario8_snapshot_updates() {
    g_phase = "S8 (g) snapshot updates";
    clickFilter();
    typeAll("eta");
    idle(1);
    auto v = seedSessions();
    for (auto& e : v)
        if (e.id == "s002-1")
            e.title = "Beta deploy v2";
    v.erase(
        std::remove_if(v.begin(), v.end(), [](const SessionEntry& e) { return e.id == "s003-2"; }),
        v.end());
    v.push_back(mk("s014-13", "Eta prime", "", 9));
    setSnapshot(std::move(v));
    idle(2);
    if (filter() != "eta")
        fail("S8: filter changed by a snapshot update");
    {
        Grid g = grid();
        int y1 = findRow(g, "Eta migrate [7]"); // 390 (snapshot order before s014)
        int y2 = findRow(g, "Eta prime [9]");   // 390
        int y3 = findRow(g, "Beta deploy v2 [5]");
        int y4 = findRow(g, "Zeta backup [4]");
        int y5 = findRow(g, "Theta profile [2]");
        int y6 = findRow(g, "Delta release [2]");
        if (y1 < 0 || y2 < 0 || y1 >= y2 || y2 >= y3 || y3 >= y4 || y4 >= y5 || y5 >= y6)
            fail("S8: re-ranked order after rename/insert wrong (y1..y6=" + std::to_string(y1) +
                 "," + std::to_string(y2) + "," + std::to_string(y3) + "," + std::to_string(y4) +
                 "," + std::to_string(y5) + "," + std::to_string(y6) + ")");
        if (findRow(g, "Gamma debug") >= 0)
            fail("S8: deleted session row still visible");
    }

    // (g2) filter with no match; a snapshot update introduces a match.
    pressKey(ImGuiKey_Backspace);
    pressKey(ImGuiKey_Backspace);
    pressKey(ImGuiKey_Backspace);
    idle(1);
    typeAll("zzq");
    idle(1);
    {
        Grid g = grid();
        if (findRow(g, "no matches") < 0)
            fail("S8g2: 'no matches' missing before update");
    }
    auto v2 = seedSessions();
    v2.push_back(mk("s015-14", "Zzq task", "", 2));
    setSnapshot(std::move(v2));
    idle(2);
    {
        Grid g = grid();
        if (findRow(g, "Zzq task [2]") < 0)
            fail("S8g2: new match not shown after snapshot update");
        if (findRow(g, "no matches") >= 0)
            fail("S8g2: 'no matches' still shown after update introduced a match");
        if (findRow(g, "Alpha refactor") >= 0)
            fail("S8g2: non-matching rows visible with active filter");
    }
    // (g3) snapshot update removes the match again -> "no matches" returns.
    setSnapshot(seedSessions());
    idle(2);
    {
        Grid g = grid();
        if (findRow(g, "no matches") < 0)
            fail("S8g3: 'no matches' did not return after match removal");
    }
    resetClean();
}

// A28: hiding the active row while the rename box is open closes the box.
void scenario9_a28_rename_closes() {
    g_phase = "S9 A28 rename closes on filter";
    resetClean();
    clickRowByTitle("Rename"); // opens the box on the active row (s001)
    idle(2);
    if (!g_state.sessionPanel.renameActive)
        fail("S9: rename box did not open");
    {
        std::string r(g_state.sessionPanel.renameBuf);
        if (r != "Alpha refactor")
            fail("S9: rename buffer not initialized from the row title ('" + r + "')");
    }
    clickFilter();
    typeAll("eta"); // s001 disappears from the filtered list
    idle(1);
    if (g_state.sessionPanel.renameActive)
        fail("S9 (A28): rename box stayed open with its row filtered out");
    if (filter() != "eta")
        fail("S9: filter lost after A28 close");
    {
        Grid g = grid();
        if (findRow(g, "Alpha refactor [3]") >= 0)
            fail("S9: filtered-out active row still rendered");
        if (findRow(g, "Alpha refactor!") >= 0)
            fail("S9: rename input text leaked into the grid");
    }
    pressKey(ImGuiKey_Escape);
    idle(1);
    if (!filterIsEmpty())
        fail("S9: filter not cleared after A28");
}

// R21: with the rename box open, Escape closes the box (rename cancelled);
//      the filter is untouched and still shown. The typing step also pins
//      the focus semantics: the box gains focus via SetKeyboardFocusHere
//      (code focus), and ImGui pre-selects the whole content on code focus
//      (same as tabbing into a field), so the first typed character
//      replaces the pre-selected title. Arrow keys / End clear the
//      selection and position the cursor for in-place edits.
void scenario10_esc_priority() {
    g_phase = "S10 (R21) Esc priority";
    clickFilter();
    typeAll("alp"); // shows s001 (active)
    idle(1);
    {
        int y = findRow(grid(), "Rename");
        int x = firstNonSpace(grid(), y);
        frame([&](ImGuiIO& io) {
            io.MousePos = ImVec2(x + 1, y);
            io.MouseDown[0] = true;
        });
        frame([&](ImGuiIO& io) {
            io.MousePos = ImVec2(x + 1, y);
            io.MouseDown[0] = false;
        });
    }
    frame(); // focus request queued by the open is applied on the next frame
    frame();
    if (!g_state.sessionPanel.renameActive)
        fail("S10: rename box did not open");
    {
        std::string r0(g_state.sessionPanel.renameBuf);
        if (r0 != "Alpha refactor")
            fail("S10: buffer right after open ('" + r0 + "')");
    }
    {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        if (g.InputTextState.Stb.select_start != 0 || g.InputTextState.Stb.select_end != 14)
            fail("S10: code-focused rename input is not pre-selected "
                 "(select " +
                 std::to_string(g.InputTextState.Stb.select_start) + ".." +
                 std::to_string(g.InputTextState.Stb.select_end) + ")");
    }
    frame([](ImGuiIO& io) { io.AddInputCharacter('!'); });
    idle(1);
    {
        std::string r(g_state.sessionPanel.renameBuf);
        if (r != "!")
            fail("S10: typing did not replace the pre-selected title ('" + r + "')");
    }
    pressKey(ImGuiKey_Escape);
    idle(1);
    if (g_state.sessionPanel.renameActive)
        fail("S10 (R21): first Esc did not close the rename box");
    if (filter() != "alp")
        fail("S10 (R21): first Esc consumed by rename but cleared the filter too");
    {
        Grid g = grid();
        if (findRow(g, "Alpha refactor [3]") < 0)
            fail("S10: filtered list lost after closing the rename box");
        if (findRow(g, "Beta deploy") >= 0)
            fail("S10: filter no longer applied (full list shown)");
    }
    pressKey(ImGuiKey_Escape);
    idle(1);
    if (!filterIsEmpty())
        fail("S10: second Esc did not clear the filter");
}

// (h) Rename box + filter coexistence: clicking the filter unfocuses but does
//      NOT close the box while the active row stays visible.
void scenario11_rename_coexist() {
    g_phase = "S11 (h) rename + filter coexist";
    resetClean();
    clickRowByTitle("Rename");
    idle(2);
    if (!g_state.sessionPanel.renameActive)
        fail("S11: rename box did not open");
    clickFilter();  // box stays open, just unfocused
    typeAll("alp"); // s001 still visible -> A28 must NOT fire
    idle(1);
    if (!g_state.sessionPanel.renameActive)
        fail("S11 (h): rename box closed while its row is still visible");
    if (filter() != "alp")
        fail("S11: filter not set while coexisting with the rename box");
    pressKey(ImGuiKey_Escape); // closes rename first
    idle(1);
    if (g_state.sessionPanel.renameActive)
        fail("S11: Esc did not close the rename box first");
    if (filter() != "alp")
        fail("S11: Esc cleared the filter instead of closing the rename box");
    pressKey(ImGuiKey_Escape);
    idle(1);
    if (!filterIsEmpty())
        fail("S11: filter not cleared on the second Esc");
}

// (d2) Enter on a single match that IS the active session: no-op.
void scenario12_enter_single_active() {
    g_phase = "S12 Enter single active match = no-op";
    clickFilter();
    typeAll("alp");
    idle(1);
    pressKey(ImGuiKey_Enter);
    idle(1);
    if (actionCount() != 0)
        fail("S12: Enter on the already-active single match must not queue a Select");
    if (!filterIsEmpty())
        fail("S12: filter not cleared after no-op Enter");
}

// N7 + R25: multi-byte title/query render valid UTF-8; matched bytes (the
// full "Caf\u00E9" including both bytes of \u00E9) carry the match highlight,
// unmatched characters keep the row color.
void scenario13_multibyte() {
    g_phase = "S13 N7/R25 multi-byte + highlight";
    clickFilter();
    typeAll("caf");
    idle(1);
    {
        Grid g = grid();
        allRowsValidUtf8(g, "S13");
        int y = findRow(g, "Caf\u00E9 au lait [2]");
        if (y < 0)
            fail("S13: 'Caf\u00E9 au lait [2]' row not found");
        if (findRow(g, "lambda caf\u00E9 index") < 0)
            fail("S13: preview row not rendered for the multi-byte session");
        for (const char* hidden : {"Alpha refactor", "Mu cleanup", "Kappa monitor"})
            if (findRow(g, hidden) >= 0)
                fail(std::string("S13: extra row for 'caf': ") + hidden);

        int x = firstNonSpace(g, y);
        if (x < 0)
            fail("S13: title row empty");
        // Row: C a f \u00E9 ' ' a u ' ' l a i t ' ' [ 2 ]
        short fc = fgAt(g, x + 0, y);
        short fa = fgAt(g, x + 1, y);
        short ff = fgAt(g, x + 2, y);
        short fe = fgAt(g, x + 3, y);
        short fn = fgAt(g, x + 4, y); // space after \u00E9
        if (g.ch[y * g.nx + x + 3] != 0xE9)
            fail("S13: \u00E9 cell not U+00E9 (got " + std::to_string(g.ch[y * g.nx + x + 3]) +
                 ")");
        if (!(fc == fa && fa == ff && ff == fe))
            fail("S13: matched run C a f \u00E9 not uniformly highlighted (fgs=" +
                 std::to_string(fc) + "," + std::to_string(fa) + "," + std::to_string(ff) + "," +
                 std::to_string(fe) + ")");
        if (fn == fc)
            fail("S13: unmatched character carries the match highlight");
        // The 'a' of "au" (x+6) must keep the row color, not the match color.
        short fau = fgAt(g, x + 6, y);
        if (fau == fc)
            fail("S13: non-matched 'a' (in 'au') carries the match highlight");
    }
    // Multi-byte QUERY: "caf\u00E9" highlights the accented character too.
    pressKey(ImGuiKey_Backspace);
    pressKey(ImGuiKey_Backspace);
    pressKey(ImGuiKey_Backspace);
    idle(1);
    typeAll("caf\u00E9");
    idle(1);
    if (filter() != "caf\u00E9")
        fail("S13: multi-byte query not stored in the filter ('" + filter() + "')");
    {
        Grid g = grid();
        int y = findRow(g, "Caf\u00E9 au lait [2]");
        if (y < 0)
            fail("S13: row not found for multi-byte query");
        int x = firstNonSpace(g, y);
        if (!(fgAt(g, x, y) == fgAt(g, x + 3, y)))
            fail("S13: \u00E9 not highlighted for the multi-byte query");
    }
    pressKey(ImGuiKey_Escape);
    idle(1);
}

// A27: a deeply-negative raw fuzzy score is clamped to 0 (row still shown,
// last); A19: the 186-char title truncates with "..." and a valid label.
void scenario14_weak_match_and_truncation() {
    g_phase = "S14 A27 clamp + A19 truncation";
    clickFilter();
    typeAll("abz");
    idle(1);
    {
        Grid g = grid();
        allRowsValidUtf8(g, "S14");
        int y = findRow(g, " [1]");
        if (y < 0)
            fail("S14: weak-match long-title row not shown");
        std::string label = rowText(g, y);
        // The row carries a one-cell left margin before the label; locate
        // the label start rather than assuming column 0.
        const std::size_t p = label.find_first_not_of(' ');
        if (p != std::string::npos && label.compare(p, 4, "aaaa") == 0) {
            if (label.find("...") == std::string::npos)
                fail("S14: long title not ellipsized: '" + label + "'");
            if (label.find(" [1]") == std::string::npos)
                fail("S14: count suffix missing: '" + label + "'");
        } else {
            fail("S14: truncated label does not start with the title: '" + label + "'");
        }
        if (findRow(g, "Alpha refactor") >= 0)
            fail("S14: non-matching row visible for 'abz'");
    }
    // A perfect match outranks the clamped weak match.
    auto v = seedSessions();
    v.push_back(mk("s016-15", "abz quick", "", 1));
    setSnapshot(std::move(v));
    idle(2);
    {
        Grid g = grid();
        int yq = findRow(g, "abz quick [1]");
        int yw = findRow(g, "aaaa");
        if (yq < 0 || yw < 0 || yq >= yw)
            fail("S14: perfect match must outrank the clamped weak match (yq=" +
                 std::to_string(yq) + ", yw=" + std::to_string(yw) + ")");
    }
    resetClean();
}

// A19: typing far beyond the 63-byte budget keeps the buffer at 63 bytes,
// the filter keeps working, and nothing crashes.
void scenario15_long_query() {
    g_phase = "S15 A19 query > 64 bytes";
    clickFilter();
    std::string q(80, 'a');
    typeAll(q);
    idle(1);
    if (filter().size() != 63)
        fail("S15: filter not clamped to 63 bytes (size " + std::to_string(filter().size()) + ")");
    {
        Grid g = grid();
        allRowsValidUtf8(g, "S15");
        if (findRow(g, "no matches") < 0)
            fail("S15: 'no matches' missing for the 63-byte query");
    }
    pressKey(ImGuiKey_Escape);
    idle(1);
    if (!filterIsEmpty())
        fail("S15: 63-byte filter not cleared by Esc");
}

// Task 11 (A31): while the filter InputText is active, arrow keys must not
// move the keyboard nav focus. The nav id captured after focusing the filter
// must be stable across Up/Down presses and idle frames.
void scenario17_nav_stability() {
    g_phase = "S17 nav stability (A31)";
    resetClean();
    clickFilter();
    typeAll("e");
    idle(1);
    if (filter() != "e")
        fail("S17: filter not focused for typing");
    int n1 = navId();
    if (n1 == 0)
        fail("S17: nav id not set while the filter is active");
    int a1 = activeId();
    if (a1 == 0)
        fail("S17: no active id while the filter is active");
    idle(2);
    if (navId() != n1 || activeId() != a1)
        fail("S17: nav/active id drifted on idle frames before any arrow");
    for (int i = 0; i < 3; ++i) {
        pressKey(ImGuiKey_DownArrow);
        idle(1);
        if (navId() != n1)
            fail("S17 (Down): nav id moved while the filter is active (" + std::to_string(n1) +
                 " -> " + std::to_string(navId()) + ")");
        if (activeId() != a1)
            fail("S17 (Down): active id moved while the filter is active");
    }
    for (int i = 0; i < 3; ++i) {
        pressKey(ImGuiKey_UpArrow);
        idle(1);
        if (navId() != n1)
            fail("S17 (Up): nav id moved while the filter is active (" + std::to_string(n1) +
                 " -> " + std::to_string(navId()) + ")");
        if (activeId() != a1)
            fail("S17 (Up): active id moved while the filter is active");
    }
    // The filter text is untouched by the arrows.
    if (filter() != "e")
        fail("S17: arrow keys altered the filter text");
    pressKey(ImGuiKey_Escape);
    idle(1);
}

// C10: after a programmatic clear (Esc), re-typing must re-apply exactly
// (fresh InputText id via filterSeq); a second Esc leaves the field empty
// (no stale text resurfacing).
void scenario18_reapply_after_clear() {
    g_phase = "S18 C10 re-apply after clear";
    clickFilter();
    typeAll("eta");
    idle(1);
    int rows1 = 0;
    {
        Grid g = grid();
        for (int y = 0; y < g.ny; ++y)
            if (rowText(g, y, 0, 30).find(" [") != std::string::npos)
                ++rows1;
    }
    if (rows1 != 7)
        fail("S18: expected 7 filtered rows, got " + std::to_string(rows1));
    int seq0 = g_state.sessionPanel.filterSeq;
    pressKey(ImGuiKey_Escape);
    idle(1);
    int seq1 = g_state.sessionPanel.filterSeq;
    if (!filterIsEmpty())
        fail("S18: filter not empty after Esc");
    // C10: the Esc clear bumps the id suffix, so the recreated input starts
    // from a fresh (empty) state and can never revert to the cleared query.
    if (seq1 <= seq0)
        fail("S18: filterSeq did not advance on the Esc clear");
    clickFilter();
    typeAll("eta");
    idle(1);
    {
        Grid g = grid();
        int rows2 = 0;
        for (int y = 0; y < g.ny; ++y)
            if (rowText(g, y, 0, 30).find(" [") != std::string::npos)
                ++rows2;
        if (rows2 != 7)
            fail("S18: filter did not re-apply after programmatic clear (rows " +
                 std::to_string(rows2) + ", expected " + std::to_string(rows1) + ")");
        if (findRow(g, "Eta migrate [7]") < 0)
            fail("S18: re-applied filter shows wrong top row");
    }
    pressKey(ImGuiKey_Escape);
    idle(1);
    {
        Grid g = grid();
        std::string frow = rowText(g, sepRowY() + 1, 0, 30);
        if (frow.find("eta") != std::string::npos)
            fail("S18: stale 'eta' resurfaced in the filter field: '" + frow + "'");
        if (findRow(g, "Alpha refactor [3]") < 0)
            fail("S18: full list not restored");
    }
}

// A23: closing the panel keeps the filter; reopening re-applies it.
void scenario20_close_reopen() {
    g_phase = "S20 A23 close/reopen";
    clickFilter();
    typeAll("eta");
    idle(1);
    clickRowByTitle("Close"); // the Close button shares the header row
    idle(1);
    if (g_state.sessionPanel.panelOpen)
        fail("S20: panel not closed");
    // A23: the buffer keeps the query across the close - the filter is
    // preserved, not lost.
    if (filterIsEmpty())
        fail("S20 (A23): filter lost when the panel was closed");
    {
        Grid g = grid();
        if (findRow(g, "Open") < 0)
            fail("S20: collapsed 'Open' strip not rendered");
        if (findRow(g, "Eta migrate") >= 0)
            fail("S20: session rows rendered while the panel is closed");
    }
    int y = findRow(grid(), "Open");
    int x = firstNonSpace(grid(), y);
    click(x + 1, y);
    idle(2);
    if (!g_state.sessionPanel.panelOpen)
        fail("S20: panel not reopened");
    {
        Grid g = grid();
        if (findRow(g, "Eta migrate [7]") < 0)
            fail("S20 (A23): filter not re-applied after reopen");
        // The filter field shows the surviving query text.
        if (rowText(g, sepRowY() + 1, 0, 30).find("eta") == std::string::npos)
            fail("S20: filter field empty after reopen");
        if (findRow(g, "Alpha refactor") >= 0)
            fail("S20: non-matching rows visible after reopen");
    }
    pressKey(ImGuiKey_Escape);
    idle(1);
    resetClean();
}

// A23/H1: a pipeline (agent streams) occupying the left slot hides the whole
// session panel; the filter survives; clearing the agents re-applies it.
void scenario21_pipeline_occupancy() {
    g_phase = "S21 A23 pipeline occupancy";
    clickFilter();
    typeAll("eta");
    idle(1);
    g_state.left.agents.push_back(AgentStream());
    idle(2);
    {
        Grid g = grid();
        if (findRow(g, "Sessions") >= 0)
            fail("S21: session panel header rendered while agents are running");
        if (findRow(g, "Eta migrate") >= 0)
            fail("S21: session rows rendered while the pipeline owns the left slot");
    }
    // A23: the buffer keeps the query while the pipeline hides the panel.
    if (filterIsEmpty())
        fail("S21 (A23): filter lost while the pipeline occupied the panel");
    g_state.left.agents.clear();
    idle(2);
    {
        Grid g = grid();
        if (findRow(g, "Eta migrate [7]") < 0)
            fail("S21: filter not re-applied after the pipeline cleared");
    }
    pressKey(ImGuiKey_Escape);
    idle(1);
}

// Empty snapshot: no rows and (correctly) no "no matches" block either.
void scenario22_empty_snapshot() {
    g_phase = "S22 empty snapshot";
    clickFilter();
    typeAll("zzq");
    idle(1);
    setSnapshot({});
    idle(2);
    {
        Grid g = grid();
        if (findRow(g, "no matches") >= 0)
            fail("S22: 'no matches' shown for an empty snapshot");
        if (findRow(g, " [") >= 0)
            fail("S22: a session row rendered from an empty snapshot");
        if (sepRowY() < 0)
            fail("S22: panel header missing for an empty snapshot");
        allRowsValidUtf8(g, "S22");
    }
    resetClean();
}

// The TUI log (isLogActive) must carry the filter lifecycle lines.
void scenario23_log_lines() {
    g_phase = "S23 log verification";
    FILE* f = std::fopen("llmfun_ui_log.txt", "rb");
    if (!f)
        fail("S23: llmfun_ui_log.txt not found (logging off?)");
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        all.append(buf, n);
    std::fclose(f);
    for (const char* needle :
         {"filter cleared (Escape)", "filter queued select", "pending select",
          "flushed pending select", "rename closed (active row filtered out, A28)",
          "rename cancelled"}) {
        if (all.find(needle) == std::string::npos)
            fail(std::string("S23: log line missing: '") + needle + "'");
    }
}

} // namespace

// ------------------------------------------------------------------ harness
// init/shutdown: mirror llmfun::tui::tuiInit/tuiShutdown minus the ncurses
// terminal (no initscr/getmaxyx/DrawScreen). Same context + theme + text
// backend setup, and the exact KeyMap table ImTui_ImplNcurses_Init installs
// (imtui-impl-ncurses.cpp:123-147) so pressKey() maps to the same
// io.KeysDown indices as the real app. DisplaySize is fixed at 80x24 — the
// PTY winsize the real app receives in an 80x24 terminal.

void harnessInit() {
    std::setlocale(LC_ALL, "");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    llmfun::tui::applyTheme();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.KeyMap[ImGuiKey_Tab] = 9;
    io.KeyMap[ImGuiKey_LeftArrow] = 260;
    io.KeyMap[ImGuiKey_RightArrow] = 261;
    io.KeyMap[ImGuiKey_UpArrow] = 259;
    io.KeyMap[ImGuiKey_DownArrow] = 258;
    io.KeyMap[ImGuiKey_PageUp] = 339;
    io.KeyMap[ImGuiKey_PageDown] = 338;
    io.KeyMap[ImGuiKey_Home] = 262;
    io.KeyMap[ImGuiKey_End] = 360;
    io.KeyMap[ImGuiKey_Insert] = 331;
    io.KeyMap[ImGuiKey_Delete] = 330;
    io.KeyMap[ImGuiKey_Backspace] = 263;
    io.KeyMap[ImGuiKey_Space] = 32;
    io.KeyMap[ImGuiKey_Enter] = 10;
    io.KeyMap[ImGuiKey_Escape] = 27;
    io.KeyMap[ImGuiKey_KeyPadEnter] = 343;
    io.KeyMap[ImGuiKey_A] = 1;
    io.KeyMap[ImGuiKey_C] = 3;
    io.KeyMap[ImGuiKey_V] = 22;
    io.KeyMap[ImGuiKey_X] = 24;
    io.KeyMap[ImGuiKey_Y] = 25;
    io.KeyMap[ImGuiKey_Z] = 26;
    io.KeyRepeatDelay = 0.050f;
    io.KeyRepeatRate = 0.050f;
    ImTui_ImplText_Init();
    io.DisplaySize = ImVec2(80.0f, 24.0f);
}

void harnessShutdown() {
    ImTui_ImplText_Shutdown();
    ImGui::DestroyContext();
}

int main() {
    g_state = TuiState();
    g_state.iniFilename = ""; // no ini persistence
    harnessInit();
    // The TScreen grid is resized by the first ImTui_ImplText_RenderDrawData
    // from the current DisplaySize (80x24), so render one frame before
    // checking the size.
    idle(1);
    if (g_screen.nx != 80 || g_screen.ny != 24) {
        std::fprintf(stderr, "unexpected display size %dx%d (want 80x24)\n", g_screen.nx,
                     g_screen.ny);
        harnessShutdown();
        return 2;
    }
    llmfun::tui::tuiSetLogging(g_state, true);
    setSnapshot(seedSessions());
    g_state.sessionPanel.activeId = "s001-0";

    scenario0_calibration();
    scenario16_focus();
    scenario1_filter_ranks();
    scenario2_no_match();
    scenario3_esc_clears();
    scenario4_enter_selects_top();
    scenario5_click_row();
    scenario6_enter_busy();
    scenario7_click_busy_last_wins();
    scenario8_snapshot_updates();
    scenario9_a28_rename_closes();
    scenario10_esc_priority();
    scenario11_rename_coexist();
    scenario12_enter_single_active();
    scenario13_multibyte();
    scenario14_weak_match_and_truncation();
    scenario15_long_query();
    scenario17_nav_stability();
    scenario18_reapply_after_clear();
    scenario20_close_reopen();
    scenario21_pipeline_occupancy();
    scenario22_empty_snapshot();
    scenario23_log_lines();

    harnessShutdown();
    std::printf("OK: all session-filter scenarios passed (%d frames)\n", g_frames);
    return 0;
}
