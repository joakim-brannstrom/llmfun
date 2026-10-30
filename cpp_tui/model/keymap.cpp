/// The one binding table (see keymap.h for the contract and the rules).
///
/// What ships here is what the UI can actually do today: every row is a chord
/// that reaches a handler in this build. The remaining actions of the design's
/// §6.7 table are appended by the task that adds their surface - `Ctrl+B` with
/// the rail's collapse bookkeeping (task 23), `Ctrl+I` and `Ctrl+5`/`Ctrl+6`
/// with the inspector (task 29), `Ctrl+Shift+L` and the Raw Log keys with the
/// overlay (task 33), and `Ctrl+L` when the thread owns its own "no widget
/// focused" predicate (tasks 25/44). A binding that resolves to an action nothing
/// handles would be a dead key, which is exactly what the registry exists to
/// prevent.
///
/// Delivery is the other half of the same rule (D-KEYGATE): a row is evidence, not a
/// wish. Where a chord is *not* delivered by one profile the action needs a second,
/// measured path - `Ctrl+I`/`Ctrl+5`/`Ctrl+6` reach the pixel profile and the grid
/// arrives at the same four actions through the palette and the panel's own
/// character-level `[`/`]` (plan/key_gate.md §10/§11).
///
/// Two chords of the design's table are not rows for evidence reasons
/// (plan/key_gate.md §3-§4, and both are unchanged by this task):
///
///  * `Ctrl+C` never reaches the UI in the terminal - `ISIG` is on, so the line
///    discipline turns it into `SIGINT` before ncurses reads it - and in the GUI
///    it is the text field's own copy chord. Binding it would either be dead
///    (grid) or break copy (pixel), so the design's "Ctrl+C = Quit" stays the
///    terminal's own spelling of quitting.
///  * `Ctrl+Shift+L` arrives as `Ctrl+L` in the terminal (the shift is lost,
///    `0x0C`), so it cannot be told apart from the design's clear-output chord.
///    Task 33 decides that pair; nothing here claims it.
#include "keymap.h"

namespace llmfun::tui::model {

std::string chordLabel(ImGuiKeyChord chord) {
    std::string label;
    if ((chord & ImGuiMod_Ctrl) != 0)
        label += "Ctrl+";
    if ((chord & ImGuiMod_Shift) != 0)
        label += "Shift+";
    if ((chord & ImGuiMod_Alt) != 0)
        label += "Alt+";
    if ((chord & ImGuiMod_Super) != 0)
        label += "Super+";

    const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    switch (key) {
    case ImGuiKey_End:
        return label + "End";
    case ImGuiKey_PageUp:
        return label + "PgUp";
    case ImGuiKey_PageDown:
        return label + "PgDn";
    case ImGuiKey_UpArrow:
        return label + "Up";
    case ImGuiKey_DownArrow:
        return label + "Down";
    case ImGuiKey_LeftArrow:
        return label + "Left";
    case ImGuiKey_RightArrow:
        return label + "Right";
    case ImGuiKey_Escape:
        return label + "Esc";
    case ImGuiKey_Enter:
        return label + "Enter";
    case ImGuiKey_Tab:
        return label + "Tab";
    case ImGuiKey_Space:
        return label + "Space";
    case ImGuiKey_Apostrophe:
        return label + "'";
    case ImGuiKey_Comma:
        return label + ",";
    case ImGuiKey_Minus:
        return label + "-";
    case ImGuiKey_Period:
        return label + ".";
    case ImGuiKey_Slash:
        return label + "/";
    case ImGuiKey_Semicolon:
        return label + ";";
    case ImGuiKey_Equal:
        return label + "=";
    case ImGuiKey_LeftBracket:
        return label + "[";
    case ImGuiKey_Backslash:
        return label + "\\";
    case ImGuiKey_RightBracket:
        return label + "]";
    case ImGuiKey_GraveAccent:
        return label + "`";
    default:
        break;
    }
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
        return label + static_cast<char>('A' + (key - ImGuiKey_A));
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
        return label + static_cast<char>('0' + (key - ImGuiKey_0));
    // `F1`-`F12`: the function keys are the design's second spelling of a few actions (`F11` is
    // focus mode, task 43), and an unnamed key would print `?` in the help page and in
    // `doc/ui_keys.md` - a row nobody can read.
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12)
        return label + "F" + std::to_string(1 + static_cast<int>(key - ImGuiKey_F1));
    return label + "?";
}

