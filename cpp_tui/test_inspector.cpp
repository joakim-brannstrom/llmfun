// test_inspector.cpp
//
// The inspector panel (views/inspector.h, design §4.8 / D-INSPECTOR / D-ESC /
// D-UI-CONF, task 29): the right-hand frame, its two tabs, the final key numbering,
// the persisted open state and tab, the overlay at narrow widths and the per-tab
// empty states.
//
// What this binary proves, in order:
//
//   1. the source. `views/inspector.cpp` writes no colour literal, takes the profile
//      from the metrics (never `tuiIsTextGrid()`), reads no display size, names no v4
//      type and does not include `tui.h` - the layering every view is held to;
//   2. the vocabulary. Two tabs, two names, `[`/`]` wrapping in both directions, and
//      the two empty sentences the task specifies;
//   3. the keys. `Ctrl+I` toggles, `Ctrl+5`/`Ctrl+6` select the two tabs, `Ctrl+1`/
//      `Ctrl+2` are the reserved digits and select *nothing*; every row is unique and
//      every action of the registry is still reachable (`./test_keymap`'s R1/R2 rule);
//   4. the toggle rule. `-1 auto` counts as visible, a press writes 0 or 1 and never
//      back to auto - the rule `Ctrl+B` follows for the rail;
//   5. the layout. `-1 auto` is docked at 100x30 and closed at 80x24; an explicit open
//      keeps the 90 %-wide right-hand panel at 80x24 (and still leaves the regions
//      underneath where they were);
//   6. the frame. `renderShell()` derives the four inputs - up, floating, tab, unread
//      count - and applies the tab the surface asked for;
//   7. the Esc step. While the panel floats `Esc` closes it and does not cancel the
//      run; the docked column is not an `Esc` target; the palette closes first;
//   8. the screen. The strip, the active tab's fill, the empty state, the unread badge
//      and the reserved-digit notice, drawn through the real text backend, nothing
//      painted outside the panel's rectangle;
//   9. the tab fallback. A queued `[`/`]` switches the tab (the terminal's only spelling
//      of `Ctrl+5`/`Ctrl+6`), and a focused text field keeps its characters;
//  10. the scale. The body renders the viewport plus one row of overscan;
//  11. the profile. `InspectorOpen=`/`InspectorTab=` survive a restart through
//      `ui.conf` - load, change, debounced flush, load again;
//  12. the goldens. Agents (with a badge), Activity (badge cleared), the 80x24 overlay
//      and the reserved-digit notice.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = usage error.
//
//   ./test_inspector [-v] [--update] [--print]
//
// `--update` rewrites the fixtures; do that only for a deliberate rendering change, and
// commit the diff with it. They are captured with `LLMFUN_ASCII=0`, so a CJK locale or
// `TERM=linux` cannot change a golden.

#include "views/inspector.h"
#include "views/palette.h"
#include "views/shell.h"

#include "tui.h"

#include "model/keymap.h"
#include "model/settings.h"
#include "model/ui_state.h"
#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/text.h"
#include "ui/tokens.h"

#include "imtui/imtui.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace ui = llmfun::tui::ui;
namespace views = llmfun::tui::views;
namespace model = llmfun::tui::model;
namespace tui = llmfun::tui;

namespace {

int g_checks = 0;
int g_failures = 0;
bool g_verbose = false;
bool g_print = false;

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
    std::fprintf(stderr, "usage: %s [-v] [--update] [--print]   (%s)\n", argv0, problem);
    std::exit(2);
}

std::string num(float v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.3f", double(v));
    std::string text = buffer;
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text;
}

