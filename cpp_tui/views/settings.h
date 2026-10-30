/// The settings sheet (`views/settings.cpp`): the three settings the MVP changes plus the
/// diagnostics paths (design §4.10, FR-17, task 34).
///
///     Settings                                       ALL  8    Close      the title row
///     Search                                                             the live filter
///     ALL  APPEARANCE  BEHAVIOUR  ADVANCED                               the section strip
///     ───────────────────────────────────────────────                    a hairline
///     APPEARANCE                                                        a section header
///     ▌ Text scale           100 %                                      the selected row's mark
///     BEHAVIOUR
///       Send key             enterNewline
///       Raw log level        off
///     ADVANCED
///       Backend              gui (window)
///       Workbench settings   ui.conf
///       ...
///     Enter breaks the line; the Send button sends                       the selected row's note
///     Esc close  Tab section  ^N/^P move  Enter change  / search         the key hints
///
/// The body is a window on a longer list: when the rows run past its bottom edge, a proportional
/// thumb in the body's right gutter marks the visible share (one rule glyph per covered cell on the
/// grid, a bar in the pixel profile), so a row below the fold - the `Copy system info` action among
/// them - is not a surprise (review M1).
///
/// Units: the geometry fields are in the *profile's* own units (cells on the text grid, pixels in
/// the pixel profile), because the sheet is the same view in both - they are deliberately not
/// `…Cells`/`…Px`, and `Rect.cells` is what states which unit a rectangle is in (design §3.9's
/// suffix rule applies to a local that holds one profile's unit, not to a dual-profile view).
///
/// It is the *only* settings surface (design §4.10, R-11): no hidden config keys are added, and
/// nothing here is a second way to write a file. The two persisted groups keep their owners -
/// the send key and the Raw Log level live in `model::ShellPrefs` (`ui.conf`, through the
/// debounced write the frame already runs), the font scale in `TuiSettings`
/// (`imgui2.conf`, the backend's file) - so the sheet edits the values
/// and the debounce/file path stays exactly where tasks 20/22 put it. A change applies on the
/// *next* frame: the layout, the metrics and every region were computed before this view ran.
///
/// What it does not do:
///   * no theme picker and no density setting (D-13 / design §4.10: Graphite is fixed and the
///     rows compact automatically);
///   * no chord of its own. `Esc` closes it through the frame's `Esc` chain
///     (`views::escStepClosesOverlay()` / `closeTopOverlay()`, D-ESC), and the keys it reads are
///     the palette's proven list set (`Ctrl+P`/`Ctrl+N` for a bare `↑`/`↓`, plan/key_gate.md
///     §4b/§4c, `Enter`, `Tab`, `/` as a character). Opening is a *visible control* - the top
///     bar's `Settings` entry, design §4.2's "gear opens Settings" - and the palette's
///     `open settings` row joins it in task 35; a chord nobody measured would be the dead key
///     D-KEYGATE forbids;
///   * no `tui.h`, no `TuiState`, no v4 type: the task's four inputs (`SettingsState&`, a `Rect`,
///     a `Metrics`, the two preference structs) plus the keyboard flag are everything it reads,
///     which is what lets `./test_settings_sheet` draw it into a cell grid with no application
///     state.
///
/// Layering: `model/settings.h` (the value catalogue), `ui/` and imgui. The diagnostics strings
/// the frame alone can know (the two settings paths, the version words, the run's flags,
/// the display size) arrive pre-formatted on `SettingsState`, so this file never reads the
/// display, the ABI or the aggregate (D-METRICS: a view is a geometry taker).
#pragma once

#include <array>
#include <string>
#include <vector>

// TODO: relative paths should never be used
// Relative, not `"model/settings.h"`: this header travels inside `tui.h`, and several test
// targets include `tui.h` with no `cpp_tui/` on the include path (the inspector header's rule).
#include "../model/settings.h"

// Forward declarations, not includes (the palette header's rule): a translation unit that
// reaches `tui.h` must not need the `cpp_tui` include root just because a view was mentioned.
namespace llmfun::tui::ui {
struct Metrics;
struct Rect;
} // namespace llmfun::tui::ui

namespace llmfun::tui {
/// The GUI's `imgui2.conf` settings (`tui_settings.h`), by reference only - see
/// `renderSettings()`. Forward-declared for the same reason the `ui/` types are.
struct TuiSettings;
/// The frame's aggregate, for `returnKeyboardBelow()` - which writes the frame's own state, so it
/// lives in the composition root and is only *declared* here.
struct TuiState;
} // namespace llmfun::tui

