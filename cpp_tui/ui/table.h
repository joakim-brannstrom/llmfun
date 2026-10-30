/// The TUI text-grid table renderer (design section 4.14, D-TABLE, FR-20): GFM tables
/// drawn on a 1-cell grid, where rich_md's own table renderer is unsafe (C-05 - it
/// paints header text onto the top border, drops the left corners on some rows, floods
/// every cell with a background and can blank the message when the table is too wide).
///
///     const auto spans = ui::findGfmTables(body);          // detection, fence-aware
///     ui::Table table;
///     if (ui::parseGfmTable(body.substr(span.begin, span.end - span.begin), table)) {
///         const ui::TableSize size = ui::measureTable(table, availCells);
///         ui::renderTable(table, {x, y, float(availCells), float(size.h), true}, metrics);
///     }
///
/// Four stages, four functions:
///
///   * **detect** - `findGfmTables()` answers the byte ranges of the tables in a markdown
///     body (a table nested in a list item included). A scanner tracks fenced code blocks
///     (their contents are never a table), counts indentation from the list item the line
///     sits in (four columns past its content column is an indented code block), and treats
///     inline code as opaque (a `|` inside backticks is text, not a separator), which is the
///     whole difference between "a table" and "a pipe in prose".
///   * **parse** - `parseGfmTable()` turns one such range into rows of plain cells:
///     header row, delimiter row for the alignment, body rows, `\|` escapes, inline code
///     kept verbatim. Anything that is not a table answers false and leaves `out`
///     untouched, so a caller never draws half a parse.
///   * **allocate** - `allocateTableWidths()` decides the cell count of every column, or
///     answers "this cannot be a grid" and the caller falls back to the vertical
///     key-value layout (`tableKeyValueLines()`).
///   * **draw** - `renderTable()` draws borders, cells and alignment. No background fill
///     anywhere (D-BLOCK): a table is text plus a rule, like every other block.
///
/// Units are **cells**, always. Column widths come from `ui::displayWidth()` (the same
/// table the ncurses backend draws with, so a CJK ideograph is two cells and a combining
/// mark is none) and never from a byte or codepoint count; wrapping is `ui::wrap()`.
/// `Rect.cells` states the unit and is asserted, like every other consumer of a `Rect`.
///
/// Cell text is **plain** (MVP: inline styling is P3 - it is the third pre-agreed cut,
/// section 9): `**bold**` loses its markers, a link keeps its label, inline code keeps its
/// content, and an escaped character keeps the character. A lone `*` (a glob in a tool
/// name, `read_file(*.cpp)`) is not a marker and is kept.
///
/// Degradation is ordered and never blank:
///
///   1. more columns than `kMaxTableColumns`, less room than `kMinTableWidth`, or no room
///      for `kMinColumnWidth` in every column -> the vertical key-value layout
///      (`Tool: read_file`, `Calls: 3`) - see `tableKeyValueLines()`;
///   2. a cell that still does not fit its column wraps (`ui::wrap`), and the row grows
///      to its tallest cell; a word longer than the column breaks at the cell budget,
///      never leaving half a wide glyph drawn;
///   3. a table taller than the rect is clipped at the rect's bottom edge - the caller
///      reserved the rows from `measureTable()`, and a block's row budget is the block's
///      business, not the renderer's.
///
/// There is no horizontal scrolling anywhere in this module (design section 4.14).
///
/// Profile: the **text grid only**. The pixel GUI keeps rich_md's native tables
/// (section 6.8: the GUI never sees a `llmfun-table` block), so `renderTable()` asserts a
/// cell rect and draws nothing for a pixel one - the same fail-closed answer
/// `ui/primitives.cpp` gives a mixed unit pair. `Metrics` is still an argument: it carries
/// the profile flag the unit check reads, and taking `(rect, metrics)` is the signature
/// every geometry taker in `ui/` has. Its hairlines are one cell, which is exactly
/// `Metrics::border` on the grid - `kTableBorderWidth()` writes that 1 into the constant,
/// so the anatomy and the chrome identity cannot disagree.
///
/// Glyphs: the box-drawing forms live here, not in `ui/icons.h` - they are geometry, not
/// state marks, and have no word beside them (the handoff `ui/icons.h` records). The
/// ASCII column is `+ - |`, chosen by the surface's `GlyphSet` (`textGlyphSet()`, so
/// `TERM=linux`, a CJK locale and `LLMFUN_ASCII=1` all get it).
///
/// The bridge, and where each part of it lives (task 37 shipped it; this is the map, not a
/// to-do list):
///
///   * `bridgeTables()` here rewrites each table into a `llmfun-table` fence (or the
///     `llmfun-table-source` one the `s` toggle asks for), at the table's container column so
///     a list item survives the transform - a pure text transform, no frame;
///   * `ui::registerTableFenceRenderers()` (`ui/markdown.h`) registers the two renderers on the
///     text grid's rich_md context, and `ui/markdown.cpp` draws the grid (or the source);
///   * `bridgeBodyForProfile()` and `bodyHasTable()` here are the seam the body renderer and
///     the `s` predicate call: one profile question in front of `bridgeTables()` and the
///     detector, so neither can bridge in a profile that draws rich_md's own tables;
///   * `views/thread.cpp` owns the key: `s` is a *character* (`ui::takeCharacter`), the same
///     route `y` takes, with the row in `plan/key_gate.md` §4h - printable keys cannot be keymap
///     chords on the grid, so there is deliberately no `model::keymap` action and no palette row;
///   * the "this block shows its source" flag is `UiState::thread.tableSource` (D-GROUPID: plain
///     data keyed by the block's stable id), and what the renderer receives is the *body's text*,
///     so no mode has to be carried beside it.
///
/// This module stays stateless on purpose: expansion and source state are `UiState` data, and the
/// source of a table is the block's own text, so there is no "is the source showing" flag here
/// that could drift from the block. `y` needs nothing of its own either: a table block's body is
/// markdown, and the block copy path (`ui::copyText(body, false)` in `ui/markdown.cpp`, reached
/// from `views/thread.cpp`) already copies it verbatim.
///
/// Layering: `ui/metrics.h`, `ui/text.h`, `ui/icons.h`, `ui/tokens.h` and imgui - no
/// `tui.h`, no view header, no v4 type. `ui/` stays the layer below the app, which is what
/// lets `./test_table` compile this module on its own and render it through the real text
/// backend.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace llmfun::tui::ui {

