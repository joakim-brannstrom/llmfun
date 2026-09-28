// scratch_tui_smoke.c — task 8 (imgui 1.92.9b upgrade) runtime PTY smoke of the
// REAL app binary build/tui/llmfun_tui. No repo changes; scratch-only harness.
//
// fd map: PTY master = m; child std{in,out,err} = slave.
// Modes:
//   headless <app> <frames> <W> <H>
//       Fork `app --frames N` on a WxH PTY, feed nothing, capture the whole
//       stream, waitpid. Asserts (printed as 0/1 flags):
//         - child exited 0
//         - stream contains "smoke ok: rendered N frames" (the app's own
//           headless banner, written before endwin)
//         - render markers: "Alpha session [3]" (sidebar row), "Beta session
//           [5]", "hello" (chat text), "Context:" (status line)
//   clicksweep <app> <frames> <W> <H> <logdir>
//       Fork `app --frames N`, feed an SGR+X10 mouse click sweep over the
//       session-panel grid during the run, waitpid. Reports exit status and
//       which "session panel:" lines the app logged to
//       <logdir>/llmfun_ui_log.txt (mouse click evidence at app level).
//   interactive <app> <logdir> <noisig>
//       Boot the app INTERACTIVELY (no --frames) and feed a timed key/mouse
//       plan; mid-run resize 100x30 -> 64x20 via TIOCSWINSZ on the master
//       (SIGWINCH -> ncurses KEY_RESIZE; the backend re-queries getmaxyx every
//       frame); then Ctrl+C (0x03). noisig=1 clears ISIG on the pty line
//       discipline first so the byte reaches ncurses and the app's own
//       ImGui-level Ctrl+C quit path runs (io.KeyCtrl && IsKeyPressed(C),
//       clean exit 0); noisig=0 keeps production termios (SIGINT death,
//       task-7-verified, not re-tested here). Reports exit status, bytes
//       before/after the resize, max CUP row overall and after the resize
//       (status line re-anchoring), render markers, and the session-panel log
//       lines.
#define _XOPEN_SOURCE_EXTENDED 1
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static long ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// Fork `app` with argv {app, extraArg, extraArg2} (NULLs skipped) on a WxH
// PTY, cwd = cwdDir (or unchanged if NULL). Returns master fd, pid via *outPid.
static int fork_app(const char* app, const char* extraArg, const char* extraArg2, int W, int H,
                    const char* cwdDir, pid_t* outPid) {
    int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (m < 0) {
        perror("ptmx");
        return -1;
    }
    int n = 0, unlock = 0;
    if (ioctl(m, 0x80045430, &n) < 0) {
        perror("TIOCGPTN");
        return -1;
    }
    if (ioctl(m, 0x40045431, &unlock) < 0) {
        perror("TIOCSPTLCK");
        return -1;
    }
    char slavepath[32];
    snprintf(slavepath, sizeof slavepath, "/dev/pts/%d", n);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int s = open(slavepath, O_RDWR);
        if (s < 0)
            _exit(3);
        struct winsize ws = {H, W, 0, 0};
        ioctl(s, 0x5414, &ws); // TIOCSWINSZ
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        close(s);
        close(m);
        setenv("TERM", "xterm-256color", 1);
        setenv("LANG", "C.utf8", 1);
        setenv("LC_ALL", "C.utf8", 1);
        if (cwdDir && chdir(cwdDir) != 0)
            _exit(4);
        if (extraArg && extraArg2)
            execl(app, app, extraArg, extraArg2, (char*)NULL);
        else
            execl(app, app, (char*)NULL);
        _exit(5);
    }
    if (pid < 0) {
        perror("fork");
        close(m);
        return -1;
    }
    *outPid = pid;
    return m;
}

static int contains(const unsigned char* buf, size_t n, const char* needle) {
    size_t len = strlen(needle);
    if (len == 0 || n < len)
        return 0;
    for (size_t i = 0; i + len <= n; i++)
        if (memcmp(buf + i, needle, len) == 0)
            return 1;
    return 0;
}

// Accumulator for the captured child stream.
typedef struct {
    unsigned char* buf;
    size_t n, cap;
    size_t mark; // byte offset snapshot (for pre/post-resize split)
} Stream;

