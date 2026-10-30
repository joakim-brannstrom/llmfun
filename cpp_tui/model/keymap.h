/// Keymap registry: one focus-aware binding table for the whole UI (D-KEYMAP).
///
/// Every chord the UI acts on is a row here, and the frame code is the only
/// place that asks ImGui whether a chord is down: a view never reads
/// `IsKeyPressed` for a global binding. Splitting the table from the ImGui read
/// is what makes the resolution testable - `resolve()` is a pure function of
/// `(chord, focus)`, and the terminal/GUI difference (which chords a backend
/// actually delivers, plan/key_gate.md) never leaks into it.
///
/// Focus. `Focus` names the region that owns the keyboard. A binding says where
/// it is live with a `Scope`: `Scope::anywhere()` is the design's *Anywhere*,
/// and `Scope(Focus::X)` scopes the row to exactly one region. `Focus::None` is
/// not "anywhere" - it means no region owns the keyboard (the shell has it), so
/// `Scope(Focus::None)` is live only while nothing is focused. Three rules
/// follow from that:
///
///  * an exact-region row beats an anywhere row for the same chord, so one chord
///    can mean two things in two regions (`Ctrl+K` opens the palette from the
///    shell and closes it from inside the palette);
///  * an overlay is a keyboard trap: while it owns the keyboard an anywhere row
///    does not fire at all, so a chord that must work inside an overlay either
///    has its own `Focus::Overlay` row (`Ctrl+Q`) or belongs to the overlay
///    itself (`Esc`, the palette's list keys);
///  * a chord that *types* a character - one with no modifier, or `Shift` plus a
///    printable key, which is how `?` is spelled on a US layout - never fires on an
///    *anywhere* row while a text field has the keyboard, so typing `m` or `?` into the
///    composer cannot reach a rail binding or open the help page (task 42). A row scoped to
///    `Focus::Composer` is the field's own key and is the
///    one exception (`↑`/`↓` history, task 27): it can only fire while the field
///    owns the keyboard, and it is what the composer's own resolve asks for. A *function*
///    key (`F1`-`F12`) is the rule's other exception: it types nothing and means nothing to a
///    field, so a plain function-key row fires in the composer too - which is what makes `F11`
///    a focus-mode toggle a user can press while typing (task 43).
///
/// Uniqueness is per `(chord, scope)` pair - the same chord may be bound twice
/// in two regions, but never twice in one. Overlaps are intentional when one row
/// is `Scope::anywhere()` and the other names a region: the exact row wins while
/// that region owns the keyboard, so a `Scope(Focus::None)` row shadows an
/// anywhere row only in the "nothing focused" state (and is not a duplicate).
/// `test_keymap` asserts the uniqueness rule, and that every `Action` in this
/// file is reachable through the table or through `resolveEscape`.
///
/// `Esc` is deliberately not a row: its action depends on the run and input
/// state, not on the focus alone (D-ESC). It has its own resolver,
/// `resolveEscape`, so the documented order - overlay, focus mode, cancel, clear,
/// blur - exists exactly once.
///
/// The `Action`/`Focus` lists are append-only: the tasks that add a surface
/// (rail, inspector, Raw Log, thread keys) append their own actions and rows,
/// and never renumber an existing value.
#pragma once

#include <cstddef>
#include <string>

#include "imgui/imgui.h"

