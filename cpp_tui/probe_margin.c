// probe_margin.c — PTY stream modeler for the TUI max-width byte-stream
// check. Primary role: run llmfun_tui on a PTY and measure whether
// terminal DATA (printable chars, xterm repeat) is written at a column
// >= cap.
//
// Model: PTY is a byte pipe with no screen memory, so the stream emitted
// by the child IS the complete picture of what a real terminal would
// receive.
//
// Modes:
//   probe_margin --tui    : child-side ncurses sequence (vestigial)
//   probe_margin --driver : forks the --tui child on a PTY (100x30),
//                           models the emitted stream, reports writes.
//   probe_margin --dump   : raw stream dump to /tmp/probe_stream.bin.
//   probe_margin --run CMD [ARGS...]
//                           : forks CMD on a PTY (PROBE_W x PROBE_H,
//                             default 100x30), models the emitted stream,
//                             reports max_col_written / max_data_col.
//                             Max-width check: run llmfun_tui --frames N
//                             with LLMFUN_TUI_MAX_WIDTH=CAP plus
//                             PROBE_UNTIL="smoke ok:" and assert
//                             max_data_col < CAP (no terminal data at col
//                             >= CAP).
//
//   Cursor model (--run): tracks CR, LF, BS (cursor-left, clamped at 0),
//   and CSI moves (CUP 'H'/'f', CUU/CUD/CUF/CUB 'A'-'D', CCHA 'G', VHAF
//   'd', xterm repeat 'b') so data-write columns are measured at the true
//   cursor position. CSI parameter bytes are parsed digit-by-digit (empty
//   ';' slots skipped); private-mode '?NNN' prefixes are consumed. Clear
//   commands ('J'/'K') and SGR/other CSI have no cursor effect here.
//
// Env for --run:
//   PROBE_W / PROBE_H : PTY window size (default 100x30).
//   PROBE_UNTIL=MARK  : data metric (max_data_col) counts only bytes
//                       before the first raw occurrence of MARK. The
//                       standalone --frames smoke mode prints a raw stdout
//                       banner ("smoke ok: ...") after the last rendered
//                       frame -- harness output, not TUI rendering -- so
//                       the check uses this to measure the render phase.
#include <ncurses.h>
#include <pty.h>

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define W 100
#define H 30
#define MARGIN_START 40

