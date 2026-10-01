// scratch_pty_getch.c — minimal ncurses-on-PTY input probe (no imgui).
// Prints EVERY wget_wch result with a millisecond timestamp so we can see
// exactly what ncurses delivers for each feeder chunk (Enter/\r, Tab/\t,
// arrow assembly from "\x1b[A...").
// fd map: 0,1 = slave (ncurses), 2 = this log (redirected by the runner).
#define _XOPEN_SOURCE_EXTENDED 1
#include <fcntl.h>
#include <ncurses.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

static long ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(void) {
    setenv("TERM", "xterm-256color", 1);
    setenv("LANG", "C.utf8", 1);
    setenv("LC_ALL", "C.utf8", 1);

    int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (m < 0) {
        fprintf(stderr, "ptmx open failed\n");
        return 2;
    }
    int n = 0, unlock = 0;
    if (ioctl(m, 0x80045430, &n) < 0) {
        fprintf(stderr, "TIOCGPTN failed\n");
        return 2;
    }
    if (ioctl(m, 0x40045431, &unlock) < 0) {
        fprintf(stderr, "TIOCSPTLCK failed\n");
        return 2;
    }
    char slavepath[32];
    snprintf(slavepath, sizeof slavepath, "/dev/pts/%d", n);

    pid_t pid = fork();
    if (pid == 0) {
        fcntl(m, F_SETFL, O_NONBLOCK);
        struct Plan {
            int ms;
            const char* keys;
        };
        struct Plan plan[] = {{100, "hi\r"}, {400, "\x1b"}, {200, "\x1b[A\x1b[B\x1b[C\x1b[D"},
                              {300, "\t"},   {200, "\x03"}, {1500, ""}};
        char b[65536];
        long t0 = ms_now();
        for (unsigned i = 0; i < sizeof(plan) / sizeof(plan[0]); ++i) {
            usleep((useconds_t)plan[i].ms * 1000);
            if (plan[i].keys[0]) {
                ssize_t r = write(m, plan[i].keys, strlen(plan[i].keys));
                fprintf(stderr, "[%ldms] feeder wrote %zu bytes: ", ms_now() - t0,
                        r > 0 ? (size_t)r : 0);
                for (const char* p = plan[i].keys; *p; ++p)
                    fprintf(stderr, "%02x ", (unsigned char)*p);
                fprintf(stderr, "\n");
            }
            for (int k = 0; k < plan[i].ms / 10 + 1; ++k) {
                while (read(m, b, sizeof b) > 0) {
                }
                usleep(10 * 1000);
            }
        }
        exit(0);
    }

    int s = open(slavepath, O_RDWR | O_NOCTTY);
    if (s < 0) {
        fprintf(stderr, "slave open failed\n");
        return 2;
    }
    struct termios t;
    if (tcgetattr(s, &t) == 0) {
        t.c_lflag &= ~(ICANON | ECHO | ISIG);
        tcsetattr(s, TCSANOW, &t);
    }
    struct winsize ws;
    ws.ws_row = 24;
    ws.ws_col = 80;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    ioctl(s, 0x5414, &ws);
    dup2(s, 0);
    dup2(s, 1);
    close(s);

    long t0 = ms_now();
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    wtimeout(stdscr,
             25); // variant probe: does a nonzero window delay enable ESC-sequence assembly?
    set_escdelay(25);
    keypad(stdscr, true);

    fprintf(stderr, "[%ldms] ncurses ready, escdelay=%d\n", ms_now() - t0, ESCDELAY);
    for (int k = 0; k < 250; ++k) { // 250 x 16ms ~= 4s
        // drain like the backend pump: consume ALL buffered events per tick
        for (;;) {
            wint_t wc;
            int ret = wget_wch(stdscr, &wc);
            if (ret == ERR)
                break;
            if (ret == KEY_CODE_YES) {
                fprintf(stderr, "[%ldms] KEY_CODE %d\n", ms_now() - t0, (int)wc);
            } else {
                fprintf(stderr, "[%ldms] char 0x%02x (%d)\n", ms_now() - t0, (unsigned)wc, (int)wc);
            }
        }
        usleep(16 * 1000);
    }
    endwin();
    fprintf(stderr, "PROBE DONE\n");
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return 0;
}