namespace llmfun::tui::model {

/// What a chord asks the UI to do. `None` is the "no binding" answer, `Count`
/// the sentinel the reachability test iterates up to - neither is a row.
enum class Action {
    None,
    /// Quit the UI. While a run is active the first press asks for confirmation
    /// (the hint line says how), the second one quits (D-KEYMAP: "Quit (confirm
    /// while running)"; the design's separate `QuitConfirm` step is dispatch
    /// policy here, not a second action - an action the table cannot reach is
    /// dead code).
    Quit,
    /// Open the `Ctrl+K` palette.
    Palette,
    /// Close the topmost overlay (D-OVERLAY).
    CloseOverlay,
    /// Re-enable follow-the-newest in the thread.
    ScrollEnd,
    /// Esc, step 2: stop the running agent (partial output kept).
    CancelRun,
    /// Esc, step 3: empty the composer's input.
    ClearInput,
    /// Esc, step 4: release the keyboard.
    Blur,
    /// Show/hide the session rail (`Ctrl+B`, task 23). It flips
    /// `ShellPrefs::railOpen` between "closed" (0) and "open" (1) and never
    /// writes `-1 auto`: the user's press is an explicit choice, and `auto` is
    /// only the state before the first press.
    ToggleRail,
    /// Reveal one page of older groups in the thread (D-BOUNDS, task 25): the same
    /// page the focusable `Load earlier` row reveals, so one press is one page
    /// whatever put the thread on screen.
    LoadEarlier,
    /// Recall the previous (older) composer query: `↑` on the draft's first line,
    /// or its `Ctrl+Up` alias (design §4.6, task 27). Performed by the composer,
    /// which is the only region that knows where the caret is.
    HistoryPrev,
    /// Recall the next (newer) query, or hand the draft back past the newest entry:
    /// `↓` on the draft's last line, or its `Ctrl+Down` alias.
    HistoryNext,
    /// Show or hide the inspector panel (`Ctrl+I`, task 29, D-INSPECTOR). The press is an
    /// explicit choice, so it writes 0 or 1 into `ShellPrefs::inspectorOpen` and never back
    /// to `-1 auto` - the rule `Ctrl+B` follows for the rail.
    ///
    /// The pixel profile delivers the chord; the terminal does not (plan/key_gate.md §3b:
    /// `Ctrl+I` arrives as `Tab`), which is why the same action is also a palette row - the
    /// grid's path to it. A row that resolved to an action nothing could reach would be the
    /// dead key the registry exists to prevent; this one is reachable in both profiles.
    ToggleInspector,
    /// Select the inspector's Agents tab (`Ctrl+5`). Delivered in the pixel profile, swallowed
    /// by the terminal (§3); the grid's spelling of the pair is the character-level `[`/`]`
    /// the panel reads itself (§10/§11 option (a)).
    InspectorAgents,
    /// Select the inspector's Activity tab (`Ctrl+6`).
    InspectorActivity,
    /// A reserved inspector digit (`Ctrl+1`/`Ctrl+2`, D-INSPECTOR). It selects *nothing* and
    /// the panel's strip says `not available yet` - the design's tab numbers 1-4 stay reserved
    /// (D-16), and a reserved key must not silently move a shipped binding.
    InspectorReserved,
    /// Show/hide the Raw Log surface (task 33, design §4.9). `Ctrl+Shift+L` is delivered in the
    /// pixel profile only (plan/key_gate.md §10: the terminal loses the Shift and delivers
    /// `Ctrl+L`), so the grid's paths are `/rawlog`, the palette's Developer row and
    /// `--debug-ui` - the same "one action, one row, a second measured path" shape the
    /// inspector's `Ctrl+I` follows.
    ToggleRawLog,
    /// Open the help page (task 42, design FR-27). `?` is the chord and, in the terminal, the
    /// character (`plan/key_gate.md` §10/§11 option (a)): `Shift+/` is delivered as a key in the
    /// pixel profile and as the typed character in the grid, and neither path fires while a text
    /// field owns the keyboard. The page is not a palette row on purpose - it is a *page* about
    /// the interface, and the palette is about doing things.
    Help,
    /// Show or hide focus mode (task 43, design FR-28): the top bar, the rail and the inspector
    /// are hidden and the transcript is drawn in a centred column, while the composer and the
    /// now bar stay (NFR-16: the run's state stays visible).
    ///
    /// Two chords, and neither is the grid's: `F11` is a function key (a plain key, but one that
    /// types nothing, so the text-field rule does not apply to it) and the terminal delivers no
    /// function keys at all, while `Ctrl+Shift+M` arrives as `Enter` there (plan/key_gate.md §3b).
    /// The palette's row is therefore the grid's way in, and `Esc` - see `ExitFocusMode` - is the
    /// way out of a mode whose chrome, the rail's and the inspector's included, is hidden.
    ToggleFocusMode,
    /// `Esc` leaves focus mode (task 43). A chain step like `CloseOverlay`, and its own action so
    /// that "one press, one step" stays a property of `resolveEscape` rather than of a view: an
    /// `Esc` in focus mode must not also clear the draft or cancel the run.
    ExitFocusMode,
    Count
};

/// The region that owns the keyboard. `None` = no region owns it (the shell has
/// it). A binding says where it is live with `Scope`, not with this enum - the
/// two used to share a value, which made "anywhere" and "nothing focused" look
/// the same in the table.
///
/// `RawLog`, `Settings` and `Help` are separate regions rather than one `Overlay` because a modal
/// surface's own key set differs: the Raw Log keeps `End` (its follower's catch-up) and the
/// palette must not receive it (`./test_palette`'s S9), the settings sheet keeps `Ctrl+K`
/// meaning "open the palette above me" instead of "close the topmost surface" - the palette's
/// own meaning for that chord, which only makes sense in the palette itself - and the help page
/// keeps quit and nothing else (task 42; its `Esc` step belongs to the chain). All of them are
/// keyboard traps for `Scope::anywhere()` rows (see `resolveBinding`).
enum class Focus { None, Composer, Rail, Thread, Overlay, RawLog, Settings, Help, Count };

/// Where a binding is live: everywhere, or in exactly one region.
class Scope {
public:
    /// `Scope::anywhere()` - the design's *Anywhere*: the row fires in every
    /// region (subject to the overlay trap, keymap.cpp).
    static constexpr Scope anywhere() { return Scope{}; }
    /// One region only. Implicit so a table row can read `Focus::Overlay`.
    constexpr Scope(Focus region) : region_(region), anywhere_(false) {}