namespace {

constexpr ImGuiKeyChord ctrl(ImGuiKey key) { return ImGuiMod_Ctrl | key; }

/// A function key (`F1`-`F12`)? Those are the plain keys that are *not* a text field's: they
/// type nothing, so a focused field has no use for them and the plain-key rule would only make
/// `F11` (focus mode, task 43) dead in the one region a user is normally in.
bool isFunctionKey(ImGuiKey key) { return key >= ImGuiKey_F1 && key <= ImGuiKey_F12; }

/// Is `chord` a `Shift` chord that *types* a character (`Shift+/` is `?`, `Shift+1` is `!`)?
///
/// A chord with no modifier at all is the field's by the plain-key rule; this is the second shape
/// that belongs to a focused text field, because the character it produces is what the user sees.
/// A `Ctrl`/`Alt`/`Super` chord is a command in every region and never text, so it keeps firing in
/// the composer (that is how `Ctrl+B` reaches the rail from there). The key list is the
/// punctuation and alphanumerics a layout prints with `Shift`; `Enter`, `Tab` and the arrows are
/// not characters here.
bool isShiftPrintable(ImGuiKeyChord chord) {
    const ImGuiKey mods = static_cast<ImGuiKey>(chord & ImGuiMod_Mask_);
    if (mods != ImGuiMod_Shift)
        return false;
    const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
        return true;
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
        return true;
    switch (key) {
    case ImGuiKey_Space:
    case ImGuiKey_Apostrophe:
    case ImGuiKey_Comma:
    case ImGuiKey_Minus:
    case ImGuiKey_Period:
    case ImGuiKey_Slash:
    case ImGuiKey_Semicolon:
    case ImGuiKey_Equal:
    case ImGuiKey_LeftBracket:
    case ImGuiKey_Backslash:
    case ImGuiKey_RightBracket:
    case ImGuiKey_GraveAccent:
        return true;
    default:
        return false;
    }
}

/// The table. `Scope::anywhere()` = every region; `Focus::X` scopes the row to X.
constexpr Binding kBindings[] = {
    // Quit: "confirm while running" is the dispatcher's two-press policy
    // (tuiRequestQuitConfirmed), so one row covers both states. It is repeated
    // for `Focus::Overlay` because an overlay is a keyboard trap (keymap.h) and
    // quitting must still work while the palette is open - the second row is what
    // says so.
    {ctrl(ImGuiKey_Q), Action::Quit, Scope::anywhere()},
    {ctrl(ImGuiKey_Q), Action::Quit, Focus::Overlay},

    // Ctrl+K toggles the palette: it opens from the shell and closes from inside
    // the overlay. Measured as delivered in both backends
    // (plan/key_gate.md §4).
    {ctrl(ImGuiKey_K), Action::Palette, Scope::anywhere()},
    {ctrl(ImGuiKey_K), Action::CloseOverlay, Focus::Overlay},

    // End re-enables follow-the-newest. No modifier, so in a text field it stays
    // the caret command it is there (keymap.h's plain-key rule) - which is also
    // what keeps the press that closes an overlay from jumping the thread.
    // plan/key_gate.md §3b records the delivery (the bytes are terminfo-driven:
    // `\EOF` for xterm, `\E[4~` for linux).
    {ImGuiKey_End, Action::ScrollEnd, Scope::anywhere()},

    // Ctrl+B toggles the rail (task 23, the design's §6.7 row). Measured as
    // delivered in both backends and in the GUI - plan/key_gate.md §4d - and
    // deliberately *anywhere*: the rail is a region, and its toggle has to work
    // from the thread, the composer and the inspector alike.
    {ctrl(ImGuiKey_B), Action::ToggleRail, Scope::anywhere()},

    // Load earlier reveals one page of older groups (task 25, D-BOUNDS).
    // `PgUp` is the scroller's own key and `Ctrl+U` the design's abbreviation;
    // both were measured as delivered in the terminal and in the GUI
    // (plan/key_gate.md §4e). They are plain/`Ctrl` keys, so the keymap's
    // plain-key rule keeps `PgUp` out of a focused composer - where it is not a
    // text command anyway - and `Ctrl+U` stays live there, like every other
    // `Ctrl+<letter>` row.
    {ImGuiKey_PageUp, Action::LoadEarlier, Scope::anywhere()},
    {ctrl(ImGuiKey_U), Action::LoadEarlier, Scope::anywhere()},

    // History (task 27, design §4.6). `↑`/`↓` are the design's keys and
    // `Ctrl+Up`/`Ctrl+Down` the shipped aliases (the v4 composer bound them
    // because the text grid delivers no bare arrow as a key at all - plan/key_gate.md
    // §4b; the terminal's arrow still arrives as `Escape` + `[A`, so the grid gets
    // history from the aliases and the pixel profile from both). They are
    // composer-scoped: history is the field's, and the plain-key rule keeps `↑`/`↓`
    // inert in every other region - where they belong to nav.
    {ImGuiKey_UpArrow, Action::HistoryPrev, Focus::Composer},
    {ImGuiKey_DownArrow, Action::HistoryNext, Focus::Composer},
    {ctrl(ImGuiKey_UpArrow), Action::HistoryPrev, Focus::Composer},
    {ctrl(ImGuiKey_DownArrow), Action::HistoryNext, Focus::Composer},

    // The inspector (task 29, D-INSPECTOR). A measured pair, and both halves of its story:
    //
    //  * `Ctrl+I` toggles the panel and `Ctrl+5`/`Ctrl+6` select its two tabs. All three are
    //    delivered in the pixel profile and *not* in the terminal (plan/key_gate.md §3b:
    //    byte 9 is `Tab`, and `0x1D`/`0x1E` are unmapped), which is why the action is also a
    //    palette row and the panel reads the character-level `[`/`]` itself (§10/§11 option
    //    (a)) - the rows exist for the profile that hears them and neither profile is left
    //    without a path to the surface;
    //  * `Ctrl+1`/`Ctrl+2` are the design's reserved tab numbers and select nothing: the
    //    panel's strip says `not available yet`. They are bound, rather than merely ignored,
    //    because "reserved" has to be visible to a user who presses one. The digits 3 and 4
    //    are reserved as well but are not bound, and the reason is delivery rather than
    //    evidence: byte 3 is `Ctrl+C` (`SIGINT`) and byte 4 is `Ctrl+\` (`SIGQUIT`), so with
    //    `ISIG` on neither can reach the application at all (`plan/key_gate.md` §3b measures
    //    `Ctrl+I` for the same reason). A row for either would be a dead key claiming an
    //    action.
    {ctrl(ImGuiKey_I), Action::ToggleInspector, Scope::anywhere()},
    {ctrl(ImGuiKey_5), Action::InspectorAgents, Scope::anywhere()},
    {ctrl(ImGuiKey_6), Action::InspectorActivity, Scope::anywhere()},
    {ctrl(ImGuiKey_1), Action::InspectorReserved, Scope::anywhere()},
    {ctrl(ImGuiKey_2), Action::InspectorReserved, Scope::anywhere()},

    // The Raw Log (task 33, design §4.9). `Ctrl+Shift+L` is the design's chord and the key gate
    // measures it as delivered **in the pixel profile only**: the terminal turns it into
    // `Ctrl+L` (the Shift is lost, `0x0C`), which this row does not match, so the grid's paths to
    // the same surface are `/rawlog`, the palette's Developer row and `--debug-ui`
    // (plan/key_gate.md §10, the row's own "not bound in the TUI"). The second row exists for the
    // same reason `Ctrl+Q`/`Ctrl+K` have one: an overlay is a keyboard trap (keymap.h), so the
    // chord that opened the Raw Log has to reach it again while the overlay owns the keyboard.
    {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_L, Action::ToggleRawLog, Scope::anywhere()},
    {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_L, Action::ToggleRawLog, Focus::Overlay},
    // The Raw Log is its own focus (`Focus::RawLog`), and it is a keyboard trap like an overlay -
    // so the four chords that must keep working inside it are repeated for that region: quit, the
    // palette (which opens above it), the toggle, and the follower's catch-up. `End` is the one
    // row that differs from the palette's set: `S9` in `./test_palette` pins that `End` must *not*
    // move the thread behind an open palette, while D-FOLLOW makes it the catch-up chord for a
    // stream - hence two regions, not one wider one.
    {ctrl(ImGuiKey_Q), Action::Quit, Focus::RawLog},
    {ctrl(ImGuiKey_K), Action::Palette, Focus::RawLog},
    {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_L, Action::ToggleRawLog, Focus::RawLog},
    {ImGuiKey_End, Action::ScrollEnd, Focus::RawLog},

    // The settings sheet (task 34) is a third trap, under the palette and above the Raw Log. Its
    // own keys are the palette's proven set plus the row keys the view reads itself (`Tab`,
    // `Enter`,
    // `Ctrl+P`/`Ctrl+N`, `/`), so only two chords need a row here - and both are already measured:
    // quit (`Ctrl+Q`, §4a) and the palette (`Ctrl+K`, §4), which opens *above* the sheet instead of
    // closing it (D-OVERLAY's z-order: palette > settings > Raw Log > inspector). A third row for
    // the sheet's own toggle would need a chord the gate has not measured, and the sheet is opened
    // by its visible control (the top bar's `Settings` entry) plus the palette's row (task 35).
    {ctrl(ImGuiKey_Q), Action::Quit, Focus::Settings},
    {ctrl(ImGuiKey_K), Action::Palette, Focus::Settings},

    // The help page (task 42). `?` is `Shift+/` in the pixel profile and the typed character in
    // the grid (plan/key_gate.md §3b/§10: a printable key never arrives as a key event there), so
    // the chord is the pixel profile's row and `views/shell.cpp` reads the character for the grid -
    // one action, one row, a second measured path, the shape `Ctrl+I` already follows.
    //
    // It is deliberately *not* repeated for `Focus::Help`: with the page up the page's own search
    // field may hold the keyboard, where `Shift+/` has to stay the `?` the user is typing - that
    // is the same rule the composer gets from `resolveBinding()`'s text-field test, and the
    // anywhere row cannot fire behind the page anyway (a modal surface is a keyboard trap).
    {ImGuiMod_Shift | ImGuiKey_Slash, Action::Help, Scope::anywhere()},
    // Quit must keep working while the page owns the keyboard, like every other trap's row.
    {ctrl(ImGuiKey_Q), Action::Quit, Focus::Help},

    // Focus mode (task 43, FR-28). `F11` is the design's key: a *function* key, so no modifier -
    // and the one plain-key shape the text-field rule lets through, because it types nothing
    // (keymap.h). `Ctrl+Shift+M` is the design's second spelling: delivered in the pixel profile
    // and *not* in the terminal, which sends `Enter` for it (plan/key_gate.md §3b) - so the grid's
    // paths are the palette's row and `Esc` to leave, the same "one action, one row, a second
    // measured path" shape `Ctrl+I` follows. No `Focus::Composer` row is needed: neither chord is
    // text, so both fire from the field.
    {ImGuiKey_F11, Action::ToggleFocusMode, Scope::anywhere()},
    {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_M, Action::ToggleFocusMode, Scope::anywhere()},
};

constexpr Keymap kDefaultKeymap{kBindings, sizeof(kBindings) / sizeof(kBindings[0])};

/// The action registry (see `ActionReach`): every action of the enum with the entry point it
/// is reached from. One row per action, in declaration order - the palette's registry test
/// walks it, so an action that arrives without a decision fails the build's test rather than
/// shipping as a dead key or an action nobody can ask for by name.
///
/// The "not a palette row" answers are deliberate, not laziness:
///   * `Palette` itself is the surface (a row that opens the palette the user is looking at
///     would be a row that does nothing), `ScrollEnd` is a scroll gesture, `HistoryPrev`/
///     `HistoryNext` belong to the field that owns the caret, and `InspectorReserved` is a
///     digit that must keep saying `not available yet` (D-INSPECTOR);
///   * `CloseOverlay`, `ClearInput` and `Blur` are `Esc` steps: `resolveEscape` decides them,
///     and the palette's `Esc` is that same first step.
constexpr ActionInfo kActions[] = {
    {Action::Quit, "Quit", ActionReach::Palette, "Quit the UI (asks once while a run is active)"},
    {Action::Palette, "Palette", ActionReach::Chord, "Open the command palette"},
    {Action::CloseOverlay, "CloseOverlay", ActionReach::EscapeChain, "Close the topmost overlay"},
    {Action::ScrollEnd, "ScrollEnd", ActionReach::Chord, "Follow the newest content again"},
    // `CancelRun` is `Esc` step 2 *and* the palette's `Stop run` row. The row is the answer
    // that matters here: `Esc` is the accelerator, the row is where a user finds it.
    {Action::CancelRun, "CancelRun", ActionReach::Palette,
     "Stop the running agent (the partial output is kept)"},
    {Action::ClearInput, "ClearInput", ActionReach::EscapeChain, "Empty the composer's draft"},
    {Action::Blur, "Blur", ActionReach::EscapeChain, "Release the keyboard"},
    {Action::ToggleRail, "ToggleRail", ActionReach::Palette, "Show or hide the session list"},
    {Action::LoadEarlier, "LoadEarlier", ActionReach::Palette,
     "Show the groups older than the window"},
    {Action::HistoryPrev, "HistoryPrev", ActionReach::Chord, "Recall the previous query"},
    {Action::HistoryNext, "HistoryNext", ActionReach::Chord, "Recall the next query"},
    {Action::ToggleInspector, "ToggleInspector", ActionReach::Palette,
     "Show or hide the inspector panel"},
    {Action::InspectorAgents, "InspectorAgents", ActionReach::Palette,
     "Select the inspector's Agents tab"},
    {Action::InspectorActivity, "InspectorActivity", ActionReach::Palette,
     "Select the inspector's Activity tab"},
    {Action::InspectorReserved, "InspectorReserved", ActionReach::Chord,
     "A reserved tab number: selects nothing and says so"},
    {Action::ToggleRawLog, "ToggleRawLog", ActionReach::Palette,
     "Show or hide the Raw Log (the developer stream)"},
    {Action::Help, "Help", ActionReach::Palette, "Open this page"},
    // Focus mode (task 43). The palette row is the grid's way in (`F11` never arrives there and
    // `Ctrl+Shift+M` is `Enter`), and it is the row the registry test walks.
    {Action::ToggleFocusMode, "ToggleFocusMode", ActionReach::Palette,
     "Give the thread the whole window (Esc leaves)"},
    {Action::ExitFocusMode, "ExitFocusMode", ActionReach::EscapeChain, "Leave focus mode"},
};

constexpr ActionList kActionList{kActions, sizeof(kActions) / sizeof(kActions[0])};
} // namespace