struct Metrics;
struct Rect;

// ---------------------------------------------------------------------------
// Constants - the contract. Tune only with a golden update.
// ---------------------------------------------------------------------------

/// Air between a border and the cell's text, per side. One cell: a grid has no
/// sub-cell spacing, and the design's `kTableBorderWidth()` identity is written for it.
inline constexpr int kCellPadding = 1;
/// The narrowest a column may be. Below this a column is not a column any more (a 3-cell
/// tool name wraps in the middle of a word), so the layout degrades instead.
inline constexpr int kMinColumnWidth = 4;
/// The narrowest a *grid* table may be drawn in. Narrower than this and a two-column
/// table is two wrapped columns of noise; the key-value layout reads better.
inline constexpr int kMinTableWidth = 20;
/// The widest table, in columns. GFM has no limit; a terminal has. Beyond eight columns
/// the key-value layout is the readable answer (design section 4.14).
inline constexpr int kMaxTableColumns = 8;
/// The cells a table spends on chrome alone: one border per column edge (`cols + 1`)
/// plus `kCellPadding` on each side of every cell (`2 * cols`).
inline constexpr int kTableBorderWidth(int cols) { return 3 * cols + 1; }

/// The fenced-block languages the bridge writes (design section 6.8, task 37).
///
/// One string, two readers: `bridgeTables()` writes the fence and `ui/markdown.cpp`
/// registers the renderer for it, so a typo cannot make the bridge write a fence nothing
/// draws. `llmfun-table` is the grid, `llmfun-table-source` the verbatim markdown (the `s`
/// toggle and the never-blank fallback) - two languages rather than one plus a flag, so the
/// block's own state is the body's text and nothing has to be carried beside it.
inline constexpr char kTableFenceLanguage[] = "llmfun-table";
inline constexpr char kTableSourceFenceLanguage[] = "llmfun-table-source";