    constexpr bool matches(Focus focus) const { return anywhere_ || region_ == focus; }
    constexpr bool isAnywhere() const { return anywhere_; }
    constexpr Focus region() const { return region_; }

    friend constexpr bool operator==(const Scope& a, const Scope& b) {
        return a.anywhere_ == b.anywhere_ && (a.anywhere_ || a.region_ == b.region_);
    }
    friend constexpr bool operator!=(const Scope& a, const Scope& b) { return !(a == b); }

private:
    constexpr Scope() = default;
    Focus region_ = Focus::None;
    bool anywhere_ = true;
};

/// One row: the chord, what it does, and where it is live.
struct Binding {
    ImGuiKeyChord chord;
    Action action;
    Scope when;
};

/// The name of an action, for the log the dispatcher writes and for test
/// diagnostics ("keymap: Ctrl+Q -> Quit"). Never a display label.
const char* actionName(Action action);

/// The chord's spelling, in the form the UI log and the two generated tables share: modifiers
/// first, then the key (`Ctrl+Shift+L`, `Shift+/`, `End`). Spelled here rather than with
/// `ImGui::GetKeyChordName()` because that needs a context and a keyboard layout, and a document
/// must not depend on either.
///
/// `doc/ui_keys.md` (`model::uiKeysMarkdown()`) and the help page's key section
/// (`views::helpKeyRows()`, task 42) both print this, so a chord cannot be spelled one way in the
/// document and another on screen. A key this module cannot name prints `?` - which a test would
/// show as a row nobody can read, so a new binding on an unnamed key adds its case here.
std::string chordLabel(ImGuiKeyChord chord);

/// How an action is reached besides the registry itself. The palette's registry test walks
/// `allActions()` and fails on an action whose entry point the build does not honour, which is
/// what D-PALETTE's "every listed action is also a key binding or a visible control" needs in
/// both directions: a row must be performable, and an action the user can ask for by name must
/// be a row.
enum class ActionReach {
    /// The palette lists it: `views::paletteActions()` has a row whose `action` is this one.
    Palette,
    /// A chord only. The palette is deliberately not its home - it is the surface itself, a
    /// scroll gesture, or a key of the field that owns the keyboard.
    Chord,
    /// A step of the `Esc` chain (`resolveEscape`, D-ESC). No row and no binding: the chain is
    /// the entry point, and its order lives in one function.
    EscapeChain,
};

/// One row of the action registry: what the action is called, where it is reached from, and the
/// one-line sentence a reader is shown.
///
/// `name` is the code's name for the action (a log line, a test diagnostic); `summary` is the
/// user-facing sentence the help page prints (task 42) - one string per action, in the same row,
/// so the page cannot describe an action the registry does not have or miss one it does. It is
/// never a substitute for `name`: `./test_ui_keys`' document and the UI log quote `name`.
struct ActionInfo {
    Action action;
    const char* name;
    ActionReach reach;
    const char* summary;
};

/// The registry: one row per `Action` except `None`/`Count`, in declaration order. Static
/// storage like the binding table, so the list cannot be built twice or built differently.
struct ActionList {
    const ActionInfo* rows = nullptr;
    std::size_t count = 0;

