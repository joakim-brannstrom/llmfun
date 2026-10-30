/// The Raw Log surface (`views/rawlog.cpp`): the opt-in developer stream (design §4.9, FR-16,
/// D-RAWLOG, task 33).
///
///     ┌ Raw Log ──────────────────── follow  x 43/5000 ┐   the title row: the surface's name,
///     │ Filter: [ llm.agent________ ]   12 match       │   its two mode chips and the counts
///     │ ─────────────────────────────────────────────  │
///     │ 12:04:31.912 info  llm.agent.pool.processTurn  │   one row per record:
///     │                     agent/pool.d:412 session…  │   time level module.function file:line
///     │ 12:04:31.940 warn  llm.tui.ui.conf …           │   message
///     │   | the full detail, when the row is expanded  │   the detail, one line per line
///     │ 3 records dropped                              │   the ring's lost records
///     │ [Copy] [Save] [Close]   f follow p pause …     │   the controls / the key hints
///     └────────────────────────────────────────────────┘
///
/// Two shapes, one body. The TUI draws it as a full-screen modal overlay (`Esc` closes, its
/// plain keys are read from the character queue); the pixel profile draws it as a bottom drawer
/// that the layout reserves between the thread and the composer, so it can never cover the
/// composer (design §4.9, `ui/layout.cpp`). Everything below the title row is the same code,
/// which is why the two profiles cannot disagree about a row's content.
///
/// What this file does *not* do:
///   * it does not read the aggregate, the display size or the profile flag - the surface is a
///     geometry taker, like every other view (D-METRICS);
///   * it does not own the open state: `UiState::rawLogOpen` is the frame's, never persisted
///     (D-RAWLOG), and the frame derives `RawLogState::open` from it each frame;
///   * it does not own the ring: it draws `model::RawLogRing`, a borrowed view of the one raw
///     store (R-08, task 32);
///   * it writes no colour or glyph literal (D-THEME): inks come from `ui::color()`, marks from
///     `ui/icons.h`.
///
/// Keyboard (design §4.9 + plan/key_gate.md §10):
///
///     f     follow the newest record again
///     p     pause/resume the follower
///     /     focus the filter field
///     y     copy the rows this window shows
///     Esc   close (the frame's step, `closeTopOverlay()`)
///
/// No `Ctrl+Shift+L` row is claimed here: the terminal delivers it as `Ctrl+L` and cannot tell
/// the two apart (plan/key_gate.md §3/§4), so the *chord* is the keymap's business
/// (`Action::ToggleRawLog`, delivered in the pixel profile) while the grid reaches the same
/// surface through `/rawlog`, the palette's Developer row and `--debug-ui`.
///
/// Layering: `model/rawlog_data.h` (the record and the ring view), `model/settings.h` and `ui/` -
/// no `tui.h`, no v4 type. That is what lets `./test_rawlog` draw the surface into a cell grid with
/// no application state.
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "../model/rawlog_data.h"
#include "../model/settings.h"
#include "../model/toast.h"

// Forward declarations, not includes (the palette header's rule): a translation unit that
// reaches `tui.h` must not need the `cpp_tui` include root just because a view was mentioned.
namespace llmfun::tui::ui {
struct Metrics;
struct Rect;
} // namespace llmfun::tui::ui

namespace llmfun::tui {
/// The frame's aggregate, for the four access paths at the bottom of this header: they write the
/// frame's own state, so a view that calls one (`views/palette.cpp`) does not have to include the
/// composition root.
struct TuiState;
} // namespace llmfun::tui

