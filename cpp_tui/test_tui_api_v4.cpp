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
//   - the cancel flag (tuiRequestCancel / tuiTakeCancel, task 3): NULL-safe,
//     consume-on-read, idempotent while a request is pending, and off the
//     frame thread only after a frame has claimed one (debugIsFrameThread);
//   - the v5 session-row fields (task 24): SessionItem's size and offsets are
//     pinned, a snapshot round-trips into SessionEntry unchanged, and the fields
//     the ABI must not carry (createdAt, unread, pinned) are absent;
//   - the v5 pipeline lane fields (task 39a): PipelineChatMessage's size and
//     offsets are pinned, the numbers and the tool name round-trip into the
//     lane the Agents tab draws, the v4 sentence is untouched, and
//     `0/0` (no reading) is told apart from `0/8192` (a real zero);
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
#include "tui.h"         // llmfun::tui::debugIsFrameThread (cancel-flag case)
#include "tui_api.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h> // mkdir/rmdir: the archive-failure case puts a directory in the way
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <vector>

// `TuiState`: the handle's private shape, shared by the contract tests that read back what a
// `tui*` call wrote.
#include "test/tui_state_handle.h"

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

void caseQueryHistory() {
    g_where = "case 1b2: tuiInitQueryHistory keeps the newest MAX_HISTORY entries";

    TuiState* state = tuiCreateState();
    expect(state != nullptr, "tuiCreateState succeeds");

    const auto load = [state](size_t count) {
        std::vector<std::string> storage;
        storage.reserve(count);
        for (size_t i = 0; i < count; ++i)
            storage.push_back("q" + std::to_string(i));
        std::vector<String> entries;
        entries.reserve(count);
        for (const auto& s : storage)
            entries.push_back(String{s.data(), s.size()});
        tuiInitQueryHistory(state, entries.data(), entries.size());
    };

    // A long history keeps the last MAX_HISTORY entries, oldest first. The
    // wrapper loop used to stop at `n` (the number to keep) instead of `count`,
    // so 1000 entries copied none and 600 copied 400.
    load(1000);
    {
        const auto& history = state->inner->userQuery.inputHistory;
        expect(history.size() == llmfun::tui::UserQueryState::MAX_HISTORY,
               "a 1000-entry history is capped at MAX_HISTORY (got " +
                   std::to_string(history.size()) + ")");
        expect(history.front() == "q500",
               "the oldest kept entry is q500 (got " + history.front() + ")");
        expect(history.back() == "q999", "the newest entry is q999 (got " + history.back() + ")");
    }
    load(600);
    {
        const auto& history = state->inner->userQuery.inputHistory;
        expect(history.size() == llmfun::tui::UserQueryState::MAX_HISTORY,
               "a 600-entry history keeps 500 entries (got " + std::to_string(history.size()) +
                   ")");
        expect(history.front() == "q100",
               "the first entry of the kept tail is q100 (got " + history.front() + ")");
        expect(history.back() == "q599", "the last entry is q599 (got " + history.back() + ")");
    }

    // A short history is copied whole, oldest first.
    load(3);
    {
        const auto& history = state->inner->userQuery.inputHistory;
        expect(history.size() == 3, "a 3-entry history is copied whole");
        expect(history.front() == "q0" && history.back() == "q2",
               "a short history keeps its order");
    }

    // An empty session forwards null/0; the setter must clear, not keep the
    // previous session's entries.
    tuiInitQueryHistory(state, nullptr, 0);
    expect(state->inner->userQuery.inputHistory.empty(),
           "an empty history clears the previous session's entries");

    tuiDestroyState(state);
}