/// Cell alignment, as the delimiter row spells it: `:---` left, `:--:` centre, `---:`
/// right. The numbers are the contract (the renderer's arithmetic reads them), so they
/// are named here rather than left to a `-1/0/+1` comment.
inline constexpr int kAlignLeft = -1;
inline constexpr int kAlignCenter = 0;
inline constexpr int kAlignRight = 1;

// ---------------------------------------------------------------------------
// The data
// ---------------------------------------------------------------------------

/// One cell of a table: the text it draws and how it is placed.
///
/// `text` is *plain* text - the inline markdown a model writes in a cell has already been
/// reduced to what the grid can draw (`**bold**` -> `bold`, `[label](url)` -> `label`,
/// `` `code` `` -> `code`, `\|` -> `|`). A cell carries the alignment of its whole column
/// (the parser writes the delimiter row's answer into every cell of the column), so a
/// consumer that reads a body cell is not surprised; the renderer takes it from the header
/// row, which is the row the delimiter belongs to.
struct TableCell {
    std::string text;
    bool header = false;
    int align = kAlignLeft;
};

/// A parsed table: `rows[0]` is the header row, the rest are the body.
///
/// Empty (`rows.empty()`) is not a table at all; a table with a header and no body rows is
/// legal GFM and draws as a header over a bottom border.
///
/// The parser gives every row `columns()` cells, so a column index is valid on every row. This
/// is a public aggregate, though, so a hand-built row may be shorter: the widths, the measure
/// and the draw all clamp to the shorter of the two rather than reading past the row.
struct Table {
    std::vector<std::vector<TableCell>> rows;

    /// Columns in the header row, 0 for an empty table. Every row has this many cells
    /// (the parser pads a short body row and drops the surplus), so a column index is
    /// valid on every row.
    int columns() const { return rows.empty() ? 0 : static_cast<int>(rows.front().size()); }
    bool empty() const { return rows.empty(); }
};

/// One GFM table inside a markdown body, as a half-open byte range:
/// `body.substr(begin, end - begin)` is exactly the table's text (its lines, without the
/// newline that follows the last one). In a CRLF body the range ends *after* the last line's
/// carriage return, so a caller that replaces the range (`body.replace(begin, end - begin,
/// fence)`, which is what task 37 does) cannot strand a CR in front of what it wrote.
///
/// The range is what a caller needs to *replace* the table (task 37's bridge rewrites it
/// into a fenced block) and to parse it. Ranges are in document order and never overlap.
/// `containerIndent` is the content column of the innermost list item the table sits in (0 at
/// the top level): the bridge writes both fence delimiters there, so a table nested in a list
/// item stays in the item instead of ending it at column zero.
struct GfmTableSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
    int containerIndent = 0;
};

// ---------------------------------------------------------------------------
// Detect
// ---------------------------------------------------------------------------

/// Every GFM table in `body`, in document order.
///
/// A table is a header line, a delimiter line with the same number of cells, and the
/// following lines that still look like a row (a non-blank line with a top-level
/// separator) in the table's container. Both the header and the delimiter must carry a
/// top-level `|`, which is what keeps a setext heading (`Title` over `---`) from being read
/// as a one-column table.
///
/// Fenced code is skipped: a line opening a fence of three or more backticks or tildes starts a
/// block whose contents are never scanned, and the fence is closed by a line of the same character
/// at least as long. A fence is a fence only while it sits at most three columns past its
/// container's content column; deeper than that it is an indented code block. An unterminated
/// fence therefore swallows the rest of the body - the same reading every markdown renderer gives
/// it, and the safe one: a table the model *wrote as example code* is not a table.
///
/// Indentation is counted from the container, not from the body. The walk keeps a stack of open
/// list-item content columns: a blank line does not close an item, a marker at a smaller indent
/// closes the deeper ones, and a paragraph's lazy continuation keeps the item open (an uncertain
/// container errs toward reporting a table). A header four columns past the innermost content
/// column is an indented code block and is not reported; a table nested in a list item is. The
/// body rows must stay in the container's content range too (a row four columns past it is an
/// indented code block); the delimiter row may be indented further (md4c reads it as a paragraph
/// continuation), but not behind the container.
/// `GfmTableSpan::containerIndent` carries the column the bridge writes its fence at. Nested
/// lists are handled; blockquotes are not (a `> `-prefixed table is not detected, so it keeps
/// rich_md's native renderer - a recorded gap).
///
/// `\r\n` is accepted: a line's text is its bytes without the newline and without a
/// trailing carriage return, so a body that came from a file still parses.
std::vector<GfmTableSpan> findGfmTables(std::string_view body);

