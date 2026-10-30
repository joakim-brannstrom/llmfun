/// Palette overlay (`Ctrl+K`, `/`): the discoverability layer (design §4.12, D-PALETTE).
///
/// One static action table plus the live session list and the command list, fuzzy-matched
/// through `session_fuzzy.h` and drawn as an overlay. An overlay is not tiled (D-OVERLAY): it is
/// its own window, drawn after the shell, and it never changes the geometry of the regions under
/// it.
///
///     Search                                                  COMMANDS 7      the title row
///     /continue                                                              the live filter
///     ------------------------------------------------------------------     the hairline
///     New session          Start a fresh session                    ACTIONS  the rows, by
///     ...                                                                    section
///     /help                Show the available commands              COMMANDS
///     ESC close  Enter run  Tab group  ^N/^P move                            the key hints
///
/// The sections are ordered by confidence - ACTIONS, SESSIONS, COMMANDS, DEVELOPER, ASK - and a
/// query ranks *within* a section, so the list keeps its shape while it narrows. The free-text
/// `ASK` row is pinned last whatever its score: it is the fallback, not a result.
///
/// Keyboard (evidence in plan/key_gate.md §4/§4c/§10):
///
///     Ctrl+K          open with an empty query (tui.cpp owns the chord)
///     `/`             open at COMMANDS with the `/` carried into the query - the composer's
///                     character filter (`views/composer.cpp`) takes the slash and hands it on,
///                     so a typed `/continue` survives. There is no `/` key binding: the
///                     character *is* the entry point (§10), and the rail reads its own `/`
///                     for its search field.
///     Up / Down       move the selection   - pixel profile only (§4b: the text backend
///                                            delivers neither as a key)
///     Ctrl+P / Ctrl+N move the selection   - measured in both backends (§4c)
///     Enter           activate the selected row
///     Tab / Shift+Tab cycle the group filter
///     Esc             close - performed by the frame-level keymap (the first step of D-ESC's
///                     chain in tui.cpp), not by this view
///
/// Every row is also a key binding or a visible control, and every row has a handler: the
/// registry test in `./test_palette` walks the whole table through `palettePerformRow()` and
/// fails on an enabled row that answers false, and it fails on a `model::Action` the palette
/// claims (through `model::allActions()`) that has no row. That is why the action table carries
/// the keymap action it performs instead of only a label.
#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

// TODO: do not use relative paths
// The row's keymap action (`PaletteItem::action`). Relative, not `"model/keymap.h"`: this header
// travels inside `tui.h`, and several test targets include `tui.h` with no `cpp_tui/` on the
// include path (views/settings.h's rule).
#include "../model/keymap.h"

namespace llmfun::tui::ui {
/// Forward declaration of the overlay box (`ui/metrics.h`, task 17). A
/// `const Rect&` parameter needs nothing more, and keeping this header free of
/// `ui/` includes lets a translation unit that reaches `tui.h` from another
/// directory compile without the `cpp_tui` root on its include path.
struct Rect;
} // namespace llmfun::tui::ui

namespace llmfun::tui {
/// Forward declaration on purpose: `views/*` is not allowed to include `tui.h`
/// (D-ADAPTER). Only the activation path in palette.cpp touches the aggregate.
struct TuiState;
} // namespace llmfun::tui

namespace llmfun::tui::views {

/// One row of the palette.
///
/// `id` is the stable identity of the action, never a display label: the renderer, the
/// activation switch and the tests match on it while `label` may change freely. `group` is the
/// section the row belongs to: "ACTIONS", "SESSIONS", "COMMANDS", "DEVELOPER" or "ASK".
///
/// `action` is the keymap action the row performs, or `model::Action::None` for a row that is
/// the palette's own command (a session pick, a command line, the free-text fallback, and the
/// four rows whose other entry point is a visible control: new session, focus composer, copy last
/// answer, open settings). The activation path *dispatches* on it, so a row cannot claim one
/// action and perform another - and the registry test can ask "is every action the palette claims
/// really listed?" without knowing a single label.
struct PaletteItem {
    const char* id;    // stable id, never a display label
    const char* group; // "ACTIONS" | "SESSIONS" | "COMMANDS" | "DEVELOPER" | "ASK"
    std::string label, description;
    bool enabled;
    model::Action action = model::Action::None;
};

/// Palette state. `open`, `query`, `selected`, `groupIndex` and `items` are the contract; the
/// members below them are the derived view this implementation needs to draw a bounded list.
struct PaletteState {
    bool open = false;
    std::array<char, 128> query = {};
    int selected = 0, groupIndex = 0;
    std::vector<PaletteItem> items; // rebuilt on open + on query change, not per frame

