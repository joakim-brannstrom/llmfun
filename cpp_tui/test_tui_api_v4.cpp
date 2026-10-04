// test_tui_api_v4.cpp
//
// Headless contract test for the v4 single-handle C API.
//
// In-process cases (no ImGui context is created in the process, so any
// accidental ImGui access on a None-safe path would fault):
//   - tuiCreateState with no init: a backend-less state is legal and inert —
//     tuiBackendNewFrame/tuiBackendRender are no-ops, tuiBackendActive
//     reports TuiBackendKind_None;
//   - the deliberate tuiRender asymmetry: tuiRender(NULL) == 0 (exit),
//     tuiRender(none-state) == 1 (continue);
//   - tuiDestroyState(NULL) and destroy-without-init are safe no-ops, and an
//     ImGui context created by a test harness itself survives a state
//     destroy;
//   - failed Gui init with a forced-empty display env (DISPLAY and
//     WAYLAND_DISPLAY unset): non-zero return, non-empty tuiLastError(), the
//     state stays backend-less, the call is retryable (no "already
//     initialized" trap), tuiBackendNote() stays empty, and the context the
//     failed init created is cleaned up.
//
// The Auto/TTY matrix needs real isatty() behaviour, which cannot be faked
// in-process, so it runs in forked children with controlled stdio:
//   - PTY stdio (both ends TTYs) -> Auto falls back to the terminal backend:
//     the pinned stderr line and the backend note are checked, a live TUI
//     frame is rendered, and the single-init guards are pinned.
//   - pipe/`/dev/null` stdio (no TTY) -> Auto fails with the GUI error and
//     does NOT fall back (checked as: the Auto failure text is byte-identical
//     to the Gui-mode failure text from the same child).
//
// Child modes can also be run directly (the pty-auto-gui mode is used for the
// xvfb Auto->GUI evidence run, where the display is kept):
//   ./test_tui_api_v4 --child pty-auto       (fallback; display forced empty)
//   ./test_tui_api_v4 --child pipe-auto      (no fallback; display forced empty)
//   ./test_tui_api_v4 --child pty-auto-gui   (display kept; expect Gui)
//
// Exit codes: 0 = all cases pass; 1 = first assertion failure. Child modes use
// distinct codes >= 10 (reported by the parent).
//
// Headless — run from the build dir:  ./test_tui_api_v4

#include "imtui/imtui.h" // ImGui:: context bookkeeping points
#include "tui_api.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

std::string g_where = "startup";

[[noreturn]] void fail(const std::string& what) {
    std::fprintf(stderr, "FAIL [%s]: %s\n", g_where.c_str(), what.c_str());
    std::exit(1);
}

void expect(bool cond, const std::string& what) {
    if (!cond)
        fail(what);
}

std::string toStd(String s) { return s.data ? std::string(s.data, s.len) : std::string(); }

String makeStr(const char* s) { return String{s, std::strlen(s)}; }

// Free an API-returned (owned) String and verify the ownership contract:
// String_Free flags a foreign pointer by setting the "not found in the owned
// set" error, so after freeing an owned string the error channel must be
// empty. Used on the tuiBackendNote() results.
bool freeOwnedStringChecked(String s) {
    String_Free(s);
    String err = tuiLastError();
    const bool ok = (err.data == nullptr || err.len == 0);
    String_Free(err);
    return ok;
}

// Case 1: backend-less state is inert; no ImGui context is ever needed.

void caseNoneStateInert() {
    g_where = "case 1: none-state no-ops";

    expect(ImGui::GetCurrentContext() == nullptr,
           "no ImGui context exists at the start of this case");

    TuiState* state = tuiCreateState();
    expect(state != nullptr, "tuiCreateState succeeds");

    expect(tuiBackendActive(state) == TuiBackendKind_None,
           "a fresh state reports TuiBackendKind_None");
    expect(tuiBackendActive(nullptr) == TuiBackendKind_None,
           "tuiBackendActive(NULL) reports TuiBackendKind_None");

    // Backend-less frame calls must be inert. There is NO ImGui context in
    // this process, so any ImGui access would fault here.
    tuiBackendNewFrame(state);
    tuiBackendRender(state);
    tuiBackendNewFrame(nullptr);
    tuiBackendRender(nullptr);
    expect(ImGui::GetCurrentContext() == nullptr,
           "backend-less frame calls did not create/need an ImGui context");

    // The NULL-vs-None asymmetry pinned by the v4 contract.
    expect(tuiRender(nullptr) == 0, "tuiRender(NULL) == 0 (exit; v3 convention)");
    expect(tuiRender(state) == 1, "tuiRender(none-state) == 1 (continue; inert, no ImGui access)");
    expect(ImGui::GetCurrentContext() == nullptr, "tuiRender(none-state) left ImGui untouched");

    tuiDestroyState(state);
}