void caseCancelFlag() {
    g_where = "case 1c: cancel flag is NULL-safe, consume-on-read and frame-thread-only";

    // NULL state: every call is a no-op, and there is no ImGui context here.
    tuiRequestCancel(nullptr);
    expect(tuiTakeCancel(nullptr) == 0, "tuiTakeCancel(NULL) == 0");

    TuiState* state = tuiCreateState();
    expect(state != nullptr, "tuiCreateState succeeds");

    // Nothing requested yet.
    expect(tuiTakeCancel(state) == 0, "no request -> 0");

    // Two requests, one slot: the first take returns 1, the second 0, and a
    // third take stays 0 (consume-on-read, no counting).
    tuiRequestCancel(state);
    tuiRequestCancel(state);
    expect(tuiTakeCancel(state) == 1, "double request -> one consumed cancel");
    expect(tuiTakeCancel(state) == 0, "the second take reports no cancel");
    expect(tuiTakeCancel(state) == 0, "the flag stays empty");

    // A request after a take is a fresh cancel, but only once the run ended: the
    // latch (cancelRequested) keeps a second request a no-op until then.
    llmfun::tui::TuiState loop; // the C++ state owns the latch/claim (not ABI)
    llmfun::tui::tuiRequestCancel(loop);
    expect(llmfun::tui::tuiTakeCancel(loop), "first request -> consumed");
    expect(loop.cancelRequested, "the UI latch survives the pending flag");
    llmfun::tui::tuiRequestCancel(loop); // second click while the agent stops
    expect(!llmfun::tui::tuiTakeCancel(loop),
           "a second request while the latch is set queues nothing");
    // The run ends: ready drops the latch, so the composer can send again.
    llmfun::tui::tuiSetReadyStatus(loop, true);
    expect(!loop.cancelRequested, "ready clears the Stopping latch");
    llmfun::tui::tuiRequestCancel(loop);
    expect(llmfun::tui::tuiTakeCancel(loop), "a new request after the run is consumable again");
    expect(loop.cancelRequested, "and re-arms the latch");

    // Frame-thread bookkeeping: before any frame, every thread counts as the
    // frame thread (headless tests and pre-init calls are legal). The C ABI
    // never renders without a backend, so the claim is made here through the
    // C++ entry point the frame loop uses.
    expect(llmfun::tui::debugIsFrameThread(loop),
           "no frame yet: the calling thread is treated as the frame thread");
    llmfun::tui::debugMarkFrameThread(loop);
    expect(llmfun::tui::debugIsFrameThread(loop), "the claiming thread stays the frame thread");
    bool otherThreadIsFrame = true;
    std::thread other([&otherThreadIsFrame, &loop]() {
        otherThreadIsFrame = llmfun::tui::debugIsFrameThread(loop);
    });
    other.join();
    expect(!otherThreadIsFrame, "another thread is not the frame thread once one is claimed");
    // The claim is per state: a fresh state is claimable from any thread.
    llmfun::tui::TuiState second;
    expect(llmfun::tui::debugIsFrameThread(second),
           "a second state's claim is independent of the first one's");
    llmfun::tui::tuiResetFrameThread(loop);
    expect(llmfun::tui::debugIsFrameThread(loop), "the claim can be released for tests");

    tuiDestroyState(state);
}

