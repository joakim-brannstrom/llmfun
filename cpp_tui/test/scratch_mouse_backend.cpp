// scratch_mouse_backend.cpp — task 8: does the REAL ncurses backend deliver SGR
// mouse clicks to ImGui (pos + button)? Runs ImTui_ImplNcurses_Init(true) with
// fd 0/1 = PTY slave, a feeder child injecting the clicktest timeline, and a
// minimal ImGui window with a button; reports io-level mouse events and hits.
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
    } // TIOCSPTLCK
    snprintf(g_slavepath, sizeof g_slavepath, "/dev/pts/%d", n);
    int s = open(g_slavepath, O_RDWR | O_NOCTTY);
    if (s < 0) {
        close(m);
        return -1;
    }
    struct termios t;
    if (tcgetattr(s, &t) == 0) {
        cfmakeraw(&t);
        t.c_lflag &= ~((tcflag_t)ECHO);
        tcsetattr(s, TCSANOW, &t);
    }
    struct winsize ws = {30, 100, 0, 0};
    ioctl(s, TIOCSWINSZ, &ws);
    (void)!dup2(s, 0);
    if (s != 1)
        (void)!dup2(s, 1);
    if (s > 1)
        close(s);
    return m;
}

static void feeder(int m) {
    // Task 8 clicktest timeline: clicks (SGR coalesced press+release) + \n pokes.
    struct {
        int ms;
        const char* bytes;
        long n;
    } steps[] = {
        {600, "\x1b[<0;2;5M\x1b[<0;2;5m", 18}, // Alpha row (row5,col2)
        {400, "\n", 1},
        {400, "\x1b[<0;2;5M\x1b[<0;2;5m", 18},
        {400, "\n", 1},
        {400, "\x1b[<0;3;2M\x1b[<0;3;2m", 18}, // Close button (row2,col3)
        {400, "\n", 1},
        {1000, "", 0},
    };
    usleep(300 * 1000);
    for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        usleep(steps[i].ms * 1000);
        if (steps[i].n)
            (void)!write(m, steps[i].bytes, (size_t)steps[i].n);
    }
    _exit(0);
}

int main() {
    ImGui::CreateContext();
    setenv("TERM", "xterm-256color", 1); // ncurses needs a terminal type
    int m = open_pty();
    if (m < 0) {
        fprintf(stderr, "open_pty failed\n");
        return 2;
    }
    pid_t pid = fork();
    if (pid == 0)
        feeder(m);

    // build default atlas before NewFrame (as in the app / task-7 probe)
    {
        unsigned char* px;
        int w, h;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        (void)px;
        (void)h;
    }

    ImTui_ImplNcurses_Init(true); // mouseSupport = true (like the app)

    ImVec2 b1min(-1, -1), b1max(-1, -1);
    int frames = 0, pos_events = 0, clicks = 0, hits = 0;
    ImVec2 lastPos(-1, -1);
    bool lastDown = false;
    for (; frames < 140; frames++) {
        ImTui_ImplNcurses_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(40, 20));
        ImGui::Begin("w", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::Button("B1", ImVec2(10, 1));
        b1min = ImGui::GetItemRectMin();
        b1max = ImGui::GetItemRectMax();
        ImGui::End();

        ImGuiIO& io = ImGui::GetIO();
        if (io.MousePos.x != lastPos.x || io.MousePos.y != lastPos.y) {
            fprintf(stderr, "frame=%d pos=(%g,%g)\n", frames, io.MousePos.x, io.MousePos.y);
            lastPos = io.MousePos;
            pos_events++;
        }
        if (io.MouseDown[0] != lastDown) {
            fprintf(stderr, "frame=%d down=%d pos=(%g,%g)\n", frames, io.MouseDown[0],
                    io.MousePos.x, io.MousePos.y);
            lastDown = io.MouseDown[0];
            if (io.MouseDown[0]) {
                clicks++;
                if (io.MousePos.x >= b1min.x && io.MousePos.x < b1max.x &&
                    io.MousePos.y >= b1min.y && io.MousePos.y < b1max.y) {
                    fprintf(stderr, "frame=%d HIT B1 rect=(%g,%g)-(%g,%g)\n", frames, b1min.x,
                            b1min.y, b1max.x, b1max.y);
                    hits++;
                }
            }
        }

        ImGui::EndFrame();
        ImTui_ImplNcurses_DrawScreen(true);
        usleep(30 * 1000);
    }
    ImTui_ImplNcurses_Shutdown();
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);

    fprintf(stderr,
            "SUMMARY frames=%d pos_events=%d clicks=%d b1_hits=%d "
            "b1rect=(%g,%g)-(%g,%g)\n",
            frames, pos_events, clicks, hits, b1min.x, b1min.y, b1max.x, b1max.y);
    return 0;
}