namespace llmfun::tui::views {

// ---------------------------------------------------------------------------
// The sheet's vocabulary
// ---------------------------------------------------------------------------

/// The title, and the search field's placeholder.
inline constexpr char kSettingsTitle[] = "Settings";
inline constexpr char kSettingsSearchHint[] = "Search";

/// The three sections of design §4.10, plus the `ALL` pseudo-section the strip starts on (the
/// search crosses sections, so a filtered list that hid its other sections would be a search
/// that answers half the question).
inline constexpr char kSettingsSectionAll[] = "ALL";
inline constexpr char kSettingsSectionAppearance[] = "APPEARANCE";
inline constexpr char kSettingsSectionBehaviour[] = "BEHAVIOUR";
inline constexpr char kSettingsSectionAdvanced[] = "ADVANCED";
inline constexpr int kSettingsSectionCount = 4;
const char* settingsSectionName(int section);

/// The empty state: a search that hides every row. A blank body reads as a broken sheet, and
/// "no setting matches" is a different fact from "there are no settings".
inline constexpr char kSettingsNoMatches[] = "No setting matches";

/// The key hints on the last row, in the grid's spelling (`^N`/`^P` are the delivered pair -
/// plan/key_gate.md §4c; the GUI shows the long form). One string, drawn whole or dropped whole:
/// a hint that is cut mid-word advertises nothing. `/` is in the list because the search field is
/// the only consumer of plain characters and it can lose the keyboard (a click on a row, a palette
/// closing above the sheet), so the way back to it has to be advertised.
inline constexpr char kSettingsHints[] =
    "Esc close  Tab section  ^N/^P move  Enter change  / search";
inline constexpr char kSettingsHintsGui[] =
    "Esc close  Tab section  Ctrl+N/Ctrl+P move  Enter change  / search";

/// The hint row's second job: the selected row's `note` (why the value is what it is, what a
/// change costs). `kSettingsCopied` is the one-shot answer to the copy action.
inline constexpr char kSettingsCopied[] = "system info copied to the clipboard";

/// The one control the sheet draws: `Esc` closes it in both profiles (the key gate delivers byte
/// 27 everywhere), but a pointer user needs a hit target for the same action, and a modal with no
/// visible way out is the mirror image of "an action hidden behind the mouse".
inline constexpr char kSettingsCloseLabel[] = "Close";

/// The diagnostics that are *not* a preference, and the one action. The label is the row a user
/// looks for; the note says what the value means.
inline constexpr char kSettingsBackendNote[] = "restart required to change the backend";
inline constexpr char kSettingsNoProfile[] = "(no profile)";
inline constexpr char kSettingsCopyLabel[] = "Copy system info";
inline constexpr char kSettingsCopyNote[] =
    "backend, size, profile, generation, flags and versions";
inline constexpr char kSettingsLogLevelOn[] = "all";
inline constexpr char kSettingsLogLevelOff[] = "off";
inline constexpr char kSettingsLogLevelNote[] =
    "the UI log archive (llmfun_ui_log.txt); --debug-ui turns it on";

/// The row ids: a row's stable identity, never its label. The view, the harness and a golden name
/// a row through these, so a wording change cannot break a check - and a row that disappears is a
/// compile error in the test rather than a silently skipped assertion.
inline constexpr char kSettingsRowFontScale[] = "appearance.fontScale";
inline constexpr char kSettingsRowSendKey[] = "behaviour.sendKey";
inline constexpr char kSettingsRowRawLog[] = "behaviour.rawLog";
inline constexpr char kSettingsRowBackend[] = "advanced.backend";
inline constexpr char kSettingsRowUiConf[] = "advanced.uiConf";
inline constexpr char kSettingsRowIniConf[] = "advanced.iniConf";
inline constexpr char kSettingsRowLogLevel[] = "advanced.logLevel";
inline constexpr char kSettingsRowCopyInfo[] = "advanced.copySystemInfo";

// ---------------------------------------------------------------------------
// The rows
// ---------------------------------------------------------------------------

/// What pressing `Enter` on a row does.
enum class SettingsRowKind {
    /// Cycle to the next value the build may offer (`Raw log level`, `Text scale`).
    Choice,
    /// Nothing: the row reports a fact (a path, the backend, the effective send key).
    ReadOnly,
    /// Run the row's one action (`Copy system info`).
    Action,
};

/// One row of the sheet, as the view draws it.
///
/// `id` is the stable identity - never the label - so a test, a golden and (later) a search
/// ranking can name a row while its wording changes. `value` is always the *effective* value:
/// built from the same words `model::uiConfText()` writes, never from what the file said, so a
/// malformed or out-of-range line shows the value the UI is actually running with.
struct SettingsRow {
    const char* id = "";
    const char* section = kSettingsSectionAll;
    std::string label;
    std::string value;
    std::string note;
    SettingsRowKind kind = SettingsRowKind::ReadOnly;
    /// The row can be activated (a `Choice` with a value to move to, or an `Action`).
    bool enabled = true;
};

/// Everything the sheet reads, plus the answers it hands back.
///
/// One writer (the frame thread). The first group is the task's contract; the second is what
/// `views/shell.cpp` derives once per frame while the sheet is up (the facts only the frame
/// knows: the profile's paths, the ABI's versions, the run's flags - and the display size,
/// which a view may never read); the third is what the view wrote and the frame applies in the
/// same frame; the fourth is what a harness and a golden measure.
struct SettingsState {
    // --- the task's contract -----------------------------------------------
    bool open = false;
    /// The search field. Substring, case-insensitive, over the row's label and section (see
    /// `settingsRows()` for why the note is not searched).
    std::array<char, 64> search = {};
    /// 0 = `ALL`, then the three sections in `kSettingsSectionName()` order. `Tab`/`Shift+Tab`
    /// cycle it and a click on a strip entry selects one; the search narrows inside it.
    int section = 0;