static int tui_child(void) {
    initscr();
    cbreak();
    noecho();

    // Marker content in cols 0..29 of row 0.
    for (int i = 0; i < 30; i++) {
        if (addch('#') == ERR)
            return 1;
    }
    wrefresh(stdscr);
    sleep(1);

    // Margin blanking: virtual-screen move + clrtoeol for all rows; the
    // next wrefresh is what flushes it.
    for (int y = 0; y < LINES; y++) {
        move(y, MARGIN_START);
        clrtoeol();
    }
    wrefresh(stdscr);
    sleep(1);

    endwin();
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "--tui") == 0) {
        if (argc >= 3)
            setenv("TERM", argv[2], 1);
        return tui_child();
    }
    if (argc >= 2 && strcmp(argv[1], "--driver") == 0) {
        int mfd = -1;
        pid_t pid = forkpty(&mfd, NULL, NULL, NULL);
        if (pid == 0) {
            struct winsize ws;
            ws.ws_col = W;
            ws.ws_row = H;
            ws.ws_xpixel = 0;
            ws.ws_ypixel = 0;
            (void)ioctl(0, TIOCSWINSZ, &ws);
            if (argc >= 3)
                setenv("TERM", argv[2], 1);
            else
                setenv("TERM", "xterm-256color", 1);
            if (argc >= 3)
                execl(argv[0], argv[0], "--tui", argv[2], (char*)NULL);
            execl(argv[0], argv[0], "--tui", (char*)NULL);
            _exit(127);
        }
        if (pid < 0) {
            perror("forkpty");
            return 1;
        }

        // Minimal terminal stream model.
        int cur_r = 0, cur_c = 0;
        long max_col = -1;
        long margin_writes = 0; // cells touched at col >= MARGIN_START
        long total_plain = 0;
        long full_clears = 0, eol_clears = 0;

        unsigned char buf[65536];
        for (;;) {
            ssize_t n = read(mfd, buf, sizeof buf);
            if (n <= 0)
                break;
            for (ssize_t i = 0; i < n; i++) {
                unsigned char c = buf[i];
                if (c == 0x1b) {
                    if (i + 1 < n && buf[i + 1] == '[') {
                        i++;
                        int params[8];
                        int np = 0;
                        while (i + 1 < n && (isdigit(buf[i + 1]) || buf[i + 1] == ';')) {
                            if (buf[i + 1] == ';') {
                                i++;
                                if (np < 8)
                                    params[np++] = -1;
                            } else {
                                int v = 0;
                                while (i + 1 < n && isdigit(buf[i + 1])) {
                                    i++;
                                    v = v * 10 + (buf[i + 1] - '0');
                                }
                                if (np < 8)
                                    params[np++] = v;
                            }
                        }
                        i++; // advance to final byte
                        char fin = i < n ? (char)buf[i] : 0;
                        if (fin == 'H' || fin == 'f') {
                            int r = (np > 0 && params[0] >= 0) ? params[0] : 1;
                            int cc = (np > 1 && params[1] >= 0) ? params[1] : 1;
                            cur_r = r - 1;
                            cur_c = cc - 1;
                        } else if (fin == 'A') {
                            int d = (np > 0 && params[0] > 0) ? params[0] : 1;
                            cur_r -= d;
                            if (cur_r < 0)
                                cur_r = 0;
                        } else if (fin == 'B') {
                            int d = (np > 0 && params[0] > 0) ? params[0] : 1;
                            cur_r += d;
                            if (cur_r >= H)
                                cur_r = H - 1;
                        } else if (fin == 'C') {
                            int d = (np > 0 && params[0] > 0) ? params[0] : 1;
                            cur_c += d;
                        } else if (fin == 'D') {
                            int d = (np > 0 && params[0] > 0) ? params[0] : 1;
                            cur_c -= d;
                            if (cur_c < 0)
                                cur_c = 0;
                        } else if (fin == 'K') {
                            eol_clears++;
                            for (int cc = cur_c; cc < W; cc++) {
                                if (cc > max_col)
                                    max_col = cc;
                                if (cc >= MARGIN_START)
                                    margin_writes++;
                            }
                        } else if (fin == 'J') {
                            full_clears++;
                            for (int rr = 0; rr < H; rr++)
                                for (int cc = 0; cc < W; cc++) {
                                    if (cc > max_col)
                                        max_col = cc;
                                    if (cc >= MARGIN_START)
                                        margin_writes++;
                                }
                        }
                        // all other CSI (m/l/h/s/u/?...) ignored
                    }
                    // other ESC sequences (ESC ), ESC =, ESC 7 ...) ignored
                } else if (c >= 0x20) {
                    total_plain++;
                    if (cur_c > max_col)
                        max_col = cur_c;
                    if (cur_c >= MARGIN_START)
                        margin_writes++;
                    cur_c++;
                    if (cur_c >= W) {
                        cur_c = 0;
                        cur_r++;
                        if (cur_r >= H)
                            cur_r = H - 1;
                    }
                }
            }
        }

        int status = 0;
        waitpid(pid, &status, 0);
        close(mfd);
        printf("child_exit=%d\n", WEXITSTATUS(status));
        printf("plain_chars=%ld full_clears=%ld eol_clears=%ld\n", total_plain, full_clears,
               eol_clears);
        printf("max_col_written=%ld\n", max_col);
        printf("margin_writes_col_%d_and_beyond=%ld\n", MARGIN_START, margin_writes);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--dump") == 0) {
        FILE* f = fopen("/tmp/probe_stream.bin", "wb");
        if (!f) {
            perror("fopen");
            return 1;
        }
        int mfd = -1;
        pid_t pid = forkpty(&mfd, NULL, NULL, NULL);
        if (pid == 0) {
            struct winsize ws;
            ws.ws_col = W;
            ws.ws_row = H;
            ws.ws_xpixel = 0;
            ws.ws_ypixel = 0;
            (void)ioctl(0, TIOCSWINSZ, &ws);
            if (argc >= 3)
                setenv("TERM", argv[2], 1);
            else
                setenv("TERM", "xterm-256color", 1);
            if (argc >= 3)
                execl(argv[0], argv[0], "--tui", argv[2], (char*)NULL);
            execl(argv[0], argv[0], "--tui", (char*)NULL);
            _exit(127);
        }
        if (pid < 0) {
            perror("forkpty");
            return 1;
        }
        unsigned char buf[65536];
        for (;;) {
            ssize_t n = read(mfd, buf, sizeof buf);
            if (n <= 0)
                break;
            fwrite(buf, 1, (size_t)n, f);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        close(mfd);
        fclose(f);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--run") == 0) {
        // Run argv[2..] on a PTY and model the emitted stream.
        // Window size via env PROBE_W / PROBE_H (default 100x30).
        int wcol = W, wrow = H;
        const char* ew = getenv("PROBE_W");
        const char* eh = getenv("PROBE_H");
        if (ew)
            wcol = atoi(ew);
        if (eh)
            wrow = atoi(eh);

        int mfd = -1;
        pid_t pid = forkpty(&mfd, NULL, NULL, NULL);
        if (pid == 0) {
            struct winsize ws = {wrow, wcol, 0, 0};
            (void)ioctl(0, TIOCSWINSZ, &ws);
            setenv("TERM", "xterm-256color", 1);
            execvp(argv[2], &argv[2]);
            _exit(127);
        }
        if (pid < 0) {
            perror("forkpty");
            return 1;
        }

        int cur_r = 0, cur_c = 0;
        long max_col = -1;
        long full_clears = 0, eol_clears = 0, total_plain = 0;
        long max_data_col = -1; // max col of DATA writes only (excl J/K/moves)

        // Read the whole child stream first, then model it. Buffering
        // enables the PROBE_UNTIL data cutoff below (a raw byte search over
        // the stream); runs emit well under a few hundred KB.
        unsigned char sbuf[65536];
        unsigned char* all = NULL;
        size_t alln = 0, allcap = 0;
        for (;;) {
            ssize_t n = read(mfd, sbuf, sizeof sbuf);
            if (n <= 0)
                break;
            if (alln + (size_t)n > allcap) {
                allcap = (alln + (size_t)n) * 2 + 65536;
                unsigned char* tmp = realloc(all, allcap);
                if (!tmp) {
                    perror("realloc");
                    _exit(1);
                }
                all = tmp;
            }
            memcpy(all + alln, sbuf, (size_t)n);
            alln += (size_t)n;
        }

        // PROBE_UNTIL: optional literal byte marker. When set, the DATA
        // metrics (max_data_col, past_col) count only bytes BEFORE the first
        // occurrence of the marker; cursor tracking still covers the whole
        // stream. The standalone --frames smoke mode prints a raw stdout
        // banner ("smoke ok: ...") after the last rendered frame -- harness
        // output, not TUI rendering -- so the max-width check sets
        // PROBE_UNTIL="smoke ok:" to measure the render phase only.
        const char* until = getenv("PROBE_UNTIL");
        size_t ulen = (until && *until) ? strlen(until) : 0;
        long cutoff = -1; // byte offset of the marker, or -1 (unset/absent)
        if (ulen > 0) {
            for (size_t o = 0; o + ulen <= alln; o++) {
                if (memcmp(all + o, until, ulen) == 0) {
                    cutoff = (long)o;
                    break;
                }
            }
        }

        for (size_t i = 0; i < alln; i++) {
            unsigned char c = all[i];
            if (c == 0x0d) { // CR
                cur_c = 0;
            } else if (c == 0x0a) { // LF
                cur_r = cur_r + 1 >= wrow ? wrow - 1 : cur_r + 1;
            } else if (c == 0x08) { // BS: cursor left, clamp at col 0
                cur_c = cur_c > 0 ? cur_c - 1 : 0;
            } else if (c == 0x1b) {
                if (i + 1 < alln && all[i + 1] == '[') {
                    i++; // at '['
                    if (i + 1 < alln && all[i + 1] == '?')
                        i++; // private-mode marker (?NNN): consume, not a digit
                    int params[8];
                    int np = 0;
                    while (i + 1 < alln && (isdigit(all[i + 1]) || all[i + 1] == ';')) {
                        if (all[i + 1] == ';') {
                            i++;
                            if (np < 8)
                                params[np++] = -1;
                        } else {
                            int v = 0;
                            while (i + 1 < alln && isdigit(all[i + 1])) {
                                i++;
                                v = v * 10 + (all[i] - '0');
                            }
                            if (np < 8)
                                params[np++] = v;
                        }
                    }
                    while (i + 1 < alln && all[i + 1] >= 0x20 && all[i + 1] <= 0x2f)
                        i++; // intermediate bytes
                    i++;
                    char fin = i < alln ? (char)all[i] : 0;
                    // Numeric params in order (empty ';' slots skipped):
                    int p1 = 0, p2 = 0;
                    for (int k = 0, seen = 0; k < np && seen < 2; k++) {
                        if (params[k] < 0)
                            continue;
                        if (seen == 0)
                            p1 = params[k];
                        else
                            p2 = params[k];
                        seen++;
                    }
                    if (fin == 'H' || fin == 'f') { // CUP (1-based)
                        int r = p1 >= 1 ? p1 : 1;
                        int cc = p2 >= 1 ? p2 : 1;
                        cur_r = r - 1 >= wrow ? wrow - 1 : r - 1;
                        cur_c = cc - 1 >= wcol ? wcol - 1 : cc - 1;
                    } else if (fin == 'A') {
                        int d = p1 > 0 ? p1 : 1;
                        cur_r = cur_r - d < 0 ? 0 : cur_r - d;
                    } else if (fin == 'B') {
                        int d = p1 > 0 ? p1 : 1;
                        cur_r = cur_r + d >= wrow ? wrow - 1 : cur_r + d;
                    } else if (fin == 'C') {
                        int d = p1 > 0 ? p1 : 1;
                        cur_c = cur_c + d >= wcol ? wcol - 1 : cur_c + d;
                    } else if (fin == 'D') {
                        int d = p1 > 0 ? p1 : 1;
                        cur_c = cur_c - d < 0 ? 0 : cur_c - d;
                    } else if (fin == 'G') { // CCHA (1-based col)
                        cur_c = p1 >= 1 ? (p1 - 1 >= wcol ? wcol - 1 : p1 - 1) : 0;
                    } else if (fin == 'd') { // VHAF (1-based row)
                        cur_r = p1 >= 1 ? (p1 - 1 >= wrow ? wrow - 1 : p1 - 1) : 0;
                    } else if (fin == 'b') {
                        // xterm "repeat previous character": p1+1 copies
                        int rep = p1 > 0 ? p1 + 1 : 1;
                        while (rep-- > 0) {
                            if (cur_c > max_col)
                                max_col = cur_c;
                            if (cutoff < 0 || (long)i < cutoff) {
                                if (cur_c > max_data_col)
                                    max_data_col = cur_c;
                            }
                            cur_c++;
                            if (cur_c >= wcol) {
                                cur_c = 0;
                                cur_r = cur_r + 1 >= wrow ? wrow - 1 : cur_r + 1;
                            }
                        }
                    } else if (fin == 'K') {
                        eol_clears++;
                        for (int cc = cur_c; cc < wcol; cc++) {
                            if (cc > max_col)
                                max_col = cc;
                        }
                    } else if (fin == 'J') {
                        full_clears++;
                        for (int cc = 0; cc < wcol; cc++)
                            if (cc > max_col)
                                max_col = cc;
                    }
                    // other CSI (m/s/l/h/t/r/u/X/...) : no cursor or data effect
                } else if (i + 2 < alln && all[i + 1] >= 0x20 && all[i + 1] <= 0x2f) {
                    i += 2; // ESC + intermediate + final (ESC ( B, ESC # 8)
                } else if (i + 1 < alln && all[i + 1] == 0x5d) {
                    // OSC (ESC ] payload BEL or ESC \): skip the payload
                    i += 2;
                    while (i + 1 < alln) {
                        if (all[i + 1] == 0x07) {
                            i++;
                            break;
                        }
                        if (all[i + 1] == 0x1b && i + 2 < alln && all[i + 2] == 0x5c) {
                            i += 2;
                            break;
                        }
                        i++;
                    }
                } else if (i + 1 < alln) {
                    i++; // two-byte ESC sequence (ESC =, ESC 7, ...)
                }
            } else if (c >= 0x20) {
                total_plain++;
                if (cur_c > max_col)
                    max_col = cur_c;
                if (cutoff < 0 || (long)i < cutoff) {
                    if (cur_c > max_data_col)
                        max_data_col = cur_c;
                }
                cur_c++;
                if (cur_c >= wcol) {
                    cur_c = 0;
                    cur_r = cur_r + 1 >= wrow ? wrow - 1 : cur_r + 1;
                }
            }
        }
        free(all);
        int status = 0;
        waitpid(pid, &status, 0);
        close(mfd);
        printf("child_exit=%d plain=%ld full_clears=%ld eol_clears=%ld\n", WEXITSTATUS(status),
               total_plain, full_clears, eol_clears);
        printf("max_col_written=%ld (window %dx%d)\n", max_col, wcol, wrow);
        printf("max_data_col=%ld\n", max_data_col);
        if (ulen > 0)
            printf("data_cutoff=%ld\n", cutoff); // -1 = marker not found
        return 0;
    }
    fprintf(stderr, "usage: %s [--tui | --driver | --dump | --run CMD ARGS...]\n", argv[0]);
    return 2;
}