void caseRuntimeStatus() {
    g_where = "case 1e: tuiSetRuntimeStatus is NULL-safe, store-and-clear";

    // NULL state, NULL status: both are no-ops, and there is no ImGui context here.
    tuiSetRuntimeStatus(nullptr, nullptr);

    TuiRuntimeStatus status{};
    status.phase = 2; /* tooling */
    status.detail = String{"read_file", 9};
    status.contextUsed = 42000;
    status.contextMax = 128000;
    status.compactAt = 115200;
    status.tokensPerSecond = 38.1;
    status.elapsedMs = 1234;
    status.pendingQuestions = 0;
    tuiSetRuntimeStatus(nullptr, &status); // still no state: no-op

    TuiState* state = tuiCreateState();
    expect(state != nullptr, "tuiCreateState succeeds");
    expect(state->inner != nullptr, "the handle wraps a state");

    // The state stores them, and the chips are formatted from them at the boundary.
    tuiSetRuntimeStatus(state, &status);
    expect(state->inner->ui.status.structured, "the state stores the numbers");
    expect(state->inner->ui.status.phase == 2, "the phase ordinal crosses");
    expect(state->inner->ui.status.detail == "read_file", "the step is copied during the call");
    expect(state->inner->ui.status.contextUsed == 42000, "the used context crosses");
    expect(state->inner->ui.status.contextMax == 128000, "the model's context size crosses");
    expect(state->inner->ui.status.compactAt == 115200, "the compaction threshold crosses");
    expect(state->inner->ui.status.tokensPerSecond == 38.1, "the rate crosses");
    expect(state->inner->ui.status.elapsedMs == 1234, "the run's clock crosses");
    expect(state->inner->ui.status.pendingQuestions == 0, "the question count crosses");

    // An ordinal this build does not know is idle, not a neighbour, and the rest of the numbers
    // still apply: a sender may be newer than the UI.
    TuiRuntimeStatus unknown = status;
    unknown.phase = 99;
    tuiSetRuntimeStatus(state, &unknown);
    expect(state->inner->ui.status.phase == 0, "an unknown phase folds to idle");
    expect(state->inner->ui.status.contextUsed == 42000, "and the numbers are still readable");

    // `phase == 6` is the one fact the sentence cannot express: the offline card and the disabled
    // composer read this flag, and nothing else writes it.
    TuiRuntimeStatus offline = status;
    offline.phase = 6;
    tuiSetRuntimeStatus(state, &offline);
    expect(state->inner->offline, "phase 6 makes the model unreachable");

    // NULL clears, and drops the fact only the numbers can carry.
    tuiSetRuntimeStatus(state, nullptr);
    expect(!state->inner->ui.status.structured, "NULL clears the structured status");
    expect(!state->inner->offline, "and the offline fact with it");

    // A clear is always honoured: the set above must be droppable again.
    tuiSetRuntimeStatus(state, &status);
    expect(state->inner->ui.status.structured, "set again before the clear");
    tuiSetRuntimeStatus(state, nullptr);
    expect(!state->inner->ui.status.structured, "and the clear drops it");

    tuiDestroyState(state);
}

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
    std::array<int, 2> errPipe{};
    if (pipe(errPipe.data()) != 0)
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
    std::array<char, 4096> buf{};
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
        const ssize_t n = read(errPipe[0], buf.data(), buf.size());
        if (n > 0) {
            r.errText.append(buf.data(), static_cast<size_t>(n));
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

// Member detectors for "the ABI must not carry this field". `createdAt` is
// deliberately not sent (D-SESSION-TS: grouping and relative time use the update
// stamp only) and `unread`/`pinned` are UI-local (FR-03, D-19), so a future edit
// that adds one of them to SessionItem fails here instead of surfacing as a rail
// that draws a fact the store never had.
template <typename T, typename = void> struct HasCreatedAt : std::false_type {};
template <typename T>
struct HasCreatedAt<T, std::void_t<decltype(&T::createdAt)>> : std::true_type {};

template <typename T, typename = void> struct HasUnread : std::false_type {};
template <typename T> struct HasUnread<T, std::void_t<decltype(&T::unread)>> : std::true_type {};

template <typename T, typename = void> struct HasPinned : std::false_type {};
template <typename T> struct HasPinned<T, std::void_t<decltype(&T::pinned)>> : std::true_type {};

// Case 1d: the v5 session-row fields (task 24)
//
// The snapshot struct grew three appended fields, so this case pins exactly what
// an ABI contract has to promise: every pre-v5 offset is unchanged, the total
// size is 80, and a snapshot round-trips through `tuiSetSessionList()` into
// `SessionEntry` unchanged. It also pins the two degradation rules the rail
// depends on: an all-zero (pre-v5-style) item is a legal snapshot, and the fields
// that must not cross the ABI (`createdAt`, `unread`, `pinned`) do not exist on it.
void caseSessionRowFields() {
    g_where = "case 1d: SessionItem v5 appended fields";

    static_assert(!HasCreatedAt<SessionItem>::value,
                  "createdAt is deliberately not sent (D-SESSION-TS)");
    static_assert(!HasUnread<SessionItem>::value && !HasPinned<SessionItem>::value,
                  "unread/pinned are UI-local and never cross the ABI (FR-03, D-19)");

    static_assert(sizeof(SessionItem) == 80, "SessionItem is 80 bytes on the 64-bit ABI");
    static_assert(offsetof(SessionItem, id) == 0, "SessionItem::id keeps offset 0");
    static_assert(offsetof(SessionItem, title) == 16, "SessionItem::title keeps offset 16");
    static_assert(offsetof(SessionItem, preview) == 32, "SessionItem::preview keeps offset 32");
    static_assert(offsetof(SessionItem, messageCount) == 48,
                  "SessionItem::messageCount keeps offset 48");
    static_assert(offsetof(SessionItem, isActive) == 56, "SessionItem::isActive keeps offset 56");
    // The three v5 appends.
    static_assert(offsetof(SessionItem, updatedAtMs) == 64, "updatedAtMs is appended at 64");
    static_assert(offsetof(SessionItem, flags) == 72, "flags is appended at 72");
    static_assert(offsetof(SessionItem, reserved) == 76, "reserved is appended at 76");
    static_assert(sizeof(SessionItem::updatedAtMs) == 8,
                  "updatedAtMs is a 64-bit millisecond count");
    static_assert(sizeof(SessionItem::flags) == 4 && sizeof(SessionItem::reserved) == 4,
                  "flags and reserved are 32-bit");
    static_assert(TuiSessionFlag_Running == 1, "bit 0 is the running flag");
    static_assert(llmfun::tui::kSessionFlagRunning == TuiSessionFlag_Running,
                  "the C++ mirror of the running flag matches the C header");

    TuiState* state = tuiCreateState();
    expect(state != nullptr, "tuiCreateState succeeds");

    // Two rows: a fully-populated one and a pre-v5-style all-zero one.
    const std::string ids[2] = {"20260101-120000-aaaa", "20260101-130000-bbbb"};
    const std::string titles[2] = {"Dated", "Undated"};
    std::vector<SessionItem> items(2);
    items[0] = SessionItem{String{ids[0].data(), ids[0].size()},
                           String{titles[0].data(), titles[0].size()},
                           String{nullptr, 0},
                           /*messageCount=*/7,
                           /*isActive=*/0,
                           /*updatedAtMs=*/1767272400000LL,
                           /*flags=*/TuiSessionFlag_Running,
                           /*reserved=*/0};
    items[1] = SessionItem{String{ids[1].data(), ids[1].size()},
                           String{titles[1].data(), titles[1].size()},
                           String{nullptr, 0},
                           /*messageCount=*/0,
                           /*isActive=*/1,
                           /*updatedAtMs=*/0,
                           /*flags=*/0,
                           /*reserved=*/0};
    tuiSetSessionList(state, items.data(), items.size());

    const auto& rows = state->inner->sessionPanel.sessions;
    expect(rows.size() == 2, "the snapshot reached SessionEntry");
    expect(rows[0].updatedAtMs == 1767272400000LL, "updatedAtMs round-trips unchanged");
    expect(rows[0].flags == TuiSessionFlag_Running, "flags bit 0 round-trips unchanged");
    expect(!rows[0].isActive, "the pre-v5 fields still carry their own values");
    expect(rows[0].messageCount == 7, "messageCount still round-trips");
    expect(rows[1].updatedAtMs == 0, "a zero timestamp stays zero (unknown, no fabrication)");
    expect(rows[1].flags == 0, "a zero flag stays zero");
    expect(state->inner->sessionPanel.activeId == ids[1], "the active id is still derived");

    // An all-zero snapshot (what a pre-v5 caller produces) is legal: no crash,
    // no fabricated time, no running row.
    std::vector<SessionItem> zeroed(1);
    zeroed[0] = SessionItem{};
    zeroed[0].id = String{ids[1].data(), ids[1].size()};
    tuiSetSessionList(state, zeroed.data(), zeroed.size());
    expect(state->inner->sessionPanel.sessions.size() == 1 &&
               state->inner->sessionPanel.sessions[0].updatedAtMs == 0 &&
               state->inner->sessionPanel.sessions[0].flags == 0,
           "a snapshot that zeroes the v5 fields is accepted as 'unknown time, no run'");

    // `reserved` is not carried anywhere: the ABI documents it as zero, and a
    // value in it must not reach the UI state as a third fact. Compare the whole
    // row, not the two v5 fields - a byte offset would put it in `id`, `title`,
    // `preview`, `messageCount` or `isActive` just as happily. The baseline is the
    // same snapshot with `reserved == 0`.
    items[0].reserved = 0;
    tuiSetSessionList(state, items.data(), items.size());
    const llmfun::tui::SessionEntry before = state->inner->sessionPanel.sessions[0];
    items[0].reserved = 1234;
    tuiSetSessionList(state, items.data(), items.size());
    const llmfun::tui::SessionEntry& after = state->inner->sessionPanel.sessions[0];
    expect(after.id == before.id && after.title == before.title &&
               after.preview == before.preview && after.messageCount == before.messageCount &&
               after.isActive == before.isActive && after.updatedAtMs == before.updatedAtMs &&
               after.flags == before.flags,
           "reserved carries nothing: every SessionEntry field is unchanged");

    tuiDestroyState(state);
}

// Case 1f: the pipeline lane fields (task 39a)
//
// `PipelineChatMessage` grew four appended fields so the Agents tab stops parsing D's text for
// the context count and the tool name. The case pins the wire contract (the size, every offset,
// the pre-v5 ones unchanged), round-trips the fields through `tuiPipelineAgentUpdate` into the
// lane the workbench draws while the v4 sentence is untouched, and pins the two degradation
// rules the row depends on: `0/0` is no reading, `0/8192` is a real zero, and `reserved` carries
// nothing.
void casePipelineLaneFields() {
    g_where = "case 1f: PipelineChatMessage v5 appended fields";

    static_assert(sizeof(PipelineChatMessage) == 120,
                  "PipelineChatMessage is 120 bytes on the 64-bit ABI");
    static_assert(offsetof(PipelineChatMessage, content) == 0, "content keeps offset 0");
    static_assert(offsetof(PipelineChatMessage, reasoning) == 16, "reasoning keeps offset 16");
    static_assert(offsetof(PipelineChatMessage, role) == 32, "role keeps offset 32");
    static_assert(offsetof(PipelineChatMessage, finishReason) == 48,
                  "finishReason keeps offset 48");
    static_assert(offsetof(PipelineChatMessage, status) == 64, "status keeps offset 64");
    // The four v5 appends.
    static_assert(offsetof(PipelineChatMessage, currentTool) == 80,
                  "currentTool is appended at 80");
    static_assert(offsetof(PipelineChatMessage, contextUsed) == 96,
                  "contextUsed is appended at 96");
    static_assert(offsetof(PipelineChatMessage, contextMax) == 104,
                  "contextMax is appended at 104");
    static_assert(offsetof(PipelineChatMessage, reserved) == 112, "reserved is appended at 112");
    static_assert(sizeof(PipelineChatMessage::contextUsed) == 8 &&
                      sizeof(PipelineChatMessage::contextMax) == 8,
                  "the context numbers are 64-bit");
    static_assert(TUI_API_VERSION == 6, "the lane fields landed inside v5 (additive, no re-bump)");
    static_assert(sizeof(SessionItem) == 80, "SessionItem is unaffected by the lane fields");

    // NULL-safe, and an empty agent id still registers nothing.
    tuiPipelineAgentUpdate(nullptr, makeStr("coder"), PipelineChatMessage{});
    TuiState* state = tuiCreateState();
    expect(state != nullptr, "tuiCreateState succeeds");
    tuiPipelineAgentUpdate(state, String{nullptr, 0}, PipelineChatMessage{});
    expect(state->inner->ui.agents.empty(), "an update with no agent id registers no lane");

    const std::string content =
        "Let me read that file.\n--- Tool ---\nread_file\n```json\n{}\n```\n";
    const std::string role = "assistant";
    const std::string status = "Context 1234/8192 tokens | 12.3 tok/s";
    std::string tool = "read_file";
    PipelineChatMessage msg{};
    msg.content = String{content.data(), content.size()};
    msg.role = String{role.data(), role.size()};
    msg.status = String{status.data(), status.size()};
    msg.currentTool = String{tool.data(), tool.size()};
    msg.contextUsed = 1234;
    msg.contextMax = 8192;

    tuiPipelineAgentUpdate(state, makeStr("coder"), msg);
    // A second tick: the release path (auto-registration builds the v4 store's `AgentStream` from
    // the first four strings only, unchanged since task 30), so the sentence is checked here.
    tuiPipelineAgentUpdate(state, makeStr("coder"), msg);

    // The v4 store is untouched by the new fields: it keeps its own copy of the sentence,
    // which nothing draws since task 41 removed the renderer that did.
    expect(state->inner->left.agents.size() == 1, "the v4 store registers the agent once");
    expect(state->inner->left.agents[0].stream.status == status,
           "the v4 store still receives the status sentence");
    expect(state->inner->left.agents[0].stream.content == content,
           "...and the agent's stream text, unchanged");

    // ... and the lane the Agents tab draws carries the count and the tool name as fields. The
    // lane is re-fetched for every assertion: a reference into the vector would dangle - turning a
    // failed check into undefined behaviour - the moment this case grows a second agent.
    expect(state->inner->ui.agents.size() == 1, "the lane mirror registers the agent too");
    const auto lane = [&state]() -> const llmfun::tui::model::AgentLane& {
        return state->inner->ui.agents.front();
    };
    expect(lane().id == "coder" && lane().label == "coder", "the lane is named after the agent id");
    expect(lane().role == role, "the role crosses");
    expect(lane().content == content, "the stream text crosses");
    expect(lane().tokens == 1234,
           "contextUsed crosses (got " + std::to_string(lane().tokens) + ")");
    expect(lane().currentTool == "read_file",
           "currentTool crosses (got " + lane().currentTool + ")");

    // Text independence: a sentence full of numbers that disagrees with the fields, and a text
    // with no `--- Tool ---` marker, change no lane value - the mirror reads fields only.
    const std::string decoyStatus = "Context 999999/8192 tokens | 999.0 tok/s";
    const std::string plainContent = "no marker in here at all";
    msg.status = String{decoyStatus.data(), decoyStatus.size()};
    msg.content = String{plainContent.data(), plainContent.size()};
    tuiPipelineAgentUpdate(state, makeStr("coder"), msg);
    expect(lane().tokens == 1234, "the count is the field, not the biggest number in the sentence");
    expect(lane().currentTool == "read_file", "the tool name is the field, not the stream's text");
    expect(state->inner->ui.agents.size() == 1, "a second update updates the same lane");

    // Zero is zero whichever way D means it: `0/8192` (a real zero) and `0/0` (no reading) both
    // store 0, so the row omits the number. `contextMax` stops at the ABI seam - it is not stored,
    // which is why the two cannot disagree in the lane.
    msg.contextUsed = 0;
    tuiPipelineAgentUpdate(state, makeStr("coder"), msg); // contextMax is still 8192 here
    expect(lane().tokens == 0, "`0/8192` stores 0, not the previous count");
    msg.contextMax = 0;
    tuiPipelineAgentUpdate(state, makeStr("coder"), msg);
    expect(lane().tokens == 0, "`0/0` stores 0 as well");

    // Every String is copied during the call (C-02): scribbling over the caller's buffer must
    // not touch the lane's copy.
    std::string scribbled = "write_file";
    msg.currentTool = String{scribbled.data(), scribbled.size()};
    msg.contextUsed = 42;
    msg.contextMax = 8192;
    tuiPipelineAgentUpdate(state, makeStr("coder"), msg);
    scribbled.assign(scribbled.size(), 'x');
    expect(lane().currentTool == "write_file",
           "currentTool was copied during the call, not borrowed");
    expect(lane().tokens == 42, "the numbers crossed with it");

    // `reserved` is documented as zero and carries nothing: a value in it must not reach the
    // lane. The rest of the update is unchanged (the buffer above is garbage now, so the String
    // is rebuilt from a fresh copy), so any difference would be `reserved`'s doing.
    std::string toolAgain = "write_file";
    msg.currentTool = String{toolAgain.data(), toolAgain.size()};
    msg.reserved = 1234;
    tuiPipelineAgentUpdate(state, makeStr("coder"), msg);
    expect(lane().content == plainContent,
           "reserved carries nothing: the stream text is unchanged");
    expect(lane().role == role, "reserved carries nothing: the role is unchanged");
    expect(lane().tokens == 42, "reserved carries nothing: the count is unchanged");
    expect(lane().currentTool == "write_file",
           "reserved carries nothing: the tool name is unchanged");
    expect(lane().state == llmfun::tui::model::kAgentStateRunning,
           "reserved carries nothing: the lane is still running");

    // Finishing the lane keeps the numbers it last reported.
    tuiPipelineAgentDone(state, makeStr("coder"));
    expect(lane().state == llmfun::tui::model::kAgentStateDone && lane().tokens == 42 &&
               lane().currentTool == "write_file",
           "tuiPipelineAgentDone keeps the lane's last numbers");

    tuiDestroyState(state);
}

// The structured log records (task 32, v5): the fields D sends arrive unchanged, a record
// with no clock is stamped at arrival, the two-string compatibility wrapper maps onto the
// same store, and the ring is the single bounded store with a dropped counter.
void caseLogRecords() {
    g_where = "case: structured log records (v5, task 32)";

    // NULL-safe: no crash, and a NULL record is a no-op.
    tuiAddLogRecord(nullptr, nullptr);
    TuiState* nullState = tuiCreateState();
    tuiAddLogRecord(nullState, nullptr);
    tuiAddLogMessageEx(nullptr, TuiLogLevel_Info, makeStr("s"), makeStr("t"));
    expect(nullState->inner->logRecords.empty(), "a NULL record appends nothing");
    tuiDestroyState(nullState);

    TuiState* state = tuiCreateState();
    expect(state != nullptr, "tuiCreateState succeeds");

    // Round trip: every field the ABI carries arrives unchanged.
    TuiLogRecord record{};
    const char* message = "tool read_file failed";
    const char* detail = "stack: at pool.d:412";
    const char* module = "llm.agent.pool";
    const char* function = "processTurn";
    const char* file = "/src/agent/pool.d";
    const char* payload = "turn-7";
    record.level = TuiLogLevel_Error;
    record.action = TuiLogAction_Retry;
    record.timestampMs = 1'782'000'000'123;
    record.line = 412;
    record.reserved = 0;
    record.message = String{message, std::strlen(message)};
    record.detail = String{detail, std::strlen(detail)};
    record.module = String{module, std::strlen(module)};
    record.function = String{function, std::strlen(function)};
    record.file = String{file, std::strlen(file)};
    record.actionPayload = String{payload, std::strlen(payload)};
    tuiAddLogRecord(state, &record);

    expect(state->inner->logRecords.size() == 1, "one call appends exactly one record");
    const llmfun::tui::LogRecord& stored = state->inner->logRecords.back();
    expect(stored.level == TuiLogLevel_Error && stored.action == TuiLogAction_Retry,
           "level and action survive the boundary");
    expect(stored.timestampMs == 1'782'000'000'123, "the sender's clock is kept");
    expect(stored.line == 412, "the source line is kept");
    expect(stored.message == message && stored.detail == detail, "message and detail are copied");
    expect(stored.module == module && stored.function == function && stored.file == file,
           "module, function and file are copied");
    expect(stored.actionPayload == payload, "the action payload is kept");
    // The strings are *copied*: scribbling over the caller's buffers must not touch the store
    // (C-02: D may free its buffers the moment the call returns). The inbound view is restored
    // afterwards, so the checks below still send the record they describe.
    {
        const String original = record.message;
        std::vector<char> scratch(message, message + std::strlen(message));
        record.message = String{scratch.data(), scratch.size()};
        scratch.assign(scratch.size(), 'x');
        expect(stored.message == message, "the message was copied during the call, not borrowed");
        record.message = original;
    }

    // The archive line keeps the historical `"%s - %s: %s [%s:%d]"` shape, formatted in C++
    // from the record (design §6.4: the archive format is preserved even though D no longer
    // formats a line).
    {
        const std::string line = llmfun::tui::logArchiveLine(stored);
        expect(line.find(" - error: tool read_file failed [processTurn:412]") != std::string::npos,
               "the archive line keeps D's historical shape (got: " + line + ")");
        expect(line.size() > 4 && line[4] == '-' &&
                   line.find("T00:00:00.123 - error: ") != std::string::npos,
               "the archive's clock is the record's, in UTC with milliseconds (got: " + line + ")");
        llmfun::tui::LogRecord withoutMetadata;
        withoutMetadata.level = TuiLogLevel_Info;
        withoutMetadata.message = "plain";
        expect(llmfun::tui::logArchiveLine(withoutMetadata).find('[') == std::string::npos,
               "the [function:line] suffix appears only when the record has metadata");
    }

    // The archive sink (`tuiSetLogging(true)` -> `llmfun_ui_log.txt`) writes the same historical
    // line: the format did not move, only the side that formats it (design §6.4).
    //
    // The file is removed first: it is append-only by design (the product's own rule) and every
    // binary in this directory writes to the same path, so a check that read stale content
    // could not notice a broken sink (task-31 review I4).
    {
        std::remove("llmfun_ui_log.txt");
        tuiSetLogging(state, 1);
        tuiAddLogRecord(state, &record);
        tuiSetLogging(state, 0); // closes the record handle, which flushes it
        FILE* file = std::fopen("llmfun_ui_log.txt", "rb");
        expect(file != nullptr, "tuiSetLogging(true) created the archive file");
        std::string all;
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof buffer, file)) > 0)
            all.append(buffer, read);
        std::fclose(file);
        const std::string line = " - error: tool read_file failed [processTurn:412]";
        expect(all.find(line) != std::string::npos,
               "this run wrote the record in D's historical line format");
        expect(all.find(line, all.find(line) + 1) == std::string::npos,
               "the file holds exactly the line this run wrote (it was empty before)");
        expect(all.find("llm.agent.pool") == std::string::npos,
               "the archive line carries the module nowhere (D never printed it)");
    }

    // Promotion, end to end (task-31 review C1): the raw ring keeps every level, the
    // user-facing projection keeps only info and above, and the two cannot disagree.
    {
        const std::size_t ringBefore = state->inner->logRecords.size();
        const std::size_t activityBefore = state->inner->ui.activity.size();
        tuiAddLogMessageEx(state, TuiLogLevel_Trace, makeStr("trace line"), String{nullptr, 0});
        tuiAddLogMessageEx(state, TuiLogLevel_Debug, makeStr("debug line"), String{nullptr, 0});
        expect(state->inner->logRecords.size() == ringBefore + 2,
               "the raw ring keeps the trace and debug records");
        expect(state->inner->ui.activity.size() == activityBefore,
               "Activity does not: trace and debug are not user-facing");
        tuiAddLogMessageEx(state, TuiLogLevel_Info, makeStr("info line"), String{nullptr, 0});
        expect(state->inner->logRecords.size() == ringBefore + 3,
               "the raw ring keeps the info record too");
        expect(state->inner->ui.activity.size() == activityBefore + 1, "and Activity promotes it");
    }

    // A record with no clock is stamped with the arrival time (0 = "the sender had none").
    TuiLogRecord noClock{};
    noClock.level = TuiLogLevel_Info;
    noClock.message = makeStr("no clock");
    tuiAddLogRecord(state, &noClock);
    const llmfun::tui::LogRecord& stamped = state->inner->logRecords.back();
    expect(stamped.timestampMs > 1'700'000'000'000,
           "timestampMs == 0 is stamped with the arrival time (got " +
               std::to_string(stamped.timestampMs) + ")");
    // ... and the projection of that arrival carries the same stamp (the Activity tab's row).
    expect(!state->inner->ui.activity.empty() &&
               state->inner->ui.activity.back().timestampMs == stamped.timestampMs,
           "the Activity projection carries the stamped clock");

    // An out-of-range level/action off the wire is folded, never stored raw - and folded once,
    // before the Activity projection, so a record the ring stores as info is not invisible in
    // the tab the same arrival is projected into (task-32 review, Minor 5).
    TuiLogRecord odd{};
    odd.level = 99;
    odd.action = 77;
    odd.line = -7;
    const std::size_t activityBeforeOdd = state->inner->ui.activity.size();
    tuiAddLogRecord(state, &odd);
    expect(state->inner->logRecords.back().level == TuiLogLevel_Info &&
               state->inner->logRecords.back().action == TuiLogAction_None &&
               state->inner->logRecords.back().line == 0,
           "an unknown level is info, an unknown action is none, a negative line is unknown");
    expect(state->inner->ui.activity.size() == activityBeforeOdd + 1 &&
               state->inner->ui.activity.back().level == TuiLogLevel_Info,
           "the clamped info record is promoted to Activity, like any other info record");

    // The two-string compatibility wrapper goes through the same store, at level info.
    tuiAddLogMessageEx(state, TuiLogLevel_Warn, makeStr("wrapper"), makeStr("detail"));
    const llmfun::tui::LogRecord& wrapped = state->inner->logRecords.back();
    expect(wrapped.level == TuiLogLevel_Warn && wrapped.message == "wrapper" &&
               wrapped.detail == "detail" && wrapped.module.empty(),
           "tuiAddLogMessageEx maps summary -> message, text -> detail and leaves metadata empty");

    // Burst: 5000 records between two frames lose nothing; 200 more drop the oldest 200 and
    // say so (NFR-17: nothing is ever lost silently).
    tuiDestroyState(state);
    state = tuiCreateState();
    TuiLogRecord burst{};
    burst.timestampMs = 1; // a fixed stamp: 5000 arrivals must not read the wall clock 5000 times
    // One error at the front is evicted by the 200 records past the bound; the 5000 info
    // arrivals after it flush its own Activity projection out of the 500-row ring, so the
    // eviction notice is the only Activity error row left (review I-B).
    const auto countDroppedErrorNotices = [&]() {
        std::size_t notices = 0;
        for (const auto& row : state->inner->ui.activity)
            if (row.message.find("log ring dropped an error record") != std::string::npos)
                ++notices;
        return notices;
    };
    for (int i = 0; i < 5000; ++i) {
        burst.level = i == 0 ? TuiLogLevel_Error : TuiLogLevel_Info;
        const std::string text = "burst " + std::to_string(i);
        burst.message = String{text.data(), text.size()};
        tuiAddLogRecord(state, &burst);
    }
    expect(state->inner->logRecords.size() == llmfun::tui::TuiState::MaxLogRecords,
           "a 5000-record burst fills the ring without losing anything (got " +
               std::to_string(state->inner->logRecords.size()) + ")");
    expect(state->inner->droppedRecords == 0, "and reports no drops");
    expect(state->inner->logRecords.front().message == "burst 0",
           "the first record is the oldest one");
    expect(countDroppedErrorNotices() == 0, "a full ring has not raised a drop notice yet");
    for (int i = 5000; i < 5200; ++i) {
        burst.level = TuiLogLevel_Info;
        const std::string text = "burst " + std::to_string(i);
        burst.message = String{text.data(), text.size()};
        tuiAddLogRecord(state, &burst);
    }
    expect(state->inner->logRecords.size() == llmfun::tui::TuiState::MaxLogRecords,
           "the ring stays bounded at its capacity");
    expect(state->inner->droppedRecords == 200,
           "200 records past the bound are dropped and counted (got " +
               std::to_string(state->inner->droppedRecords) + ")");
    expect(state->inner->logRecords.back().message == "burst 5199", "the newest record is kept");
    expect(state->inner->logRecordSeq == 5200, "the arrival counter counted every record");
    expect(countDroppedErrorNotices() == 1,
           "the eviction of the error raises exactly one Activity notice");
    bool noticeIsError = false;
    for (const auto& row : state->inner->ui.activity)
        if (row.message.find("log ring dropped an error record") != std::string::npos)
            noticeIsError = row.level == TuiLogLevel_Error;
    expect(noticeIsError, "and the notice is an Activity error row");

    // Nothing but info is evicted now: the same loop must stay silent. Clearing Activity makes
    // the check independent of the notice above.
    state->inner->ui.activity.clear();
    for (int i = 5200; i < 5400; ++i) {
        burst.level = TuiLogLevel_Info;
        const std::string text = "burst " + std::to_string(i);
        burst.message = String{text.data(), text.size()};
        tuiAddLogRecord(state, &burst);
    }
    expect(countDroppedErrorNotices() == 0, "an all-info eviction raises no error notice");

    tuiDestroyState(state);
}

