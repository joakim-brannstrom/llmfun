// test_keymap.cpp
//
// Task 11's contract for quit and key hygiene (D-KEYMAP / D-ESC):
//
//   * one binding table, focus-aware: no two rows claim the same (chord, scope)
//     pair, every `Action` of the registry is reachable through the table or
//     through the `Esc` chain, a plain key never fires while a text field has
//     the keyboard, and an overlay is a trap except for the chords it lists
//     itself;
//   * `Ctrl+Q` quits, one press per quit, exactly one hint line in the UI log and
//     nothing written into the thread; modifier matching is exact, so
//     `Ctrl+Shift+Q` is not a quit;
//   * while a run is active the same chord asks first: the first press arms the
//     confirmation (and the composer's status row shows the prompt, so the press
//     visibly does something even with the UI log off), the second press quits,
//     and the arming dies with the run;
//   * `Ctrl+K` still toggles the palette, now through the registry, and quitting
//     works while the palette owns the keyboard;
//   * `End` dispatches `ScrollEnd` - and does not while a text field has the
//     keyboard (the intentional behaviour change of the plain-key rule);
//   * `Esc` walks the documented chain - overlay, cancel run, clear input, blur -
//     one step per press, with the run's partial output kept. The cancel step
//     works whatever has the keyboard: blurred composer, Log tab. A view-local
//     consumer (rail rename/filter, popup/menu) owns its press: the frame-level
//     cancel stands down, and the next press still cancels (per-frame latch).
//
// The registry checks drive `model::resolve` with a test-local table, so the
// scoping rules are pinned independently of what the shipped table binds; the
// interaction checks drive the real frame pipeline (the headless TScreen harness
// of test_session_filter_smoke / test_palette) at 80x24.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = setup error.
//
//   ./test_keymap

#include "imtui/imtui.h"

#include "imgui/imgui_internal.h"

#include "model/keymap.h"
#include "tui.h"
#include "views/palette.h"
#include "views/rawlog.h"
#include "views/shell.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

using namespace llmfun::tui;

int g_failures = 0;
int g_frames = 0;
const char* g_phase = "startup";

/// The UI log the frame loop appends to (TuiState::isLogActive). The harness runs
/// from the build directory and measures only what its own gesture appended, so
/// the file it shares with test_session_filter_smoke is never truncated.
constexpr const char* kLogName = "llmfun_ui_log.txt";

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

[[noreturn]] void setupError(const std::string& what) {
    std::fprintf(stderr, "SETUP [%s]: %s\n", g_phase, what.c_str());
    std::exit(2);
}

std::string actionLabel(model::Action action) { return model::actionName(action); }

std::string focusName(model::Focus focus) {
    switch (focus) {
    case model::Focus::None:
        return "None";
    case model::Focus::Composer:
        return "Composer";
    case model::Focus::Rail:
        return "Rail";
    case model::Focus::Thread:
        return "Thread";
    case model::Focus::Overlay:
        return "Overlay";
    case model::Focus::RawLog:
        return "RawLog";
    case model::Focus::Settings:
        return "Settings";
    case model::Focus::Help:
        return "Help";
    case model::Focus::Count:
        break;
    }
    return "Count";
}

std::string scopeName(const model::Scope& scope) {
    return scope.isAnywhere() ? "Anywhere" : focusName(scope.region());
}

// ---------------------------------------------------------------------------
// Registry checks: the table and its resolution rules (no renderer needed)
// ---------------------------------------------------------------------------

constexpr model::Focus kAllFocuses[] = {
    model::Focus::None,    model::Focus::Composer, model::Focus::Rail,     model::Focus::Thread,
    model::Focus::Overlay, model::Focus::RawLog,   model::Focus::Settings, model::Focus::Help};

/// The one collision rule: a chord may be bound once per scope, never twice.
/// An anywhere row and a region row overlap by design (the region row wins
/// there); `Scope(Focus::None)` shadows an anywhere row only while nothing is
/// focused, so that pair is an intentional override, not a duplicate
/// (keymap.h explains the shadowing rule next to `Scope`).
bool findDuplicatePair(const model::Keymap& keymap, std::string& what) {
    for (std::size_t i = 0; i < keymap.count; ++i) {
        for (std::size_t j = i + 1; j < keymap.count; ++j) {
            if (keymap.rows[i].chord == keymap.rows[j].chord &&
                keymap.rows[i].when == keymap.rows[j].when) {
                what = "rows " + std::to_string(i) + " and " + std::to_string(j) + " both bind " +
                       actionLabel(keymap.rows[i].action) + "/" +
                       actionLabel(keymap.rows[j].action) + " in " + scopeName(keymap.rows[i].when);
                return true;
            }
        }
    }
    return false;
}