std::string trimTrailingSpaces(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
    return text;
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

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path);
    if (!in.good())
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
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
    std::vector<uint8_t> fg;
    std::vector<uint8_t> bg;

    uint32_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return ch[static_cast<std::size_t>(y) * nx + x];
    }
    uint8_t bgAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return bg[static_cast<std::size_t>(y) * nx + x];
    }
    uint8_t fgAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return fg[static_cast<std::size_t>(y) * nx + x];
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

    bool hasText(const std::string& needle) const {
        return allText().find(needle) != std::string::npos;
    }

    /// The row a needle stands on, or -1. The *first* row it appears on.
    int rowOf(const std::string& needle) const {
        for (int y = 0; y < ny; ++y) {
            if (rowText(y).find(needle) != std::string::npos)
                return y;
        }
        return -1;
    }

    /// The column a needle starts at on row `y`, or -1.
    int colOf(int y, const std::string& needle) const {
        const std::string row = rowText(y);
        const std::size_t at = row.find(needle);
        return at == std::string::npos ? -1 : static_cast<int>(at);
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

/// One frame of the text grid at `g_w x g_h`: a decoration-free root window, the body
/// drawing inside it, then the frame through the real backend. `inject` runs *before*
/// `NewFrame`, which is when ImGui consumes its input queue.
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
    ImGui::Begin("inspector-frame", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    body(ui::metricsFor(true, 1.0f, 1.0f));
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    if (g_print) {
        const Grid grid = capture();
        std::printf("--- frame %dx%d\n", g_w, g_h);
        for (int y = 0; y < grid.ny; ++y)
            std::printf("%2d |%s|\n", y, grid.rowText(y).c_str());
    }
    return capture();
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

const ui::Metrics& gridMetrics() {
    static const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    return m;
}

/// The panel's rectangle at `w x h` for `prefs` - the geometry the frame really answers,
/// through the same `shellLayoutFor()` it calls (auto rule included).
ui::Rect inspectorRect(int w, int h, const model::ShellPrefs& prefs, bool& overlay) {
    ui::LayoutPrefs layoutPrefs;
    layoutPrefs.inspectorOpen = prefs.inspectorOpen != 0;
    const ui::ShellLayout layout =
        views::shellLayoutFor(layoutPrefs, ImVec2(static_cast<float>(w), static_cast<float>(h)),
                              /*inspectorAuto=*/prefs.inspectorOpen < 0);
    overlay = layout.inspectorOverlay;
    return layout.inspector;
}

views::InspectorState openInspector(model::InspectorTab tab) {
    views::InspectorState state;
    state.open = true;
    state.tab = tab;
    return state;
}

/// The lane list these scenes have: none. The Agents tab's rows arrive with task 30, and the
/// scenes here are the panel's *frame* - its strip, its badge, its empty state and its
/// geometry - so a lane list would change the subject. `./test_agents_tab` draws the lanes.
const std::vector<model::AgentLane>& noLanes() {
    static const std::vector<model::AgentLane> lanes;
    return lanes;
}

/// The grid a scene paints, through the whole shell: the panel never positions itself, so
/// the screen is the only honest proof that it sits where the layout says.
views::ShellFrame shellFrame(tui::TuiState& state, int w, int h,
                             const std::function<void(ImGuiIO&)>& inject = nullptr) {
    views::ShellFrame out;
    g_w = w;
    g_h = h;
    frame(
        [&](const ui::Metrics&) {
            out = views::renderShell(state, ImVec2(static_cast<float>(w), static_cast<float>(h)));
        },
        inject);
    return out;
}

/// The same frame, plus the cell grid it painted: paint *order* is only visible on the screen.
views::ShellFrame
shellFrameGrid(tui::TuiState& state, int w, int h, Grid& grid,
               const std::function<void(ImGuiIO&, const ui::Metrics&)>& body = nullptr) {
    views::ShellFrame out;
    g_w = w;
    g_h = h;
    frame([&](const ui::Metrics& m) {
        if (body)
            body(ImGui::GetIO(), m);
        out = views::renderShell(state, ImVec2(static_cast<float>(w), static_cast<float>(h)));
    });
    grid = capture();
    return out;
}

bool inside(const views::InspectorPart& part, const ui::Rect& box) {
    if (part.empty())
        return true;
    return part.x >= box.x - 0.01f && part.y >= box.y - 0.01f &&
           part.right() <= box.right() + 0.01f && part.bottom() <= box.bottom() + 0.01f;
}

// ---------------------------------------------------------------------------
// 1. The source
// ---------------------------------------------------------------------------

void checkSource() {
    const std::string source = readWhole(TEST_INSPECTOR_SOURCE);
    check(!source.empty(), "views/inspector.cpp is readable");
    check(source.find("tuiIsTextGrid") == std::string::npos,
          "the panel takes the profile from the metrics, never from tuiIsTextGrid()");
    check(source.find("ImVec4(") == std::string::npos,
          "the panel writes no colour literal (D-THEME)");
    check(source.find("DisplaySize") == std::string::npos,
          "the panel reads no display size (a view is a geometry taker)");
    check(source.find("ChatMessage") == std::string::npos &&
              source.find("RenderGroup") == std::string::npos,
          "the panel names no v4 type (D-ADAPTER)");
    check(source.find("#include \"tui.h\"") == std::string::npos,
          "the panel does not include tui.h (views/ names no aggregate)");
    {
        const std::size_t pin = source.find("SetScrollHereY");
        check(pin == std::string::npos ||
                  source.find("activityScrollToEnd", pin) != std::string::npos,
              "the body is not pinned to the bottom every frame (D-FOLLOW)");
        // The call, not the word: the comment above the call names the function too.
        const std::string pinCall = "SetScrollHereY(1.0f)";
        const std::size_t firstCall = source.find(pinCall);
        check(firstCall == std::string::npos ||
                  source.find(pinCall, firstCall + 1) == std::string::npos,
              "the one pin is the Activity tab's one-shot catch-up, never a second site");
    }
    check(source.find("splitStatusText") == std::string::npos,
          "the panel formats no status: it draws the values it is given");

    // The frame layer's half of the wiring, which a headless harness cannot exercise (it needs
    // a backend to run a frame loop): the `ui.conf` lifecycle and the new actions are called
    // from `tui.cpp`, and nowhere else.
    const std::string frame = readWhole(TEST_INSPECTOR_FRAME_SOURCE);
    check(!frame.empty(), "tui.cpp is readable");
    check(frame.find("views::uiConfFrame(") != std::string::npos,
          "the frame layer reads and flushes ui.conf (views/shell.h owns the call)");
    check(frame.find("views::uiConfQuit(") != std::string::npos,
          "...and saves it on the quit path");
    // D-UI-CONF: there is one renderer, and the frame layer owns the file - one read/flush per
    // frame and two save sites (the quit frame and the destroy path, which every exit reaches).
    {
        std::size_t calls = 0;
        for (std::size_t at = frame.find("views::uiConfQuit("); at != std::string::npos;
             at = frame.find("views::uiConfQuit(", at + 1))
            ++calls;
        check(calls == 2,
              "the ui.conf save sits on the quit frame and on the shutdown path (D-UI-CONF)");
    }
    check(frame.find("model::Action::ToggleInspector") != std::string::npos &&
              frame.find("model::Action::InspectorReserved") != std::string::npos,
          "the frame layer handles the inspector's actions");
    check(frame.find("views::escStepClosesOverlay(") != std::string::npos &&
              frame.find("views::closeTopOverlay(") != std::string::npos,
          "and its Esc step asks the shell's predicate instead of re-deciding it");
}

// ---------------------------------------------------------------------------
// 2. The tab vocabulary
// ---------------------------------------------------------------------------

void checkVocabulary() {
    check(std::string(views::inspectorTabName(model::InspectorTab::Agents)) == "agents",
          "the Agents tab names itself agents");
    check(std::string(views::inspectorTabName(model::InspectorTab::Activity)) == "activity",
          "the Activity tab names itself activity");
    check(std::string(views::inspectorEmptyText(model::InspectorTab::Agents)) ==
              std::string("No agents this run"),
          "the Agents empty state is the task's sentence");
    check(std::string(views::inspectorEmptyText(model::InspectorTab::Activity)) ==
              std::string("Nothing to report yet"),
          "the Activity empty state is the task's sentence");
    check(views::inspectorNextTab(model::InspectorTab::Agents) == model::InspectorTab::Activity,
          "] moves to the next tab");
    check(views::inspectorNextTab(model::InspectorTab::Activity) == model::InspectorTab::Agents,
          "...and wraps");
    check(views::inspectorPreviousTab(model::InspectorTab::Activity) == model::InspectorTab::Agents,
          "[ moves to the previous tab");
    check(views::kInspectorAgentsDigit == 5 && views::kInspectorActivityDigit == 6,
          "the shipped tab digits are 5 and 6 (D-INSPECTOR)");
    check(std::string(views::kInspectorReservedHint) == std::string("not available yet"),
          "a reserved digit's notice is the task's sentence");
}

// ---------------------------------------------------------------------------
// 3. The keys
// ---------------------------------------------------------------------------

void checkKeymap() {
    const model::Keymap& keymap = model::defaultKeymap();
    auto actionOf = [&keymap](ImGuiKeyChord chord) {
        return model::resolve(keymap, chord, model::Focus::None);
    };
    check(actionOf(ImGuiMod_Ctrl | ImGuiKey_I) == model::Action::ToggleInspector,
          "Ctrl+I toggles the inspector");
    check(actionOf(ImGuiMod_Ctrl | ImGuiKey_5) == model::Action::InspectorAgents,
          "Ctrl+5 selects the Agents tab");
    check(actionOf(ImGuiMod_Ctrl | ImGuiKey_6) == model::Action::InspectorActivity,
          "Ctrl+6 selects the Activity tab");
    check(actionOf(ImGuiMod_Ctrl | ImGuiKey_1) == model::Action::InspectorReserved &&
              actionOf(ImGuiMod_Ctrl | ImGuiKey_2) == model::Action::InspectorReserved,
          "Ctrl+1 and Ctrl+2 are the reserved digits");

    // No shipped binding was renumbered: the digits a user could reach before still resolve
    // to what they did, and the reserved pair resolves to *one* action rather than to a tab.
    check(actionOf(ImGuiMod_Ctrl | ImGuiKey_K) == model::Action::Palette,
          "Ctrl+K is still the palette");
    check(actionOf(ImGuiMod_Ctrl | ImGuiKey_B) == model::Action::ToggleRail,
          "Ctrl+B is still the rail toggle");
    check(actionOf(ImGuiMod_Ctrl | ImGuiKey_Q) == model::Action::Quit, "Ctrl+Q is still quit");
    check(actionOf(ImGuiMod_Ctrl | ImGuiKey_5) != model::Action::InspectorReserved,
          "a shipped tab digit is not the reserved action");

    // The registry's own rules: one row per (chord, scope), and every action reachable.
    bool duplicate = false;
    for (std::size_t i = 0; i < keymap.count && !duplicate; ++i) {
        for (std::size_t j = i + 1; j < keymap.count; ++j) {
            if (keymap.rows[i].chord == keymap.rows[j].chord &&
                keymap.rows[i].when == keymap.rows[j].when) {
                duplicate = true;
                break;
            }
        }
    }
    check(!duplicate, "every (chord, scope) pair is still bound once (test_keymap's R1)");
    bool reachable[static_cast<int>(model::Action::Count)] = {};
    for (const model::Binding& row : keymap) {
        for (const model::Focus focus :
             {model::Focus::None, model::Focus::Composer, model::Focus::Rail, model::Focus::Thread,
              model::Focus::Overlay, model::Focus::RawLog}) {
            const model::Action action = model::resolve(keymap, row.chord, focus);
            if (action != model::Action::None)
                reachable[static_cast<int>(action)] = true;
        }
    }
    // The `Esc` answers are reached through the resolver, not through a row - the same two lines
    // `./test_keymap` needs for the same reason (and the focus-mode step, task 43, is the fourth:
    // it exists only as a chain step).
    for (const model::EscapeContext& context :
         {model::EscapeContext{true, false, false}, model::EscapeContext{false, true, false},
          model::EscapeContext{false, false, true}, model::EscapeContext{false, false, false},
          model::EscapeContext{false, false, false, true}}) {
        reachable[static_cast<int>(model::resolveEscape(context))] = true;
    }
    bool allReachable = true;
    for (int i = 1; i < static_cast<int>(model::Action::Count); ++i) {
        if (!reachable[i])
            allReachable = false;
    }
    check(allReachable, "every Action is still reachable through the table (R2)");
}

// ---------------------------------------------------------------------------
// 4. The toggle rule and the persisted tab
// ---------------------------------------------------------------------------

void checkToggle() {
    model::ShellPrefs prefs;
    check(prefs.inspectorOpen == -1, "first run: the inspector preference is auto");
    check(prefs.tab == model::InspectorTab::Activity, "first run: the Activity tab is up");

    // The input is what is *on screen*, not the preference: `-1 auto` is a docked panel on a
    // wide display and no panel at all on a narrow one, so the same preference means two
    // different things and the toggle must be told which one it is looking at.
    model::toggleInspector(prefs, /*visible=*/true);
    check(prefs.inspectorOpen == 0, "a visible panel closes on the press");
    model::toggleInspector(prefs, /*visible=*/false);
    check(prefs.inspectorOpen == 1, "an invisible one opens it explicitly");
    model::toggleInspector(prefs, /*visible=*/true);
    check(prefs.inspectorOpen == 0, "a press never writes back -1 auto");

    prefs.inspectorOpen = -1;
    model::toggleInspector(prefs, /*visible=*/false);
    check(prefs.inspectorOpen == 1,
          "auto on a narrow display (not visible) opens on the *first* press");
    prefs.inspectorOpen = -1;
    model::toggleInspector(prefs, /*visible=*/true);
    check(prefs.inspectorOpen == 0, "...and auto on a wide display closes on the first press");
}

// ---------------------------------------------------------------------------
// 5. The layout: the auto rule, the explicit open and the 80x24 overlay
// ---------------------------------------------------------------------------

void checkLayout() {
    const ui::Metrics& m = gridMetrics();
    model::ShellPrefs prefs;

    bool overlay = false;
    const ui::Rect autoWide = inspectorRect(100, 30, prefs, overlay);
    check(autoWide.w == m.inspectorW && !overlay,
          "auto at 100x30: the docked column (" + num(autoWide.w) + " cells)");
    const ui::Rect autoNarrow = inspectorRect(80, 24, prefs, overlay);
    check(autoNarrow.w <= 0.0f, "auto at 80x24: closed, not a panel nobody asked for");

    prefs.inspectorOpen = 1;
    const ui::Rect wide = inspectorRect(100, 30, prefs, overlay);
    check(wide.w == m.inspectorW && !overlay && wide.right() == 100.0f,
          "explicit open at 100x30: the same docked column, flush with the right edge");
    const ui::Rect narrow = inspectorRect(80, 24, prefs, overlay);
    check(overlay, "explicit open at 80x24: the floating shape");
    check(narrow.w == 72.0f, "the panel is 90 % of the display (" + num(narrow.w) + " cells)");
    check(narrow.right() == 80.0f, "anchored on the right edge");
    check(narrow.y == 1.0f && narrow.h == 22.0f,
          "and spans the top bar's bottom to the now bar's top (" + num(narrow.h) + " rows)");

    // The regions underneath do not move: the panel is an overlay, not a tiling change.
    ui::LayoutPrefs closedPrefs;
    ui::LayoutPrefs openPrefs;
    openPrefs.inspectorOpen = true;
    const ui::ShellLayout closedLayout = ui::computeShell(m, ImVec2(80.0f, 24.0f), closedPrefs);
    const ui::ShellLayout openLayout = ui::computeShell(m, ImVec2(80.0f, 24.0f), openPrefs);
    check(closedLayout.thread.w == openLayout.thread.w &&
              closedLayout.composer.h == openLayout.composer.h,
          "the thread and the composer keep their rectangles under the panel");
    check(openLayout.top == ui::OverlayLayer::Inspector,
          "the panel is the topmost overlay while it floats");
}

// ---------------------------------------------------------------------------
// 6. The frame: the four derived inputs, and the tab the surface asked for
// ---------------------------------------------------------------------------

void checkFrame() {
    tui::TuiState state;
    const ui::Metrics& m = gridMetrics();

    // Auto, wide: docked and open.
    views::ShellFrame frame = shellFrame(state, 100, 30);
    check(state.inspector.open, "the frame opens the panel where auto means docked");
    check(!state.inspector.overlay, "...and says it is not floating");
    check(frame.layout.inspector.w == m.inspectorW, "...on the layout's own column");
    check(state.inspector.tab == state.ui.prefs.tab,
          "the drawn tab is the preference's (" +
              std::string(views::inspectorTabName(state.inspector.tab)) + ")");

    // A saved tab wins.
    state.ui.prefs.tab = model::InspectorTab::Agents;
    shellFrame(state, 100, 30);
    check(state.inspector.tab == model::InspectorTab::Agents, "a saved tab is the drawn tab");

    // Auto, narrow: closed - and the flag follows.
    tui::TuiState narrow;
    shellFrame(narrow, 80, 24);
    check(!narrow.inspector.open, "the frame closes the panel at 80x24 under auto");

    // Explicit, narrow: the floating shape.
    narrow.ui.prefs.inspectorOpen = 1;
    const views::ShellFrame floating = shellFrame(narrow, 80, 24);
    check(narrow.inspector.open && narrow.inspector.overlay,
          "an explicit open at 80x24 floats the panel");
    check(floating.layout.inspector.w == 72.0f, "on the 90 % panel rectangle");

    // The surface asks for a tab; the frame writes the preference in the same frame.
    tui::TuiState requested;
    requested.ui.prefs.inspectorOpen = 1;
    shellFrame(requested, 100, 30);
    requested.inspector.requestTab(model::InspectorTab::Agents);
    requested.ui.prefs.tab = model::InspectorTab::Activity;
    shellFrame(requested, 100, 30);
    check(requested.ui.prefs.tab == model::InspectorTab::Agents,
          "the tab the surface asked for becomes the preference");
    check(!requested.inspector.tabRequestPending, "...and the request is consumed once");
    shellFrame(requested, 100, 30);
    check(requested.inspector.tab == model::InspectorTab::Agents, "the next frame draws it");

    // A tab press also shows a closed panel: selecting a tab nothing displays is a press
    // that does nothing.
    tui::TuiState closed;
    closed.ui.prefs.inspectorOpen = 0;
    shellFrame(closed, 100, 30);
    check(!closed.inspector.open, "premise: a closed panel at 100x30");
    closed.inspector.requestTab(model::InspectorTab::Activity);
    shellFrame(closed, 100, 30);
    check(closed.ui.prefs.inspectorOpen == 1, "a tab press opens the panel explicitly");

    // The toggle's input is what is *drawn*: with `-1 auto` the panel is closed at 80x24 and
    // docked at 100x30, so one press must open the first and close the second - the same
    // preference, two shapes, and a toggle that read the preference would need two presses on
    // the narrow display where the panel is hardest to reach.
    tui::TuiState toggling;
    shellFrame(toggling, 80, 24);
    check(!toggling.inspector.open, "premise: auto at 80x24 is closed");
    model::toggleInspector(toggling.ui.prefs, toggling.inspector.open);
    shellFrame(toggling, 80, 24);
    check(toggling.inspector.open, "one press from auto at 80x24 opens the panel");
    model::toggleInspector(toggling.ui.prefs, toggling.inspector.open);
    shellFrame(toggling, 80, 24);
    check(!toggling.inspector.open, "and the next press closes it");

    tui::TuiState wide;
    shellFrame(wide, 100, 30);
    check(wide.inspector.open, "premise: auto at 100x30 is docked");
    model::toggleInspector(wide.ui.prefs, wide.inspector.open);
    shellFrame(wide, 100, 30);
    check(!wide.inspector.open, "one press from auto at 100x30 closes it");
}

// ---------------------------------------------------------------------------
// 7. The Esc step
// ---------------------------------------------------------------------------

void checkEscape() {
    tui::TuiState state;
    state.ui.prefs.inspectorOpen = 1;
    state.readyStatus = false; // a run is in flight: Esc's step 2 is available
    shellFrame(state, 80, 24);
    check(state.inspector.open && state.inspector.overlay, "premise: the panel floats");
    check(views::escStepClosesOverlay(state), "Esc's first step applies to it");
    const model::Action step = model::resolveEscape(
        {views::escStepClosesOverlay(state), tui::cancelStepOpen(state), false});
    check(step == model::Action::CloseOverlay,
          "so Esc closes the overlay instead of cancelling the run");
    check(views::closeTopOverlay(state) == ui::OverlayLayer::Inspector,
          "and the close names the inspector");
    check(state.ui.prefs.inspectorOpen == 0,
          "the close is explicit (0), so it cannot re-open on the next resize");
    check(!state.cancelRequested, "the run was not cancelled (one step per press)");
    shellFrame(state, 80, 24);
    check(!state.inspector.open, "the panel is gone on the next frame");
    check(!views::escStepClosesOverlay(state), "and Esc no longer targets it");

    // The docked column is not an Esc target: it is one of the six regions.
    tui::TuiState docked;
    docked.ui.prefs.inspectorOpen = 1;
    shellFrame(docked, 100, 30);
    check(docked.inspector.open && !docked.inspector.overlay, "premise: the docked column");
    check(!views::escStepClosesOverlay(docked), "Esc does not close the docked column");
    check(views::closeTopOverlay(docked) == ui::OverlayLayer::None,
          "...and there is nothing for it to close");
    check(docked.ui.prefs.inspectorOpen == 1, "...so the preference is untouched");

    // The palette closes first: it is the topmost surface of the two.
    tui::TuiState both;
    both.ui.prefs.inspectorOpen = 1;
    shellFrame(both, 80, 24);
    views::openPaletteForState(both.palette, both, "");
    check(both.palette.open && both.inspector.overlay, "premise: both surfaces are up");
    check(views::closeTopOverlay(both) == ui::OverlayLayer::Palette,
          "Esc closes the palette first (the design's z-order)");
    check(both.ui.prefs.inspectorOpen == 1, "and leaves the panel alone");

    // A focused composer does not spend a press the panel's step owns: with the panel
    // floating, `Esc` is the overlay's, so the view's chain stands down and the frame layer's
    // step is what runs (D-ESC: one step per press). The press still reaches the field's
    // widget (ImGui's `cancel_edit` reverts the buffer inside it), which is why the composer
    // restores the draft on the overlay path - the typed case below.
    tui::TuiState draft;
    draft.ui.prefs.inspectorOpen = 1;
    draft.userQuery.inputBuf = "half a question";
    shellFrame(draft, 80, 24);
    check(draft.inspector.overlay, "premise: the panel floats over a draft");
    // The press itself, delivered the way a backend delivers it.
    shellFrame(draft, 80, 24, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, true); });
    check(!draft.composer.escapeConsumed && !draft.escapeConsumed,
          "the composer's chain stands down while the panel owns the press");
    shellFrame(draft, 80, 24, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, false); });
    check(views::closeTopOverlay(draft) == ui::OverlayLayer::Inspector,
          "so the frame's step closes the panel");

    // The draft the panel floats over survives the press. The text has to be typed *after*
    // the field took focus: `cancel_edit` restores the activation snapshot, so a draft that
    // was already in the buffer when the field activated would pass without the fix.
    tui::TuiState typed;
    typed.ui.prefs.inspectorOpen = 1;
    shellFrame(typed, 80, 24);
    shellFrame(typed, 80, 24);
    check(typed.composer.focused, "premise: the field holds the keyboard under the panel");
    shellFrame(typed, 80, 24, [](ImGuiIO& io) { io.AddInputCharactersUTF8("peek"); });
    check(typed.userQuery.inputBuf == "peek", "premise: the draft is the field's");
    // The press, then the step the frame layer performs on it.
    shellFrame(typed, 80, 24, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, true); });
    check(typed.userQuery.inputBuf == "peek",
          "one Esc closes the panel without eating the draft under it");
    check(typed.composer.focusRequested || typed.composer.focused,
          "and the field asks for the keyboard back");
    check(views::closeTopOverlay(typed) == ui::OverlayLayer::Inspector,
          "the frame's step closes the panel");
    shellFrame(typed, 80, 24, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, false); });
    shellFrame(typed, 80, 24); // the one-shot focus request is consumed on this frame
    check(typed.composer.focused, "the field is the keyboard's again");
    shellFrame(typed, 80, 24, [](ImGuiIO& io) { io.AddInputCharactersUTF8("!"); });
    check(typed.userQuery.inputBuf == "peek!",
          "and the next keystroke lands where the user left off");
}

