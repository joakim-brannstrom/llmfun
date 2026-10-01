// scratch_mouse_probe.c — what does THIS ncurses deliver for SGR mouse bytes?
// Runs on the pty slave; driver (scratch_tui_smoke mouseprobe) feeds SGR clicks.
// Writes decoded events to scratch_mouse_probe.out (NOT stdout — avoids any
// pty output-buffer interaction). File is truncated at start.
#define NCURSES_WIDECHAR 1
#include <ncurses.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static FILE* out;
static void onalrm(int sig) {
    (void)sig;
    endwin();
    if (out)
        fclose(out);
    _exit(0);
}

int main(void) {
    out = fopen("scratch_mouse_probe.out", "w");
    if (!out)
        return 3;
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    mouseinterval(0);
    refresh();
    (void)!write(1, "\x1b[?1003h\x1b[?1006h", 14);
    signal(SIGALRM, onalrm);
    alarm(8);
    int events = 0;
    while (events < 60) {
        int c = ERR;
        int r = get_wch(&c);
        if (r == ERR) {
            usleep(5000);
            continue;
        }
        if (c == KEY_MOUSE) {
            MEVENT ev;
            if (getmouse(&ev) == OK)
                fprintf(out, "MOUSE x=%d y=%d bstate=%08lx\n", ev.x, ev.y,
                        (unsigned long)ev.bstate);
            else
                fprintf(out, "MOUSE getmouse FAIL\n");
        } else {
            fprintf(out, "KEY %d\n", c);
        }
        fflush(out);
        events++;
    }
    endwin();
    fclose(out);
    return 0;
}