void checkRegistry() {
    g_phase = "R1 collisions";
    std::string what;
    check(!findDuplicatePair(model::defaultKeymap(), what),
          "R1: no (chord, scope) pair is bound twice" + (what.empty() ? "" : " - " + what));

    // The detector itself must not be vacuous: a table that repeats a pair is
    // reported. Without this the check above could pass for any table.
    const model::Binding twins[] = {
        {ImGuiMod_Ctrl | ImGuiKey_Q, model::Action::Quit, model::Scope::anywhere()},
        {ImGuiMod_Ctrl | ImGuiKey_Q, model::Action::Palette, model::Scope::anywhere()},
    };
    const model::Keymap twinMap{twins, 2};
    std::string twinWhat;
    check(findDuplicatePair(twinMap, twinWhat), "R1: a duplicated pair is detected");

    g_phase = "R2 reachability";
    bool reachable[static_cast<int>(model::Action::Count)] = {};
    for (const model::Binding& row : model::defaultKeymap()) {
        for (const model::Focus focus : kAllFocuses) {
            const model::Action action = model::resolve(model::defaultKeymap(), row.chord, focus);
            if (action != model::Action::None)
                reachable[static_cast<int>(action)] = true;
        }
    }
    const model::EscapeContext escapeContexts[] = {{true, false, false},
                                                   {false, true, false},
                                                   {false, false, true},
                                                   {false, false, false},
                                                   {false, false, false, true}};
    for (const model::EscapeContext& context : escapeContexts)
        reachable[static_cast<int>(model::resolveEscape(context))] = true;
    for (int i = 1; i < static_cast<int>(model::Action::Count); ++i) {
        const std::string name = actionLabel(static_cast<model::Action>(i));
        check(reachable[i], "R2: every Action is reachable (" + name + ")");
    }

    g_phase = "R3 scoping";
    // A plain key bound to the rail must stay inert in a text field, must not
    // leak into another region, and must fire in its own.
    const model::Binding localRows[] = {
        {ImGuiKey_M, model::Action::Palette, model::Focus::Rail},
        {ImGuiMod_Ctrl | ImGuiKey_Q, model::Action::Quit, model::Scope::anywhere()},
        {ImGuiKey_End, model::Action::ScrollEnd, model::Focus::Thread},
        {ImGuiMod_Ctrl | ImGuiKey_Q, model::Action::CloseOverlay, model::Focus::Overlay},
    };
    const model::Keymap local{localRows, sizeof(localRows) / sizeof(localRows[0])};
    check(model::resolve(local, ImGuiKey_M, model::Focus::Rail) == model::Action::Palette,
          "R3: a plain key fires in its own region");
    check(model::resolve(local, ImGuiKey_M, model::Focus::Composer) == model::Action::None,
          "R3: a plain key is inert while a text field has the keyboard");
    check(model::resolve(local, ImGuiKey_M, model::Focus::Thread) == model::Action::None,
          "R3: a rail-scoped plain key does not fire in the thread");
    check(model::resolve(local, ImGuiMod_Ctrl | ImGuiKey_Q, model::Focus::Rail) ==
              model::Action::Quit,
          "R3: an anywhere row fires in every region");
    check(model::resolve(local, ImGuiMod_Ctrl | ImGuiKey_Q, model::Focus::Overlay) ==
              model::Action::CloseOverlay,
          "R3: an exact-region row beats an anywhere row for the same chord");
    check(model::resolve(local, ImGuiKey_End, model::Focus::Thread) == model::Action::ScrollEnd &&
              model::resolve(local, ImGuiKey_End, model::Focus::RawLog) == model::Action::None,
          "R3: a region-scoped row fires there and nowhere else");

    g_phase = "R4 overlay trap";
    check(model::resolve(model::defaultKeymap(), ImGuiMod_Ctrl | ImGuiKey_Q,
                         model::Focus::Overlay) == model::Action::Quit,
          "R4: quitting is bound inside an overlay (its own row)");
    check(model::resolve(model::defaultKeymap(), ImGuiKey_End, model::Focus::Overlay) ==
              model::Action::None,
          "R4: an anywhere row does not fire behind an open overlay");

    g_phase = "R5 pressed resolution";
    const auto pressed = [](ImGuiKeyChord chord) { return chord == (ImGuiMod_Ctrl | ImGuiKey_Q); };
    const model::Binding* fired = model::resolvePressedBinding(local, model::Focus::Rail, pressed);
    check(fired != nullptr && fired->action == model::Action::Quit &&
              fired->chord == (ImGuiMod_Ctrl | ImGuiKey_Q),
          "R5: resolvePressedBinding names the pressed row (chord included)");
    check(model::resolvePressed(local, model::Focus::Rail, pressed) == model::Action::Quit,
          "R5: resolvePressed answers with that row's action");
    check(model::resolvePressedBinding(local, model::Focus::Rail,
                                       [](ImGuiKeyChord) { return false; }) == nullptr &&
              model::resolvePressed(local, model::Focus::Rail,
                                    [](ImGuiKeyChord) { return false; }) == model::Action::None,
          "R5: nothing pressed -> no row, no action");

    g_phase = "R6 esc chain order";
    check(model::resolveEscape({true, true, true}) == model::Action::CloseOverlay &&
              model::resolveEscape({false, true, true}) == model::Action::CancelRun &&
              model::resolveEscape({false, false, true}) == model::Action::ClearInput &&
              model::resolveEscape({false, false, false}) == model::Action::Blur,
          "R6: Esc is overlay -> cancel -> clear -> blur");
    // Focus mode (task 43): a step under the overlay's and above the composer's three, so a press
    // that leaves the mode never also clears the draft or stops the run.
    check(model::resolveEscape({false, true, true, true}) == model::Action::ExitFocusMode &&
              model::resolveEscape({false, false, false, true}) == model::Action::ExitFocusMode &&
              model::resolveEscape({true, true, true, true}) == model::Action::CloseOverlay,
          "R6: focus mode's step sits under the overlay's and above the composer's");
    // The now bar's `Esc stop` hint asks this and not the run's state: a press that leaves focus
    // mode (or closes an overlay) stops nothing, and a hint may only name the step it performs.
    check(model::escapeStopsRun({false, true, false, false}) &&
              !model::escapeStopsRun({false, true, false, true}) &&
              !model::escapeStopsRun({true, true, false, false}),
          "R6: escapeStopsRun is true exactly when the press would cancel the run");

    g_phase = "R7 function keys";
    // A function key is a plain key that types nothing, so the text-field rule must let it
    // through: `F11` is focus mode's key and has to work while the user is typing (task 43).
    check(model::resolve(model::defaultKeymap(), ImGuiKey_F11, model::Focus::Composer) ==
                  model::Action::ToggleFocusMode &&
              model::resolve(model::defaultKeymap(), ImGuiKey_F11, model::Focus::None) ==
                  model::Action::ToggleFocusMode,
          "R7: F11 toggles focus mode, the composer's keyboard included");
    // Both of the mode's chords resolve to the action, from the field as well: a `Ctrl+Shift`
    // chord is exactly the shape the text-field rule could swallow, so it is asserted rather than
    // assumed (the terminal does not deliver it - plan/key_gate.md §3b - the pixel profile does).
    check(model::resolve(model::defaultKeymap(), ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_M,
                         model::Focus::Composer) == model::Action::ToggleFocusMode &&
              model::resolve(model::defaultKeymap(), ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_M,
                             model::Focus::None) == model::Action::ToggleFocusMode,
          "R7: Ctrl+Shift+M resolves to the same action, the composer's keyboard included");
    // ...and the rule it is an exception to still holds for the keys that *are* the field's.
    check(model::resolve(model::defaultKeymap(), ImGuiKey_End, model::Focus::Composer) ==
                  model::Action::None &&
              model::resolve(model::defaultKeymap(), ImGuiKey_UpArrow, model::Focus::Composer) ==
                  model::Action::HistoryPrev,
          "R7: End stays the field's (inert) and Up stays its history key");
    check(model::chordLabel(ImGuiKey_F11) == "F11" &&
              model::chordLabel(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_M) == "Ctrl+Shift+M",
          "R7: the new chords have names (a '?' would reach the help page and the document)");
}