namespace llmfun::tui::views {

// ---------------------------------------------------------------------------
// The surface's vocabulary
// ---------------------------------------------------------------------------

/// The title, the `RAW` badge's long form: the now bar and the top bar own the three-letter
/// badge (`views/chrome.h`), and this row is where its meaning is spelled out.
inline constexpr char kRawLogTitle[] = "Raw Log";
/// The filter field's placeholder. The field filters *rows*, and saying so is cheaper than a
/// tooltip nobody hovers.
inline constexpr char kRawLogFilterHint[] = "Filter";
/// The two mode chips. `paused` is a state, not a filter: it stops the follower without
/// claiming the stream has stopped.
inline constexpr char kRawLogChipFollow[] = "follow";
inline constexpr char kRawLogChipPaused[] = "paused";
/// The empty states. A closed ring and a filter that hides everything are different facts, and
/// a blank body reads as a broken surface.
inline constexpr char kRawLogEmpty[] = "No records yet";
inline constexpr char kRawLogNoMatches[] = "No records match";
/// `N records dropped` (NFR-17: a bound that loses data has to say so).
inline constexpr char kRawLogDroppedSuffix[] = " records dropped";
/// The controls. `Save` exports the filtered records, `Copy` the rows on screen - two different
/// questions, so two different controls.
inline constexpr char kRawLogCopyLabel[] = "Copy";
inline constexpr char kRawLogSaveLabel[] = "Save";
inline constexpr char kRawLogCloseLabel[] = "Close";
/// The key hints the overlay draws on its last row (the grid's spelling of the design's key
/// list). One join, drawn whole or dropped whole - a hint cut mid-word advertises nothing.
inline constexpr char kRawLogHints[] = "f follow  p pause  / filter  y copy  Esc close";
/// The save notice: the path `Save` wrote, and the reason it could not. Both are one line and
/// the frame publishes the same string as a feedback item (design §4.9: "reports the path in the
/// now bar"), so the two surfaces cannot print different answers.
inline constexpr char kRawLogSavedPrefix[] = "Saved to ";
inline constexpr char kRawLogSaveFailedPrefix[] = "Save failed: ";
/// What `Copy` answers: the rows that reached the clipboard (task 45). The surface knows both the
/// payload and the count, so it is the one place that can say it.
inline constexpr char kRawLogCopiedPrefix[] = "Copied ";
/// The drawer's grip label: one word, right-aligned on the grip row (a control the user cannot
/// see is a control they will not find).
inline constexpr char kRawLogResizeLabel[] = "resize";
/// The smallest drawer the grip will ask for, in rows: the layout's own minimum (four rows), so a
/// drag cannot ask for a height the layout would silently replace.
inline constexpr int kRawLogMinResizeUnits = 4;
/// The command the composer intercepts, exactly like D's typed `/stop`.
inline constexpr char kRawLogCommand[] = "/rawlog";

/// Is `query` the Raw Log's command? The comparison is exact after the shipped submit
/// handshake trimmed the draft's trailing newline and NULs, so `/rawlog ` (with a trailing
/// blank the user typed) is accepted and `/rawlog extra` is a query, not a command - a command
/// with an argument it cannot use would silently swallow the query behind it.
bool isRawLogCommand(std::string_view query);

/// One record's stable identity, for the expansion latch: the arrival stamp plus the three
/// coordinates that make two records different. Two records with the same stamp, module,
/// function and message are indistinguishable on screen, so one identity for both is honest.
std::string rawLogKey(const model::RawLogRecord& record);

// ---------------------------------------------------------------------------
// The input
// ---------------------------------------------------------------------------

/// Everything the surface reads, plus the answers it hands back.
///
/// One writer (the frame thread). The first group is derived by `views/shell.cpp` once per
/// frame; the second group is what the surface wrote and the frame applies in the same frame;
/// the third is what a harness and a golden measure.
struct RawLogState {
    // --- derived by the frame ---------------------------------------------
    /// The surface is up (`UiState::rawLogOpen`).
    bool open = false;
    /// The floating shape - the text grid's full-screen overlay. It is what makes the surface
    /// *modal* (`TuiState::overlayOwnsKeyboard()`), what makes `Esc` close it, and what lets the
    /// plain keys (`f`, `p`, `/`, `y`) be read from the character queue - the frame also requires
    /// this surface to be the topmost overlay. The pixel profile's drawer is part of the tiling:
    /// the composer keeps the keyboard there.
    bool overlayShape = false;
    /// Follow the newest record (D-FOLLOW, pointer-owned: wheel/scrollbar input clears it, and
    /// `f`/`rawLogFollowNewest()` re-arm it - the frame's `End` action calls the same function
    /// while the surface is up, through its `Focus::RawLog` keymap row).
    bool follow = true;
    /// The follower is paused: the ring keeps filling (one store) and the view stops moving.
    bool paused = false;
    /// The ring's dropped-record counter (the row the surface draws when it advances).
    std::size_t dropped = 0;
    /// The smallest severity the surface shows, from `UiLog` (`model::rawLogFloor()`): the
    /// frame derives it, so a preference change reaches the surface without the view reading a
    /// preference.
    int minLevel = model::kRawLogLevelInfo;