/// Whether one *line* is a GFM table row, as the bridge above reads rows: it carries a
/// top-level separator - a `|` outside inline code and outside an escape - which is exactly
/// `splitRow(line).separated` in `table.cpp`. A blank line is not a row.
///
/// Exposed for the live answer's caret (task 38): a glyph appended after a row's last pipe
/// changes how the row and the delimiter row parse, so `ui::withStreamingCaret()` puts the caret
/// on the next line instead. A row line is necessary but not sufficient - prose (`ls | grep`) and
/// a code line inside a fence carry a pipe too - so the caret asks this question as a cheap
/// pre-check and then asks the scanner whether the body's last line is inside a table span.
bool isTableRowLine(std::string_view line);

// ---------------------------------------------------------------------------
// Bridge (design section 6.8, task 37)
// ---------------------------------------------------------------------------

/// `body` with every GFM table replaced by a fenced block, written to `out`.
///
/// The bridge's pure half: the store adapter calls this on a message body before the body
/// reaches the renderer, and `ui/markdown.cpp`'s fenced renderer draws what it wrote. The
/// replacement for one table is
///
///     ```llmfun-table
///     | Tool | Calls |
///     |------|------:|
///     | read_file | 3 |
///     ```
///
/// i.e. the table's own lines inside a `kTableFenceLanguage` fence (or a
/// `kTableSourceFenceLanguage` one when `showSource`), because the source has to stay in the
/// block: the grid is a rendering of it, and `s` (show the source) and `y` (copy the markdown)
/// are always possible (section 6.8). Everything else in `body` is byte-identical. The fence is
/// written at the span's `containerIndent` (zero for a top-level table), so a table nested in a
/// list item does not end the item; the table's own lines keep their indentation. A table is
/// *not* rewritten when it sits inside a code block - a fenced one (`findGfmTables()` never
/// reports it) or an indented one four columns past its container's content column
/// (`findGfmTables()` never reports that one either, so `s` has nothing to offer there).
///
/// Answers false - leaving `out` untouched - when nothing was rewritten, so a caller pays a scan
/// and no copy for the ordinary body. `out` is replaced when the answer is true; `body` and `out`
/// may be the same object (the replacement is built before either is written).
///
/// Idempotent: the fences this writes are skipped by the detection, so a second pass answers
/// false and changes nothing. That is what lets a caller transform a body once and a second
/// caller transform it again without either knowing about the other.
bool bridgeTables(std::string_view body, bool showSource, std::string& out);

/// The bridge as a body renderer uses it: `textGrid` is the profile the caller measured, then
/// `bridgeTables()`.
///
/// The profile is a parameter rather than a lookup here on purpose: `ui/` may not read the
/// live profile (`metricsFor()` belongs to the frame layer), so the module that *has* the
/// measurement passes the one flag, and the transform, its fence renderers and `s` all agree
/// with it.
bool bridgeBodyForProfile(bool textGrid, std::string_view body, bool showSource, std::string& out);

/// Does `body` hold a table this profile would draw as a grid?
///
/// The detector is the answer - it is the same span list `bridgeTables()` rewrites - so a
/// toggle can never offer a body the renderer would not rewrite, and the predicate pays a scan
/// rather than building the bridged copy. False in the pixel profile.
bool bodyHasTable(bool textGrid, std::string_view body);

// ---------------------------------------------------------------------------
// Parse
// ---------------------------------------------------------------------------

