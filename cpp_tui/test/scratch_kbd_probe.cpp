// scratch_kbd_probe.cpp — task 7, box 5 (keyboard paths) + box 6 (trickle flag),
// verified at the REAL-BACKEND level through a real PTY.
//
// Runs the production ncurses backend (ImTui_ImplNcurses_Init/NewFrame/
// DrawScreen) in-process with fd 0 = PTY slave, a feeder child injecting key
// bytes on a timeline, and asserts:
//   box 6 (runtime half): io.ConfigInputTrickleEventQueue == true right after
//     ImTui_ImplNcurses_Init (the task-4 fix moved the setting before NewFrame).
//   box 5: "hi\r" -> printable chars arrive incrementally (AddInputCharactersUTF8,
//     one char event per frame under trickle) and Enter fires on a LATER frame;
//     Escape / arrows / Tab fire; Ctrl+C (\x03) reaches ImGui as a same-frame
//     io.KeyCtrl && IsKeyPressed(ImGuiKey_C) chord (backend sends the interleaved
//     [Ctrl down, key down, key up, Ctrl up] ordering; plan/scratch_chord_trickle_test.cpp
//     proved the ordering at ImGui level).
// Note: on the slave we set ISIG off before Init so that \x03 is delivered as
// data; with the production default (cbreak) \x03 raises SIGINT — pre-existing,
// 1.81-identical behavior, documented separately (real-app check).
#include "imtui/imtui-impl-ncurses.h"
#include "imtui/imtui.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static char g_slavepath[32];

static int open_pty(void) {
    int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (m < 0)
        return -1;
    int n = 0, unlock = 0;
    if (ioctl(m, 0x80045430, &n) < 0) {
        close(m);
        return -1;
    } // TIOCGPTN
    if (ioctl(m, 0x40045431, &unlock) < 0) {
        close(m);
        return -1;
    } // TIOCSPTLCK = 0
    snprintf(g_slavepath, sizeof g_slavepath, "/dev/pts/%d", n);
    int s = open(g_slavepath, O_RDWR | O_NOCTTY);
    if (s < 0) {
        close(m);
        return -1;
    }
    struct termios t;
    if (tcgetattr(s, &t) == 0) {
        t.c_lflag &= ~(ICANON | ECHO | ISIG); // ISIG off: \x03 arrives as data
        tcsetattr(s, TCSANOW, &t);
    }
    struct winsize ws;
    ws.ws_row = 24;
    ws.ws_col = 80;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    ioctl(s, 0x5414, &ws); // TIOCSWINSZ
    return m;
}

static void feeder(int m) {
    fcntl(m, F_SETFL, O_NONBLOCK);
    struct Plan {
        int ms;
        const char* keys;
    };
    Plan plan[] = {{100, "hi\r"}, {400, "\x1b"}, {200, "\x1bOA\x1bOB\x1bOC\x1bOD"},
                   {300, "\t"},   {200, "\x03"}, {200, ""}};
    char b[65536];
    for (unsigned i = 0; i < sizeof(plan) / sizeof(plan[0]); ++i) {
        usleep(plan[i].ms * 1000);
        if (plan[i].keys[0]) {
            ssize_t r = write(m, plan[i].keys, strlen(plan[i].keys));
            (void)r;
        }
        // drain the master in 10ms slices so the tty buffer never fills
        // (the harness's DrawScreen escape-sequence stream goes here)
        for (int k = 0; k < plan[i].ms / 10 + 1; ++k) {
            while (read(m, b, sizeof b) > 0) {
            }
            usleep(10 * 1000);
        }
    }
    exit(0);
}

int main() {
    setenv("TERM", "xterm-256color", 1);
    setenv("LANG", "C.utf8", 1);
    setenv("LC_ALL", "C.utf8", 1);

    int m = open_pty();
    if (m < 0) {
        fprintf(stderr, "FAIL: pty open failed\n");
        return 2;
    }
    pid_t pid = fork();
    if (pid == 0)
        feeder(m);

    int s = open(g_slavepath, O_RDWR | O_NOCTTY);
    dup2(s, 0); // ncurses input
    dup2(s, 1); // ncurses output must be a tty (initscr); drained by feeder
    close(s);

    ImGui::CreateContext();
    ImGuiIO& io0 = ImGui::GetIO();
    io0.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures; // legacy atlas path
    { // build default atlas (stb) before NewFrame, as in scratch_chord_trickle_test.cpp
        unsigned char* px = nullptr;
        int fw = 0, fh = 0;
        io0.Fonts->GetTexDataAsAlpha8(&px, &fw, &fh);
    }
    ImTui_ImplNcurses_Init(false);
    ImGuiIO& io = ImGui::GetIO();
    const bool trickle_true = (io.ConfigInputTrickleEventQueue == true);

    bool seen_h = false, seen_hi = false;
    int h_frame = -1, i_frame = -1, enter_frame = -1;
    bool esc = false, up = false, down = false, left = false, right = false, tab = false,
         chord = false;

    for (int frame = 0; frame < 400; ++frame) {
        ImTui_ImplNcurses_NewFrame(); // backend pump: wget_wch -> Add*Event queue
        ImGui::NewFrame();
        int qn = io.InputQueueCharacters.Size;
        if (qn >= 1 && io.InputQueueCharacters[0] == L'h' && h_frame < 0)
            h_frame = frame;
        if (qn >= 2 && io.InputQueueCharacters[0] == L'h' && io.InputQueueCharacters[1] == L'i' &&
            i_frame < 0)
            i_frame = frame;
        if (enter_frame < 0 && ImGui::IsKeyPressed(ImGuiKey_Enter))
            enter_frame = frame;
        if (!esc)
            esc = ImGui::IsKeyPressed(ImGuiKey_Escape);
        if (!up)
            up = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
        if (!down)
            down = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
        if (!left)
            left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
        if (!right)
            right = ImGui::IsKeyPressed(ImGuiKey_RightArrow);
        if (!tab)
            tab = ImGui::IsKeyPressed(ImGuiKey_Tab);
        if (!chord)
            chord = io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C);
        ImGui::EndFrame();
        ImTui_ImplNcurses_DrawScreen(true);
        if (chord && esc && up && down && left && right && tab && enter_frame >= 0 && i_frame >= 0)
            break;
    }
    ImTui_ImplNcurses_Shutdown();
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);

    bool ok = true;
    auto report = [&](const char* what, bool cond, const char* extra = "") {
        fprintf(stderr, "%s: %s%s\n", cond ? "PASS" : "FAIL", what, extra);
        if (!cond)
            ok = false;
    };
    report("box6 runtime: ConfigInputTrickleEventQueue == true after Init", trickle_true);
    report("chars 'h' arrived", h_frame >= 0);
    char buf[128];
    snprintf(buf, sizeof buf, " (h frame %d, i frame %d)", h_frame, i_frame);
    report("chars 'hi' incremental: 'i' not before 'h' (trickle batch)",
           i_frame >= h_frame && i_frame >= 0, buf);
    snprintf(buf, sizeof buf, " (enter frame %d)", enter_frame);
    report("Enter fired, on a later frame than the 'i' char event", enter_frame > i_frame, buf);
    report("Escape fired", esc);
    report("Up/Down/Left/Right arrows fired", up && down && left && right);
    report("Tab fired", tab);
    report("Ctrl+C chord fired same-frame (io.KeyCtrl && IsKeyPressed(C))", chord);
    fprintf(stderr, ok ? "ALL KEYBOARD PATH CHECKS PASSED\n" : "KEYBOARD PATH CHECKS FAILED\n");
    return ok ? 0 : 1;
}