// ---------------------------------------------------------------------------
// Interaction harness: the real frame pipeline onto a TScreen cell grid
// ---------------------------------------------------------------------------

ImTui::TScreen g_screen;
TuiState g_state;

/// Render one frame. The quit scenarios set `quitRequested`, which makes the
/// next render report "user terminated" - that is asserted where it matters, so
/// the harness records it instead of stopping the run.
bool g_lastRenderContinues = true;

// Defined below, used by the key helpers: what the UI log gained during a
// gesture.
std::string readLog();
std::string logSince(std::size_t offset);

void frame(const std::function<void(ImGuiIO&)>& inject = nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    if (inject)
        inject(io);
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    g_lastRenderContinues = llmfun::tui::tuiRender(g_state);
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    ++g_frames;
}

void idle(int n = 1) {
    for (int i = 0; i < n; ++i)
        frame();
}

void pressKey(ImGuiKey key) {
    frame([&](ImGuiIO& io) { io.AddKeyEvent(key, true); });
    frame([&](ImGuiIO& io) { io.AddKeyEvent(key, false); });
}

/// One key press as a terminal delivers it: the key is down and up inside one frame's worth
/// of input, so `IsKeyPressed` fires exactly once. The two-frame `pressKey` above is the shape
/// a *held* key has (down, then release), which the text backend can report twice.
void tapKey(ImGuiKey key) {
    frame([&](ImGuiIO& io) {
        io.AddKeyEvent(key, true);
        io.AddKeyEvent(key, false);
    });
    idle(1);
}

/// A chord with the modifier already held: one frame per edge, so the trickled
/// event queue cannot split the pair across frames. Reports whether a quit was
/// requested on the press frame; the flag is cleared again so the harness keeps
/// rendering. `logWritten`, when given, receives what the press frame wrote to the
/// UI log.
bool pressCtrl(ImGuiKey key, std::string* logWritten = nullptr) {
    frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, true); });
    const std::size_t logMark = readLog().size();
    frame([&](ImGuiIO& io) { io.AddKeyEvent(key, true); });
    const bool quit = g_state.quitRequested;
    g_state.quitRequested = false;
    if (logWritten != nullptr)
        *logWritten = logSince(logMark);
    frame([&](ImGuiIO& io) { io.AddKeyEvent(key, false); });
    frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, false); });
    idle(1);
    return quit;
}

/// Ctrl+Shift+<key>: proves the chord matcher compares modifiers exactly.
bool pressCtrlShift(ImGuiKey key) {
    frame([](ImGuiIO& io) {
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiMod_Shift, true);
    });
    frame([&](ImGuiIO& io) { io.AddKeyEvent(key, true); });
    const bool quit = g_state.quitRequested;
    g_state.quitRequested = false;
    frame([&](ImGuiIO& io) { io.AddKeyEvent(key, false); });
    frame([](ImGuiIO& io) {
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        io.AddKeyEvent(ImGuiMod_Shift, false);
    });
    idle(1);
    return quit;
}

/// The composer takes the keyboard through its one-shot focus request, exactly
/// like the Chat menu and the palette's close do.
void focusComposer() {
    // The workbench composer takes the keyboard through its own one-shot request (task 27);
    // `UserQueryState::isSubmitted` is the shipped field's legacy flag and is kept set so a
    // state that also carries it behaves the same.
    g_state.userQuery.isSubmitted = true;
    g_state.composer.focusRequested = true;
    idle(2);
}

std::string input() { return std::string(g_state.userQuery.inputBuf.c_str()); }

/// Type like a user: one character event per frame, then an idle frame so the
/// widget's state and the buffer agree before the next assertion.
void typeText(const std::string& text) {
    for (char c : text) {
        frame([&](ImGuiIO& io) { io.AddInputCharacter(static_cast<unsigned int>(c)); });
        idle(1);
    }
}