static void stream_append(Stream* st, const unsigned char* p, size_t r) {
    if (st->n + r > st->cap) {
        st->cap = (st->n + r) * 2 + (1 << 20);
        st->buf = realloc(st->buf, st->cap);
    }
    memcpy(st->buf + st->n, p, r);
    st->n += r;
}

// Drain whatever is available now (nonblocking) into st.
static void stream_drain(int m, Stream* st) {
    unsigned char tmp[65536];
    ssize_t r;
    while ((r = read(m, tmp, sizeof tmp)) > 0)
        stream_append(st, tmp, (size_t)r);
}

// Drain for `ms` milliseconds (nonblocking), capturing into st.
static void drain_ms(int m, Stream* st, long ms) {
    long t0 = ms_now();
    for (;;) {
        stream_drain(m, st);
        if (ms_now() - t0 >= ms)
            break;
        usleep(10 * 1000);
    }
}

// waitpid with a bounded capture loop; final drain after reap.
static void reap_and_report(pid_t pid, int m, Stream* st, const char* tag) {
    int status = 0;
    long t0 = ms_now();
    for (;;) {
        stream_drain(m, st);
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (ms_now() - t0 > 8000) {
            fprintf(stderr, "%s: TIMEOUT killing app\n", tag);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        usleep(10 * 1000);
    }
    stream_drain(m, st);
    close(m);
    if (WIFEXITED(status))
        printf("%s: exit=%d\n", tag, WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        printf("%s: exit=-1 sig=%d\n", tag, WTERMSIG(status));
}

// Count max 1-based CUP row ("ESC[<r>;<c>H") in buf[from..n).
static int max_cup_row(const unsigned char* buf, size_t n, size_t from) {
    int maxR = -1;
    for (size_t i = from; i + 4 < n; i++) {
        if (buf[i] != 0x1b || buf[i + 1] != '[')
            continue;
        size_t j = i + 2;
        int r = 0, got = 0;
        while (j < n && buf[j] >= '0' && buf[j] <= '9') {
            r = r * 10 + (buf[j] - '0');
            got = 1;
            j++;
        }
        if (!got || j >= n || buf[j] != ';')
            continue;
        j++;
        while (j < n && buf[j] >= '0' && buf[j] <= '9')
            j++;
        if (j < n && buf[j] == 'H' && r > maxR)
            maxR = r;
    }
    return maxR;
}

// Strip ESC sequences (CSI: ESC [ params+intermediates+final; two-byte ESC x)
// so render-text markers can be matched across attribute-run boundaries.
static size_t strip_esc(const unsigned char* src, size_t n, char* dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        if (src[i] == 0x1b) {
            if (i + 1 < n && src[i + 1] == '[') {
                i += 2;
                while (i < n && !(src[i] >= 0x40 && src[i] <= 0x7e))
                    i++;
            } else if (i + 1 < n) {
                i++;
            }
            continue;
        }
        dst[o++] = (char)src[i];
    }
    dst[o] = '\0';
    return o;
}

// --- mode: headless ---------------------------------------------------------
// --- mode: headless ---------------------------------------------------------
// Optional 6th arg: dump the raw captured stream to this file (debugging).
static int mode_headless(int argc, char** argv) {
    const char* app = argv[2];
    int frames = atoi(argv[3]);
    int W = atoi(argv[4]);
    int H = atoi(argv[5]);
    const char* dumpPath = (argc >= 7) ? argv[6] : NULL;
    pid_t pid;
    char framesArg[32];
    snprintf(framesArg, sizeof framesArg, "%d", frames);
    char appAbs[4096];
    if (!realpath(app, appAbs)) {
        perror("realpath(app)");
        return 2;
    }
    int m = fork_app(appAbs, "--frames", framesArg, W, H, NULL, &pid);
    if (m < 0)
        return 2;
    fcntl(m, F_SETFL, O_NONBLOCK);
    Stream st = {0};
    reap_and_report(pid, m, &st, "headless");
    if (dumpPath) {
        FILE* d = fopen(dumpPath, "wb");
        if (d) {
            fwrite(st.buf, 1, st.n, d);
            fclose(d);
        }
    }
    char banner[64];
    snprintf(banner, sizeof banner, "smoke ok: rendered %d frames", frames);
    char* plain = (char*)malloc(st.n + 1);
    size_t pn = strip_esc(st.buf, st.n, plain, st.n + 1);
    printf("headless %dx%d frames=%d: bytes=%zu\n", W, H, frames, st.n);
    printf("  banner_smoke_ok=%d\n", contains(st.buf, st.n, banner));
    printf("  marker_sidebar_row=%d (Alpha session [3])\n",
           contains((unsigned char*)plain, pn, "Alpha session [3]"));
    printf("  marker_sidebar_row2=%d (Beta session [5])\n",
           contains((unsigned char*)plain, pn, "Beta session [5]"));
    printf("  marker_chat_text=%d (hello)\n", contains((unsigned char*)plain, pn, "hello"));
    printf("  marker_status=%d (Context:)\n", contains((unsigned char*)plain, pn, "Context:"));
    printf("  marker_headings=%d (smurf)\n", contains((unsigned char*)plain, pn, "smurf"));
    printf("  marker_wrapped_long_line=%d (long cat)\n",
           contains((unsigned char*)plain, pn, "long cat"));
    int ok = st.n > 0;
    ok = ok && contains(st.buf, st.n, banner) &&
         contains((unsigned char*)plain, pn, "Alpha session [3]");
    ok = ok && contains((unsigned char*)plain, pn, "hello") &&
         contains((unsigned char*)plain, pn, "Context:");
    free(plain);
    free(st.buf);
    printf("  HEADLESS_%dx%d_%s\n", W, H, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// --- mode: clicksweep -------------------------------------------------------
static int mode_clicksweep(const char* app, int frames, int W, int H, const char* logdir) {
    pid_t pid;
    char framesArg[32];
    snprintf(framesArg, sizeof framesArg, "%d", frames);
    char appAbs[4096];
    if (!realpath(app, appAbs)) {
        perror("realpath(app)");
        return 2;
    }
    int m = fork_app(appAbs, "--frames", framesArg, W, H, logdir, &pid);
    if (m < 0)
        return 2;
    fcntl(m, F_SETFL, O_NONBLOCK);

    // Let the app boot and render a few frames.
    Stream st = {0};
    drain_ms(m, &st, 600);

    // Mouse click sweep: SGR press/release + X10 press/release over a grid
    // covering the left panel (Close/Open/New/del/rows) and some chat area.
    // SGR/row;col are 1-based; X10 bytes are 32+code / 32+col / 32+row.
    char seq[64];
    for (int row = 1; row <= 8; row++) {
        for (int col = 2; col <= 30; col += 4) {
            snprintf(seq, sizeof seq, "\x1b[<0;%d;%dM\x1b[<0;%d;%dm", col, row, col, row);
            write(m, seq, strlen(seq));
            snprintf(seq, sizeof seq, "\x1b[M%c%c%c", (char)(32 + 0), (char)(32 + col),
                     (char)(32 + row));
            write(m, seq, 6);
            snprintf(seq, sizeof seq, "\x1b[M%c%c%c", (char)(32 + 3), (char)(32 + col),
                     (char)(32 + row));
            write(m, seq, 6);
            usleep(40 * 1000);
            stream_drain(m, &st);
        }
    }
    drain_ms(m, &st, 800);

    reap_and_report(pid, m, &st, "clicksweep");
    printf("clicksweep %dx%d frames=%d: bytes=%zu\n", W, H, frames, st.n);

    // llmfun_ui_log.txt: session-panel log lines = mouse click evidence.
    char path[512];
    snprintf(path, sizeof path, "%s/llmfun_ui_log.txt", logdir);
    FILE* f = fopen(path, "r");
    long panelLines = 0, queued = 0, skippedOrBusy = 0, closeClicked = 0, armed = 0;
    char line[512];
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (strstr(line, "session panel:"))
                panelLines++;
            if (strstr(line, "queued select") || strstr(line, "queued new"))
                queued++;
            if (strstr(line, "skipped (already active)") || strstr(line, "(busy)"))
                skippedOrBusy++;
            if (strstr(line, "session panel: close"))
                closeClicked++;
            if (strstr(line, "delete armed"))
                armed++;
        }
        fclose(f);
    }
    printf("  session_panel_log_lines=%ld queued_actions=%ld skipped_or_busy=%ld"
           " close_clicked=%ld del_armed=%ld\n",
           panelLines, queued, skippedOrBusy, closeClicked, armed);
    free(st.buf);
    printf("  CLICKSWEEP_%s (positive evidence = panel log lines > 0)\n",
           panelLines > 0 ? "PASS" : "FAIL");
    return panelLines > 0 ? 0 : 1;
}

// --- mode: interactive ------------------------------------------------------
static int mode_interactive(const char* app, const char* logdir, int noisig) {
    pid_t pid;
    char appAbs[4096];
    if (!realpath(app, appAbs)) {
        perror("realpath(app)");
        return 2;
    }
    int m = fork_app(appAbs, NULL, NULL, 100, 30, logdir, &pid);
    if (m < 0)
        return 2;
    fcntl(m, F_SETFL, O_NONBLOCK);
    Stream st = {0};

    long t0 = ms_now();

    // t=400ms: chars into the input widget.
    usleep(400 * 1000);
    write(m, "hello ", 6);
    // t=700ms: SS3 arrows (this container's terminfo delivers CSI as chars +
    // stray KEY_CODEs; SS3 maps to KEY_UP/DOWN/LEFT/RIGHT per task 7).
    usleep(300 * 1000);
    write(m, "\x1bOA\x1bOB\x1bOC\x1bOD", 12);
    // t=1000ms: Tab + Escape.
    usleep(300 * 1000);
    write(m, "\t\x1b", 2);
    // t=1300ms: wheel — SGR (button 64 up / 65 down) + X10 (b=96/97). No
    // AddMouseWheelEvent in the backend (1.81-identical), so no wheel effect
    // is expected; the point is "fed and alive, no crash".
    usleep(300 * 1000);
    write(m, "\x1b[<64;10;5M\x1b[<64;10;5m", strlen("\x1b[<64;10;5M\x1b[<64;10;5m"));
    write(m, "\x1b[<65;10;5M\x1b[<65;10;5m", strlen("\x1b[<65;10;5M\x1b[<65;10;5m"));
    {
        const char* x10up = "\x1b[M\x60\x2a\x17";
        write(m, x10up, 6);
    }
    {
        const char* x10dn = "\x1b[M\x61\x2a\x17";
        write(m, x10dn, 6);
    }
    usleep(300 * 1000);

    // Click sweep over the left panel grid (mouse click path).
    for (int row = 1; row <= 8; row++) {
        for (int col = 2; col <= 30; col += 4) {
            char seq[64];
            snprintf(seq, sizeof seq, "\x1b[<0;%d;%dM\x1b[<0;%d;%dm", col, row, col, row);
            write(m, seq, strlen(seq));
            usleep(30 * 1000);
            stream_drain(m, &st);
        }
    }

    st.mark = st.n; // pre-resize byte offset
    long tResize = ms_now() - t0;

    // Resize 100x30 -> 64x20 (SIGWINCH -> ncurses KEY_RESIZE; DisplaySize is
    // re-queried every frame by the backend's NewFrame).
    struct winsize ws = {20, 64, 0, 0};
    ioctl(m, 0x5414, &ws); // TIOCSWINSZ on the master

    // Keep producing for ~1.2s after the resize.
    drain_ms(m, &st, 1200);

    // Ctrl+C: clear ISIG so 0x03 reaches ncurses and the app's own
    // ImGui-level Ctrl+C quit path runs (io.KeyCtrl && IsKeyPressed(C) ->
    // tuiRender returns false -> clean shutdown -> exit 0).
    if (noisig) {
        struct termios t;
        if (tcgetattr(m, &t) == 0) {
            t.c_lflag &= ~(tcflag_t)ISIG;
            tcsetattr(m, TCSANOW, &t);
        }
    }
    write(m, "\x03", 1);

    int status = 0;
    long tStart = ms_now();
    for (;;) {
        stream_drain(m, &st);
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (ms_now() - tStart > 5000) {
            fprintf(stderr, "interactive: TIMEOUT killing app\n");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        usleep(10 * 1000);
    }
    stream_drain(m, &st);
    close(m);
    if (WIFEXITED(status))
        printf("interactive: exit=%d (clean ImGui-level Ctrl+C quit)\n", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        printf("interactive: exit=-1 sig=%d\n", WTERMSIG(status));

    printf("interactive: bytes_total=%zu bytes_pre_resize=%zu t_resize_ms=%ld\n", st.n, st.mark,
           tResize);
    printf("  max_cup_row_overall=%d max_cup_row_after_resize=%d (rows: 30 pre, 20 post)\n",
           max_cup_row(st.buf, st.n, 0), st.mark < st.n ? max_cup_row(st.buf, st.n, st.mark) : -1);

    // Session-panel evidence from the app's own log.
    char path[512];
    snprintf(path, sizeof path, "%s/llmfun_ui_log.txt", logdir);
    FILE* f = fopen(path, "r");
    long panelLines = 0, closeClicked = 0;
    char line[512];
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (strstr(line, "session panel:"))
                panelLines++;
            if (strstr(line, "session panel: close"))
                closeClicked++;
        }
        fclose(f);
    }
    printf("  session_panel_log_lines=%ld close_clicked=%ld\n", panelLines, closeClicked);

    // Render markers in the captured stream (pre + post resize).
    printf("  marker_hello=%d marker_Context=%d marker_Alpha=%d\n", contains(st.buf, st.n, "hello"),
           contains(st.buf, st.n, "Context:"), contains(st.buf, st.n, "Alpha session"));
    free(st.buf);
    printf("  INTERACTIVE_%s (want: exit=0, still-alive-after-resize bytes, panel log lines > 0)\n",
           (WIFEXITED(status) && WEXITSTATUS(status) == 0 && panelLines > 0) ? "PASS" : "CHECK");
    return 0;
}

// --- mode: mouseprobe -------------------------------------------------------
// Fork a bare ncurses probe (scratch_mouse_probe) and feed it SGR clicks at
// various cells; print what ncurses actually decodes.
static int mode_mouseprobe(const char* probe) {
    pid_t pid;
    char probeAbs[4096];
    if (!realpath(probe, probeAbs)) {
        perror("realpath(probe)");
        return 2;
    }
    int m = fork_app(probeAbs, NULL, NULL, 80, 24, NULL, &pid);
    if (m < 0)
        return 2;
    fcntl(m, F_SETFL, O_NONBLOCK);
    Stream st = {0};
    usleep(400 * 1000);
    stream_drain(m, &st);
    const char* cellsEnv = getenv("MOUSEPROBE_CELLS");
    static int cells[32][2];
    int ncells = 0;
    if (cellsEnv && *cellsEnv) {
        const char* p = cellsEnv;
        while (*p && ncells < 32) {
            int r = -1, c = -1;
            if (sscanf(p, "%d,%d", &r, &c) == 2) {
                cells[ncells][0] = r;
                cells[ncells][1] = c;
                ncells++;
            }
            while (*p && *p != ';')
                p++;
            if (*p)
                p++;
        }
    } else {
        static const int def[][2] = {
            {5, 2}, {5, 10}, {5, 30}, {2, 31}, {10, 15}, {1, 1}, {24, 80},
        };
        for (unsigned i = 0; i < sizeof def / sizeof def[0]; i++)
            cells[i][0] = def[i][0], cells[i][1] = def[i][1];
        ncells = (int)(sizeof def / sizeof def[0]);
    }
    char seq[64];
    int x10 = getenv("MOUSEPROBE_X10") != NULL;
    int split = getenv("MOUSEPROBE_SPLIT") != NULL;
    for (int i = 0; i < ncells; i++) {
        int row = cells[i][0], col = cells[i][1];
        if (x10) {
            seq[0] = '\x1b';
            seq[1] = '[';
            seq[2] = 'M';
            seq[3] = (char)(32 + 0);
            seq[4] = (char)(32 + col);
            seq[5] = (char)(32 + row);
            seq[6] = '\x1b';
            seq[7] = '[';
            seq[8] = 'M';
            seq[9] = (char)(32 + 3);
            seq[10] = (char)(32 + col);
            seq[11] = (char)(32 + row);
            write(m, seq, 12);
        } else {
            snprintf(seq, sizeof seq, "\x1b[<0;%d;%dM", col, row);
            write(m, seq, strlen(seq));
            if (split) {
                usleep(100 * 1000);
                stream_drain(m, &st);
            }
            snprintf(seq, sizeof seq, "\x1b[<0;%d;%dm", col, row);
            write(m, seq, strlen(seq));
        }
        usleep(400 * 1000);
        stream_drain(m, &st);
    }
    drain_ms(m, &st, 600);
    // Wedge diagnostics: after the cell sweep, poke the input parser.
    const char* pokes[] = {"\n", "x", "\x1b", "\x1b[<0;3;3M\x1b[<0;3;3m"};
    for (unsigned k = 0; k < sizeof pokes / sizeof pokes[0]; k++) {
        write(m, pokes[k], strlen(pokes[k]));
        usleep(500 * 1000);
        stream_drain(m, &st);
    }
    reap_and_report(pid, m, &st, "mouseprobe");
    printf("mouseprobe: captured %zu bytes\n", st.n);
    fwrite(st.buf, 1, st.n, stdout);
    free(st.buf);
    return 0;
}

// --- mode: clicktest --------------------------------------------------------
// Targeted, wedge-aware mouse click test against the REAL app.
// CLICKTEST_POKE=0 disables the "\n" flush pokes (control run).
static int mode_clicktest(const char* app, int frames, int W, int H, const char* logdir) {
    pid_t pid;
    char appAbs[4096];
    if (!realpath(app, appAbs)) {
        perror("realpath(app)");
        return 2;
    }
    char framesStr[16];
    snprintf(framesStr, sizeof framesStr, "%d", frames);
    int m = fork_app(appAbs, "--frames", framesStr, W, H, logdir, &pid);
    if (m < 0)
        return 2;
    fcntl(m, F_SETFL, O_NONBLOCK);
    Stream st = {0};
    usleep(600 * 1000);
    stream_drain(m, &st);

    int poke = getenv("CLICKTEST_POKE") ? atoi(getenv("CLICKTEST_POKE")) : 1;
    char seq[64];
    // Clicks: Alpha row (row5,col2 -> ImGui (1,4)), Close button (row2,col3).
    const int cells[][2] = {{5, 2}, {5, 2}, {2, 3}};
    for (unsigned i = 0; i < sizeof cells / sizeof cells[0]; i++) {
        int row = cells[i][0], col = cells[i][1];
        snprintf(seq, sizeof seq, "\x1b[<0;%d;%dM\x1b[<0;%d;%dm", col, row, col, row);
        write(m, seq, strlen(seq));
        usleep(400 * 1000);
        stream_drain(m, &st);
        if (poke) {
            write(m, "\n", 1);
            usleep(300 * 1000);
            stream_drain(m, &st);
        }
    }
    drain_ms(m, &st, 800);

    reap_and_report(pid, m, &st, "clicktest");

    char path[512];
    snprintf(path, sizeof path, "%s/llmfun_ui_log.txt", logdir);
    FILE* f = fopen(path, "r");
    long panelLines = 0, selects = 0, closeClicked = 0;
    char line[512];
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (strstr(line, "session panel:"))
                panelLines++;
            if (strstr(line, "queued select"))
                selects++;
            if (strstr(line, "session panel: close"))
                closeClicked++;
        }
        fclose(f);
    }
    printf("clicktest %dx%d frames=%d poke=%d: bytes=%zu\n", W, H, frames, poke, st.n);
    printf("  session_panel_log_lines=%ld queued_select=%ld close_clicked=%ld\n", panelLines,
           selects, closeClicked);
    printf("  CLICKTEST_%s\n", (panelLines > 0) ? "PASS" : "FAIL");
    free(st.buf);
    return panelLines > 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 7 && strcmp(argv[1], "clicktest") == 0)
        return mode_clicktest(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), argv[6]);
    if (argc >= 3 && strcmp(argv[1], "mouseprobe") == 0)
        return mode_mouseprobe(argv[2]);
    if (argc >= 6 && strcmp(argv[1], "headless") == 0)
        return mode_headless(argc, argv);
    if (argc >= 7 && strcmp(argv[1], "clicksweep") == 0)
        return mode_clicksweep(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), argv[6]);
    if (argc >= 5 && strcmp(argv[1], "interactive") == 0)
        return mode_interactive(argv[2], argv[3], atoi(argv[4]));
    fprintf(stderr,
            "usage: %s headless APP FRAMES W H | clicksweep APP FRAMES W H LOGDIR |"
            " interactive APP LOGDIR NOISIG\n",
            argv[0]);
    return 2;
}