// ---------------------------------------------------------------------------
// 8. The screen
// ---------------------------------------------------------------------------

void checkScreen() {
    const int w = 100;
    const int h = 30;
    model::ShellPrefs prefs;
    bool overlay = false;
    const ui::Rect rect = inspectorRect(w, h, prefs, overlay);

    // Agents, drawn alone into the layout's rectangle: nothing outside it.
    views::InspectorState agents = openInspector(model::InspectorTab::Agents);
    agents.unreadActivity = 2;
    g_w = w;
    g_h = h;
    const Grid alone = frame(
        [&](const ui::Metrics& m) { views::renderInspector(agents, noLanes(), rect, m, true); });
    bool stray = false;
    for (int y = 0; y < alone.ny && !stray; ++y) {
        for (int x = 0; x < alone.nx; ++x) {
            if (alone.at(x, y) == 0 && alone.bgAt(x, y) == 0)
                continue;
            const bool insideRect =
                static_cast<float>(x) >= rect.x && static_cast<float>(x) < rect.right() &&
                static_cast<float>(y) >= rect.y && static_cast<float>(y) < rect.bottom();
            if (!insideRect)
                stray = true;
        }
    }
    check(!stray, "the panel paints nothing outside its rectangle");

    const int agentsRow = alone.rowOf("[5] Agents");
    check(agentsRow >= 0, "the strip names the Agents tab with its digit");
    check(alone.colOf(agentsRow, "[6] Activity") >= 0, "...and the Activity tab");
    const int agentsCol = alone.colOf(agentsRow, "[5] Agents");
    const int activityCol = alone.colOf(agentsRow, "[6] Activity");

    // The active tab is inverted: the selection fill behind its label, and only there. The
    // panel's own surface fill covers every cell of the region, so the question is not "is
    // there a fill" but "is the active label's fill the other one".
    check(agentsRow >= 0 && agentsCol >= 0 && activityCol >= 0,
          "premise: both tabs are on the strip row");
    check(alone.bgAt(agentsCol + 1, agentsRow) != alone.bgAt(activityCol + 1, agentsRow),
          "the active tab's label sits on a different fill than the inactive one (reverse video)");
    check(alone.fgAt(agentsCol + 4, agentsRow) != alone.fgAt(activityCol + 4, agentsRow),
          "and the two labels are in different inks");
    check(alone.hasText("No agents this run"), "the Agents tab draws its empty state");

    // The measured parts are inside the panel - the geometry the screen above was drawn from.
    views::InspectorParts parts;
    g_w = w;
    g_h = h;
    frame([&](const ui::Metrics& m) {
        views::InspectorState drawn = openInspector(model::InspectorTab::Agents);
        drawn.unreadActivity = 2;
        parts = views::inspectorMeasure(drawn, noLanes(), rect, m);
    });
    check(inside(parts.agents, rect) && inside(parts.activity, rect) && inside(parts.badge, rect) &&
              inside(parts.body, rect),
          "every measured part is inside the panel's rectangle");
    check(!parts.badge.empty() && parts.badgeText == std::string("\u2022"
                                                                 "2"),
          "the badge is the unread mark and its count (" + parts.badgeText + ")");
    check(parts.bodyRows == 1, "the body has its empty-state row");
    check(parts.emptyText == std::string("No agents this run"),
          "and names the state the tab draws");

    // A narrow docked panel still fits the strip: the full anatomy is the 28-cell column.
    const float stripW = parts.agents.w + parts.activity.w + 3.0f;
    check(stripW <= rect.w,
          "the two tabs fit the docked column (" + num(stripW) + " of " + num(rect.w) + " cells)");

    // The badge: the unread count beside the Activity label.
    check(alone.hasText("\u2022"
                        "2"),
          "the unread count is drawn beside the Activity tab");

    // Activity: the other sentence.
    g_w = w;
    g_h = h;
    const Grid activityGrid = frame([&](const ui::Metrics& m) {
        views::InspectorState activity = openInspector(model::InspectorTab::Activity);
        views::renderInspector(activity, noLanes(), rect, m, true);
    });
    check(activityGrid.hasText("Nothing to report yet"), "the Activity tab draws its empty state");

    // The reserved digit's notice: a second strip row, and no tab changed.
    views::InspectorState reserved = openInspector(model::InspectorTab::Agents);
    reserved.reservedPressed = true;
    g_w = w;
    g_h = h;
    const Grid notice = frame(
        [&](const ui::Metrics& m) { views::renderInspector(reserved, noLanes(), rect, m, true); });
    check(notice.hasText("not available yet"), "a reserved digit shows the notice");
    check(notice.rowOf("not available yet") == agentsRow + 1,
          "the notice is the strip's second row");

    // A closed panel paints nothing at all.
    views::InspectorState shut;
    g_w = w;
    g_h = h;
    const Grid blank = frame(
        [&](const ui::Metrics& m) { views::renderInspector(shut, noLanes(), rect, m, true); });
    check(blank.rowText(blank.ny / 2).find_first_not_of(' ') == std::string::npos,
          "a closed panel paints nothing");
    check(shut.drawnRows == 0, "...and draws no rows");
}