// Case 2: destroy null-safety + a harness context survives a destroy.

void caseDestroyBasics() {
    g_where = "case 2: destroy-without-init";

    tuiDestroyState(nullptr); // must not crash

    TuiState* state = tuiCreateState();
    expect(state != nullptr, "tuiCreateState succeeds");
    tuiDestroyState(state); // no backend, no context: must not crash

    // A context created by the caller (the headless-harness pattern) must NOT
    // be destroyed by tuiDestroyState on a state that never ran tuiInit.
    ImGui::CreateContext();
    ImGuiContext* harness = ImGui::GetCurrentContext();
    expect(harness != nullptr, "harness ImGui context created");

    TuiState* st = tuiCreateState();
    expect(st != nullptr, "tuiCreateState succeeds with a harness context alive");
    tuiSetStatusText(st, makeStr("harness context alive")); // data path works
    tuiDestroyState(st);

    expect(ImGui::GetCurrentContext() == harness,
           "destroying a never-initialized state leaves the harness context current");

    ImGui::DestroyContext();
    expect(ImGui::GetCurrentContext() == nullptr, "harness context destroyed");
}

// Case 3: a failed Gui init is clean, retryable, and leaves no context.

void caseFailedGuiInit() {
    g_where = "case 3: failed Gui init";

    // Environment pinning: force an empty display env so GUI init fails
    // deterministically (the documented in-process failure case).
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");

    TuiState* state = tuiCreateState();
    expect(state != nullptr, "tuiCreateState succeeds");

    expect(tuiInit(state, TuiBackendMode_Gui) != 0, "Gui init fails without a display");
    String err1 = tuiLastError();
    expect(err1.data && err1.len > 0, "tuiLastError() reports the failed init");
    const std::string error1 = toStd(err1);
    String_Free(err1);
    std::fprintf(stderr, "  [info] Gui init failure reason: %s\n", error1.c_str());

    expect(tuiBackendActive(state) == TuiBackendKind_None,
           "the failed state stays backend-less (None)");
    String note = tuiBackendNote();
    expect(note.len == 0, "tuiBackendNote() is empty after a plain Gui failure (no fallback)");
    String_Free(note);
    expect(ImGui::GetCurrentContext() == nullptr,
           "the failed init cleaned up the ImGui context it created");

    // Retry is allowed on the same state — and must fail cleanly again, not
    // with an "already initialized" trap.
    expect(tuiInit(state, TuiBackendMode_Gui) != 0, "retry after failure fails cleanly");
    String err2 = tuiLastError();
    expect(err2.data && err2.len > 0, "retry reports a non-empty error");
    const std::string error2 = toStd(err2);
    String_Free(err2);
    expect(error2 == error1, "retry reports the same Gui-stage reason");
    expect(error2.find("already") == std::string::npos,
           "retry did not hit the already-initialized guard");
    expect(tuiBackendActive(state) == TuiBackendKind_None,
           "retry failure keeps the state backend-less");
    expect(ImGui::GetCurrentContext() == nullptr, "the retry also cleaned up its context");

    // tuiInit(NULL) is rejected with the pinned message.
    expect(tuiInit(nullptr, TuiBackendMode_Auto) != 0, "tuiInit(NULL) fails");
    String errN = tuiLastError();
    expect(toStd(errN) == "tuiInit: NULL state", "tuiInit(NULL) reports 'tuiInit: NULL state'");
    String_Free(errN);

    tuiDestroyState(state);
}

// Auto/TTY matrix children

// Arrange a PTY as stdin+stdout (both isatty() true). false = setup failure.
bool setupPtyStdio() {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0)
        return false;
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master);
        return false;
    }
    const char* slaveName = ptsname(master);
    if (slaveName == nullptr) {
        close(master);
        return false;
    }
    // Detach so the slave open below can claim a controlling terminal. When
    // this process is already a process-group leader (e.g. a direct --child
    // run under job control), setsid() fails with EPERM — harmless: the pty
    // fds below still serve the isatty checks and ncurses. Do not fail on it.
    (void)setsid();
    int slave = open(slaveName, O_RDWR);
    if (slave < 0) {
        close(master);
        return false;
    }
    struct winsize ws = {30, 100, 0, 0};
    ioctl(slave, TIOCSWINSZ, &ws);
    if (dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0) {
        close(slave);
        close(master);
        return false;
    }
    if (slave > STDERR_FILENO)
        close(slave);
    // master intentionally stays open until process exit (closing it would
    // SIGHUP the session).
    return true;
}

