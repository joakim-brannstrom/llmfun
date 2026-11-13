// test_thread_anchor.cpp
//
// The thread's scroll anchor and its `Load earlier` key (task 25), measured on real
// frames through the real GUI backend.
//
// Why a frame-level binary: anchoring is an ordering property. A group header is
// toggled inside the view (a click on the row), the follow-the-newest
// pin runs at the end of the same frame, and ImGui clamps the scroll at `EndChild`
// of the next one. None of that is visible to a model-level test, so the assertions
// below read the thread child's own `Scroll.y`/`ScrollMax.y` and the header row the
// view recorded for this frame - the numbers the user sees.
//
// What is pinned:
//   * a click on a group header toggles that group,
//   * the click stops the follow (it is the user taking the view over, like a wheel
//     notch) and the view is *not* yanked to the bottom,
//   * the header keeps its screen row while the content below it grows - the anchor,
//   * `PgUp` and `Ctrl+U` reveal one page of older groups (D-BOUNDS), and the window
//     the page is measured against is the layout's, not a constant,
//   * a batch folding in place - by hand or by the live batch settling - never
//     moves the group's header row: the fold changes how many rows
//     follow the header, never where the header starts.
//
// Needs a display; runs headless under xvfb:
//   xvfb-run -s "0 1280x820x24" ./test_thread_anchor [--out DIR]
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = setup error.

#include "tui.h"
#include "tui_backend.h"
#include "tui_settings.h"

#include "test/typed_seed.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "ui/layout.h"
#include "ui/metrics.h"
#include "views/shell.h"

#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace {

const char* g_phase = "startup";
int g_checks = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL [%s]: %s\n", g_phase, what.c_str());
        std::exit(1);
    }
}

void phase(const char* what) { g_phase = what; }