// ---------------------------------------------------------------------------
// 8b. Paint order: a floating panel is above the tiling (D-OVERLAY)
// ---------------------------------------------------------------------------

void checkPaintOrder() {
    const int w = 80;
    const int h = 24;
    tui::TuiState state;
    state.ui.prefs.inspectorOpen = 1;
    // A draft with words the composer would draw: if any of them lands inside the panel's
    // rectangle, the composer painted over the panel.
    state.userQuery.inputBuf = "a draft the composer must not paint over the panel";

    Grid grid;
    const views::ShellFrame frameOut = shellFrameGrid(state, w, h, grid);
    const ui::Rect panel = frameOut.layout.inspector;
    const ui::Rect composer = frameOut.layout.composer;
    check(state.inspector.overlay, "premise: the panel floats");
    check(panel.bottom() > composer.y, "premise: the panel's rectangle spans over the composer (" +
                                           num(panel.bottom()) + " > " + num(composer.y) + ")");

    bool composerInsidePanel = false;
    bool composerVisibleBelowPanel = false;
    for (int y = static_cast<int>(composer.y); y < static_cast<int>(composer.bottom()); ++y) {
        const std::string row = grid.rowText(y);
        const std::string inside =
            row.substr(static_cast<std::size_t>(panel.x), static_cast<std::size_t>(panel.w));
        if (inside.find_first_not_of(' ') != std::string::npos)
            composerInsidePanel = true;
        // The part of the composer left of the panel is still the composer's: the panel covers
        // it from its own left edge, and nothing beyond that.
        if (row.substr(0, static_cast<std::size_t>(panel.x)).find_first_not_of(' ') !=
            std::string::npos)
            composerVisibleBelowPanel = true;
    }
    check(!composerInsidePanel,
          "no composer row survives inside the floating panel (not even a child widget's text)");
    check(composerVisibleBelowPanel, "and the composer keeps the columns the panel does not cover");

    // The band is the panel's own for its whole height: the fill is the same from the first
    // body row down to the last one (the bottom row is the hairline and is checked below).
    // This scene draws no reserved-digit notice, so the body starts one row below the strip.
    const int probeX = static_cast<int>(panel.x) + 1;
    const views::InspectorParts paintParts =
        views::inspectorMeasure(state.inspector, {}, panel, ui::metricsFor(true, 1.0f, 1.0f));
    const int firstBodyRow = static_cast<int>(paintParts.body.y);
    const uint8_t fill = grid.bgAt(probeX, firstBodyRow);
    bool uniform = fill != 0;
    for (int y = firstBodyRow; y < static_cast<int>(panel.bottom()) - 1; ++y) {
        if (grid.bgAt(probeX, y) != fill)
            uniform = false;
    }
    check(uniform, "the panel's fill runs to its last body row (nothing repaints it)");

    // The bottom row is the panel's hairline, a different fill from the body by design - so it
    // needs its own assertion, and it is the row the composer used to cut off.
    const int hairRow = static_cast<int>(panel.bottom()) - 1;
    const uint8_t hair = grid.bgAt(static_cast<int>(panel.x), hairRow);
    bool hairline = hair != 0;
    for (int x = static_cast<int>(panel.x); x < static_cast<int>(panel.right()); ++x) {
        if (grid.bgAt(x, hairRow) != hair)
            hairline = false;
    }
    check(hair != fill, "the bottom row is the hairline, not the body fill");
    check(hairline, "and the hairline runs the panel's whole width (nothing repaints it)");

    // The same panel at the pixel profile's window minimum: 640-1199 px is the overlay band, so
    // the shape and the overlap exist there too - and the pixels come from the layout, which is
    // pure (`metricsFor(false, ...)`), so this needs no screen.
    const ui::Metrics px = ui::metricsFor(false, 1.0f, 1.0f);
    ui::LayoutPrefs prefs;
    prefs.inspectorOpen = true;
    const ui::ShellLayout pxLayout = ui::computeShell(px, ImVec2(800.0f, 600.0f), prefs);
    check(pxLayout.inspectorOverlay, "800x600 px: the panel is the overlay shape");
    check(pxLayout.inspector.w == 720.0f && pxLayout.inspector.right() == 800.0f,
          "90 % wide, anchored right (" + num(pxLayout.inspector.w) + " px)");
    check(pxLayout.inspector.y == pxLayout.topBar.bottom() &&
              pxLayout.inspector.bottom() == pxLayout.nowBar.y,
          "and it spans the top bar to the now bar, over the composer");
    check(pxLayout.top == ui::OverlayLayer::Inspector, "the panel is the topmost overlay");
}