/// Click one cell of the 80x24 text grid (the text backend's mouse position is
/// in cells). Down and up are separate frames so the widget sees a real click
/// edge, exactly like the filter's click-to-focus path.
void clickAt(float x, float y) {
    frame([&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
    });
    frame([&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    idle(1);
}

/// True while any ImGui popup/menu (the rail's row menu, the menu bar) is open.
bool anyPopupOpen() {
    return ImGui::IsPopupOpen(static_cast<ImGuiID>(0),
                              ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

std::string readLog() {
    std::string all;
    FILE* file = std::fopen(kLogName, "rb");
    if (file == nullptr)
        return all;
    char buf[4096];
    std::size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof buf, file)) > 0)
        all.append(buf, got);
    std::fclose(file);
    return all;
}

/// The log text appended after `offset`, i.e. what one gesture wrote.
std::string logSince(std::size_t offset) {
    const std::string all = readLog();
    return offset < all.size() ? all.substr(offset) : std::string();
}

/// How many lines of `text` mention `needle`: a dispatch is asserted this way so
/// an unrelated log line from another view cannot fail the count.
int countMatchingLines(const std::string& text, const std::string& needle) {
    int matches = 0;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        if (text.substr(start, end - start).find(needle) != std::string::npos)
            ++matches;
        start = end + 1;
    }
    return matches;
}

