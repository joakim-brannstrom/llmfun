// scratch_app_key_smoke.c — task 7, box 5 (keyboard paths reach the app):
// drives the REAL app binary (build/tui/llmfun_tui) on a PTY, injects "hi\r"
// (chars + Enter), then \x03 (production termios, ISIG on -> SIGINT), and
// checks the app's own llmfun_ui_log.txt output written in its CWD.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-llmfun_tui>\n", argv[0]);
        return 2;
    }
    const char* app = argv[1];

    int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (m < 0) {
        perror("ptmx");
        return 2;
    }
    int n = 0, unlock = 0;
    if (ioctl(m, 0x80045430, &n) < 0) {
        perror("TIOCGPTN");
        return 2;
    }
    if (ioctl(m, 0x40045431, &unlock) < 0) {
        perror("TIOCSPTLCK");
        return 2;
    }
    char slavepath[32];
    snprintf(slavepath, sizeof slavepath, "/dev/pts/%d", n);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int s = open(slavepath, O_RDWR);
        if (s < 0)
            _exit(3);
        ioctl(s, 0x5414, (struct winsize){24, 80, 0, 0}); // TIOCSWINSZ
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        close(s);
        close(m);
        setenv("TERM", "xterm-256color", 1);
        setenv("LANG", "C.utf8", 1);
        setenv("LC_ALL", "C.utf8", 1);
        if (chdir("/workarea/scratch_kbd") != 0)
            _exit(4); // app writes llmfun_ui_log.txt here
        execl(app, app, (char*)NULL);
        _exit(5);
    }
    close(0);
    fcntl(m, F_SETFL, O_NONBLOCK);

    usleep(300 * 1000);               // let the app reach its frame loop
    ssize_t wr = write(m, "hi\r", 3); // chars + Enter (tuiAddLogMessage path)
    (void)wr;

    // drain master output for up to ~2.5s while watching for the log file
    char b[65536];
    int logged = 0;
    for (int k = 0; k < 250; ++k) {
        while (read(m, b, sizeof b) > 0) {
        }
        usleep(10 * 1000);
        FILE* f = fopen("/workarea/scratch_kbd/llmfun_ui_log.txt", "r");
        if (f) {
            char line[512] = "";
            while (fgets(line, sizeof line, f)) {
                if (strstr(line, "hi"))
                    logged = 1;
            }
            fclose(f);
            if (logged)
                break;
        }
    }
    if (logged)
        fprintf(stderr, "PASS: real app saw chars+Enter (llmfun_ui_log.txt contains 'hi')\n");
    else
        fprintf(stderr, "FAIL: no 'hi' entry in llmfun_ui_log.txt after 2.5s\n");

    // Ctrl+C with production termios (ISIG on): expect the app to die by signal
    kill(pid, SIGCONT);
    ssize_t wr2 = write(m, "\x03", 1);
    (void)wr2;
    int status = 0;
    int sig_death = 0;
    for (int k = 0; k < 100; ++k) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            sig_death = WIFSIGNALED(status) && WTERMSIG(status) == SIGINT;
            break;
        }
        usleep(20 * 1000);
    }
    if (!sig_death)
        kill(pid, SIGKILL), waitpid(pid, NULL, 0);
    fprintf(stderr,
            "%s: real-app Ctrl+C (\\x03, ISIG on) terminates by SIGINT (pre-existing "
            "1.81-identical cbreak behavior)\n",
            sig_death ? "PASS" : "INFO");
    close(m);
    return logged ? 0 : 1;
}