    // --- the filter field --------------------------------------------------
    /// The filter text. `filterSeq` suffixes the field's ImGui id so a programmatic clear makes
    /// the widget re-read the buffer instead of its stale edit state - the rail's rule. The one
    /// writer is `closeRawLog()` (which clears the filter, so the next open starts as a stream
    /// with no query); the test bumps it to prove the id path.
    std::array<char, 96> filter = {};
    int filterSeq = 0;
    /// Ask for the field to take the keyboard (`/`): the request stays armed until the field
    /// really owns it, because ImGui applies it on the item's next Begin.
    bool filterFocus = false;
    /// The field owns the keyboard this frame (an observation: the harness reads it, and it is
    /// what tells the plain keys they belong to the field).
    bool filterFocused = false;

    // --- what the surface answers, applied by the frame --------------------
    /// `Close`, or the drawer's close button.
    bool closeRequested = false;
    /// `p` / the pause chip: flip `UiState::rawLogPaused`.
    bool pauseRequested = false;
    /// `f`: re-arm follow (`UiState::rawLogFollow = true`).
    bool followRequested = false;
    /// The pointer asked for the follower off (a wheel notch or a scrollbar press).
    bool followCleared = false;

    // --- one-shot actions the surface performs itself ----------------------
    /// `y` / the copy button, and the save button. Both are performed by the surface at the end
    /// of the frame that drew the control - the clipboard and the file are `ui`/`model` calls,
    /// so no answer has to travel through the frame for them.
    bool copiedNow = false;
    bool saveRequested = false;

    // --- observations, for the harness, the golden and the log -------------
    /// Records that pass the filter, rows that were drawn (bounded: viewport + one row of
    /// overscan), and the follower's own counters. `followPins` is the 10 fps cap's evidence:
    /// the pin only runs when the clock allows it and the content sits below the fold.
    std::size_t matchingRecords = 0;
    std::size_t drawnRows = 0;
    std::size_t followPins = 0;
    /// The body's first row and row capacity this frame: what `y` copies (see
    /// `rawLogVisibleText()`).
    std::size_t firstVisible = 0;
    std::size_t windowRows = 0;
    /// The rows `y` copied last (the harness reads them; empty when nothing was copied).
    std::string copiedRows;
    /// The drawer's grip was dragged: the height it asks for (in the profile's units), which the
    /// shell writes into `UiState::rawLogH` for the layout to clamp. 0 = no request this frame.
    float resizeTo = 0.0f;
    /// The last action's outcome, as the frame publishes it. `notice` is the one-shot sentence the
    /// frame turns into a feedback item (task 45: the now bar's flash in the grid, a toast card in
    /// the pixel profile) and `noticeKind` its severity - a saved path is a success, a copy is one
    /// too, and a write that failed is an error that stays until the user answers it. The frame
    /// clears both after reading them, so one press is one item.
    std::string savePath;
    std::string saveError;
    std::string notice;
    model::ToastKind noticeKind = model::ToastKind::Info;

    // --- the cached filtered index list ------------------------------------
    /// The ring is 5000 records and the render path may not re-filter it per frame (§6.4): the
    /// surface keeps the indices that pass the filter and rebuilds them only when the filter, the
    /// ring or the floor changed. `builtFilter` is the filter text; `builtArrivals` is the ring's
    /// monotonic accept counter, which is the only fact that changes on *every* arrival - a deque
    /// at capacity keeps its size, and a front timestamp is ambiguous when a burst stamps several
    /// records with the same millisecond.
    std::vector<std::size_t> matching;
    std::string builtFilter;
    std::size_t builtSize = 0;
    std::size_t builtArrivals = 0;
    int builtMinLevel = model::kRawLogLevelInfo;
    /// The one expanded record (the detail's continuation lines are drawn under it), by
    /// `rawLogKey()`. One at a time: the surface's job is a stream, and a stream with three
    /// open disclosures is a tree nobody asked for. `expandedAt` is its index in `matching` and
    /// `expandedLines` its rendered detail, both refreshed by `refreshExpansion()` (a click is not
    /// an arrival, so it does not re-filter the ring).
    std::string expandedKey;
    std::string builtExpanded;
    std::size_t expandedAt = static_cast<std::size_t>(-1);
    std::vector<std::string> expandedLines;
    /// The follower's clock, in ImGui seconds: the next time a pin may run. 10 fps.
    double nextPinAt = 0.0;