// ---------------------------------------------------------------------------
// 8c. A reserved digit with the panel closed: the notice has to be visible
// ---------------------------------------------------------------------------

void checkReservedPress() {
    tui::TuiState state;
    state.ui.prefs.inspectorOpen = 0;
    shellFrame(state, 100, 30);
    check(!state.inspector.open, "premise: the panel is closed");

    // The frame layer's handler (`Action::InspectorReserved` -> `views::pressReservedDigit()`).
    views::pressReservedDigit(state);
    check(state.inspector.reservedPressed, "the press is remembered");
    check(state.ui.prefs.inspectorOpen == 1,
          "and it opens the panel, because the notice lives in the strip");

    Grid grid;
    shellFrameGrid(state, 100, 30, grid);
    check(state.inspector.open, "the next frame draws the panel");
    check(grid.hasText("not available yet"), "with the reserved digit's notice in it");
    check(state.inspector.tab == model::InspectorTab::Activity, "and nothing was selected");

    // A notice is not a mode: selecting a tab clears it.
    state.inspector.requestTab(model::InspectorTab::Agents);
    shellFrame(state, 100, 30);
    check(!state.inspector.reservedPressed, "a tab selection clears the notice");
}

// ---------------------------------------------------------------------------
// 9. The tab fallback: `[`/`]`, and a focused field keeps its characters
// ---------------------------------------------------------------------------