// The archive's failure latch belongs to one *request*, not to the session (task-32 review,
// Minor 7): `tuiSetLogging(false)` + `(true)` is the user asking again, so it must retry. A
// directory in the archive's way makes the `fopen` fail deterministically (running as root, a
// permission bit would not).
void caseLogArchiveRetry() {
    g_where = "case: the archive's failed open is retried on the next tuiSetLogging(true)";

    const char* kLog = "llmfun_ui_log.txt";
    std::remove(kLog);
    ::rmdir(kLog);

    TuiState* state = tuiCreateState();
    expect(state != nullptr, "tuiCreateState succeeds");

    // First request: the open fails, and the latch stops a per-record retry.
    expect(::mkdir(kLog, 0777) == 0, "a directory in the archive's way makes fopen fail");
    tuiSetLogging(state, 1);
    TuiLogRecord record{};
    record.level = TuiLogLevel_Info;
    record.message = makeStr("blocked");
    tuiAddLogRecord(state, &record);
    expect(state->inner->logArchive.file == nullptr && state->inner->logArchive.unavailable,
           "a failed open leaves no handle and latches the archive");
    tuiAddLogRecord(state, &record);
    expect(state->inner->logArchive.unavailable, "the latch survives the next record");

    // Second request: off (closes nothing), obstacle gone, on again - a new attempt.
    tuiSetLogging(state, 0);
    expect(::rmdir(kLog) == 0, "the obstacle directory is removable");
    tuiSetLogging(state, 1);
    tuiAddLogRecord(state, &record);
    expect(state->inner->logArchive.file != nullptr,
           "tuiSetLogging(true) clears the latch: the archive opens on the next record");
    FILE* file = std::fopen(kLog, "rb");
    expect(file != nullptr, "and the record created the archive file");
    if (file != nullptr)
        std::fclose(file);

    // The write half of the same promise: a failed write latches *and* drops the stream, so the
    // next request is a new `fopen` rather than another write to a handle whose error flag is
    // set. A link to `/dev/full` makes the write fail deterministically (ENOSPC), the same way
    // the directory made the open fail.
    tuiSetLogging(state, 0);
    std::remove(kLog);
    expect(::symlink("/dev/full", kLog) == 0, "the archive path is a link to /dev/full");
    tuiSetLogging(state, 1);
    tuiAddLogRecord(state, &record);
    expect(state->inner->logArchive.file == nullptr && state->inner->logArchive.unavailable,
           "a failed write latches the archive and drops the stream (ENOSPC)");
    std::remove(kLog);
    tuiSetLogging(state, 1); // a bare on->on: the next record must open a new file
    tuiAddLogRecord(state, &record);
    expect(state->inner->logArchive.file != nullptr,
           "the next request opens a real file, not the stream that just failed");

    tuiSetLogging(state, 0);
    tuiDestroyState(state);
    std::remove(kLog);
}

} // namespace