    /// `items` indices in ranked order: what the renderer walks. Rebuilt
    /// together with `items`, never per frame (design §4.12: matching runs over
    /// the model, not per draw).
    std::vector<int> visible;
    /// Owns every id that is not a string literal (the session rows). A deque
    /// never relocates its elements, so `PaletteItem::id` stays valid until the
    /// next rebuild; a `std::vector<std::string>` could reallocate mid-build.
    std::deque<std::string> itemIds;
    /// The query `visible` was ranked for. A differing field text is the
    /// "query changed" trigger - one string compare per frame, no ranking.
    std::string builtQuery;
    /// First row of the visible window. Follows `selected` while the list is
    /// longer than the viewport, so the window scrolls instead of the selection
    /// walking off the bottom.
    int firstVisible = 0;
    /// A seeded query is a *prefix*, not a draft: ImGui selects the whole buffer when the
    /// field is activated by the keyboard, so the first typed character would replace the
    /// `/` the composer handed over (measured: seeded `/` + `continue` gave `continue`).
    /// While this is set the field's caret is put after the seed, once.
    bool seedCaretPending = false;
    /// The copy row's answer flag and the thread *shape* it was computed for (group count, the
    /// newest group's id and its block count). The check builds a block descriptor per block, so
    /// the per-frame refresh re-walks only when the thread's shape moved - an answer cannot appear
    /// or disappear without that.
    std::size_t answerStamp = 0;
    bool answerKnown = false;
};

/// One command line the palette offers: the text a row submits and the sentence that explains
/// it. A static list is the MVP's agreement (P-20, design §4.12: "the full command registry is
/// post-MVP"), so it must not claim a command it cannot send: the label *is* the line
/// (`palettePerformRow()`), and `/rawlog` is the UI's own command (`views::handleRawLogCommand`),
/// which is why it is here as well as in the DEVELOPER group.
struct CommandRow {
    const char* label;
    const char* description;
};

/// A read-only view of that table - `model::Keymap`'s shape, so a consumer walks it instead of
/// copying it.
struct CommandList {
    const CommandRow* rows = nullptr;
    std::size_t count = 0;

    const CommandRow* begin() const { return rows; }
    const CommandRow* end() const { return rows + count; }
};

/// The command table: the palette's COMMANDS group and the help page's Commands section both read
/// it (task 42), so a command cannot be listed in one place and missing - or described
/// differently - in the other.
const CommandList& commandTable();

/// Build the whole row table for `state`: the static action table (ACTIONS + DEVELOPER), one
/// row per live session, the command lines and - only with a non-empty query - the free-text ASK
/// row last. `idStorage` owns the runtime ids (the session rows and the command rows, whose ids
/// carry the command name); it must outlive the returned items and is expected to be the
/// PaletteState member of the same name.
///
/// The rows are built in section order, which is the order the unfiltered list shows and the
/// order `rankItems()` keeps while a query narrows it.
std::vector<PaletteItem> paletteActions(const TuiState& state, const std::string& query,
                                        std::deque<std::string>& idStorage);

/// Open the palette with `items` and `initialQuery`, ranked and selected from
/// the first row. Kept signature-compatible with the task's contract: it needs
/// no aggregate, so it also serves as the seam a later shell (task 18) can call
/// with its own table.
void openPalette(PaletteState&, std::vector<PaletteItem> items, const char* initialQuery);

/// Open the palette for `state`: rebuilds the table from the live model and
/// seeds the query. This is what `Ctrl+K` calls.
void openPaletteForState(PaletteState&, TuiState&, const char* initialQuery);

/// Open the palette at its COMMANDS group with `seed` in the query field - the composer's `/`
/// on an empty line (`views/composer.h` answers it, `views/shell.cpp` passes `"/"`).
///
/// The seed is the character the composer took but did not insert: it belongs to the palette
/// now, so typing `continue` gives `/continue` and Enter submits a command line rather than the
/// word `continue` (design §6.2: a slash line goes through the normal submit handshake, so D's
/// registry runs it). `seedCaretPending` is what stops ImGui's select-all-on-activation from
/// eating it.
///
/// The group filter is the palette's own (Tab walks the same list); this entry point only
/// decides where the cycle starts. An empty `seed` therefore opens the command list itself, and
/// a query that narrows to nothing is still the COMMANDS group - it says `no matches` rather
/// than pretending the user pressed a different key.
void openPaletteForCommands(PaletteState&, TuiState&, const char* seed);

/// Point the group filter at `group` (a section name, `""` for ALL) and re-rank, selecting the
/// first row of the new filter. One function, so a filter the title row prints is always a filter
/// the list was ranked with - `Tab` lands here, the `/` entry point opens here, and a harness (or
/// a golden) can ask for a section without pressing keys.
void selectGroup(PaletteState&, const char* group);

/// Perform one row: the one dispatcher behind `Enter`, a click and the registry test.
///
/// Every row the table builds answers true in the state where it is `enabled` - `./test_palette`
/// walks the whole table through this call, which is what makes a dead entry a test failure rather
/// than a user's discovery. `false` means "this row cannot act here" and never "the row was
/// forgotten": the states are the ones the table draws disabled - the free-text row while a run is
/// active, `New session` while a run is active (the rail's `+` refuses the same way), `Stop run`
/// while nothing is running, and `Focus composer` while a settings sheet or a Raw Log overlay is
/// still open under the palette (the keyboard would go back to *that* surface, not to the
/// composer).
///
/// The row's `action` is performed through the same model calls the keymap dispatcher uses
/// (`model::toggleRail()`, `model::toggleInspector()`, `views::toggleRawLog()`, ...), so the chord
/// and the row are one implementation each; `Action::None` rows are the palette's own commands (a
/// session pick, a command line, the free-text fallback) and the four whose other entry point is a
/// visible control.
bool palettePerformRow(const PaletteItem&, PaletteState&, TuiState&);
bool palettePerformRow(const PaletteItem&, PaletteState&, TuiState&);

/// Close the palette and hand the keyboard back to the composer. Drawing
/// nothing next frame is the caller's job (`open` is false).
void closePalette(PaletteState&, TuiState&);

/// Draw and drive the overlay inside `overlay` - the box `ui::computeShell()`
/// answered for the palette (task 17: no view positions itself). The sheet
/// shrinks to its content inside that box and never leaves it. Returns true when
/// an item was activated, false otherwise (including "not open", "closed with
/// Esc" and "nothing enabled"). Must be called after the shell has drawn, so the
/// overlay sits on top.
bool renderPalette(PaletteState&, llmfun::tui::TuiState&, const ui::Rect& overlay);

} // namespace llmfun::tui::views