void checkTabKeys() {
    tui::TuiState state;
    state.ui.prefs.inspectorOpen = 1;
    shellFrame(state, 100, 30);
    check(state.ui.prefs.tab == model::InspectorTab::Activity, "premise: Activity is up");

    // The composer's field takes the keyboard on its first frame, and a character key typed
    // into a focused field is typing (D-KEYMAP's plain-key rule) - so the fallback is live
    // exactly where the rail's own character keys are: with no text field focused, which `Esc`
    // produces (D-ESC's last step). The harness states that fact directly rather than staging a
    // keystroke: `WantTextInput` is what the view's `ui::takeCharacter()` asks.
    auto blurredFrame = [&state](ImWchar queued) {
        g_w = 100;
        g_h = 30;
        frame(
            [&state](const ui::Metrics&) {
                ImGui::GetIO().WantTextInput = false;
                views::renderShell(state, ImVec2(100.0f, 30.0f));
            },
            [queued](ImGuiIO& io) {
                if (queued != 0)
                    io.InputQueueCharacters.push_back(queued);
            });
    };

    blurredFrame(static_cast<ImWchar>('['));
    check(state.ui.prefs.tab == model::InspectorTab::Agents,
          "`[` moves to the previous tab (the terminal's spelling of Ctrl+5)");
    blurredFrame(0);
    check(state.inspector.tab == model::InspectorTab::Agents, "and the next frame draws it");

    blurredFrame(static_cast<ImWchar>(']'));
    check(state.ui.prefs.tab == model::InspectorTab::Activity, "`]` moves forward again");

    // A text field that owns the keyboard keeps its characters: `[` in the composer is typing.
    tui::TuiState typing;
    typing.ui.prefs.inspectorOpen = 1;
    g_w = 100;
    g_h = 30;
    frame(
        [&](const ui::Metrics& m) {
            ImGui::GetIO().WantTextInput = true;
            views::renderShell(typing, ImVec2(100.0f, 30.0f));
            (void)m;
        },
        [](ImGuiIO& io) { io.InputQueueCharacters.push_back(static_cast<ImWchar>('[')); });
    check(typing.ui.prefs.tab == model::InspectorTab::Activity,
          "a focused text field keeps `[` (the plain-key rule)");

    // A surface that owns the `Esc` chain's first step keeps the character keys too: with the
    // palette up (and no text input, as right after a click on a result row) `]` must not switch
    // the tab behind it - the view asks the shell's `acceptKeys` before it reads the queue.
    tui::TuiState behind;
    behind.ui.prefs.inspectorOpen = 1;
    shellFrame(behind, 80, 24);
    views::openPaletteForState(behind.palette, behind, "");
    g_w = 80;
    g_h = 24;
    frame(
        [&](const ui::Metrics& m) {
            ImGui::GetIO().WantTextInput = false;
            views::renderShell(behind, ImVec2(80.0f, 24.0f));
            (void)m;
        },
        [](ImGuiIO& io) { io.InputQueueCharacters.push_back(static_cast<ImWchar>(']')); });
    check(behind.palette.open, "premise: the palette is up");
    check(behind.ui.prefs.tab == model::InspectorTab::Activity,
          "the palette keeps `]`: the panel behind it does not switch tabs");
}

// ---------------------------------------------------------------------------
// 10. Scale: the body renders the viewport plus one row of overscan
// ---------------------------------------------------------------------------