// Pipe/`/dev/null` as stdin+stdout (neither isatty()), /dev/null also
// absorbs stdout chatter.
bool setupPipeStdio() {
    int devnull = open("/dev/null", O_RDWR);
    if (devnull < 0)
        return false;
    if (dup2(devnull, STDIN_FILENO) < 0 || dup2(devnull, STDOUT_FILENO) < 0) {
        close(devnull);
        return false;
    }
    if (devnull > STDERR_FILENO)
        close(devnull);
    return true;
}

// Child: Auto mode with PTY stdio and a forced-empty display env — the
// fallback-to-TUI path. Return codes >= 10 carry the failing step.
int childPtyAuto() {
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");
    setenv("TERM", "xterm-256color", 1);
    if (!setupPtyStdio()) {
        std::fprintf(stderr, "child: pty setup failed\n");
        return 90;
    }

    TuiState* state = tuiCreateState();
    if (!state) {
        std::fprintf(stderr, "child: tuiCreateState failed\n");
        return 10;
    }
    if (tuiInit(state, TuiBackendMode_Auto) != 0) {
        String err = tuiLastError();
        std::fprintf(stderr, "child: tuiInit(Auto) failed: %s\n", toStd(err).c_str());
        String_Free(err);
        return 11;
    }
    if (tuiBackendActive(state) != TuiBackendKind_Tui) {
        std::fprintf(stderr, "child: expected Tui kind after fallback, got %d\n",
                     tuiBackendActive(state));
        return 12;
    }

    String note = tuiBackendNote();
    const std::string noteText = toStd(note);
    if (!freeOwnedStringChecked(note)) {
        std::fprintf(stderr, "child: tuiBackendNote() did not yield an owned String\n");
        return 23;
    }
    const std::string notePrefix = "GUI unavailable: ";
    const std::string noteSuffix = "; falling back to terminal UI";
    if (noteText.size() <= notePrefix.size() + noteSuffix.size() ||
        noteText.compare(0, notePrefix.size(), notePrefix) != 0 ||
        noteText.compare(noteText.size() - noteSuffix.size(), noteSuffix.size(), noteSuffix) != 0) {
        std::fprintf(stderr, "child: fallback note malformed: [%s]\n", noteText.c_str());
        return 13;
    }

    // Single-shot per success: a second init of the initialized state fails
    // cleanly and keeps the backend attached.
    if (tuiInit(state, TuiBackendMode_Tui) == 0) {
        std::fprintf(stderr, "child: re-init of an initialized state unexpectedly succeeded\n");
        return 14;
    }
    String err2 = tuiLastError();
    const std::string error2 = toStd(err2);
    String_Free(err2);
    if (error2.empty() || error2.find("another") != std::string::npos) {
        std::fprintf(stderr, "child: unexpected re-init error text: [%s]\n", error2.c_str());
        return 15;
    }
    if (tuiBackendActive(state) != TuiBackendKind_Tui) {
        std::fprintf(stderr, "child: backend kind changed after the failed re-init\n");
        return 16;
    }
    // The most recent tuiInit call had no fallback: the note is reset even
    // though an earlier successful init recorded one.
    String noteAfter = tuiBackendNote();
    if (noteAfter.len != 0) {
        std::fprintf(stderr, "child: note not reset by the rejected re-init\n");
        String_Free(noteAfter);
        return 22;
    }
    if (!freeOwnedStringChecked(noteAfter)) {
        std::fprintf(stderr, "child: tuiBackendNote() did not yield an owned String\n");
        return 24;
    }

    // At most one initialized state per process: a second state cannot init.
    TuiState* other = tuiCreateState();
    if (!other) {
        std::fprintf(stderr, "child: tuiCreateState (second state) failed\n");
        return 17;
    }
    if (tuiInit(other, TuiBackendMode_Tui) == 0) {
        std::fprintf(stderr, "child: second state init unexpectedly succeeded\n");
        return 18;
    }
    String err3 = tuiLastError();
    const std::string error3 = toStd(err3);
    String_Free(err3);
    if (error3.find("another") == std::string::npos) {
        std::fprintf(stderr, "child: unexpected second-state error text: [%s]\n", error3.c_str());
        return 19;
    }
    if (tuiBackendActive(other) != TuiBackendKind_None) {
        std::fprintf(stderr, "child: second state is not backend-less\n");
        return 20;
    }
    tuiDestroyState(other);

    // One real frame through the terminal backend on the pty.
    tuiBackendNewFrame(state);
    if (tuiRender(state) != 1) {
        std::fprintf(stderr, "child: tuiRender returned exit on a live TUI backend\n");
        return 21;
    }
    tuiBackendRender(state);

    tuiDestroyState(state);
    return 0;
}