    const ActionInfo* begin() const { return rows; }
    const ActionInfo* end() const { return rows + count; }
};

/// The whole registry (keymap.cpp). Appending an `Action` without appending its row here is
/// what `./test_palette` fails on: the entry point has to be decided, not discovered later.
const ActionList& allActions();

/// The registry row for `action`, or nullptr for `None`/`Count`.
const ActionInfo* actionInfo(Action action);

/// A read-only view of a binding table: the table itself is static storage, so
/// the registry stays allocation-free and cannot be built twice.
struct Keymap {
    const Binding* rows = nullptr;
    std::size_t count = 0;

    const Binding* begin() const { return rows; }
    const Binding* end() const { return rows + count; }
};

/// The one table (keymap.cpp). Rows are added by the tasks that add their
/// surface; nothing else may build a `Keymap` for the frame code.
const Keymap& defaultKeymap();

/// Resolve one chord in one region to the row that wins (nullptr when none):
/// exact-region rows beat anywhere rows for the same chord, a plain key never
/// fires while a text field owns the keyboard, and an anywhere row is suppressed
/// while an overlay owns it.
const Binding* resolveBinding(const Keymap& keymap, ImGuiKeyChord chord, Focus focus);

/// The action of `resolveBinding`, or `Action::None`.
Action resolve(const Keymap& keymap, ImGuiKeyChord chord, Focus focus);

/// Resolve the row the pressed chord implies (nullptr when none): `pressed` is
/// asked once per distinct chord of the table (rows may repeat a chord for
/// another region), and the first pressed chord with a resolvable row wins.
/// Kept a template so the dispatcher can hand in `ImGui::IsKeyChordPressed` while
/// a test hands in a plain set - the model never calls ImGui. Returns the row, not
/// just its action, so the dispatcher acts on (and logs) the row that actually
/// won rather than the first one that mentions the chord.
template <typename IsPressed>
const Binding* resolvePressedBinding(const Keymap& keymap, Focus focus, IsPressed pressed) {
    for (std::size_t i = 0; i < keymap.count; ++i) {
        bool seenBefore = false;
        for (std::size_t j = 0; j < i; ++j) {
            if (keymap.rows[j].chord == keymap.rows[i].chord) {
                seenBefore = true;
                break;
            }
        }
        if (seenBefore || !pressed(keymap.rows[i].chord))
            continue;
        if (const Binding* binding = resolveBinding(keymap, keymap.rows[i].chord, focus))
            return binding;
    }
    return nullptr;
}

/// The action of `resolvePressedBinding`, or `Action::None`.
template <typename IsPressed>
Action resolvePressed(const Keymap& keymap, Focus focus, IsPressed pressed) {
    const Binding* binding = resolvePressedBinding(keymap, focus, pressed);
    return binding != nullptr ? binding->action : Action::None;
}

/// The state the `Esc` chain decides on.
///
/// The chain's order lives in `resolveEscape`'s code - overlay, focus mode, cancel, clear, blur -
/// not in this struct's member order, and `focusMode` is appended last so that the three-argument
/// initialisers written before task 43 keep their meaning: a positional list is the wrong place to
/// discover a new step.
struct EscapeContext {
    bool overlayOpen = false;   ///< an overlay owns the keyboard
    bool runActive = false;     ///< a run is active and no cancel was asked for yet
    bool inputNonEmpty = false; ///< the composer holds a draft
    /// Focus mode is on (task 43). It is a *mode*, not a surface, so it sits below the overlay
    /// step in the chain (an open palette is above the shell, and closing it is the press the
    /// user expects) and above the three steps that belong to the composer: an `Esc` that
    /// leaves the mode must not also clear the draft or cancel the run.
    bool focusMode = false;
};

/// Next `Esc` step: overlay, focus mode, cancel, clear, blur - one step per press. Always
/// returns one of the five: a caller performs the steps it owns and leaves the
/// rest to the region that can (the composer's clear must run after its widget,
/// so it is the composer that performs it).
Action resolveEscape(const EscapeContext& context);

/// Would this press perform the *cancel* step?
///
/// The one question the now bar's `Esc stop` hint may ask. It is not `runActive`: the steps above
/// the cancel one can spend the press (an overlay closes, and in focus mode the mode is left - task
/// 43), so a hint that read the run's state directly would advertise a press that stops nothing.
/// A hint exists to name what the next press does, so it asks the resolver.
bool escapeStopsRun(const EscapeContext& context);

} // namespace llmfun::tui::model
