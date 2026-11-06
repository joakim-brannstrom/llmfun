// scratch_app_pty.cpp — the REAL app (tuiRender frame loop) + the REAL
// ncurses backend on a PTY + REAL SGR mouse clicks; asserts the app's own log
// (llmfun_ui_log.txt) gets "session panel:" lines.
#include "tui_api.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static std::array<char, 32> g_slavepath{};

static int open_pty(void) {
    int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (m < 0)
        return -1;
    int n = 0, unlock = 0;
    if (ioctl(m, 0x80045430, &n) < 0) {
        close(m);
        return -1;
    }
    if (ioctl(m, 0x40045431, &unlock) < 0) {
        close(m);
        return -1;
    }
    snprintf(g_slavepath.data(), g_slavepath.size(), "/dev/pts/%d", n);
    int s = open(g_slavepath.data(), O_RDWR | O_NOCTTY);
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
    struct Step {
        int ms;
        const char* bytes;
        long n;
    };
    const std::array<Step, 7> steps = {{
        {600, "\x1b[<0;2;8M\x1b[<0;2;8m",
         18}, // Beta row first (row8,col2, inactive -> queued select)
        {800, "\n", 1},
        {400, "\x1b[<0;2;5M\x1b[<0;2;5m", 18},
        {400, "\n", 1},
        {400, "\x1b[<0;3;2M\x1b[<0;3;2m", 18}, // Close button (row2,col3)
        {400, "\n", 1},
        {1500, "", 0},
    }};
    usleep(300 * 1000);
    for (size_t i = 0; i < steps.size(); i++) {
        usleep(steps[i].ms * 1000);
        if (steps[i].n)
            (void)!write(m, steps[i].bytes, (size_t)steps[i].n);
    }
    _exit(0);
}

static String makeStr(const char* s) { return String{s, strlen(s)}; }

int main() {
    setlocale(LC_ALL, "");
    setenv("TERM", "xterm-256color", 1);
    if (chdir("/workarea/scratch_app_pty_logdir") != 0) {
        perror("chdir");
        return 2;
    }
    unlink("llmfun_ui_log.txt");

    int m = open_pty();
    if (m < 0) {
        fprintf(stderr, "open_pty failed\n");
        return 2;
    }
    pid_t pid = fork();
    if (pid == 0)
        feeder(m);

    TuiState* state = tuiCreateState();
    if (!state) {
        fprintf(stderr, "tuiCreateState failed\n");
        return 1;
    }
    // v4 single-handle flow: Auto mode (the real app's default). This process
    // runs on the pty as both stdio ends, so a GUI-unavailable environment
    // falls back to the terminal backend here.
    // Before tuiInit: the filename is captured into io.IniFilename when the
    // ImGui context is created.
    tuiSetIniFilename(state, makeStr("imgui2.ini"));

    if (tuiInit(state, TuiBackendMode_Auto) != 0) {
        String err = tuiLastError();
        if (err.data && err.len > 0)
            fprintf(stderr, "tuiInit failed: %.*s\n", (int)err.len, err.data);
        else
            fprintf(stderr, "tuiInit failed\n");
        String_Free(err);
        tuiDestroyState(state);
        return 1;
    }
    tuiSetLogging(state, true);
    // The status row is seeded by tuiCreateState, as in the app's main.cpp.

    // Same headless seed as the app's main.cpp.
    const std::array<SessionItem, 3> sessions = {{
        {makeStr("20260815-100000-aaaa"), makeStr("Alpha session"), makeStr("first user preview"),
         3, 1},
        {makeStr("20260815-100000-bbbb"), makeStr("Beta session"), makeStr("second user preview"),
         5, 0},
        {makeStr("20260815-100000-cccc"), makeStr("Gamma session"), makeStr(""), 0, 0},
    }};
    tuiSetSessionList(state, sessions.data(), sessions.size());

    for (int frame = 0; frame < 600; frame++) {
        tuiBackendNewFrame(state);
        if (tuiRender(state) == 0)
            break;
        usleep(30 * 1000);
    }
    tuiDestroyState(state);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);

    FILE* f = fopen("llmfun_ui_log.txt", "r");
    long panelLines = 0, selects = 0, closeClicked = 0;
    std::array<char, 512> line{};
    if (f) {
        while (fgets(line.data(), line.size(), f)) {
            if (strstr(line.data(), "session panel:"))
                panelLines++;
            if (strstr(line.data(), "queued select"))
                selects++;
            if (strstr(line.data(), "session panel: close"))
                closeClicked++;
        }
        fclose(f);
    }
    fprintf(stderr, "app_pty: session_panel_log_lines=%ld queued_select=%ld close_clicked=%ld\n",
            panelLines, selects, closeClicked);
    fprintf(stderr, "APP_PTY_%s\n", panelLines > 0 ? "PASS" : "FAIL");
    return panelLines > 0 ? 0 : 1;
}