// Child: Auto mode with pipe stdio (no TTY) and a forced-empty display env —
// must fail with the GUI error and not attempt the TUI.
int childPipeAuto() {
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");
    if (!setupPipeStdio()) {
        std::fprintf(stderr, "child: pipe stdio setup failed\n");
        return 90;
    }

    TuiState* state = tuiCreateState();
    if (!state) {
        std::fprintf(stderr, "child: tuiCreateState failed\n");
        return 30;
    }

    // The Gui-mode failure text is the baseline the Auto failure must equal
    // ("fail with the GUI error").
    if (tuiInit(state, TuiBackendMode_Gui) == 0) {
        std::fprintf(stderr, "child: Gui init unexpectedly succeeded without a display\n");
        return 31;
    }
    String guiErr = tuiLastError();
    const std::string guiError = toStd(guiErr);
    String_Free(guiErr);
    if (guiError.empty()) {
        std::fprintf(stderr, "child: Gui failure carried no error text\n");
        return 32;
    }

    if (tuiInit(state, TuiBackendMode_Auto) == 0) {
        std::fprintf(stderr, "child: Auto init unexpectedly succeeded without a display\n");
        return 33;
    }
    String autoErr = tuiLastError();
    const std::string autoError = toStd(autoErr);
    String_Free(autoErr);
    if (autoError != guiError) {
        std::fprintf(stderr,
                     "child: Auto failure is not the GUI error\n  Gui : [%s]\n  Auto: [%s]\n",
                     guiError.c_str(), autoError.c_str());
        return 34;
    }
    if (tuiBackendActive(state) != TuiBackendKind_None) {
        std::fprintf(stderr, "child: state is not backend-less after the Auto failure\n");
        return 35;
    }
    String note = tuiBackendNote();
    if (note.len != 0) {
        std::fprintf(stderr, "child: note non-empty after a no-fallback failure\n");
        String_Free(note);
        return 36;
    }
    if (!freeOwnedStringChecked(note)) {
        std::fprintf(stderr, "child: tuiBackendNote() did not yield an owned String\n");
        return 38;
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        std::fprintf(stderr, "child: ImGui context leaked after the failures\n");
        return 37;
    }

    tuiDestroyState(state);
    return 0;
}

// Child: Auto mode with PTY stdio and the display KEPT — expected to resolve
// to the GUI backend. Not part of the default (displayless) run; used by the
// xvfb evidence run:
//   xvfb-run -a ./test_tui_api_v4 --child pty-auto-gui
int childPtyAutoGui() {
    setenv("TERM", "xterm-256color", 1);
    if (!setupPtyStdio()) {
        std::fprintf(stderr, "child: pty setup failed\n");
        return 90;
    }

    TuiState* state = tuiCreateState();
    if (!state) {
        std::fprintf(stderr, "child: tuiCreateState failed\n");
        return 60;
    }
    if (tuiInit(state, TuiBackendMode_Auto) != 0) {
        String err = tuiLastError();
        std::fprintf(stderr, "child: Auto init failed (display missing?): %s\n",
                     toStd(err).c_str());
        String_Free(err);
        return 61;
    }
    if (tuiBackendActive(state) != TuiBackendKind_Gui) {
        std::fprintf(stderr, "child: expected Gui kind, got %d\n", tuiBackendActive(state));
        return 62;
    }
    String note = tuiBackendNote();
    if (note.len != 0) {
        std::fprintf(stderr, "child: note non-empty after a GUI init (no fallback)\n");
        String_Free(note);
        return 63;
    }
    if (!freeOwnedStringChecked(note)) {
        std::fprintf(stderr, "child: tuiBackendNote() did not yield an owned String\n");
        return 65;
    }

    tuiBackendNewFrame(state);
    if (tuiRender(state) != 1) {
        std::fprintf(stderr, "child: tuiRender returned exit on a live GUI backend\n");
        return 64;
    }
    tuiBackendRender(state);

    tuiDestroyState(state);
    return 0;
}