    // --- derived by the frame ----------------------------------------------
    /// `ui.conf`, or empty when the state has no profile name (a harness, a state D never
    /// pointed at a file). The sheet prints `kSettingsNoProfile` for an empty one rather than an
    /// empty cell.
    std::string uiConfPath;
    /// `imgui2.conf` - the font scale's and the window's file, which the backends own.
    std::string iniConfPath;
    /// The UI log archive is on (`TuiState::isLogActive`, `--debug-ui`).
    bool logArchive = false;
    /// `tui api 5, ui.conf v1` - the versions the copy action reports, formatted by the frame
    /// because only it can see `TUI_API_VERSION`.
    std::string versions;
    /// The run's flags as `name=0/1` pairs (`offline`, `run`, `stop`, `rawlog`).
    std::string flags;
    /// The display, in the profile's units (cells / px). The layout's box is derived from it, so
    /// a copy that reported anything else would describe a window that is not on screen.
    int displayW = 0;
    int displayH = 0;

    // --- the field's cross-frame state -------------------------------------
    /// The search field's ImGui id carries this sequence, so a programmatic clear (open, close)
    /// makes the widget re-read its buffer instead of its stale edit state - the rail's rule.
    int searchSeq = 0;
    /// A one-shot "focus the field" request (on open, and on `/`). It stays armed until the
    /// field really owns the keyboard: ImGui applies it on the item's next Begin, which can be a
    /// frame later (and is dropped on a frame where the window itself is being focused).
    bool searchFocus = false;
    /// The field owns the keyboard (an observation: the harness reads it, and it is what tells
    /// `takeKeys()` that `Enter` belongs to the field). Set by the draw, so during one frame it is
    /// the *previous* frame's answer when the keys are read - the field is drawn after them. The
    /// `Enter` gate is this flag, `searchFocus` and `fieldReleased`, never ImGui's process-global
    /// `io.WantTextInput`: that one describes *any* field and is a frame stale.
    bool searchFocused = false;
    /// The visible row the keyboard acts on (an index into `rows`).
    int selected = 0;
    /// The first visible row: follows `selected` while the list is longer than the body.
    int firstVisible = 0;
    /// The list owns the keyboard: after a `Tab` or a cursor key the field must let go of it and
    /// must not take it back by itself. ImGui's Tab-nav re-activates the only text field on the
    /// Tab-*release* frame, one frame after a one-shot blur would have been consumed, and a field
    /// that takes the keyboard this way eats the next `Enter` (review round 2, N1). So the release
    /// is a latch: `renderSettings()` clears the field's ImGui active id for as long as it holds,
    /// and only a request the user made - `/`, a click in the field, or opening the sheet - clears
    /// it.
    bool fieldReleased = false;

    // --- what the sheet answers, applied by the frame ----------------------
    /// The `Close` control (the `Esc` step is the frame's, `closeTopOverlay()`).
    bool closeRequested = false;
    /// The font scale was committed: the frame writes `imgui2.conf` once (`saveTuiSettings`),
    /// not per frame - the value itself is already in the `TuiSettings` the view edited.
    bool fontScaleChanged = false;

