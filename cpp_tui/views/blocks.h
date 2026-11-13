/// The block renderer: one thread message, drawn as a block (design §4.5, task 26).
///
/// The thread draws *blocks* rather than a role row per message: the adapter describes
/// one block at a time (`store::ThreadStoreAdapter::blockAt`), this module draws its
/// chrome - the role rule and label, the disclosure mark, the two-line tool
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
///   ToolCall / ToolResult       two lines: [TOOL][ · 1.2 s] / [summary]
///   Thinking                    disclosure mark + "Thinking · 3.2 s", dim body below
///   System                      [rule][SYSTEM][ · time][  preview], neutral text
///   Code                        the Answer/Assistant shape; the body is one fenced
///                               code block and the copy path copies its *code*
///
/// Rules the drawing follows: no background fill (role is a foreground plus a
/// rule), the state paints no glyph (a failure renders
/// the red FAIL word on the card's header), every colour and distance is a
/// token, and the row is interactive over its whole width (ImGui's
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

/// True for the two-line compact rows (a tool call and its result).
bool blockIsCompactRow(BlockKind kind);

/// True for the rows whose header is a *disclosure* (a mark that toggles) rather
/// than a role rule: the work group, the thinking body, and a batch's owner row
/// - the disclosure is a property of the row, not of its kind, so the owner
/// (a `BlockKind::Assistant` row) folds too.
bool blockHasDisclosure(const BlockView& block);

/// A duration as the header writes it: nothing at zero, `320 ms` under a second,
/// `3.2 s` under a minute, then `1m 5s` / `1h 2m`. One decimal is the resolution
/// the store has (`prettyDuration`), so the row never claims more.
std::string blockTimingLabel(double seconds);

/// The row's header text, fitted to `availUnits` in the profile's own unit
/// (`ui::textUnits`). Order: label -> duration -> chips. A folded batch owner drops the
/// role word. Chips drop one token at a time from the right before the duration
/// does; the label never drops.
std::string blockHeaderText(const BlockView& block, const BlockDraw& draw, const ui::Metrics& m,
                            float availUnits);

/// The compact row's second line: the summary, and nothing else.
std::string blockRowText(const BlockView& block);

} // namespace llmfun::tui::views