    /// Modal in this shape? One predicate, so the frame's `overlayOwnsKeyboard()`, the key
    /// reader and the frame's `Esc` chain cannot disagree.
    bool ownsKeyboard() const { return open && overlayShape; }
};

// ---------------------------------------------------------------------------
// The view
// ---------------------------------------------------------------------------

/// Draw the surface into `region` - the box `ui::computeShell()` answered (the grid's
/// `ShellLayout::rawLogOverlay` or the pixel profile's drawer) - and apply the keys it owns.
///
/// `ring` is the frame's borrowed view of the one raw store. The surface never copies it: the
/// filtered index list (`RawLogState::matching`) is the only thing it builds, and only when the
/// filter or the ring changed.
///
/// Must be called from inside a window the frame has begun (the shell's root, or a harness's
/// own): the title row's buttons and the body's child are real ImGui items, and a view never
/// opens a window the caller did not ask for.
///
/// `ownsKeyboard` is false when a higher overlay is up: the surface still draws, but it does not
/// read the plain keys or take the focus. The frame passes `layout.top == OverlayLayer::RawLog`;
/// a standalone harness keeps the default.
void renderRawLog(RawLogState& state, const ui::Rect& region, const ui::Metrics& m,
                  const model::RawLogRing& ring, bool ownsKeyboard = true);

/// The rows `y` copies: the records this window currently shows, one per line, with the detail
/// lines of an expanded record. `windowRows` is the body's row capacity (the viewport), and
/// `first` the first body row - so what reaches the clipboard is exactly what the user selected
/// with their eyes, never the whole ring (that is `Save`).
///
/// Exposed because the copy is a pure function of the state and the ring: the harness proves the
/// payload without a clipboard, and the surface calls the same function for the real one.
std::string rawLogVisibleText(const RawLogState& state, const model::RawLogRing& ring,
                              std::size_t first, std::size_t windowRows);

/// Re-arm the follower and owe one catch-up to the newest record (`f`, `End`, the palette row).
/// The latch is the one-shot pin the next `renderRawLog()` performs; a per-frame pin would make
/// the follower fight the wheel (D-FOLLOW).
void rawLogFollowNewest(RawLogState& state);

// ---------------------------------------------------------------------------
// The access paths (defined in views/shell.cpp - the composition root)
// ---------------------------------------------------------------------------
//
// Declared here rather than in `views/shell.h` because the *palette* is an entry point: a view may
// not include the composition root to call one function (that would invert the layering the
// surface's header states), and these four are the Raw Log's own vocabulary. The definitions stay
// in `views/shell.cpp`, next to the frame state they write.

/// Toggle the surface (`Ctrl+Shift+L`, `/rawlog`, the palette's Developer row, `--debug-ui`).
/// Opening is explicit and never persisted; it owes one catch-up to the newest record.
void toggleRawLog(TuiState& state);

/// Close the surface (the `Esc` step, the chord while it is up, the `Close` control), clearing the
/// filter so the next open starts as a stream with no query, and restoring the drawer's height to
/// the design's 30 %.
void closeRawLog(TuiState& state);

/// Publish a one-line answer for the given severity: the path `Save` wrote, the rows `Copy` took,
/// or the reason a write failed. The frame owns the item's lifetime (task 45): the grid's now bar
/// flashes it for two seconds, and a warning or an error holds the row until the user answers it.
void notifyRawLog(TuiState& state, std::string text, model::ToastKind kind, const ui::Metrics& m);

/// Carry out a typed `/rawlog`, or answer false when `query` is not that command.
///
/// The composer's submit path calls this before it treats the draft as a question: a command is
/// never submitted and never enters the history (the typed `/stop` rule, one layer up).
bool handleRawLogCommand(TuiState& state, std::string_view query);

} // namespace llmfun::tui::views