int main(int argc, char** argv) {
    // Direct child modes (the stdio arrangement is part of the mode), also
    // used for the xvfb Auto->GUI evidence run.
    if (argc == 3 && std::strcmp(argv[1], "--child") == 0)
        return runChildMode(argv[2]);

    // A case that fails midway exits without unwinding (`fail()` -> `std::exit(1)`), so its
    // obstacle would outlive it and break the next binary that reads the shared archive
    // (`test_keymap`): clean-up has to survive `std::exit`, which a destructor does not. The
    // obstacle is a directory or a link to /dev/full, never a regular file, so a regular log
    // another case left behind is not deleted.
    std::atexit([] {
        struct stat st{};
        if (::lstat("llmfun_ui_log.txt", &st) == 0 && !S_ISREG(st.st_mode))
            (void)std::remove("llmfun_ui_log.txt");
    });

    static_assert(TUI_API_VERSION == 6, "this contract test pins the current API generation (v6)");

    caseNoneStateInert();
    caseLogRecords();
    caseLogArchiveRetry();
    caseSessionRowFields();
    casePipelineLaneFields();
    caseQueryHistory();
    caseCancelFlag();
    caseRuntimeStatus();
    caseDestroyBasics();
    caseFailedGuiInit();
    caseAutoTtyMatrix();

    std::printf("OK: all v4/v5 API contract cases passed\n");
    return 0;
}
