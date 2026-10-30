/// The block renderer: one thread message, drawn as a block (design §4.5, task 26).
///
/// The thread draws *blocks* rather than a role row per message: the adapter describes
/// one block at a time (`store::ThreadStoreAdapter::blockAt`), this module draws its
/// chrome - the role rule and label, the disclosure mark, the state glyph, the two-line tool
/// row, the focus ring and the copy control - and the adapter draws its body
/// (`renderBlockBody`), so rich text, tables and code keep the renderer that already
/// knows how to draw them.
///
/// What the split buys: the chrome is one implementation for both backends and both
/// roles, the *body* stays with the code that owns markdown, and `views/` still names
/// no v4 type (D-ADAPTER).
///
/// Anatomy (design §4.5):
///
///   User / Answer / Assistant   [rule][LABEL][ · 1.2 s][  preview], body below
///   WorkGroup                   disclosure mark + "WORK · 4 steps", body rows below
///   ToolCall / ToolResult       two lines: [glyph][TOOL][ · 1.2 s] / [summary]
///   Thinking                    disclosure mark + "Thinking · 3.2 s", dim body below
///   System                      [rule][SYSTEM][ · time][  preview], neutral text
///   Code                        the Answer/Assistant shape; the body is one fenced
///                               code block and the copy path copies its *code*
///
/// Rules the drawing follows, all from D-BLOCK: no background fill (role is a
/// foreground plus a rule), state is a glyph *and* a word, every colour and distance
/// is a token, and the row is interactive over its whole width (ImGui's
/// `InvisibleButton` gives it the mouse and Space/Enter while it holds the nav
/// focus).
///
/// Focus (task 26: "one block at a time, visible focus ring"): a click on any row
/// focuses that block, named by its `model::BlockId`. The focused block lifts its
/// label to `accent`, draws a `focusRing` rectangle in the pixel profile and shows its
/// `Copy` chip when it has a body - the focused block's copy path. The grid has no ring
/// to draw in a cell, so there the accent label *is* the ring (the design's accepted
/// hole for a cell renderer).
///
/// Copy itself no longer requires the focus (task 44). Two doors reach the same action:
/// `y` on the focused block (read by the thread view) and the block menu, which a
/// right-click opens on *any* row. The terminal keeps `Ctrl+C` for `SIGINT`, so copy is
/// these two and never the terminal's own drag selection (plan/tui_copy_probe.md).
#pragma once

#include <string>

#include "model/block.h"

namespace llmfun::tui::ui {
struct Metrics;
struct Rect;
enum class ColorToken : int;
enum class Icon : int;
} // namespace llmfun::tui::ui

namespace llmfun::tui::views {

/// The block vocabulary is the model's (`model/block.h` explains why), re-exported
/// under the names the task sketch uses.
using BlockKind = model::BlockKind;
using BlockState = model::BlockState;
using BlockView = model::BlockDesc;

/// The block's state as this frame draws it. `focused` is the thread's answer to
/// "which block owns the focus" - the view never keeps a second copy.
struct BlockDraw {
    bool expanded = true;
    bool focused = false;
    /// This block owns the open block menu (`UiState::thread.menuBlockId`). Only the owner
    /// submits the popup: an ImGui id alone would let an id recycled by a transcript reset
    /// inherit an open menu.
    bool menuOwner = false;
};

/// What one drawn block answers. Every field is read in the same frame, in the same
/// id scope, which is what makes a click unambiguous: `toggled` flips the
/// disclosure, `focusTaken` moves the focus, `copyPressed` copies the block's text
/// (`ui::copyText`) - the focused block's chip or the block menu's `Copy` item, one
/// answer either way - and `drawBody` says the caller may draw the body now (the
/// cursor has already been advanced past the header).
struct BlockResult {
    bool toggled = false;
    bool focusTaken = false;
    bool copyPressed = false;
    /// The right-click asked for this block's menu. The caller latches the owner
    /// (`UiState::thread.menuBlockId`), so the popup's life is a block identity and not
    /// just an ImGui id that a transcript reset could recycle.
    bool menuRequested = false;
    /// The block menu was submitted and open for this block this frame. False on every
    /// other row, and false once ImGui has closed it (Esc, a click elsewhere, the item
    /// acting) - the caller drops the owner on the first frame this is false.
    bool menuOpen = false;
    bool drawBody = false;
};

/// The header's height in the profile's units: one row, or two while a compact row (a
/// tool call/result) is closed and its summary is its second line. The body is not part
/// of this number - it is the caller's, and a block whose hint disagrees with it is a
/// hint being wrong, not a clipper contract.
///
/// `draw` is the same state `renderBlock()` is about to draw with, so "measured two
/// lines, drew one" cannot happen: the expansion is an argument, never a second read of
/// the descriptor.
float measureBlockHeader(const BlockView& block, const ui::Metrics& m, const BlockDraw& draw);

/// Draw one block's chrome at `rect` and submit its hit box.
///
/// `rect` must be in the metrics' units (a rectangle in the wrong unit paints
/// nothing: mixed units are the bug `Rect.cells` exists to catch). The header is
/// drawn at the rectangle's top-left and claims exactly `measureBlockHeader()`
/// rows, so the caller's cursor ends where the body starts.
BlockResult renderBlock(const BlockView& block, const ui::Rect& rect, const ui::Metrics& m,
                        const BlockDraw& draw);

// ---------------------------------------------------------------------------
// The pure half (tested without a frame)
// ---------------------------------------------------------------------------

/// The label a kind draws when the descriptor carries none: the role words of
/// design §4.8 (`YOU`, `AGENT`, `TOOL`, `ANSWER`, `SYSTEM`) plus the two the
/// block restyle introduces (`WORK` for the group, `Thinking` for the disclosure).
const char* defaultBlockLabel(BlockKind kind);

/// The role's foreground (D-BLOCK: role via foreground + rule, never a fill).
ui::ColorToken blockInk(BlockKind kind);

/// The state mark for `state`, or `Icon::Count` when the block carries none - the
/// caller draws no glyph then, rather than a mark that means nothing.
ui::Icon blockStateGlyph(BlockState state);

/// True for the two-line compact rows (a tool call and its result).
bool blockIsCompactRow(BlockKind kind);

/// True for the kinds whose header is a *disclosure* (a mark that toggles) rather
/// than a role rule: the work group and the thinking body.
bool blockHasDisclosure(BlockKind kind);

/// A duration as the header writes it: nothing at zero, `320 ms` under a second,
/// `3.2 s` under a minute, then `1m 5s` / `1h 2m`. One decimal is the resolution
/// the store has (`prettyDuration`), so the row never claims more.
std::string blockTimingLabel(double seconds);

/// The header line: label, duration, step count, and - while the block is closed -
/// the one-line preview. `draw` is the same frame state `renderBlock()` is handed,
/// so the preview cannot disagree with the height and the disclosure mark; `textGrid`
/// picks the ASCII separator (` - `) over the middle dot, which a `LC_ALL=C` terminal
/// would draw as a replacement character.
std::string blockHeaderText(const BlockView& block, const BlockDraw& draw, bool textGrid);

/// The compact row's second line: the summary, and nothing else.
std::string blockRowText(const BlockView& block);

} // namespace llmfun::tui::views