/// Parse one table block - a whole `GfmTableSpan` range, or a body that is nothing but a
/// table - into `out`.
///
/// Accepted: an optional leading and trailing `|` on every row, a delimiter row whose
/// every cell is `:?-+:?`, body rows with fewer cells (padded empty) or more (dropped), and
/// no body rows at all. Cell text is trimmed and reduced to plain text (see the header).
///
/// Rejected, with `out` left untouched: fewer than two lines (a header and its delimiter), an
/// interior blank line (leading and trailing ones are ignored), a row count mismatch between header
/// and delimiter, a delimiter cell that is not a valid alignment spec, and a block that is not a
/// table at all. False is "not a table", never "a table with a problem" - a caller that gets false
/// renders the source instead of a guess.
bool parseGfmTable(std::string_view block, Table& out);

// ---------------------------------------------------------------------------
// Allocate
// ---------------------------------------------------------------------------

/// The cell count of every column, or `fits == false` when a grid cannot work at
/// `availCells` (the caller then uses `tableKeyValueLines()`).
///
/// The arithmetic, in order:
///
///   1. the chrome is `kTableBorderWidth(columns)`; the content budget is what is left;
///   2. `fits` is false when `availCells < kMinTableWidth`, when the table has more than
///      `kMaxTableColumns` columns, or when the content budget cannot give every column
///      `kMinColumnWidth`;
///   3. a column's natural width is its widest cell (header included), never below
///      `kMinColumnWidth`;
///   4. every column is capped at `max(kMinColumnWidth, content / columns)` - the equal
///      share - so one long cell cannot starve its neighbours;
///   5. the cells left over once every column has `min(natural, cap)` go back to the
///      columns that are still short of their natural width (the ones that will wrap).
///      The cap exists to protect the *other* columns, so once they are satisfied the room
///      is theirs. A table whose columns all fit takes only what it needs: the spare is
///      left to the right of the table, never used to stretch a column.
struct TableWidths {
    bool fits = false;
    /// Content cells per column (borders and padding excluded), `columns()` entries.
    std::vector<int> colCells;
    /// The whole table in cells: `kTableBorderWidth(columns) + sum(colCells)`.
    int widthCells = 0;
};
TableWidths allocateTableWidths(const Table& table, int availCells);

/// The size `renderTable()` draws in, in cells: `w` is the table's own width (at most
/// `availCells`) and `h` its height - two border rows, one line per wrapped cell line, and
/// one header separator when there is a body. For the key-value fallback `w` is the widest
/// line (at most `availCells`) and `h` the number of lines.
///
/// Cost: one `ui::wrap()` per cell. The caller keeps the answer (a block reserves its rows once,
/// when its content changes) instead of measuring on every frame it draws.
struct TableSize {
    int w = 0;
    int h = 0;
};
TableSize measureTable(const Table& table, int availCells);

/// The vertical key-value layout of `table` - the fallback when a grid cannot fit:
/// one `"<header>: <value>"` line per cell of each body row, with one empty line between
/// rows. A cell whose header is empty draws as its own text. A table with no body rows
/// answers one line per header cell, so the fallback can never answer nothing. A body row
/// shorter than the header (a hand-built `Table`) contributes its missing cells as empty
/// values, so the line count never depends on a row's length.
///
/// This is the *in-renderer* degradation, not the source-only fallback of
/// `plan/table_fallback.md` (that one is the block's own markdown, drawn by the code-block
/// path). It is a public function because a caller that has no room for a grid at all - a
/// block row budget of three lines, say - needs the same text without a rect.
std::vector<std::string> tableKeyValueLines(const Table& table);

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

/// Draw `table` at `r` - the room the caller has, in cells - and nothing else.
///
/// Border glyphs are the box-drawing set, or `+ - |` on a surface whose `GlyphSet` is
/// ASCII; the header row is `accent`, the body `textHi`, the borders `lineStrong` (an
/// essential boundary - `lineSoft` is invisible at 256 colours). **No cell is filled**:
/// a table paints glyphs only, so the region surface behind it stays the region's.
///
/// Everything is clipped to `r`, a table taller than `r.h` stops at the last row that
/// fits, and a table wider than `r.w` degrades to the key-value layout rather than
/// overrunning. A rect in the wrong unit, or one with no room, draws nothing.
void renderTable(const Table& table, const Rect& r, const Metrics& m);

} // namespace llmfun::tui::ui