const Keymap& defaultKeymap() { return kDefaultKeymap; }

const ActionList& allActions() { return kActionList; }

const ActionInfo* actionInfo(Action action) {
    for (const ActionInfo& info : kActionList) {
        if (info.action == action)
            return &info;
    }
    return nullptr;
}

const char* actionName(Action action) {
    if (const ActionInfo* info = actionInfo(action))
        return info->name;
    // The two sentinels are not rows of the registry: they name "no action" and the end of
    // the list, which is what the dispatcher's switch and the tests need to print.
    return action == Action::None ? "None" : "Count";
}

const Binding* resolveBinding(const Keymap& keymap, ImGuiKeyChord chord, Focus focus) {
    const Binding* anywhere = nullptr;
    for (const Binding& binding : keymap) {
        if (binding.chord != chord)
            continue;
        if (binding.when.isAnywhere()) {
            anywhere = &binding;
            continue;
        }
        if (binding.when.matches(focus))
            return &binding; // the exact region wins
    }
    // A chord that *types* a character belongs to a focused text field: no *anywhere* row fires
    // there, so typing `m` into the composer can never reach a rail binding. That is the plain-key
    // rule, and `Shift` plus a printable key is its second shape - `Shift+/` is `?`, and a help
    // page that opened while the user typed a question mark into the composer would be the same
    // defect as a rail row firing on an `m`. The test sits after the exact-region loop on purpose:
    // a row scoped to `Focus::Composer` is the field's own key (`Up`/`Down` history) and can only
    // fire while the field owns the keyboard, which is exactly the region it names. A *function*
    // key is the rule's third shape and the one exception it makes: `F11` types nothing, so a
    // field has no use for it and focus mode has to be reachable while the user is typing.
    const bool plain = (chord & ImGuiMod_Mask_) == 0;
    const ImGuiKey plainKey = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    if (((plain && !isFunctionKey(plainKey)) || isShiftPrintable(chord)) &&
        focus == Focus::Composer)
        return nullptr;
    // Every modal surface is a keyboard trap: an anywhere row does not fire behind it. The Raw
    // Log, the settings sheet and the help page are their own focuses (`Focus::RawLog` /
    // `Focus::Settings` / `Focus::Help`) and not `Overlay`, because they have different own-key
    // sets (`End` catches up in the Raw Log; the palette's S9 rule is that `End` must not move the
    // thread behind *it*; the sheet keeps `Ctrl+K` opening the palette above it; the help page
    // keeps quit and nothing else), but none of them lets an anywhere row act on the regions
    // below.
    if (focus == Focus::Overlay || focus == Focus::RawLog || focus == Focus::Settings ||
        focus == Focus::Help)
        return nullptr;
    return anywhere;
}

Action resolve(const Keymap& keymap, ImGuiKeyChord chord, Focus focus) {
    const Binding* binding = resolveBinding(keymap, chord, focus);
    return binding != nullptr ? binding->action : Action::None;
}

Action resolveEscape(const EscapeContext& context) {
    if (context.overlayOpen)
        return Action::CloseOverlay;
    // Focus mode sits under the overlay step and above the composer's three (D-ESC): a surface
    // that happens to be open is above the shell and closes first, while a press meant to leave
    // the mode must not also clear the draft or stop the run (task 43).
    if (context.focusMode)
        return Action::ExitFocusMode;
    if (context.runActive)
        return Action::CancelRun;
    if (context.inputNonEmpty)
        return Action::ClearInput;
    return Action::Blur;
}

bool escapeStopsRun(const EscapeContext& context) {
    return resolveEscape(context) == Action::CancelRun;
}

} // namespace llmfun::tui::model