    // --- observations, for the harness, the golden and the log -------------
    /// The rows this frame drew (bounded: the body's capacity plus one row of overscan) and the
    /// rows that passed the search, so a fixture can assert the bound instead of trusting it.
    std::size_t rowsBuilt = 0;
    std::size_t rowsDrawn = 0;
    /// `copied` was pressed this frame, and until when the hint row says so (ImGui seconds).
    double copiedUntil = 0.0;

    /// The row list of this frame, rebuilt by `renderSettings()` (see `settingsRows()`).
    std::vector<SettingsRow> rows;
};

// ---------------------------------------------------------------------------
// The rows and the sheet
// ---------------------------------------------------------------------------

/// Build the sheet's rows for the current values, section and search - in display order.
///
/// Section headers are rows of their own (`SettingsRowKind::ReadOnly`, an empty `value`) so a
/// filtered list keeps the context of what it is showing; `id` is empty for them, which is what
/// keeps them out of the selection.
///
/// The search is a case-insensitive substring over the label *and* the section name, not over
/// the note: a note is prose, and matching prose makes a one-letter query hit everything (the
/// palette's reasoning for its own descriptions). An empty or blanks-only query matches every
/// row (the rail's rule).
std::vector<SettingsRow> settingsRows(const SettingsState& state, const model::ShellPrefs& prefs,
                                      const TuiSettings& settings, const ui::Metrics& m);

/// Draw the sheet inside `region` - the box `ui::computeShell()` answered
/// (`ShellLayout::settingsOverlay`) - and apply the keys it owns.
///
/// `prefs` and `settings` are edited *in place*: they are the one owner of each value
/// (`model::ShellPrefs` in `ui.conf`, `TuiSettings` in `imgui2.conf`), so a change is a change
/// to the thing the rest of the frame reads, and the frame's own write paths carry it to disk.
/// Nothing is written by this file.
///
/// The sheet opens its own window (the palette's pattern): the rectangle comes from the layout
/// engine, so the view still positions nothing, and the window is what makes an opaque surface
/// out of a cell renderer - a draw-list fill would recolour the cells but keep the thread's
/// glyphs (the task-33 lesson).
///
/// `ownsKeyboard` is false while a surface above this one is up (the palette): the sheet still
/// draws, but it takes neither the focus nor the plain keys - only the topmost overlay may
/// (D-OVERLAY). The frame passes `layout.top == OverlayLayer::Settings`; a standalone harness
/// keeps the default.
void renderSettings(SettingsState& state, const ui::Rect& region, const ui::Metrics& m,
                    model::ShellPrefs& prefs, TuiSettings& settings, bool ownsKeyboard = true);

/// The text the copy action puts on the clipboard: `backend`, `display`, `profile`,
/// `generation`, `flags` and `versions`, one `key: value` line each.
///
/// Pure, and deliberately free of anything a session put in the transcript: the six documented
/// fields are the whole payload (verification: "contains no message text"). Exposed so the
/// harness can prove the payload without a clipboard; `renderSettings()` calls the same
/// function for the real one.
std::string systemInfoText(const SettingsState& state, const ui::Metrics& m);

/// Close the sheet: everything the *next* open must not inherit (the search, the cursor, the
/// notice). The frame's `closeTopOverlay()` calls it, and so does the `Close` control.
void closeSettings(SettingsState& state);

/// Open the sheet for the first time (or again): a fresh search, the first row and the field
/// focused. Never opens on a stale query - a mode the user left is not a filter.
void openSettings(SettingsState& state);

// ---------------------------------------------------------------------------
// The keyboard hand-back (defined in views/shell.cpp - the composition root)
// ---------------------------------------------------------------------------
// Declared here for the same reason `views/rawlog.h` declares its own access paths: a view that
// closes a surface (`views/palette.cpp`, `views/shell.cpp`) may not include the composition root to
// reach one function, and a second inline copy of the rule is how one of the three surfaces ends up
// handing the keyboard to a composer nobody can see.

/// Hand the keyboard to whatever is under an overlay that just closed.
///
/// A Raw Log overlay still open becomes the next top surface and takes the focus itself; with
/// nothing else up, the composer gets the one-shot request `closePalette()` uses. A *sheet* that is
/// still open keeps its own field: it is the topmost surface then, so the composer must not take a
/// focus request behind it (and the sheet re-arms its search field instead).
void returnKeyboardBelow(TuiState& state);

} // namespace llmfun::tui::views