void checkBounded() {
    views::InspectorWindow empty = views::inspectorWindow(0, 0.0f, 20.0f, 1.0f);
    check(empty.count == 0, "no rows: nothing to draw");
    views::InspectorWindow one = views::inspectorWindow(1, 0.0f, 20.0f, 1.0f);
    check(one.first == 0 && one.count == 1, "one row in a tall viewport: that row");
    views::InspectorWindow viewport = views::inspectorWindow(100, 0.0f, 20.0f, 1.0f);
    check(viewport.first == 0 && viewport.count == 21,
          "a viewport of 20 rows over 100 draws 20 plus one of overscan (" +
              std::to_string(viewport.count) + ")");
    views::InspectorWindow scrolled = views::inspectorWindow(100, 40.0f, 20.0f, 1.0f);
    check(scrolled.first == 40 && scrolled.count == 21,
          "the window follows the scroll offset, with the same overscan");
    views::InspectorWindow end = views::inspectorWindow(100, 90.0f, 20.0f, 1.0f);
    check(end.first == 90 && end.count == 10,
          "and the clamp keeps it inside the rows (" + std::to_string(end.count) + ")");
    views::InspectorWindow bad = views::inspectorWindow(10, 0.0f, 20.0f, 0.0f);
    check(bad.count == 0, "a zero row height draws nothing rather than dividing by zero");

    // A fractional scroll offset touches a partially visible row above *and* below, so the bound
    // there is one row wider than the grid's: `ceil(span) + 2`, never more. The pixel profile
    // scrolls smoothly, so this is the normal case there rather than a corner.
    views::InspectorWindow smooth = views::inspectorWindow(100, 40.5f, 20.0f, 1.0f);
    check(smooth.first == 40 && smooth.count == 22,
          "a fractional offset draws the partial row above and below (" +
              std::to_string(smooth.count) + " rows)");
    check(static_cast<float>(smooth.count) <= std::ceil(20.0f / 1.0f) + 2.0f,
          "which is the documented `ceil(span) + 2` bound");

    // In a real frame: a tall body draws its one row, never more than the viewport + 1.
    model::ShellPrefs prefs;
    bool overlay = false;
    const ui::Rect rect = inspectorRect(100, 30, prefs, overlay);
    views::InspectorState state = openInspector(model::InspectorTab::Agents);
    g_w = 100;
    g_h = 30;
    frame([&](const ui::Metrics& m) { views::renderInspector(state, noLanes(), rect, m, true); });
    const float viewportRows = 20.0f; // the body is taller than one row at 30 rows
    check(state.drawnRows >= 1, "a real draw counts the rows it drew");
    check(static_cast<float>(state.drawnRows) <= viewportRows + 1.0f,
          "and never more than the viewport plus one row (" + std::to_string(state.drawnRows) +
              ")");
}

// ---------------------------------------------------------------------------
// 10b. The pin prune (D-UI-CONF): `Pin.<id>` lines for sessions the store lost
// ---------------------------------------------------------------------------

void checkPins() {
    tui::TuiState state;
    state.ui.prefs.pinned = {"gone", "kept"};
    // A snapshot with one of the two sessions. Nothing is pruned before a snapshot arrives
    // (a loading store would lose the pins it has not listed yet), so the first frame is the
    // moment the id set becomes known.
    tui::SessionEntry entry;
    entry.id = "kept";
    entry.title = "Kept session";
    entry.updatedAtMs = 1;
    state.sessionPanel.sessions.push_back(entry);

    shellFrame(state, 100, 30);
    check(state.uiConfPinsPruned, "the first complete snapshot prunes the pins");
    check(state.ui.prefs.pinned.size() == 1 && state.ui.prefs.pinned.count("kept") == 1,
          "the pin of a session the store no longer has is dropped");
    check(state.ui.prefs.pinned.count("gone") == 0, "...and only that one");

    // One-shot: a pin added afterwards is not pruned by the next snapshot, even though the
    // snapshot does not name it (it may be a session the list is still paging in).
    state.ui.prefs.pinned.insert("later");
    shellFrame(state, 100, 30);
    check(state.ui.prefs.pinned.count("later") == 1, "the prune happens once, not every frame");
}

// ---------------------------------------------------------------------------
// 11. The profile: `ui.conf` survives a restart
// ---------------------------------------------------------------------------

const char* kScratch = "test_inspector_scratch";

void makeScratch() {
    ::mkdir(kScratch, 0755);
    const std::string dir = std::string(kScratch) + "/profile";
    ::mkdir(dir.c_str(), 0755);
}

std::string profileIni() { return std::string(kScratch) + "/profile/imgui2.ini"; }

void checkPersistence() {
    makeScratch();
    const std::string ini = profileIni();
    std::remove((std::string(kScratch) + "/profile/ui.conf").c_str());

    model::UiState freshRun;
    bool loaded = false;
    views::uiConfFrame(freshRun, loaded, ini, 0.0);
    check(loaded, "the first frame reads the profile");
    check(freshRun.prefs.inspectorOpen == -1, "a missing file keeps the defaults (auto)");

    // The user's choices: the panel open, the Agents tab. One press from `-1 auto` opens it -
    // the press means "not visible right now, show me" - and it is recorded as an explicit 1.
    model::toggleInspector(freshRun.prefs, /*visible=*/false);
    check(freshRun.prefs.inspectorOpen == 1, "premise: the user opened the panel by hand");
    freshRun.prefs.tab = model::InspectorTab::Agents;
    views::uiConfFrame(freshRun, loaded, ini, 1.0); // the change opens the window
    views::uiConfFrame(freshRun, loaded, ini, 1.5); // still inside it: no write
    std::string text;
    check(!readFile(std::string(kScratch) + "/profile/ui.conf", text),
          "nothing is written inside the idle window");
    views::uiConfFrame(freshRun, loaded, ini, 4.0); // idle long enough
    check(readFile(std::string(kScratch) + "/profile/ui.conf", text),
          "the debounced flush writes the file once the user stops");

    // A restart: the same profile, a new state.
    model::UiState restart;
    bool restartLoaded = false;
    views::uiConfFrame(restart, restartLoaded, ini, 0.0);
    check(restart.prefs.inspectorOpen == 1, "InspectorOpen survives the restart");
    check(restart.prefs.tab == model::InspectorTab::Agents, "InspectorTab survives it too");

    // The quit path saves without waiting for the window.
    model::UiState quitting;
    bool quitLoaded = false;
    views::uiConfFrame(quitting, quitLoaded, ini, 0.0);
    quitting.prefs.tab = model::InspectorTab::Activity;
    views::uiConfQuit(quitting, ini);
    model::UiState afterQuit;
    bool afterQuitLoaded = false;
    views::uiConfFrame(afterQuit, afterQuitLoaded, ini, 0.0);
    check(afterQuit.prefs.tab == model::InspectorTab::Activity,
          "a quit two seconds after a change still saves it");

    // An empty profile name (every headless harness) touches nothing.
    model::UiState noProfile;
    bool noProfileLoaded = false;
    views::uiConfFrame(noProfile, noProfileLoaded, "", 0.0);
    check(!noProfileLoaded, "an empty ini filename is no profile: nothing is read");
    views::uiConfQuit(noProfile, "");
    check(true, "and the quit save is a no-op for it");

    // The close path (a GUI window's X never sets `quitRequested`, so `tuiDestroyState()` is
    // the one place every exit reaches): the sheet's choice is saved there (D-UI-CONF).
    tui::TuiState workbench;
    workbench.iniFilename = ini;
    workbench.ui.prefs.tab = model::InspectorTab::Agents;
    tui::saveUiConfOnShutdown(workbench);
    model::UiState afterClose;
    bool afterCloseLoaded = false;
    views::uiConfFrame(afterClose, afterCloseLoaded, ini, 0.0);
    check(afterClose.prefs.tab == model::InspectorTab::Agents,
          "the close path saves the workbench's choice");
}

// ---------------------------------------------------------------------------
// 12. The goldens
// ---------------------------------------------------------------------------

struct Scene {
    const char* name;
    model::InspectorTab tab;
    int inspectorOpen;
    int unread;
    bool reserved;
    int w;
    int h;
};

