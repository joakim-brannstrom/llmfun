/// The shell: the frame's composition root (design §3.3, D-METRICS, D-OVERLAY).
///
/// One frame is one layout pass and one draw pass over the six tiled regions:
///
///     const views::ShellFrame frame = views::renderShell(state, m);
///     ... keys ...
///     views::renderOverlays(state, frame);
///
/// `ui::computeShell()` owns every rectangle, so no view positions itself; this
/// file is where the rectangles are handed to the region views and where the
/// `LayoutPrefs` of a frame is derived from the user's preferences and the state
/// the frame owns (which overlays are up). It is also the only place that knows
/// both the model and the v4 store - `views/shell.cpp` is the composition root,
/// which is why the region views below it stay free of the aggregate.
///
/// The overlay layer is drawn by the frame layer *after* the keymap ran, so a
/// chord that opens an overlay draws it in the same frame, and it reuses the
/// rectangles of the pass above instead of laying out a second time.
#pragma once

#include "imgui/imgui.h"

#include "model/ui_state.h"
#include "ui/layout.h"
#include "views/nowbar.h"

namespace llmfun::tui {
struct TuiState;
} // namespace llmfun::tui

namespace llmfun::tui::views {

/// The geometry of one frame, handed back so the overlay layer draws on the
/// rectangles the regions were drawn on.
///
/// `prefs` and `display` are kept because the keymap runs between the two calls: a
/// chord that opens an overlay arrives *after* the pass that would have given it a
/// box, and the overlay layer re-lays out from these values rather than from the
/// live state - which is what makes its box a function of the tiling that is
/// actually on screen. `display` is a value and not a call because a view never
/// reads the display size itself.
struct ShellFrame {
    ui::ShellLayout layout;
    /// The preferences `layout` was computed from - the tiling the shell painted.
    ui::LayoutPrefs prefs;
    ImVec2 display = ImVec2(0.0f, 0.0f);
    /// The inspector's open preference was `-1 auto` this frame, so the width rule (below) owns
    /// its shape. Kept with the painted preferences because the overlay layer re-lays out from
    /// them: a rule that was applied once must be applied by the second pass too, or the
    /// palette's re-layout would open a panel the shell had closed.
    bool inspectorAuto = true;
    /// The rail's open preference was `-1 auto` this frame - the same rule, one region over.
    ///
    /// Below the rail's own breakpoint the shape it would get is a *panel*, which covers the
    /// thread and the composer (design §3.3's "single column, both overlays"). A panel nobody
    /// asked for is not what "open" means on a narrow display - the inspector's `-1 auto` rule
    /// says the same thing and has said it since task 29 - so an automatic rail closes there and
    /// only an explicit 1 opens the panel. Carried on the frame for the overlay layer's re-layout,
    /// like `inspectorAuto`.
    bool railAuto = true;
    /// The now bar's input exactly as this frame drew it (`views/shell.cpp` derives it once
    /// and hands it to `renderNowBar`), so a harness - and a golden - can measure the row
    /// that is on screen instead of rebuilding a fixture that may drift from the derivation.
    NowBarInput nowBar;
};

/// The `LayoutPrefs` of this frame: the user's shell preferences (`ui.prefs`)
/// plus the state that changes the tiling (an open Raw Log) and the overlays that
/// are up (the palette, the settings sheet). One derivation, so a preference cannot
/// mean two things in two places.
///
/// `composerH` is the composer's own answer from the previous frame
/// (`views::ComposerState::height`, task 27): auto-grow is the one region whose size
/// the view measures, and the layout reserves it so the thread can never be drawn
/// under the field. 0 means "as tall as it is when empty" (`LayoutPrefs`' contract),
/// which is also the first frame's answer.
///
/// `settingsOpen` is the settings sheet's flag (`views::SettingsState::open`, task 34) and
/// `helpOpen` the help page's (`views::HelpState::open`, task 42). Neither changes a rectangle of
/// the tiling - like the palette, both are overlays with their own invariant - but they decide
/// that their box exists and where it sits in `ShellLayout::top`, and therefore what `Esc` closes
/// first (D-OVERLAY).
ui::LayoutPrefs layoutPrefsFor(const model::UiState& ui, bool paletteOpen, float composerH = 0.0f,
                               bool settingsOpen = false, bool helpOpen = false);

/// "Show me the search" - the one answer from the rail that has to change the
/// tiling, because the collapsed rail has no field to focus.
///
/// The request is a *width*, not an open flag: `LayoutPrefs::railOpen` is a bool and
/// the collapsed shape comes from the width breakpoint, so nothing the rail can write
/// to `railOpen` widens a 7-cell column - only a width request can (`UiState::railW`,
/// whose "0" means "the profile's own width"). `fullW` is that width, in the profile's
/// units (`ui::metricsFor().railW`); the caller passes it so this function stays a rule
/// and not a profile lookup.
///
/// Session-scoped on purpose: `ui.conf` has no rail-width key (design §5.3), so the
/// widened rail lives as long as the process - the same lifetime a dragged width has.
/// `closeRailSearch()` is the other end of it.
void openRailForSearch(model::UiState& ui, float fullW);

/// The search is over: hand the rail back the width it had before the request, so the
/// collapsed column returns on a display that chose it. Called by the frame layer when
/// the view reports the mode ended (an empty query and a field that no longer holds the
/// keyboard) and when the rail is closed under it (`Ctrl+B`) - a width that was only
/// ever borrowed for the search must not outlive it, or a narrow display would keep the
/// widened column until the process restarted.
void closeRailSearch(model::UiState& ui);

/// The one layout pass: `ui::computeShell()` plus the two `-1 auto` rules (the inspector's and
/// the rail's), which live here and nowhere else so the shell and the overlay layer cannot
/// disagree about them.
///
/// "Auto" - and only auto - is: docked on a display wide enough for a column, and closed where
/// the width breakpoint would turn the region into a panel - a panel the user never asked for
/// covers the thread *and* the composer, which is not what "open" means on a narrow screen. An
/// *explicit* open (`Ctrl+I`/`Ctrl+B`, the palette's rows, a saved `InspectorOpen=1`/
/// `RailOpen=1`) keeps the panel shape the engine answered, because that is what the user asked
/// for and `Esc` closes it again (D-ESC, D-OVERLAY). The layout engine has already answered which
/// shape this display gives the region, so the rule asks the engine instead of re-deriving a
/// breakpoint here (D-METRICS: one owner for the geometry). `prefs` is not modified, so the same
/// arguments always answer the same layout.
///
/// `inspectorAuto`/`railAuto` are the `-1` states, which `LayoutPrefs` cannot express (its two
/// open flags are resolved bools). The defaults are the first-run preferences, so a caller that
/// only fills the design's four fields keeps the documented behaviour; a caller that means an
/// *explicit* open of a region which this display turns into a panel passes `false` for it.
ui::ShellLayout shellLayoutFor(const ui::LayoutPrefs& prefs, ImVec2 display,
                               bool inspectorAuto = true, bool railAuto = true);

/// The layout the overlay layer draws on: the tiling the shell *painted*
/// (`frame.prefs`, not the live preferences) with the overlay flags as they are now.
///
/// This is what keeps the overlay layer on the tiling that is on screen. Only an
/// overlay flag may change between the shell and the overlay layer, because only an
/// overlay flag leaves that tiling alone; everything else in `LayoutPrefs` moves
/// rectangles the shell has already drawn this frame. Taking the painted
/// preferences means such a change costs one frame of the *old* tiling (which is
/// what the screen shows) instead of putting an overlay inside a thread rect that is
/// 30 % shorter than the one on screen. A key that must re-tile in the same frame
/// has to be dispatched before the shell; today no key writes `ui.prefs.*` or
/// `ui.rawLogOpen`.
ui::ShellLayout overlayLayoutFor(const ShellFrame& frame, bool paletteOpen,
                                 bool settingsOpen = false, bool helpOpen = false);

/// Lay out and draw the six tiled regions, and return the layout they used.
///
/// `display` is the frame's display size in the profile's own units (cells on the
/// text grid, pixels in the GUI). It is a *parameter* on purpose: the display
/// belongs to the frame layer, and a view that reads it itself could lay itself
/// out against a size the layout engine never saw.
ShellFrame renderShell(TuiState& state, ImVec2 display);

/// The overlay layer, drawn after the shell and never part of its tiling
/// (D-OVERLAY). The z-order is `ui::ShellLayout::top`'s, and `Esc` closes the
/// topmost surface first (D-ESC).
///
/// `frame` is an in/out parameter: the keymap runs between the shell and this
/// call, so an overlay it just opened has no rectangle yet. The re-layout is one
/// pure call on the frame an overlay opens, and none on any other frame; it uses
/// `overlayLayoutFor()`, i.e. the tiling the shell painted - see there for why.
void renderOverlays(TuiState& state, ShellFrame& frame);

// ---------------------------------------------------------------------------
// The Esc chain's overlay step, and the `ui.conf` lifecycle (tasks 29/34/42)
// ---------------------------------------------------------------------------

/// Does `Esc`'s first step apply - is there an overlay to close (D-ESC, D-OVERLAY)?
///
/// Five surfaces answer yes and they are read in the layout's own z-order: the help page (task
/// 42 - the top of the stack), the palette (a modal overlay - it takes the keyboard while it is
/// up), the settings sheet (modal as well, task 34),
/// the inspector **panel** and the rail **panel**, which both count only while they *float*: the
/// docked column is one of the six regions and `Esc` there belongs to the run and the composer.
/// The predicate is one function because the frame layer's `Esc` step, the now bar's `Esc stop`
/// hint, the composer's own chain and every view that spends a local `Esc` must agree on it - a
/// hint or a field that outlived its step is exactly the defect this prevents.
/// `TuiState::overlayOwnsKeyboard()` answers the *other* question (does a modal surface take the
/// keyboard), which is why the two are not one flag.
///
/// The Raw Log's full-screen overlay is the third clause (task 33); its *drawer* shape is part of
/// the tiling and is deliberately not closeable this way. The rail panel is the last clause: it is
/// the bottom of the overlay stack (`ui::OverlayLayer`) and the only surface that can cover the
/// composer while it is up, so it must be dismissible with the same press as the others - the
/// layout has carried `OverlayLayer::Rail` since task 17 for exactly this reason.
bool escStepClosesOverlay(const TuiState& state);

/// Perform that step: close the topmost surface `escStepClosesOverlay()` names, and answer
/// which one it was (`OverlayLayer::None` when there was nothing to close).
///
/// The help page closes first (task 42: it is the top of the stack), then the palette through its
/// own call (`closePalette()`, which hands the keyboard back); the settings sheet closes through
/// `closeSettings()` plus the same keyboard hand-back, and it does
/// not write a preference (the sheet's open state is a mode of this process, never a `ui.conf`
/// key); the inspector panel closes as the user's explicit choice, so it writes `0` into
/// `ShellPrefs::inspectorOpen` and never back to `-1 auto` - a panel the user closed must not
/// re-open on the next resize; the rail panel closes the same way, into `ShellPrefs::railOpen`.
/// Every one of them is one step of one press: `Esc` never closes two.
ui::OverlayLayer closeTopOverlay(TuiState& state);

/// True while the rail holds an `Esc` step of its own: an open rename box, an open row menu,
/// or the search field that owns the keyboard (its own `Esc` clears the query).
///
/// The rail is drawn *last* (its panel shape has to be a window created after the thread's
/// child, views/rail.cpp), so the composer cannot learn from `rail.escapeConsumed` that the
/// press was already spent - it draws earlier in the same frame. This predicate is the answer
/// the composer asks before it reads the key, the same way `escStepClosesOverlay()` answers for
/// the overlays: one press, one step (D-ESC).
bool railOwnsEscape(const TuiState& state);

/// A reserved inspector digit arrived (`Ctrl+1`/`Ctrl+2`, D-INSPECTOR): nothing is selected
/// and the strip says `not available yet`.
///
/// The notice is drawn *in the strip*, so a press with the panel closed would say nothing at
/// all - exactly the "I pressed a key and nothing happened" the notice exists to avoid. A
/// reserved press therefore opens the panel like a tab press does (an explicit `1`), and the
/// frame's next pass draws the notice. Cleared by the next tab selection or toggle.
void pressReservedDigit(TuiState& state);

/// Read `ui.conf` once for this profile and keep it up to date (D-UI-CONF).
///
/// The first call reads the profile (`model::loadUiConf`, before any view can change a
/// preference) and sets `loaded`; every call after it offers the debounced write: the profile's
/// text is rebuilt (a hundred-odd bytes, no I/O) and compared, so a settled preference costs one
/// string build and a changed one reaches the disk once the user stops
/// (`kUiConfIdleSeconds`), never per frame. An empty `iniFilename` is "no profile" and does
/// nothing - which is what every harness that does not set one gets.
///
/// `loaded` is the caller's latch (a `TuiState` member) so the read happens on the frame the
/// frame layer first runs and not on every one.
void uiConfFrame(model::UiState& ui, bool& loaded, const std::string& iniFilename,
                 double nowSeconds);

/// The quit path's save: the debounce window may not have elapsed, and a user who quits two
/// seconds after a change must still find it next time. It carries a change and nothing else -
/// a quit that changed no preference leaves the file's bytes alone (`model::saveUiConf`).
void uiConfQuit(const model::UiState& ui, const std::string& iniFilename);

} // namespace llmfun::tui::views