int runChildMode(const char* mode) {
    // Backstop against a hung child: self-terminate rather than hang a direct
    // evidence run (the parent-side runner also enforces its own deadline).
    alarm(90);
    if (std::strcmp(mode, "pty-auto") == 0)
        return childPtyAuto();
    if (std::strcmp(mode, "pipe-auto") == 0)
        return childPipeAuto();
    if (std::strcmp(mode, "pty-auto-gui") == 0)
        return childPtyAutoGui();
    std::fprintf(stderr, "unknown child mode: %s\n", mode);
    return 99;
}

// Child runner

struct ChildResult {
    bool exited = false; // false = died by signal
    int code = -1;       // exit code when exited
    int signal = 0;
    std::string errText; // captured child stderr
};

std::string describeChild(const ChildResult& r) {
    std::string d = "stderr=[" + r.errText + "]";
    if (r.exited)
        d = "exit=" + std::to_string(r.code) + " " + d;
    else
        d = "killed by signal " + std::to_string(r.signal) + " " + d;
    return d;
}

// Fork a child that runs `mode` with stderr wired to a capture pipe. The
// parent enforces an overall deadline (poll + SIGKILL) so a blocking child
// fails the suite instead of hanging it.
ChildResult runChild(const char* mode) {
    ChildResult r;
    int errPipe[2];
    if (pipe(errPipe) != 0)
        fail("pipe() failed");
    pid_t pid = fork();
    if (pid < 0) {
        close(errPipe[0]);
        close(errPipe[1]);
        fail("fork() failed");
    }
    if (pid == 0) {
        close(errPipe[0]);
        if (dup2(errPipe[1], STDERR_FILENO) < 0)
            _exit(97);
        if (errPipe[1] != STDERR_FILENO)
            close(errPipe[1]);
        _exit(runChildMode(mode));
    }
    close(errPipe[1]);

    static constexpr int kChildTimeoutSeconds = 60;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(kChildTimeoutSeconds);
    bool timedOut = false;
    char buf[4096];
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timedOut = true;
            break;
        }
        const auto remainingMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        struct pollfd pfd{errPipe[0], POLLIN, 0};
        const int pr = poll(&pfd, 1, static_cast<int>(remainingMs));
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break; // stop draining; the child status below reports the outcome
        }
        if (pr == 0) {
            timedOut = true;
            break;
        }
        const ssize_t n = read(errPipe[0], buf, sizeof(buf));
        if (n > 0) {
            r.errText.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break; // EOF (n == 0) or an unrecoverable read error
    }
    close(errPipe[0]);
    if (timedOut) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        fail("child '" + std::string(mode) + "' timed out after " +
             std::to_string(kChildTimeoutSeconds) + "s; stderr so far: [" + r.errText + "]");
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        fail("waitpid() failed");
    if (WIFEXITED(status)) {
        r.exited = true;
        r.code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        r.signal = WTERMSIG(status);
    }
    return r;
}

// Cases 4/5: the Auto/TTY matrix

void caseAutoTtyMatrix() {
    g_where = "case 4: Auto + PTY stdio -> fallback to the terminal backend";
    ChildResult pty = runChild("pty-auto");
    expect(pty.exited && pty.code == 0, "pty-auto child failed: " + describeChild(pty));
    expect(pty.errText.find("GUI unavailable: ") != std::string::npos &&
               pty.errText.find("; falling back to terminal UI") != std::string::npos,
           "the pinned fallback line reached stderr: " + describeChild(pty));

    g_where = "case 5: Auto + pipe stdio -> GUI error, no fallback";
    ChildResult pipe = runChild("pipe-auto");
    expect(pipe.exited && pipe.code == 0, "pipe-auto child failed: " + describeChild(pipe));
    expect(pipe.errText.find("falling back") == std::string::npos,
           "no fallback line expected for non-TTY stdio: " + describeChild(pipe));
}

} // namespace

int main(int argc, char** argv) {
    // Direct child modes (the stdio arrangement is part of the mode), also
    // used for the xvfb Auto->GUI evidence run.
    if (argc == 3 && std::strcmp(argv[1], "--child") == 0)
        return runChildMode(argv[2]);

    static_assert(TUI_API_VERSION == 4, "this contract test pins the v4 API generation");

    caseNoneStateInert();
    caseDestroyBasics();
    caseFailedGuiInit();
    caseAutoTtyMatrix();

    std::printf("OK: all v4 API contract cases passed\n");
    return 0;
}
