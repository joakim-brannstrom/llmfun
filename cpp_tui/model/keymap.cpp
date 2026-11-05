/// The one binding table (see keymap.h for the contract and the rules).
///
/// What ships here is what the UI can actually do today: every row is a chord that reaches a
/// handler in this build - a binding that resolves to an action nothing handles would be a dead
/// key, which is exactly what the registry exists to prevent. Where a chord is *not* delivered by
/// one profile the action needs a second, measured path: the registry's row is
/// evidence, not a wish.
///
/// Two chords are deliberately not rows:
///
///  * `Ctrl+C` never reaches the UI in the terminal - `ISIG` is on, so the line discipline turns
///    it into `SIGINT` before ncurses reads it - and in the GUI it is the text field's own copy
///    chord. Binding it would either be dead (grid) or break copy (pixel), so
///    "Ctrl+C = Quit" stays the terminal's own spelling of quitting.
///  * `Ctrl+Shift+L` arrives as `Ctrl+L` in the terminal (the shift is lost, `0x0C`), so it
///    cannot be told apart from a plain `Ctrl+L` - the clear-output chord.
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
    {ctrl(ImGuiKey_Q), Action::Quit, Scope::anywhere()},
    {ctrl(ImGuiKey_Q), Action::Quit, Focus::Overlay},
    {ctrl(ImGuiKey_K), Action::Palette, Scope::anywhere()},
    {ctrl(ImGuiKey_K), Action::CloseOverlay, Focus::Overlay},
    {ImGuiKey_End, Action::ScrollEnd, Scope::anywhere()},
    {ctrl(ImGuiKey_B), Action::ToggleRail, Scope::anywhere()},
    {ImGuiKey_PageUp, Action::LoadEarlier, Scope::anywhere()},
    {ctrl(ImGuiKey_U), Action::LoadEarlier, Scope::anywhere()},
    {ImGuiKey_PageUp, Action::HistoryPrev, Focus::Composer},
    {ImGuiKey_PageDown, Action::HistoryNext, Focus::Composer},
    {ctrl(ImGuiKey_O), Action::ToggleInspector, Scope::anywhere()},
    {ctrl(ImGuiKey_5), Action::InspectorAgents, Scope::anywhere()},
    {ctrl(ImGuiKey_6), Action::InspectorActivity, Scope::anywhere()},
    {ctrl(ImGuiKey_1), Action::InspectorReserved, Scope::anywhere()},
    {ctrl(ImGuiKey_2), Action::InspectorReserved, Scope::anywhere()},
    {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_L, Action::ToggleRawLog, Scope::anywhere()},
    {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_L, Action::ToggleRawLog, Focus::Overlay},
    {ctrl(ImGuiKey_Q), Action::Quit, Focus::RawLog},
    {ctrl(ImGuiKey_K), Action::Palette, Focus::RawLog},
    {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_L, Action::ToggleRawLog, Focus::RawLog},
    {ImGuiKey_End, Action::ScrollEnd, Focus::RawLog},
    {ctrl(ImGuiKey_Q), Action::Quit, Focus::Settings},
    {ctrl(ImGuiKey_K), Action::Palette, Focus::Settings},
    {ImGuiMod_Shift | ImGuiKey_Slash, Action::Help, Scope::anywhere()},
    {ctrl(ImGuiKey_Q), Action::Quit, Focus::Help},
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
