// test_tui_maxwidth.cpp
//
// Headless clamp test for the TUI max-width cap (implementation plan Task 5,
// design T5). Drives a REAL llmfun::tui::TuiState through the real
// tuiRender() on the imtui TEXT backend — no ncurses, no terminal, no PTY
// (same pattern as test_session_filter_smoke, text backend only) — and
// asserts the clamp math: effective width = min(terminal width, maxWidth).
//
// The clamp under test lives at the top of tuiRender (cpp_tui/tui.cpp): when
// state.maxWidth > 0 and io.DisplaySize.x > state.maxWidth it writes the
// clamped value back to ImGui::GetIO().DisplaySize, which
// ImTui_ImplText_RenderDrawData then consumes to resize the TScreen grid.
// So the grid width (screen.nx) is the observable proof that the clamp took
// effect — DrawScreen (ncurses or text) can then write at most maxWidth
// columns (R2).
//
// Per case: set ImGui::GetIO().DisplaySize = ImVec2(80, 24) (the simulated
// 80x24 terminal), set maxWidth, run ONE frame
// (io reset + ImTui_ImplText_NewFrame + ImGui::NewFrame + tuiRender +
// ImGui::Render + ImTui_ImplText_RenderDrawData), then assert. The write-back
// makes the clamp sticky, so each case re-arms DisplaySize first.
//
// Cases:
//   A (cap below terminal):  maxWidth = 40  -> io.DisplaySize.x == 40
//                                                 AND text grid screen.nx == 40
//   B (default, regression): maxWidth = 0   -> text grid screen.nx == 80
//                                                 (0 = unlimited: byte-identical
//                                                 no-op, terminal width kept)
//   C (cap above terminal):  maxWidth = 120 -> text grid screen.nx == 80
//                                                 (min() behavior: terminal wins)
//   D (C API floor, Task 2
//     carry-forward): C-API tuiSetMaxWidth(30) on a tuiCreateState() handle
//                                                 -> inner->maxWidth == 40,
//                                                 i.e. the floor equals the
//                                                 core MIN_TERMINAL_WIDTH
//                                                 (kCoreMinWidth, mirrored
//                                                 below), and one frame
//                                                 renders at 40 (a floored
//                                                 cap is always renderable —
//                                                 never stuck on the
//                                                 "Terminal too small!"
//                                                 screen). C-API
//                                                 tuiSetMaxWidth(-1) ->
//                                                 unlimited (grid 80).
//
// Exit codes: 0 = all cases pass; 1 = first assertion failure (the grid is
// dumped to stderr); 2 = environment error (init/display size).
//
// Headless (text backend only) — run from the build dir:
//   ./test_tui_maxwidth

#include "imtui/imtui.h" // imgui + ImTui_ImplText_* (imtui-impl-text.h)

#include "tui.h"     // llmfun::tui: TuiState, tuiRender, core tuiSetMaxWidth
#include "tui_api.h" // C API: tuiCreateState/tuiDestroyState/C-API tuiSetMaxWidth

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace llmfun::tui {
// Theme setup defined in tui.cpp, not declared in tui.h (same pattern as
// test_session_filter_smoke).
void applyTheme();
} // namespace llmfun::tui

// Legal completion of the C API's forward declaration
// `typedef struct TuiState TuiState;` (tui_api.h) — layout matches
// tui_api.cpp exactly. Lets the test read back the core state the C API
// wrapped, i.e. the white-box observation point for Case D's floor assert.
struct TuiState {
    ::llmfun::tui::TuiState* inner;
};

// The TUI's minimum render width in columns. MUST stay in sync with
// MIN_TERMINAL_WIDTH in cpp_tui/tui.cpp (tuiRender). Case D asserts the C
// API's positive sub-minimum floor equals this value, so a floored cap
// always yields a renderable width.
static constexpr int kCoreMinWidth = 40;