/// The thread's scrolling child, found by name: child window names are
/// "Parent/child_<ID>", so the substring is the stable part.
ImGuiWindow* findThreadWindow() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (int i = 0; i < g.Windows.Size; ++i) {
        if (std::strstr(g.Windows[i]->Name, "thread_groups") != nullptr)
            return g.Windows[i];
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    std::string outDir = ".";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) {
            outDir = argv[++i];
            continue;
        }
        std::fprintf(stderr, "usage: %s [--out DIR]\n", argv[0]);
        return 2;
    }

    phase("init");
    std::setlocale(LC_ALL, "");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;

    llmfun::tui::TuiState state;
    state.readyStatus = true;

    const std::string iniPath = outDir + "/thread-anchor.ini";
    // The profile the workbench loads in `tuiInit`; this harness stands in
    // for it: seed the merged `ui.conf` through the model's writer and read it into the
    // one preferences struct the backend and the frame both share.
    llmfun::tui::model::ShellPrefs view;
    view.fontScaleMain = 1.0f;
    view.windowWidth = 1280;
    view.windowHeight = 820;
    std::remove(llmfun::tui::model::uiConfPath(iniPath).c_str()); // a stale profile
    llmfun::tui::model::saveUiConf(view, iniPath);
    llmfun::tui::model::loadUiConf(state.ui.prefs, iniPath);

    std::string error;
    state.backend = llmfun::tui::makeGuiBackend(state.ui.prefs);
    if (!state.backend->init(error)) {
        std::fprintf(stderr, "GUI backend init failed (no display?): %s\n", error.c_str());
        ImGui::DestroyContext();
        return 2;
    }
    llmfun::tui::initMarkdownGuiMode(state);
    llmfun::tui::applyUiTheme();

    llmfun::tui::SessionEntry entry;
    entry.id = "s001-0";
    entry.title = "Alpha session";
    entry.isActive = true;
    state.sessionPanel.sessions = {entry};
    state.sessionPanel.activeId = entry.id;

    const auto frame = [&state]() {
        state.backend->newFrame();
        llmfun::tui::tuiRender(state);
        state.backend->renderFrame();
    };
    const auto frames = [&frame](int count) {
        for (int i = 0; i < count; ++i)
            frame();
    };

    /// One click at (x, y): the press and the release are separate frames, which is
    /// how ImGui's own `ButtonBehavior` activates an item.
    const auto clickAt = [&](float x, float y) {
        ImGui::GetIO().AddMousePosEvent(x, y);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(0, true);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        frame();
    };

    // The typed store's seeding: a `User` message
    // opens a turn with its query block, an assistant step hangs a content block
    // off the newest turn, and a transcript reset restarts the walk's ids at 1.
    typed_seed::Seeder seed{&state, 1};
    int stepSeq = 0; ///< the newest turn's next content-block index
    const auto addUser = [&](const std::string& text) {
        const std::int64_t id = ++seed.turnSeq;
        seed.upsertTurn(id, TuiTurnState_Done, 1);
        seed.upsertBlock(id, "query", std::to_string(id) + "#q", TuiBlockKind_Prose,
                         TuiBlockStatus_None, "user", text);
        stepSeq = 0;
    };
    const auto addStep = [&](const std::string& text) { // an assistant prose step
        if (seed.turnSeq == 0) {
            seed.turnSeq = 1;
            seed.upsertTurn(1, TuiTurnState_Done, 1);
        }
        seed.upsertBlock(seed.turnSeq, "content#" + std::to_string(stepSeq),
                         std::to_string(seed.turnSeq) + "#" + std::to_string(stepSeq),
                         TuiBlockKind_Prose, TuiBlockStatus_None, "assistant", text);
        ++stepSeq;
    };
    const auto addToolStep = [&](const std::string& name, const std::string& body) {
        if (seed.turnSeq == 0) {
            seed.turnSeq = 1;
            seed.upsertTurn(1, TuiTurnState_Done, 1);
        }
        seed.upsertBlock(seed.turnSeq, "tool#" + std::to_string(stepSeq),
                         std::to_string(seed.turnSeq) + "#" + std::to_string(stepSeq),
                         TuiBlockKind_Tool, TuiBlockStatus_Ok, nullptr, body, name.c_str());
        ++stepSeq;
    };
    const auto clearThread = [&]() {
        state.threadAdapter.reset();
        llmfun::tui::model::resetForSessionSwitch(state.ui);
        state.chat.typedTurns.threadClear();
        seed = typed_seed::Seeder(&state, 1); // the transcript starts over
        stepSeq = 0;
    };

    // Sixty overfull groups: the bounded window (30 groups in the pixel profile) hides
    // most of them, so `Load earlier` has work to do and the thread scrolls. Every
    // group carries a work step, so each one draws its header row - the row the
    // anchor's recording samples; a query-only turn draws no header at all.
    clearThread();
    for (size_t i = 0; i < 60; ++i) {
        const std::string text = "question " + std::to_string(i) + "\nsecond line\nthird line";
        addUser(text);
        addStep("step body " + std::to_string(i) + "\nsecond line");
    }

    phase("follow-on");
    frames(4);
    check(state.ui.view == llmfun::tui::model::ThreadView::Ready,
          "the workbench thread is the ordinary thread");
    ImGuiWindow* thread = findThreadWindow();
    check(thread != nullptr, "the thread child window exists");
    const float maxAtStart = thread->ScrollMax.y;
    check(maxAtStart > 0.0f,
          "the fixture overflows the viewport (ScrollMax " + std::to_string(maxAtStart) + ")");
    check(state.ui.follow, "a fresh thread follows the newest group");
    check(std::fabs(thread->Scroll.y - maxAtStart) <= 1.0f,
          "the first thread opens at its newest content");

    // The bounded window: 30 groups in the pixel profile, out of 60.
    check(state.ui.thread.pageGroups == llmfun::tui::model::kThreadWindowGroupsGui,
          "the window is the pixel profile's 30 groups (got " +
              std::to_string(state.ui.thread.pageGroups) + ")");
    check(state.ui.thread.drawnGroups == 30, "the thread draws the bounded window only (got " +
                                                 std::to_string(state.ui.thread.drawnGroups) +
                                                 " groups)");

    // --- A header click while following: the hand-over -------------------------
    // A header click is the user taking the view over, so it stops the follow exactly
    // like a wheel notch does. The property that matters is the *next* frame that
    // brings content: with the follow still on, the new message would pin the view to
    // the bottom and throw away the group the user just opened.
    phase("click-while-following");
    check(state.ui.follow, "premise: the thread is following before the click");
    thread = findThreadWindow();
    check(thread != nullptr, "the thread child window exists");
    check(std::fabs(thread->Scroll.y - thread->ScrollMax.y) <= 1.0f,
          "premise: the follower is at the bottom (scroll " + std::to_string(thread->Scroll.y) +
              "/" + std::to_string(thread->ScrollMax.y) + ")");
    const size_t followFirstId = state.ui.thread.firstGroupId;
    const float followHeaderY = state.ui.thread.firstGroupY;
    check(followFirstId != 0, "the thread recorded the first visible group");
    check(followHeaderY > thread->Pos.y && followHeaderY < thread->Pos.y + thread->Size.y,
          "the recorded header row is inside the thread (" + std::to_string(followHeaderY) + ")");
    const bool followWasExpanded = state.threadAdapter.expanded(followFirstId);

    ImGui::GetIO().AddMousePosEvent(thread->Pos.x + thread->Size.x * 0.5f, followHeaderY + 2.0f);
    frame();
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    frame();
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    frame();
    check(state.threadAdapter.expanded(followFirstId) != followWasExpanded,
          "a click on the header row toggles that group");
    check(!state.ui.follow, "a header click hands the view to the user (the follow stops)");

    // The frames after the click settle the scroll (a collapse that shrank the content
    // clamps it - the anchor cannot keep a row that no longer has content under it), and
    // then new content arrives. A view the user took over must not follow that content.
    frames(3);
    thread = findThreadWindow();
    check(thread != nullptr, "the thread child window exists after the settle");
    const float settledY = thread->Scroll.y;
    frames(3);
    thread = findThreadWindow();
    for (int i = 0; i < 4; ++i) {
        // Taller than the groups the window slides out, so the growth is visible in
        // ScrollMax and not a wash of identical heights.
        std::string text = "late " + std::to_string(i);
        for (int l = 0; l < 12; ++l)
            text += "\nlate body line " + std::to_string(l);
        addUser(text);
        frame();
    }
    frames(2);
    thread = findThreadWindow();
    check(thread != nullptr, "the thread child window exists after the new content");
    check(!state.ui.follow, "new content does not re-arm the follow");
    check(std::fabs(thread->Scroll.y - settledY) <= 1.0f,
          "new content does not yank the view the user took over (scroll " +
              std::to_string(thread->Scroll.y) + " vs " + std::to_string(settledY) + ")");
    check(thread->ScrollMax.y > settledY + 1.0f, "the new content is below the fold (ScrollMax " +
                                                     std::to_string(thread->ScrollMax.y) + ")");

    // --- The same click with the view parked by the wheel ----------------------
    phase("park");
    ImGui::GetIO().AddMousePosEvent(thread->Pos.x + thread->Size.x * 0.5f,
                                    thread->Pos.y + thread->Size.y * 0.5f);
    frames(2);
    ImGui::GetIO().AddMouseWheelEvent(0.0f, 6.0f);
    frames(2);
    check(!state.ui.follow, "a wheel notch stops the follow");
    thread = findThreadWindow();
    check(thread != nullptr, "the thread child window still exists");
    const float parkedY = thread->Scroll.y;
    const size_t parkedFirstId = state.ui.thread.firstGroupId;
    const float parkedHeaderY = state.ui.thread.firstGroupY;
    check(parkedFirstId != 0, "the thread recorded the first visible group of the parked view");
    const bool parkedWasExpanded = state.threadAdapter.expanded(parkedFirstId);

    phase("click-parked-header");
    clickAt(thread->Pos.x + thread->Size.x * 0.5f, parkedHeaderY + 2.0f);
    check(state.threadAdapter.expanded(parkedFirstId) != parkedWasExpanded,
          "a click on the parked header row toggles that group too");
    frames(2);
    thread = findThreadWindow();
    check(thread != nullptr, "the thread child window exists after the parked click");
    check(std::fabs(thread->Scroll.y - parkedY) <= 1.0f,
          "a parked view does not move either (scroll " + std::to_string(thread->Scroll.y) +
              " vs " + std::to_string(parkedY) + ")");
    // The same group must still be the first visible one - if the content above it had
    // moved, a different group would be at the top - and it must still be on its row.
    // The two halves are one assertion, not alternatives: a row is only meaningful for
    // the group it was sampled from.
    check(state.ui.thread.firstGroupId == parkedFirstId,
          "the clicked group is still the first visible one (" + std::to_string(parkedFirstId) +
              " -> " + std::to_string(state.ui.thread.firstGroupId) + ")");
    check(std::fabs(state.ui.thread.firstGroupY - parkedHeaderY) <= 1.0f,
          "the parked header keeps its row (" + std::to_string(parkedHeaderY) + " -> " +
              std::to_string(state.ui.thread.firstGroupY) + ")");

    phase("load-earlier-key");
    // PgUp is the thread's own scroll-away key (the ncurses backend forwards no wheel
    // events, plan/key_gate.md §4e): one press reveals one page of older groups.
    const int page = state.ui.thread.pageGroups;
    const size_t drawnBefore = state.ui.thread.drawnGroups;
    ImGui::GetIO().AddKeyEvent(ImGuiKey_PageUp, true);
    frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_PageUp, false);
    frame();
    check(state.ui.earlierGroups == page,
          "PgUp reveals one page (" + std::to_string(state.ui.earlierGroups) + " groups)");
    check(state.ui.thread.drawnGroups > drawnBefore,
          "the revealed page is drawn (" + std::to_string(drawnBefore) + " -> " +
              std::to_string(state.ui.thread.drawnGroups) + " groups)");
    check(state.ui.thread.drawnGroups <= 60, "the thread never draws more groups than it has");

    // Ctrl+U is the second spelling of the same action (D-KEYMAP: one action, two
    // chords), and it must not double-fire from one press.
    const int beforeCtrlU = state.ui.earlierGroups;
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_U, true);
    frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_U, false);
    frame();
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    frame();
    check(state.ui.earlierGroups == beforeCtrlU + page,
          "Ctrl+U reveals one page as well (" + std::to_string(state.ui.earlierGroups) + ")");
    // Two pages of the pixel profile's 30-group window cover every group of the
    // fixture, so the paging row is not offered any more.
    const std::size_t groups = state.threadAdapter.groupCount();
    check(state.ui.thread.drawnGroups == groups, "two pages show every group (" +
                                                     std::to_string(state.ui.thread.drawnGroups) +
                                                     " of " + std::to_string(groups) + ")");

    // --- Dragging the scrollbar is pointer ownership too --------------------------
    // The scrollbar owns ImGui's ActiveId while it is held; IsWindowHovered() must
    // allow that, or the follower re-pins the view to the bottom after every drag
    // frame (the TUI symptom: the only way out was to click a row, which stops the
    // follow through the toggle path).
    phase("scrollbar-drag");
    state.ui.follow = true;
    frames(4);
    thread = findThreadWindow();
    check(thread != nullptr && thread->ScrollMax.y > 1.0f,
          "the thread has a scrollbar to drag (scroll " + std::to_string(thread->ScrollMax.y) +
              ")");
    check(std::fabs(thread->Scroll.y - thread->ScrollMax.y) <= 1.0f,
          "the fixture starts at the bottom before the drag");
    const ImRect bar = ImGui::GetWindowScrollbarRect(thread, ImGuiAxis_Y);
    check(bar.GetWidth() > 0.0f && bar.GetHeight() > 1.0f, "the thread scrollbar has a hit rect");
    ImGuiIO& io = ImGui::GetIO();
    const float barX = (bar.Min.x + bar.Max.x) * 0.5f;
    const float barY = bar.Min.y + bar.GetHeight() * 0.5f;
    io.AddMousePosEvent(barX, barY);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    for (int i = 1; i <= 4; ++i) {
        io.AddMousePosEvent(barX, barY - bar.GetHeight() * 0.1f * static_cast<float>(i));
        frame();
    }
    check(!state.ui.follow, "dragging the scrollbar stops the follow");
    check(thread->Scroll.y < thread->ScrollMax.y - 1.0f,
          "the scrollbar drag left the bottom (scroll " + std::to_string(thread->Scroll.y) + "/" +
              std::to_string(thread->ScrollMax.y) + ")");
    io.AddMouseButtonEvent(0, false);
    frames(4);
    check(!state.ui.follow, "the follow stays off after the scrollbar is released");
    check(thread->Scroll.y < thread->ScrollMax.y - 1.0f,
          "the view does not snap back after the scrollbar drag (scroll " +
              std::to_string(thread->Scroll.y) + "/" + std::to_string(thread->ScrollMax.y) + ")");

    // --- A click on a row *inside* an agent group (review round 1) --------------
    // The rows inside an `AssistantWork` group toggle the group's own fold (the
    // typed store's `groupPins` bit keyed by the turn id), which a fresh
    // sync reports as a toggle. The adapter has to see that too: a collapsed
    // agent step is the same kind of user action as a collapsed group - it
    // must hand the view over, or the next content pins the view back to the bottom.
    phase("nested-toggle");
    clearThread();
    frames(2);
    // A fixture whose *first* drawn group is the work group: the window starts at it,
    // so its header row is `firstGroupY` and the first item's header is one row below.
    addStep("step body 1\nsecond line");
    addToolStep("step 2", "step body 2\nsecond line");
    addStep("step body 3\nsecond line");
    addToolStep("step 4", "step body 4\nsecond line");
    frames(4);
    check(state.ui.follow, "a cleared thread follows again (a catch-up is owed)");
    check(state.ui.thread.drawnGroups == 1,
          "the fixture is one agent group (" + std::to_string(state.ui.thread.drawnGroups) + ")");
    const size_t workGroupId = state.ui.thread.firstGroupId;
    // The typed store seeds every work group folded; the run's own group is the
    // one the D default unfolds (only the newest work group draws open).
    state.threadAdapter.setExpanded(workGroupId, true);
    check(workGroupId != 0 && state.threadAdapter.expanded(workGroupId),
          "the newest work group is open");
    const float groupHeaderY = state.ui.thread.firstGroupY;
    // The group's header is one row; the first item's header sits directly below it.
    const float itemHeaderY = groupHeaderY + llmfun::tui::ui::metricsFor().row;
    thread = findThreadWindow();
    check(thread != nullptr, "the thread child window exists for the nested click");
    clickAt(thread->Pos.x + thread->Size.x * 0.5f, itemHeaderY + 2.0f);
    check(!state.ui.follow,
          "a click on a row inside an agent group hands the view over (the follow stops)");
    check(state.ui.thread.anchorGroupId == workGroupId && state.ui.thread.anchorPending,
          "the nested toggle anchors the group's header row (id " +
              std::to_string(state.ui.thread.anchorGroupId) + ")");

    phase("end-re-arms");
    // `End` still returns to the newest content, and the follow keeps it there: the
    // anchor is not a mode.
    ImGui::GetIO().AddKeyEvent(ImGuiKey_End, true);
    frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_End, false);
    frames(2);
    thread = findThreadWindow();
    check(state.ui.follow, "End re-arms the follow");
    check(thread != nullptr && std::fabs(thread->Scroll.y - thread->ScrollMax.y) <= 1.0f,
          "End returns to the bottom");

    // --- A step arriving while following: the default is not a toggle ----------
    // An `AssistantWork` group gains steps after it is born. Its items get their
    // one-shot default at build time (`sync()`), so an arriving step changes no
    // expansion set inside the draw. If it did, the adapter's membership compare
    // would read it as a user toggle: the follow would stop and an anchor would be
    // armed on every step of a run, and the thread would stop following the run.
    phase("arriving-step");
    clearThread();
    frames(2);
    check(state.ui.follow, "a cleared thread follows again (the premise)");

    for (size_t i = 1; i <= 2; ++i) {
        const std::string text = "step " + std::to_string(i) + "\nbody line two";
        addStep(text);
        frame();
    }
    frames(2);
    thread = findThreadWindow();
    const size_t workId = state.ui.thread.firstGroupId;
    // The typed store seeds work groups folded (only the newest unfolds); the
    // fixture expands this one, as D's replay does.
    state.threadAdapter.setExpanded(workId, true);
    check(workId != 0 && state.threadAdapter.expanded(workId),
          "the fixture is one open work group");
    check(state.ui.follow, "the thread follows the work group it just built");

    for (size_t i = 3; i <= 6; ++i) {
        const std::string text = "step " + std::to_string(i) + "\nbody line two";
        addStep(text);
        frame();
        check(state.ui.follow,
              "an arriving step keeps the follow (step " + std::to_string(i) + ")");
        check(state.ui.thread.anchorGroupId == 0 && !state.ui.thread.anchorPending,
              "an arriving step arms no anchor (step " + std::to_string(i) + ")");
    }
    thread = findThreadWindow();
    check(thread != nullptr && thread->Scroll.y >= thread->ScrollMax.y - 1.0f,
          "the follower is still at the bottom after the steps");

    // The same arrival into a collapsed group: its items are not drawn, so this
    // pins that the default is applied at build time rather than by `renderMessage`.
    state.threadAdapter.setExpanded(workId, false);
    frame();
    check(!state.threadAdapter.expanded(workId), "the group is collapsed for the second half");
    addStep("step 7\nbody line two");
    frame();
    check(state.ui.follow, "an arriving step in a collapsed group keeps the follow");
    check(state.ui.thread.anchorGroupId == 0 && !state.ui.thread.anchorPending,
          "an arriving step in a collapsed group arms no anchor");
    state.threadAdapter.setExpanded(workId, true);
    frame();
    check(state.threadAdapter.expanded(workId), "the group opens again (and with it the step)");

    // --- A session switch opens the new thread at its newest content ---------
    // The `##thread_groups` child outlives the session, so its scroll offset is
    // whatever the session we left was parked at. The switch asks for a bottom-open
    // in `resetForSessionSwitch()`; two frames later the long thread must be at its
    // own max, not at the short thread's offset (the old child follows only on the
    // frame after it has measured the new content, which is too late for the first
    // frame the user sees).
    phase("session-switch-scroll");
    clearThread();
    for (size_t i = 1; i <= 60; ++i) {
        const std::string text = "question " + std::to_string(i) + "\nsecond line\nthird line";
        addUser(text);
    }
    frames(4);
    thread = findThreadWindow();
    check(thread != nullptr && thread->Scroll.y >= thread->ScrollMax.y - 1.0f,
          "premise: the long thread is at the bottom");

    clearThread();
    addUser("short\nsecond line");
    frames(4);
    thread = findThreadWindow();
    check(thread != nullptr && thread->ScrollMax.y <= 1.0f, "the short thread does not scroll");

    clearThread();
    for (size_t i = 1; i <= 60; ++i) {
        const std::string text = "question " + std::to_string(i) + "\nsecond line\nthird line";
        addUser(text);
    }
    frame();
    frame();
    thread = findThreadWindow();
    check(thread != nullptr && thread->Scroll.y >= thread->ScrollMax.y - 1.0f,
          "a long session opens at the bottom after a short one (scroll " +
              std::to_string(thread ? thread->Scroll.y : -1.0f) + "/" +
              std::to_string(thread ? thread->ScrollMax.y : -1.0f) + ")");
    check(state.ui.thread.scrollToBottom == 0, "the switch's bottom-open request is one-shot");

    // --- A live answer: content too, and still not the view's owner ------------
    // Streaming v2 (task 38): the stream is a group like any other while the user is reading, so
    // a scrollbar drag has to hand the view over exactly as it does without one - and the text
    // that keeps arriving afterwards must not yank it back (D-FOLLOW's rule, applied to the one
    // content that arrives by itself).
    phase("stream-follow");
    clearThread();
    for (size_t i = 1; i <= 40; ++i) {
        const std::string text = "question " + std::to_string(i) + "\nsecond line\nthird line";
        addUser(text);
    }
    frames(4);
    std::string streamed = "The answer begins";
    for (int i = 0; i < 8; ++i) {
        streamed += "\nanother streamed line";
        if (seed.liveTurnId == 0)
            seed.runStart("stream this"); // the run's start: a Running turn, a query block
        seed.updateStream(streamed);      // the whole accumulated answer, every tick
        frame();
    }
    frames(2);
    thread = findThreadWindow();
    frame();
    frame();
    check(thread != nullptr && thread->ScrollMax.y > 1.0f,
          "the live answer gives the thread a scrollbar (ScrollMax " +
              std::to_string(thread ? thread->ScrollMax.y : -1.0f) + ")");
    check(state.ui.follow, "the thread follows the live answer");
    // The pinned reference is the last drawn item - the live turn's folded
    // content row - and the row's trailing spacing leaves a 3 px tail below it,
    // so "at the bottom" is measured against that tail, not to the pixel.
    check(std::fabs(thread->Scroll.y - thread->ScrollMax.y) <= 8.0f,
          "and sits at its bottom while it streams (scroll " + std::to_string(thread->Scroll.y) +
              "/" + std::to_string(thread->ScrollMax.y) + ")");

    phase("stream-scrollbar-drag");
    const ImRect streamBar = ImGui::GetWindowScrollbarRect(thread, ImGuiAxis_Y);
    check(streamBar.GetWidth() > 0.0f && streamBar.GetHeight() > 1.0f,
          "the streaming thread's scrollbar has a hit rect");
    const float streamBarX = (streamBar.Min.x + streamBar.Max.x) * 0.5f;
    const float streamBarY = streamBar.Min.y + streamBar.GetHeight() * 0.5f;
    io.AddMousePosEvent(streamBarX, streamBarY);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    for (int i = 1; i <= 4; ++i) {
        io.AddMousePosEvent(streamBarX,
                            streamBarY - streamBar.GetHeight() * 0.1f * static_cast<float>(i));
        frame();
    }
    check(!state.ui.follow, "dragging the scrollbar stops the follow while an answer streams");
    io.AddMouseButtonEvent(0, false);
    frames(2);
    thread = findThreadWindow();
    const float parkedStreamY = thread != nullptr ? thread->Scroll.y : -1.0f;
    check(thread != nullptr && parkedStreamY < thread->ScrollMax.y - 1.0f,
          "the drag left the bottom (scroll " + std::to_string(parkedStreamY) + "/" +
              std::to_string(thread != nullptr ? thread->ScrollMax.y : -1.0f) + ")");

    // More text arrives while the view is parked. It is below the fold, so the read does not move.
    for (int i = 0; i < 6; ++i) {
        streamed += "\nanother streamed line";
        seed.updateStream(streamed);
        frame();
    }
    frames(2);
    thread = findThreadWindow();
    check(!state.ui.follow, "new streamed text does not re-arm the follow");
    check(thread != nullptr && std::fabs(thread->Scroll.y - parkedStreamY) <= 1.0f,
          "and it does not yank the parked view (scroll " +
              std::to_string(thread != nullptr ? thread->Scroll.y : -1.0f) + " vs " +
              std::to_string(parkedStreamY) + ")");

    // --- A batch folding in place never moves the group's header row -----------
    // The group's header row is the anchor, and a fold - by hand or by
    // the live batch settling - changes how many rows follow the header, never
    // where the header starts. Both cases run in the maximal mode (`threadMinimal`
    // off), where the fold is visible: the owner row draws open with its children
    // under it and folding hides them. (The minimal shape - the chips line dying
    // under the folded header while the segment stays - is the goldens' in
    // test_blocks; this file owns the anchor row's numbers.) The fixture stays
    // short so the child does not scroll: a parked view that clamps would move
    // the header for reasons the anchor does not govern.
    phase("batch-fold-by-hand");
    clearThread();
    frames(2);
    state.ui.prefs.threadMinimal =
        false; // maximal: the fold's effect is visible
               // (the shell syncs the chat flag from the prefs every frame)
    seed.runStart("fold the batch by hand");
    seed.content("prose while the call runs");
    seed.tool("bash", "ls -la", TuiBlockStatus_Running);
    frames(3);
    const std::int64_t foldLiveId = seed.liveTurnId;
    check(foldLiveId != 0, "the live run is seeded");
    std::size_t foldK = 0;
    for (std::size_t k = 0; k < state.threadAdapter.groupCount(); ++k)
        if (state.threadAdapter.groupAt(k).id == static_cast<std::size_t>(foldLiveId))
            foldK = k;
    check(state.threadAdapter.groupCount() == 1 && foldK == 0, "the fixture is the one live group");
    check(state.ui.thread.firstGroupId == static_cast<std::size_t>(foldLiveId),
          "the live group's header is the latched anchor row");
    check(state.ui.thread.anchorGroupId == 0 && !state.ui.thread.anchorPending,
          "no anchor is pending before the fold");
    std::size_t foldOwnerB = 0;
    std::size_t foldCardB = 0;
    bool foldSawOwner = false;
    bool foldSawCard = false;
    for (std::size_t b = 0; b < state.threadAdapter.blockCount(foldK); ++b) {
        const llmfun::tui::model::BlockDesc foldDesc = state.threadAdapter.blockAt(foldK, b);
        if (foldDesc.ownsBatch) {
            foldOwnerB = b;
            foldSawOwner = true;
        }
        if (foldDesc.kind == llmfun::tui::model::BlockKind::ToolStep) {
            foldCardB = b;
            foldSawCard = true;
        }
    }
    check(foldSawOwner && foldSawCard, "the live batch carries an owner row and a card");
    check(state.threadAdapter.blockAt(foldK, foldOwnerB).expanded,
          "the running batch's owner draws open (the live default)");
    check(state.threadAdapter.blockAt(foldK, foldCardB).visible,
          "the card under the open owner is visible");
    thread = findThreadWindow();
    check(thread != nullptr, "the thread child window exists for the fold");
    const float preFoldY = state.ui.thread.firstGroupY;
    const float preFoldScroll = thread->Scroll.y;

    // The fold, by hand, mid-stream: the call is still Running. The adapter
    // latches the toggle (`setBlockExpanded` returns `open` - false here, the
    // fold's own answer) and the next frame arms the anchor on the header row.
    (void)state.threadAdapter.setBlockExpanded(foldK, foldOwnerB, false);
    frame();
    check(!state.threadAdapter.blockAt(foldK, foldOwnerB).expanded,
          "the fold pinned the owner row closed");
    check(state.ui.thread.anchorGroupId == static_cast<std::size_t>(foldLiveId) &&
              state.ui.thread.anchorPending,
          "the fold anchors the group's header row");
    frames(2);
    thread = findThreadWindow();
    check(thread != nullptr, "the thread child window survives the fold");
    check(state.ui.thread.firstGroupId == static_cast<std::size_t>(foldLiveId),
          "the folded group is still the latched one");
    check(state.threadAdapter.blockAt(foldK, foldCardB).kind ==
                  llmfun::tui::model::BlockKind::ToolStep &&
              !state.threadAdapter.blockAt(foldK, foldCardB).visible,
          "the fold hid the card below the header");
    check(std::fabs(state.ui.thread.firstGroupY - preFoldY) <= 1.0f,
          "the fold did not move the header row (" + std::to_string(preFoldY) + " -> " +
              std::to_string(state.ui.thread.firstGroupY) + ")");
    check(std::fabs(thread->Scroll.y - preFoldScroll) <= 1.0f,
          "the fold did not move the scroll (scroll " + std::to_string(thread->Scroll.y) + ")");
    check(state.ui.thread.anchorGroupId == 0 && !state.ui.thread.anchorPending,
          "the anchor applied its correction and cleared");

    // The settled batch: a fresh fixture - the fold above pinned its owner
    // closed, and a pin outranks the default it must not mask. The batch
    // settles the way a response does: the call answers, the content gains its
    // duration, and the owner's live default folds it - a store change the view
    // did not perform, so no anchor may arm, and the rows that hide sit below
    // the header, which stays where the user read it.

    phase("batch-fold-settles");
    clearThread();
    frames(2);
    seed.runStart("settle the batch");
    seed.content("prose while the call runs");
    seed.tool("grep", "needle", TuiBlockStatus_Running);
    frames(3);
    const std::int64_t settleLiveId = seed.liveTurnId;
    const std::string settleBatch = seed.batchId();
    const std::string settleContentId = "content#" + std::to_string(seed.liveBatch);
    const std::string settleCallId = "tool#" + std::to_string(seed.callSeq);
    std::size_t settleK = 0;
    for (std::size_t k = 0; k < state.threadAdapter.groupCount(); ++k)
        if (state.threadAdapter.groupAt(k).id == static_cast<std::size_t>(settleLiveId))
            settleK = k;
    check(settleLiveId != 0 && settleK == 0 &&
              state.ui.thread.firstGroupId == static_cast<std::size_t>(settleLiveId),
          "the settled case's live group is the latched one");
    check(state.ui.thread.anchorGroupId == 0 && !state.ui.thread.anchorPending,
          "no anchor is pending before the settle");
    std::size_t settleOwnerB = 0;
    std::size_t settleCardB = 0;
    for (std::size_t b = 0; b < state.threadAdapter.blockCount(settleK); ++b) {
        const llmfun::tui::model::BlockDesc settleDesc = state.threadAdapter.blockAt(settleK, b);
        if (settleDesc.ownsBatch)
            settleOwnerB = b;
        if (settleDesc.kind == llmfun::tui::model::BlockKind::ToolStep)
            settleCardB = b;
    }
    check(state.threadAdapter.blockAt(settleK, settleOwnerB).expanded,
          "the running batch's owner draws open (the live default)");
    check(state.threadAdapter.blockAt(settleK, settleCardB).visible,
          "the card is visible before the settle");
    thread = findThreadWindow();
    check(thread != nullptr, "the thread child window exists before the settle");
    const float preSettleY = state.ui.thread.firstGroupY;
    const float preSettleScroll = thread->Scroll.y;

    // The settle itself: the same block ids, the call done, the content timed.
    seed.upsertBlock(seed.liveTurnId, settleContentId, settleBatch, TuiBlockKind_Prose,
                     TuiBlockStatus_None, "assistant", "prose while the call runs", nullptr, 2500);
    seed.upsertBlock(seed.liveTurnId, settleCallId, settleBatch, TuiBlockKind_Tool,
                     TuiBlockStatus_Ok, nullptr, "needle", "grep", 1500);
    frame();
    check(state.ui.thread.anchorGroupId == 0 && !state.ui.thread.anchorPending,
          "a settling batch arms no anchor (a default is not a toggle)");
    check(!state.threadAdapter.blockAt(settleK, settleOwnerB).expanded,
          "the settled batch's owner folded (the live default)");
    check(state.threadAdapter.blockAt(settleK, settleCardB).kind ==
                  llmfun::tui::model::BlockKind::ToolStep &&
              !state.threadAdapter.blockAt(settleK, settleCardB).visible,
          "the settle hid the card below the header");
    frames(2);
    thread = findThreadWindow();
    check(thread != nullptr, "the thread child window survives the settle");
    check(state.ui.thread.firstGroupId == static_cast<std::size_t>(settleLiveId),
          "the settled group is still the latched one");
    check(std::fabs(state.ui.thread.firstGroupY - preSettleY) <= 1.0f,
          "the settle did not move the header row (" + std::to_string(preSettleY) + " -> " +
              std::to_string(state.ui.thread.firstGroupY) + ")");
    check(std::fabs(thread->Scroll.y - preSettleScroll) <= 1.0f,
          "the settle did not move the scroll (scroll " + std::to_string(thread->Scroll.y) + ")");
    check(state.ui.thread.anchorGroupId == 0 && !state.ui.thread.anchorPending,
          "no anchor lingers after the settle");
    state.ui.prefs.threadMinimal = true; // the factory mode again

    state.backend->shutdown();
    state.backend.reset();
    ImGui::DestroyContext();
    std::printf("OK: %d checks (anchor, follow hand-over, arriving steps, Load earlier keys, "
                "fold-stable headers)\n",
                g_checks);
    return 0;
}