bool screenHas(const std::string& needle) {
    for (int y = 0; y < g_screen.ny; ++y) {
        std::string row;
        for (int x = 0; x < g_screen.nx; ++x) {
            const int c = g_screen.data[y * g_screen.nx + x].ch;
            row.push_back((c >= 32 && c < 127) ? char(c) : ' ');
        }
        if (row.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

/// Where `needle` starts on the last frame's grid (the pointer targets a cell, not a rect).
bool findOnScreen(const std::string& needle, int& x, int& y) {
    for (int row = 0; row < g_screen.ny; ++row) {
        std::string line;
        for (int col = 0; col < g_screen.nx; ++col) {
            const int c = g_screen.data[row * g_screen.nx + col].ch;
            line.push_back((c >= 32 && c < 127) ? char(c) : ' ');
        }
        const std::size_t at = line.find(needle);
        if (at != std::string::npos) {
            x = static_cast<int>(at);
            y = row;
            return true;
        }
    }
    return false;
}

SessionEntry mkSession(const char* id, const char* title, const char* preview, int count,
                       bool active = false) {
    SessionEntry entry;
    entry.id = id;
    entry.title = title;
    entry.preview = preview;
    entry.messageCount = static_cast<std::size_t>(count);
    entry.isActive = active;
    return entry;
}

void seedThread() {
    g_state.chat.outputLines.clear();
    g_state.chat.outputLineOpen.clear();
    g_state.chat.expansionDefaultThroughId = 0;
    g_state.chat.outputLineNextId = 1;
    g_state.chat.outputLines.push_back(ChatMessage{"[1] run the tests", "run the tests", "",
                                                   ChatMessageType::User,
                                                   g_state.chat.outputLineNextId++});
    g_state.chat.outputLines.push_back(ChatMessage{"[2] running", "partial output", "",
                                                   ChatMessageType::Assistant,
                                                   g_state.chat.outputLineNextId++});
}

/// Fresh state for one scenario: three sessions, a two-message thread, no run,
/// Chat tab, composer focused, every latch clear.
void resetClean() {
    g_state.palette.open = false;
    g_state.palette.items.clear();
    g_state.palette.visible.clear();
    g_state.palette.itemIds.clear();
    g_state.palette.query.fill('\0');
    g_state.palette.builtQuery.clear();
    g_state.sessionPanel.actions.clear();
    g_state.sessionPanel.activeId = "s001-0";
    g_state.sessionPanel.sessions = {
        mkSession("s001-0", "Alpha refactor", "", 3, true),
        mkSession("s002-1", "Beta deploy", "deploy the beta build", 5),
        mkSession("s003-2", "Gamma debug", "", 1),
    };
    g_state.userQuery = UserQueryState{};
    g_state.readyStatus = true;
    g_state.cancelPending = false;
    g_state.cancelRequested = false;
    g_state.quitRequested = false;
    g_state.quitConfirmArmed = false;
    g_state.escapeConsumed = false;
    g_state.popupOpenLastFrame = false;
    g_state.offline = false;
    g_state.autoScroll = true;
    g_state.isLogActive = false;
    seedThread();
    idle(2);
    focusComposer();
}

/// Release the composer's keyboard (no run, empty input -> the chain's blur
/// step), then confirm it is released.
void blurComposer() {
    pressKey(ImGuiKey_Escape);
    idle(1);
    if (ImGui::GetIO().WantTextInput)
        setupError("the composer did not release the keyboard");
}

// S1: the quit chord, its single hint line, and the untouched thread.
void scenarioQuit() {
    g_phase = "S1 quit";
    resetClean();
    g_state.isLogActive = true;
    idle(1);
    const std::size_t threadBefore = g_state.chat.outputLines.size();
    std::string written;
    const bool quit = pressCtrl(ImGuiKey_Q, &written);
    check(quit, "S1: Ctrl+Q requests the quit");
    check(countMatchingLines(written, "Ctrl+Q -> Quit") == 1,
          "S1: the quit chord logs exactly one line (" +
              std::to_string(countMatchingLines(written, "Ctrl+Q -> Quit")) + ")");
    check(g_state.chat.outputLines.size() == threadBefore,
          "S1: nothing was printed into the thread");

    g_phase = "S1 exact modifiers";
    const std::size_t logBeforeShift = readLog().size();
    check(!pressCtrlShift(ImGuiKey_Q), "S1: Ctrl+Shift+Q is not a quit");
    check(logSince(logBeforeShift).empty(), "S1: and it writes nothing to the log");

    // The frame after the request reports "user terminated": that handshake is
    // the only thing the chord has to produce (the D loop owns the rest).
    g_state.quitRequested = true;
    frame();
    check(!g_lastRenderContinues, "S1: the frame after a quit reports 'user terminated'");
    g_state.quitRequested = false;
    idle(1);
    g_state.isLogActive = false;
}

// S2: the confirmation while a run is active - two presses, a prompt the user can
// see, and the arming dies with the run.
void scenarioQuitConfirm() {
    g_phase = "S2 quit confirm";
    resetClean();
    llmfun::tui::tuiSetReadyStatus(g_state, false); // a run is active
    g_state.isLogActive = true;
    idle(1);
    check(!screenHas("Ctrl+Q again"), "S2: no prompt before the chord is pressed");
    const std::size_t threadBefore = g_state.chat.outputLines.size();

    std::string first;
    check(!pressCtrl(ImGuiKey_Q, &first), "S2: the first Ctrl+Q during a run does not quit");
    check(g_state.quitConfirmArmed, "S2: it arms the confirmation");
    check(countMatchingLines(first, "Ctrl+Q -> Quit (armed)") == 1,
          "S2: the armed press is logged as an arm, not as a quit");
    // The prompt is independent of the UI log: turn the log off before the
    // frame that draws it, so the assertion pins the status row, not the log.
    g_state.isLogActive = false;
    idle(1);
    check(screenHas("Ctrl+Q again"),
          "S2: the armed confirmation is on screen (it is visible with the UI log off)");
    check(g_state.chat.outputLines.size() == threadBefore,
          "S2: the prompt did not write anything into the thread");

    check(pressCtrl(ImGuiKey_Q), "S2: the second Ctrl+Q quits");
    check(!g_state.quitConfirmArmed, "S2: quitting clears the arming");
    g_state.quitRequested = false;
    idle(2);
    check(!screenHas("Ctrl+Q again"), "S2: the prompt is gone once answered");

    // A new run disarms again: one press must not quit an idle UI.
    g_state.quitConfirmArmed = true;
    llmfun::tui::tuiSetReadyStatus(g_state, true);
    check(!g_state.quitConfirmArmed, "S2: the arming dies with the run");
    check(pressCtrl(ImGuiKey_Q), "S2: with no run a single press quits again");
    g_state.isLogActive = false;
}

// S3: the palette toggle went through the registry, and the quit chord still
// works while the overlay owns the keyboard.
void scenarioPaletteToggle() {
    g_phase = "S3 palette toggle";
    resetClean();
    pressCtrl(ImGuiKey_K);
    check(g_state.palette.open, "S3: Ctrl+K opens the palette through the keymap");
    idle(1);
    const bool quitInsideOverlay = pressCtrl(ImGuiKey_Q);
    check(quitInsideOverlay, "S3: Ctrl+Q quits while the palette is open");
    check(g_state.palette.open, "S3: the quit press did not close the overlay first");

    g_state.isLogActive = true;
    const std::size_t kMark = readLog().size();
    pressCtrl(ImGuiKey_K);
    check(countMatchingLines(logSince(kMark), "Ctrl+K -> CloseOverlay") == 1,
          "S3: the toggle resolves to CloseOverlay inside the overlay");
    g_state.isLogActive = false;
    check(!g_state.palette.open, "S3: Ctrl+K closes it again");
}

// S4: the Esc chain while the composer owns the keyboard - one step per press.
void scenarioEscapeChain() {
    g_phase = "S4 esc chain";
    resetClean();
    typeText("draft text");
    check(input() == "draft text", "S4: the composer holds the draft");
    check(ImGui::GetIO().WantTextInput, "S4: the composer owns the keyboard");
    llmfun::tui::tuiSetReadyStatus(g_state, false); // a run is active
    const std::size_t threadBefore = g_state.chat.outputLines.size();

    // Step 2: the run is cancelled and nothing else happens.
    pressKey(ImGuiKey_Escape);
    check(g_state.cancelPending && g_state.cancelRequested, "S4: the first Esc cancels the run");
    check(input() == "draft text", "S4: it keeps the draft (one step per press)");
    check(!g_state.palette.open, "S4: it does not touch the overlay");
    check(g_state.chat.outputLines.size() == threadBefore, "S4: the run's partial output is kept");

    // Step 3: with the stop already requested, the same key clears the input.
    focusComposer();
    pressKey(ImGuiKey_Escape);
    check(input().empty(), "S4: the second Esc clears the input");
    check(g_state.chat.outputLines.size() == threadBefore, "S4: the thread is still untouched");

    // Step 4: nothing left to clear, so the keyboard is released.
    focusComposer();
    pressKey(ImGuiKey_Escape);
    check(!ImGui::GetIO().WantTextInput, "S4: the third Esc releases the keyboard");
}

// S5: an open overlay takes the first Esc, and only that one.
void scenarioOverlayFirst() {
    g_phase = "S5 overlay first";
    resetClean();
    typeText("draft text");
    llmfun::tui::tuiSetReadyStatus(g_state, false); // a run is active
    pressCtrl(ImGuiKey_K);
    check(g_state.palette.open, "S5: the palette is open on top of a running agent");

    pressKey(ImGuiKey_Escape);
    check(!g_state.palette.open, "S5: the first Esc closes the overlay");
    check(!g_state.cancelRequested, "S5: and does not also cancel the run");
    check(input() == "draft text", "S5: and does not clear the draft");
}

// S6: End dispatches ScrollEnd, and - the plain-key rule - not while a text field
// has the keyboard. Both are asserted on the dispatch (the log line), because the
// derived `autoScroll` flag can be true for other reasons.
void scenarioEndKey() {
    g_phase = "S6 End focused";
    resetClean();
    g_state.isLogActive = true;
    g_state.autoScroll = false;
    idle(1);
    check(ImGui::GetIO().WantTextInput, "S6: the composer owns the keyboard");
    std::size_t mark = readLog().size();
    pressKey(ImGuiKey_End);
    check(countMatchingLines(logSince(mark), "End -> ScrollEnd") == 0,
          "S6: End is inert while a text field has the keyboard (plain-key rule)");
    check(!g_state.autoScroll, "S6: ...so follow-the-newest was not re-enabled");

    g_phase = "S6 End blurred";
    blurComposer();
    mark = readLog().size();
    pressKey(ImGuiKey_End);
    check(countMatchingLines(logSince(mark), "End -> ScrollEnd") == 1,
          "S6: End dispatches ScrollEnd once the field is released");
    check(g_state.autoScroll, "S6: ...and follow-the-newest is on");

    g_phase = "S6 End in an overlay";
    g_state.autoScroll = false;
    pressCtrl(ImGuiKey_K);
    idle(1);
    mark = readLog().size();
    pressKey(ImGuiKey_End);
    check(countMatchingLines(logSince(mark), "End -> ScrollEnd") == 0,
          "S6: End inside the palette does not jump the thread behind it");
    check(!g_state.autoScroll, "S6: ...and follow-the-newest stayed off");
    pressCtrl(ImGuiKey_K);
    g_state.isLogActive = false;
}

// S7: the cancel step does not depend on the composer having the keyboard.
void scenarioCancelWhileBlurred() {
    g_phase = "S7 cancel blurred";
    resetClean();
    blurComposer();
    llmfun::tui::tuiSetReadyStatus(g_state, false); // a run is active
    g_state.isLogActive = true;
    idle(1);
    const std::size_t mark = readLog().size();
    pressKey(ImGuiKey_Escape);
    check(g_state.cancelPending && g_state.cancelRequested,
          "S7: Esc cancels the run with the composer blurred");
    check(countMatchingLines(logSince(mark), "Esc -> CancelRun") == 1,
          "S7: and logs the step once");
    check(!g_state.palette.open, "S7: the overlay was not involved");

    // A second press must not re-request anything (the latch is set).
    const std::size_t markAgain = readLog().size();
    pressKey(ImGuiKey_Escape);
    check(countMatchingLines(logSince(markAgain), "Esc -> CancelRun") == 0,
          "S7: a second Esc does not cancel again (one step per press)");
    g_state.isLogActive = false;
}

// S8: the chain's cancel step also works from a surface that does not draw the composer. The Raw
// Log's full-screen overlay is that surface (the Log tab it replaced retired in task 41), and
// D-ESC's order holds there: the first press closes the overlay, the second one cancels the run -
// the chain does not depend on the composer having the keyboard.
void scenarioCancelFromOverlay() {
    g_phase = "S8 cancel from the Raw Log";
    resetClean();
    pressKey(ImGuiKey_Escape); // release the composer first: the overlay takes the keyboard
    idle(1);
    llmfun::tui::views::toggleRawLog(g_state);
    idle(2);
    check(g_state.ui.rawLogOpen, "S8: the Raw Log overlay is up");
    llmfun::tui::tuiSetReadyStatus(g_state, false); // a run is active
    idle(1);

    pressKey(ImGuiKey_Escape);
    check(!g_state.ui.rawLogOpen, "S8: the first Esc closes the overlay");
    check(!g_state.cancelPending && !g_state.cancelRequested,
          "S8: ...and does not also cancel the run (one step per press)");

    pressKey(ImGuiKey_Escape);
    check(g_state.cancelPending && g_state.cancelRequested,
          "S8: the next Esc cancels the run without the composer on screen");
}

// S10: a view-local Esc consumer owns the press. The frame-level cancel must
// stand down for the press that closes it, then still work on the next press
// (the latch is per frame, not sticky). Covers the rename box, the focused
// filter and an ImGui popup/menu - the three consumers that used to double-act.
void scenarioEscapeOwnership() {
    // One press, one action (D-ESC): the rail's own Esc consumers (the rename box, the search
    // field, the row menu) spend the press before the frame-level chain can cancel the run. The
    // rail is the region that owns those keys now; `./test_rail` pins its side of the contract,
    // and this scenario is the frame's half - the same press must not reach both.
    //
    // The rail is a *column* at this width: at 80x24 the shell gives it the collapsed strip,
    // whose policy is "no dialog and no query while collapsed" (correctly - it clears them), so
    // the scenario that needs a rename box has to run where a rename box can be drawn.
    ImGui::GetIO().DisplaySize = ImVec2(120.0f, 36.0f);
    g_phase = "S10 escape ownership: rename";
    resetClean();
    g_state.ui.prefs.railOpen = 1;
    g_state.ui.rail.navRowId = "s002-1";
    g_state.ui.rail.renameRowId = "s002-1";
    g_state.ui.rail.renameFocus = true;
    std::snprintf(g_state.ui.rail.renameBuf.data(), g_state.ui.rail.renameBuf.size(), "%s",
                  "Beta deploy renamed");
    llmfun::tui::tuiSetReadyStatus(g_state, false); // a run is active
    idle(2);
    check(!g_state.ui.rail.renameRowId.empty(), "S10: the rename box is open");
    tapKey(ImGuiKey_Escape);
    check(g_state.ui.rail.renameRowId.empty(), "S10: Esc closes the rename box");
    check(!g_state.cancelPending && !g_state.cancelRequested,
          "S10: closing the rename did not cancel the running agent");
    pressKey(ImGuiKey_Escape);
    check(g_state.cancelPending && g_state.cancelRequested,
          "S10: the next Esc cancels the run (the latch is per frame)");

    g_phase = "S10 escape ownership: search";
    resetClean();
    g_state.ui.prefs.railOpen = 1;
    g_state.ui.rail.filterFocus = true;
    idle(2);
    typeText("alp");
    check(std::string(g_state.ui.rail.filterBuf.data()) == "alp",
          "S10: the rail's search field holds the query");
    llmfun::tui::tuiSetReadyStatus(g_state, false);
    idle(1);
    tapKey(ImGuiKey_Escape);
    check(std::string(g_state.ui.rail.filterBuf.data()).empty(), "S10: Esc clears the search");
    check(!g_state.cancelPending && !g_state.cancelRequested,
          "S10: clearing the search did not cancel the running agent");
    pressKey(ImGuiKey_Escape);
    check(g_state.cancelPending && g_state.cancelRequested, "S10: the next Esc cancels the run");

    g_phase = "S10 escape ownership: popup";
    resetClean();
    g_state.ui.prefs.railOpen = 1;
    blurComposer();
    // The row keys act on the row the user aimed at: the pointer's row, or the keyboard-focused
    // one. Aim at the second session's title on screen.
    int rowX = -1;
    int rowY = -1;
    check(findOnScreen("Beta deploy", rowX, rowY), "S10: premise - a session row is on screen");
    clickAt(static_cast<float>(rowX), static_cast<float>(rowY));
    idle(1);
    // The row keys are characters (plan/key_gate.md F2), not key events.
    typeText("m");
    idle(1);
    check(anyPopupOpen(), "S10: the rail row menu is open");
    llmfun::tui::tuiSetReadyStatus(g_state, false);
    idle(1);
    pressKey(ImGuiKey_Escape);
    check(!anyPopupOpen(), "S10: Esc closes the row menu");
    check(!g_state.cancelPending && !g_state.cancelRequested,
          "S10: closing the row menu did not cancel the running agent");
    pressKey(ImGuiKey_Escape);
    check(g_state.cancelPending && g_state.cancelRequested, "S10: the next Esc cancels the run");

    ImGui::GetIO().DisplaySize = ImVec2(80.0f, 24.0f);
}

// S9: the shell still renders and the thread is what the earlier tasks left -
// the dispatcher must not become a second renderer.
void scenarioShellIntact() {
    g_phase = "S9 shell";
    resetClean();
    check(g_screen.nx == 80 && g_screen.ny == 24, "S9: the harness renders at 80x24 (" +
                                                      std::to_string(g_screen.nx) + "x" +
                                                      std::to_string(g_screen.ny) + ")");
    check(screenHas("llmfun"), "S9: the top bar's wordmark is on screen");
    check(screenHas("Ask anything"), "S9: the composer placeholder is on screen");
    const std::size_t threadBefore = g_state.chat.outputLines.size();
    pressKey(ImGuiKey_End);
    check(g_state.chat.outputLines.size() == threadBefore, "S9: End does not touch the thread");
}

void scenarioFocusMode() {
    g_phase = "S10 focus mode";
    resetClean();
    idle(1);
    check(ImGui::GetIO().WantTextInput, "S10 premise: the composer owns the keyboard");
    g_state.isLogActive = true;

    // In: the chord from the field the user is normally in. A function key types nothing, so the
    // plain-key rule lets it through (the registry half of this is `checkRegistry()`'s R7).
    std::size_t mark = readLog().size();
    pressKey(ImGuiKey_F11);
    check(g_state.ui.focusMode, "S10: F11 enters focus mode from the composer");
    check(countMatchingLines(logSince(mark), "F11 -> ToggleFocusMode") == 1,
          "S10: ...and the dispatcher logs the row");
    idle(2);

    // Out by the same key, so the mode is a toggle and not a one-way door.
    pressKey(ImGuiKey_F11);
    check(!g_state.ui.focusMode, "S10: a second F11 leaves focus mode");

    // The palette's row is the grid's way in (no terminal delivers `F11`): the same rule, one
    // call (`model::toggleFocusMode`), so the two entry points cannot disagree.
    g_state.ui.focusMode = false;
    pressCtrl(ImGuiKey_K);
    check(g_state.palette.open, "S10: the palette opens");
    idle(1);
    g_state.ui.focusMode = false;
    {
        bool performed = false;
        for (const views::PaletteItem& item : g_state.palette.items) {
            if (item.action == model::Action::ToggleFocusMode)
                performed = views::palettePerformRow(item, g_state.palette, g_state);
        }
        check(performed && g_state.ui.focusMode, "S10: the palette's row toggles the same mode");
    }
    g_state.palette = views::PaletteState{};
    idle(2);

    // Out by `Esc`, with a draft: the mode is the shell's, the text is the field's. One press must
    // leave the mode, keep the draft and keep the keyboard - the rule the cancel step follows.
    g_state.ui.focusMode = true;
    focusComposer();
    typeText("draft");
    check(input() == "draft", "S10: the field holds a draft in focus mode");
    mark = readLog().size();
    pressKey(ImGuiKey_Escape);
    check(!g_state.ui.focusMode, "S10: Esc leaves focus mode");
    check(countMatchingLines(logSince(mark), "Esc -> ExitFocusMode") == 1,
          "S10: ...as the chain step, once");
    check(input() == "draft", "S10: ...and the draft survives the press");
    // The refocus is a one-frame request (`SetKeyboardFocusHere` takes the field back on the next
    // frame), so the keyboard is asserted after it was consumed.
    idle(2);
    check(ImGui::GetIO().WantTextInput, "S10: ...and the field keeps the keyboard");

    // Order: a surface above the shell closes first, so the mode is still on after that press.
    g_state.ui.focusMode = true;
    pressCtrl(ImGuiKey_K);
    check(g_state.palette.open, "S10 premise: the palette is open above focus mode");
    pressKey(ImGuiKey_Escape);
    check(!g_state.palette.open, "S10: the first Esc closes the palette");
    check(g_state.ui.focusMode, "S10: ...and leaves focus mode on for the next press");
    pressKey(ImGuiKey_Escape);
    check(!g_state.ui.focusMode, "S10: the second Esc leaves the mode");

    // A hidden rail owns no press (I2 of the review): the rail's own `Esc` step reads latches that
    // only its renderer refreshes, and in focus mode that renderer does not run. A rail left in a
    // search would otherwise make the composer keyboard-dead behind a panel nobody can see.
    g_state.ui.rail.open = true;
    g_state.ui.rail.searchWide = true;
    g_state.ui.rail.filterFocus = true;
    g_state.ui.rail.filterHadFocus = true;
    check(llmfun::tui::views::railOwnsEscape(g_state),
          "S10 premise: a rail in its search owns Esc while it is on screen");
    g_state.ui.focusMode = true;
    check(!llmfun::tui::views::railOwnsEscape(g_state),
          "S10: ...and owns nothing while focus mode hides it");
    // The proof that the field kept its permission: `↑` recalls the newest history entry, and the
    // composer only reads the keymap when `acceptKeys` is true.
    llmfun::tui::views::pushHistory(g_state, "recalled query");
    g_state.userQuery.inputBuf.clear();
    focusComposer();
    idle(1);
    pressKey(ImGuiKey_UpArrow);
    check(input() == "recalled query", "S10: the composer's own keys stay alive behind the mode");
    g_state.ui.rail = model::RailState{};
    g_state.ui.focusMode = false;

    // The region toggles cannot act on a region the mode hides (M1 of the review): a press there
    // would only write a preference nobody can see, and the panel would appear when the mode ends.
    g_state.ui.focusMode = true;
    g_state.ui.prefs.railOpen = 0;
    g_state.ui.prefs.inspectorOpen = 0;
    pressCtrl(ImGuiKey_B);
    pressCtrl(ImGuiKey_I);
    check(g_state.ui.prefs.railOpen == 0 && g_state.ui.prefs.inspectorOpen == 0,
          "S10: Ctrl+B/Ctrl+I leave a hidden region's preference alone");
    g_state.ui.focusMode = false;
    pressCtrl(ImGuiKey_B);
    pressCtrl(ImGuiKey_I);
    check(g_state.ui.prefs.railOpen == 1 && g_state.ui.prefs.inspectorOpen == 1,
          "S10: ...and act again once the mode is left");

    // The inspector's tab controls are the same rule: `Ctrl+5`/`Ctrl+6` request a tab and
    // `Ctrl+1`/`Ctrl+2` arm a notice in the strip, and in focus mode the panel has neither - the
    // frame's one writer drops the request instead of selecting a tab nobody can see and opening
    // the panel on the way out.
    g_state.ui.focusMode = true;
    g_state.ui.prefs.inspectorOpen = 0;
    g_state.ui.prefs.tab = model::InspectorTab::Activity;
    g_state.inspector.reservedPressed = false;
    pressCtrl(ImGuiKey_6);
    pressCtrl(ImGuiKey_1);
    pressCtrl(ImGuiKey_5);
    check(g_state.ui.prefs.tab == model::InspectorTab::Activity &&
              g_state.ui.prefs.inspectorOpen == 0 && !g_state.inspector.reservedPressed,
          "S10: the tab controls leave a hidden panel's tab, open flag and notice alone");
    g_state.ui.focusMode = false;
    pressCtrl(ImGuiKey_5);
    check(g_state.ui.prefs.tab == model::InspectorTab::Agents &&
              g_state.ui.prefs.inspectorOpen == 1,
          "S10: ...and a tab press shows the panel again once the mode is left");

    // `Esc` with a run in flight (I1 of the review): the press leaves the mode and the run keeps
    // going; only the *next* press cancels - which is exactly what the now bar's hint must say
    // (`model::escapeStopsRun`, R6). The side regions go back to closed first: an open panel is a
    // surface, and `Esc` closes a surface before it reaches the chain's later steps.
    g_state.ui.prefs.railOpen = 0;
    g_state.ui.prefs.inspectorOpen = 0;
    g_state.ui.focusMode = true;
    llmfun::tui::tuiSetReadyStatus(g_state, false);
    idle(2);
    check(!views::escStepClosesOverlay(g_state), "S10 premise: no surface owns the first Esc step");
    check(!model::escapeStopsRun({false, true, false, true}),
          "S10 premise: with the mode on, Esc would leave it, not stop the run");
    pressKey(ImGuiKey_Escape);
    check(!g_state.ui.focusMode, "S10: Esc leaves focus mode while a run is active");
    check(!g_state.cancelRequested, "S10: ...and does not cancel the run");
    idle(1);
    check(llmfun::tui::cancelStepOpen(g_state), "S10: the cancel step is open for the next press");
    pressKey(ImGuiKey_Escape);
    check(g_state.cancelRequested, "S10: the next Esc cancels the run");
    g_state.isLogActive = false;
}

void harnessInit() {
    std::setlocale(LC_ALL, "");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    llmfun::tui::applyUiTheme();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigInputTextCursorBlink = false;
    io.KeyRepeatDelay = 0.050f;
    io.KeyRepeatRate = 0.050f;
    ImTui_ImplText_Init();
    io.DisplaySize = ImVec2(80.0f, 24.0f);
}

void harnessShutdown() {
    ImTui_ImplText_Shutdown();
    ImGui::DestroyContext();
}

int runHarness() {
    harnessInit();
    idle(1);
    scenarioShellIntact();
    scenarioQuit();
    scenarioQuitConfirm();
    scenarioPaletteToggle();
    scenarioEscapeChain();
    scenarioOverlayFirst();
    scenarioEndKey();
    scenarioCancelWhileBlurred();
    scenarioCancelFromOverlay();
    scenarioEscapeOwnership();
    scenarioFocusMode();
    harnessShutdown();
    return g_failures;
}

} // namespace

int main() {
    checkRegistry();
    runHarness();
    std::printf("%s: %d checks failed (%d frames)\n", g_failures == 0 ? "OK" : "FAILED", g_failures,
                g_frames);
    return g_failures == 0 ? 0 : 1;
}