namespace {

ImTui::TScreen g_screen;
char g_case = '0';
int g_frames = 0;

void fail(const std::string& what) {
    std::fprintf(stderr, "FAIL [case %c @ frame %d]: %s\n", g_case, g_frames, what.c_str());
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

void expect(bool cond, const std::string& what) {
    if (!cond)
        fail(what);
}

// Run exactly one frame on the text backend: the per-frame io reset mirrors
// ImTui_ImplNcurses_NewFrame's input handling (imtui-impl-ncurses.cpp:208-317)
// without a terminal (no keys, no modifiers, mouse at (0,0), 60fps delta —
// same as test_session_filter_smoke's frame driver), then the same pipeline
// as main.cpp minus the ncurses parts:
//     ImTui_ImplText_NewFrame()
//     ImGui::NewFrame()
//     llmfun::tui::tuiRender(state)  // app render; owns the clamp + EndFrame
//     ImGui::Render()
//     ImTui_ImplText_RenderDrawData(drawData, screen)  // into the TScreen grid
void frame(llmfun::tui::TuiState& state) {
    ImGuiIO& io = ImGui::GetIO();
    std::fill(io.KeysDown, io.KeysDown + 512, 0);
    io.KeyCtrl = false;
    io.KeyShift = false;
    io.MousePos = ImVec2(0.0f, 0.0f);
    io.MouseDown[0] = false;
    io.MouseDown[1] = false;
    io.DeltaTime = 1.0f / 60.0f;

    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    if (!llmfun::tui::tuiRender(state))
        fail("tuiRender returned false (quit)");
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    ++g_frames;
}

// Set the simulated terminal to 80x24 (the PTY winsize the real app gets in
// an 80x24 terminal), apply the cap, render one frame.
void renderAt(llmfun::tui::TuiState& state, int maxWidth) {
    ImGui::GetIO().DisplaySize = ImVec2(80.0f, 24.0f);
    llmfun::tui::tuiSetMaxWidth(state, maxWidth); // core setter (no floor)
    frame(state);
}

// Harness init/shutdown: mirror llmfun::tui::tuiInit/tuiShutdown minus the
// ncurses terminal (no initscr/getmaxyx/DrawScreen) — same context + theme +
// text backend setup as test_session_filter_smoke. KeyMap is not installed
// because this test injects no keyboard input. DisplaySize is re-armed per
// case (the clamp under test overwrites it).
void harnessInit() {
    std::setlocale(LC_ALL, "");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    llmfun::tui::applyTheme();
    ImGui::GetIO().IniFilename = nullptr;
    ImTui_ImplText_Init();
    ImGui::GetIO().DisplaySize = ImVec2(80.0f, 24.0f);
}

void harnessShutdown() {
    ImTui_ImplText_Shutdown();
    ImGui::DestroyContext();
}

} // namespace

int main() {
    // Core state for Cases A-C, driven through the C++ core API (the clamp
    // under test); no chat content, so markdown config is never exercised
    // (same default-state assumption as test_session_filter_smoke).
    llmfun::tui::TuiState state;
    state.iniFilename = ""; // no ini persistence

    harnessInit();
    // The TScreen grid is resized by the first ImTui_ImplText_RenderDrawData
    // from the current DisplaySize (80x24), so render one warm-up frame
    // before checking the size.
    frame(state);
    if (g_screen.nx != 80 || g_screen.ny != 24) {
        std::fprintf(stderr, "unexpected display size %dx%d (want 80x24)\n", g_screen.nx,
                     g_screen.ny);
        harnessShutdown();
        return 2;
    }

    // --- Case A: cap below terminal (40 < 80) ---------------------------
    // The clamp engages: io.DisplaySize.x is written back to the cap and
    // RenderDrawData sizes the grid from it.
    g_case = 'A';
    renderAt(state, 40);
    expect(ImGui::GetIO().DisplaySize.x == 40.0f, "case A: io.DisplaySize.x must be clamped to 40");
    expect(g_screen.nx == 40, "case A: text grid nx must be 40");

    // --- Case B: default, regression guard (0 = unlimited) --------------
    // maxWidth = 0 must be a no-op: no write-back, grid at full terminal
    // width — the default stays byte-identical to the pre-cap behavior.
    g_case = 'B';
    renderAt(state, 0);
    expect(ImGui::GetIO().DisplaySize.x == 80.0f,
           "case B: io.DisplaySize.x must stay 80 (default is a no-op)");
    expect(g_screen.nx == 80, "case B: text grid nx must be 80");

    // --- Case C: cap above terminal (120 > 80) --------------------------
    // min() behavior: the terminal width wins; no write-back.
    g_case = 'C';
    renderAt(state, 120);
    expect(ImGui::GetIO().DisplaySize.x == 80.0f,
           "case C: io.DisplaySize.x must stay 80 (terminal wins)");
    expect(g_screen.nx == 80, "case C: text grid nx must be 80");

    // --- Case D: C API floor == core minimum (Task 2 carry-forward) -----
    // The C API tuiSetMaxWidth floors a positive sub-40 cap to the TUI's
    // 40-column minimum render width (a smaller cap would leave the TUI
    // stuck on its "Terminal too small!" screen). Drive the REAL C API
    // handle (tuiCreateState) through tuiRender — exactly how the D caller
    // uses it — and assert the floor equals the core MIN_TERMINAL_WIDTH and
    // that the floored width renders.
    g_case = 'D';
    ::TuiState* cstate = tuiCreateState();
    expect(cstate != nullptr && cstate->inner != nullptr, "case D: tuiCreateState succeeds");
    ::tuiSetMaxWidth(cstate, 30); // positive sub-minimum cap
    expect(cstate->inner->maxWidth == kCoreMinWidth,
           "case D: C API must floor 30 to the core minimum (40)");
    ImGui::GetIO().DisplaySize = ImVec2(80.0f, 24.0f);
    frame(*cstate->inner);
    expect(ImGui::GetIO().DisplaySize.x == 40.0f,
           "case D: effective width must be the core minimum (40)");
    expect(g_screen.nx == 40, "case D: text grid nx must be 40 (renders)");

    // Negative values are unlimited (tui_api.h contract; the core clamp
    // checks > 0, so any non-positive cap is a no-op).
    ::tuiSetMaxWidth(cstate, -1);
    ImGui::GetIO().DisplaySize = ImVec2(80.0f, 24.0f);
    frame(*cstate->inner);
    expect(ImGui::GetIO().DisplaySize.x == 80.0f,
           "case D: negative cap must be unlimited (DisplaySize stays 80)");
    expect(g_screen.nx == 80, "case D: negative cap must be unlimited (grid 80)");

    tuiDestroyState(cstate);

    harnessShutdown();
    std::printf("OK: all max-width clamp cases passed (%d frames)\n", g_frames);
    return 0;
}