std::vector<Scene> scenes() {
    return {
        // Docked, Agents up, two records unread behind it.
        {"inspector-agents", model::InspectorTab::Agents, 1, 2, false, 100, 30},
        // Docked, Activity up: the badge cleared itself the moment the tab was shown.
        {"inspector-activity", model::InspectorTab::Activity, 1, 2, false, 100, 30},
        // Narrow: the panel floats from the right, 90 % wide, over an unchanged tiling.
        {"inspector-overlay", model::InspectorTab::Agents, 1, 0, false, 80, 24},
        // A reserved digit: nothing selected, and the strip says so.
        {"inspector-reserved", model::InspectorTab::Agents, 1, 0, true, 100, 30},
    };
}

std::string goldenText(const Scene& scene) {
    tui::TuiState state;
    state.ui.prefs.inspectorOpen = scene.inspectorOpen;
    state.ui.prefs.tab = scene.tab;
    // The unread count is the frame's: a record arrives while the tab is not the one on
    // screen, so the counter is advanced by pushing real records through the API.
    // The unread count is the frame's: a record arrives while the tab is not the one on screen.
    // The records go through the projection's own entry point with no clock (`--:--:--`), so
    // this golden cannot embed the wall clock or the reader's zone - `./test_activity` is the
    // binary that pins both and draws real times.
    for (int i = 0; i < scene.unread; ++i) {
        model::LogRecord record;
        record.timestampMs = 0;
        record.level = model::kActivityLevelInfo;
        record.message = "record " + std::to_string(i);
        model::appendActivity(state.ui, record, record.level, /*visible=*/false);
    }
    // The reserved-digit notice, as the frame layer sets it from `Ctrl+1`/`Ctrl+2`.
    state.inspector.reservedPressed = scene.reserved;

    // Two frames, and the second one is captured. Every harness shares one ImGui context, so a
    // single frame would inherit whatever active id / nav request the checks above left behind -
    // it decided, for example, whether the composer's field is focused and the golden showed a
    // caret or the placeholder. The scene's own first frame consumes the composer's focus
    // request, so the second is a function of this scene and nothing else.
    shellFrame(state, scene.w, scene.h);
    const views::ShellFrame frameOut = shellFrame(state, scene.w, scene.h);
    const Grid grid = capture();
    const ui::Metrics& m = gridMetrics();
    // The record list is passed here too: the header must describe the panel that was
    // captured, not an empty one (task-31 review M1).
    const views::InspectorParts parts = views::inspectorMeasure(
        state.inspector, noLanes(), frameOut.layout.inspector, m, state.ui.activity);

    std::string text = "name: ";
    text += scene.name;
    text += "\nscreen: " + std::to_string(scene.w) + "x" + std::to_string(scene.h);
    text += "\ninspector: x=" + num(frameOut.layout.inspector.x) +
            " y=" + num(frameOut.layout.inspector.y) + " w=" + num(frameOut.layout.inspector.w) +
            " h=" + num(frameOut.layout.inspector.h);
    text += "\nopen: " + std::string(state.inspector.open ? "1" : "0");
    text += " overlay: " + std::string(state.inspector.overlay ? "1" : "0");
    text += " tab: " + std::string(views::inspectorTabName(state.inspector.tab));
    text += " unread: " + std::to_string(state.inspector.unreadActivity);
    text += "\nparts: agents=" + num(parts.agents.w) + " activity=" + num(parts.activity.w) +
            " badge=" + num(parts.badge.w) + " notice=" + num(parts.notice.w) +
            " body=" + num(parts.body.w) + "x" + num(parts.body.h) +
            " rows=" + std::to_string(parts.bodyRows) +
            " drawn=" + std::to_string(state.inspector.drawnRows);
    text += "\nlabels: agents='" + parts.agentsText + "' activity='" + parts.activityText +
            "' badge='" + parts.badgeText + "' notice='" + parts.noticeText + "' empty='" +
            parts.emptyText + "'";
    text += "\ntext:\n";
    for (int y = 0; y < grid.ny; ++y) {
        text += trimTrailingSpaces(grid.rowText(y));
        text += '\n';
    }
    text += "fills:\n";
    for (int y = 0; y < grid.ny; ++y) {
        std::string runs;
        int start = -1;
        int index = -1;
        for (int x = 0; x <= grid.nx; ++x) {
            const int value = x < grid.nx ? static_cast<int>(grid.bgAt(x, y)) : -1;
            const bool same = start >= 0 && value == index;
            if (start >= 0 && !same) {
                runs += (runs.empty() ? "" : ", ") + std::to_string(start) + "-" +
                        std::to_string(x - 1) + "=" + std::to_string(index);
                start = -1;
            }
            if (value > 0 && start < 0) {
                start = x;
                index = value;
            } else if (value <= 0) {
                start = -1;
            }
        }
        text += std::to_string(y) + ": " + runs + "\n";
    }
    return text;
}

void checkGoldens(bool update) {
    const std::string dir = TEST_INSPECTOR_GOLDEN_DIR;
    for (const Scene& scene : scenes()) {
        const std::string path = dir + "/" + scene.name + "-" + std::to_string(scene.w) + "x" +
                                 std::to_string(scene.h) + ".txt";
        const std::string text = goldenText(scene);
        if (update) {
            check(writeFile(path, text), "golden written: " + path);
            continue;
        }
        std::string expected;
        if (!readFile(path, expected)) {
            check(false, "golden is readable: " + path + " (run with --update to create it)");
            continue;
        }
        if (expected == text) {
            check(true, std::string("the ") + scene.name + " " + std::to_string(scene.w) + "x" +
                            std::to_string(scene.h) + " golden matches");
            continue;
        }
        std::size_t line = 1;
        std::size_t at = 0;
        while (at < expected.size() && at < text.size() && expected[at] == text[at]) {
            line += expected[at] == '\n' ? 1 : 0;
            ++at;
        }
        check(false, std::string("the ") + scene.name + " " + std::to_string(scene.w) + "x" +
                         std::to_string(scene.h) + " golden matches (first difference on line " +
                         std::to_string(line) + ": " + path + ")");
    }
}

} // namespace

int main(int argc, char** argv) {
    bool update = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0)
            g_verbose = true;
        else if (std::strcmp(argv[i], "--update") == 0)
            update = true;
        else if (std::strcmp(argv[i], "--print") == 0)
            g_print = true;
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
            usage(argv[0], "print every check with -v, rewrite the goldens with --update, "
                           "dump the grids with --print");
        else
            usage(argv[0], "unknown argument");
    }

    // One glyph set for every frame, whatever terminal runs this: `LLMFUN_ASCII=0` outranks
    // TERM and the locale in the detection order, so a golden cannot depend on the environment.
    ::setenv("LLMFUN_ASCII", "0", 1);

    ImGui::CreateContext();
    ImTui_ImplText_Init();

    checkSource();
    checkVocabulary();
    checkKeymap();
    checkToggle();
    checkLayout();
    checkFrame();
    checkEscape();
    checkScreen();
    checkPaintOrder();
    checkReservedPress();
    checkTabKeys();
    checkBounded();
    checkPins();
    checkPersistence();
    checkGoldens(update);

    ImGui::DestroyContext();

    if (g_failures == 0) {
        std::printf("test_inspector: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "test_inspector: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
