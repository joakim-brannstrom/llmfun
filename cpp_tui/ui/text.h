/// Text and width utilities (design §4.15 "Text"): the one UTF-8-safe place
/// that measures, cuts and pads text.
///
/// The unit is the terminal **cell**, never a byte and never a codepoint: a CJK
/// ideograph takes two cells, a combining mark takes none, so "12 bytes" can be
/// nine cells or six. The text grid is built of cells and every label, chip and
/// table cell in it is measured in them, so the measure lives here once:
///
///     const int titleCells = ui::displayWidth(title);
///     draw(ui::truncate(title, budgetCells));
///     for (std::string_view lineCells : ui::wrap(body, widthCells))
///         drawLine(lineCells);
///
/// A cell measure is not a pixel measure: a GUI view sized in pixels measures
/// with `ImGui::CalcTextSize`, and only the text grid (and the cell-based table
/// renderer built on this module) uses these functions.
///
/// Widths come from a copy of `imtui_wcwidth()` in
/// `vendor/imgui/imgui/imgui_draw.cpp` - the exact table the ncurses backend
/// renders with - because that function is `static` inside the vendored
/// translation unit and `ui/` neither links ImGui nor may reach into it. The
/// copy is small next to the cost of the alternative (a shared table would make
/// the renderer depend on the app): `test_text` pins every range boundary and
/// sweeps all of Unicode against an independent oracle, and
/// `imtui_utf8_grid_test` renders a corpus through the real backend, so a drift
/// between the two tables fails a run rather than a screenshot.
///
/// Layering: `<string>`, `<string_view>` and `<vector>` only, which makes this
/// the one module every other one may include (`ui/`, `model/`, `views/` and the
/// store adapter).
///
/// Cost: these functions return their text by value, so a view calls them when
/// the value changes, not once per frame (§3.9: no per-frame formatting on a
/// render path). `wrap()` is the exception - its lines are views into the input
/// and only the vector is allocated.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace llmfun::tui::ui {

/// Cells `utf8` occupies: wide glyphs (East Asian Wide/Fullwidth, and the
/// emoji-presentation class the backend renders double-width) count 2, combining
/// marks and zero-width format characters count 0, everything else counts 1.
///
/// A sequence that is not well-formed UTF-8 measures 1 cell and consumes the
/// bytes its lead byte claims - capped by what is left and by a zero byte - the
/// renderer's own rule (`ImTextCharFromUtf8` returns that length and substitutes
/// one U+FFFD, drawn as one cell), so a truncated or invalid glyph cannot make a
/// line measure wider than the backend draws it. A stray continuation byte and
/// the 5/6-byte leads claim one byte each. The invalidity conditions are the
/// renderer's too (non-canonical, surrogate half, out of range, bad tail), so
/// the only inputs where the two can disagree are beyond corrupt text.
///
/// A `\0` is a control character and measures 0, like the carriage return: the
/// backend's width-0 rule draws nothing for either, so text after them still
/// lines up.
///
/// A newline measures 0 (it is a control character like any other) - call
/// `wrap()` first when the text has more than one line.
int displayWidth(std::string_view utf8);

/// `utf8` cut to at most `cells` cells, with `ui::icon(Icon::Ellipsis)` marking the
/// cut - "…" on a surface with the glyphs, "..." on one that gets the ASCII column,
/// so the module that cuts text cannot emit a glyph the surface cannot draw.
///
/// Text that already fits comes back byte-identical (a copy, not a re-encoding):
/// a caller compares labels and the golden screens compare cells, so an
/// untouched string must stay untouched.
///
/// When the cut happens, it happens between codepoints, at the last one that
/// fits beside the marker - never in the middle of a sequence, never leaving a
/// wide glyph half drawn. A zero-width mark costs nothing, so it stays with the
/// base glyph it belongs to ("e" alone is a different character from "e" plus
/// U+0301). The result is never wider than `cells`.
///
/// `cells <= 0` answers the empty string: a column with no room draws nothing.
/// A marker that does not fit in `cells` at all is itself cut (`cells` 1 with
/// "..." answers "."), because a marker that overflows would defeat the budget
/// it exists to respect.
///
/// `ui/text.cpp` includes `ui/icons.h` for that default (a leaf header, so this
/// module's include surface is unchanged); the marker is chosen per call, which is
/// what lets `LLMFUN_ASCII=1` reach truncation at all.
std::string truncate(std::string_view utf8, int cells);

/// The same cut with an explicit marker: "" or `nullptr` is a hard cut with no
/// marker at all, and any other string is measured in cells like the text it marks
/// (so a three-cell "..." cannot overflow a one-cell budget either).
std::string truncate(std::string_view utf8, int cells, const char* ellipsis);

/// `utf8` split into the lines `cells` can hold, one entry per line.
///
/// Lines are line-faithful: `count('\n') + 1` entries, the same split a plain
/// `find('\n')` walk produces, so "a\n" is two lines and the last one is empty
/// (that empty row is what the terminal draws). A paragraph that fits is one
/// entry with its bytes untouched, including its trailing spaces.
///
/// The fill is greedy at word boundaries: a run of spaces or tabs is where a
/// line may break, and the break consumes that run - it is drawn at neither end.
/// A word that cannot fit on a line of its own is broken at the cell budget
/// (CJK has no spaces, and a long path is one word), and a single glyph wider
/// than `cells` still gets a line of its own when the line is empty: a wide
/// glyph cannot be split, so the "never wider than `cells`" rule yields to
/// "never a partial sequence". A line that already holds cells (an indent, a
/// previous word) never overshoots: the word moves down instead.
///
/// `\r` is not stripped and measures 0 - normalise CRLF before wrapping. The
/// text grid's renderer drops a bare CR, but a cell-based consumer (the table
/// renderer) would otherwise write one into a cell.
///
/// Every entry is a view into `utf8` - no per-line allocation, and the caller
/// keeps the text alive as long as it reads the lines. `cells <= 0` answers no
/// lines: nothing can be drawn, and a visible empty line would be a lie about
/// the space that is available.
std::vector<std::string_view> wrap(std::string_view utf8, int cells);

/// `utf8` plus the spaces that bring it to `cells` cells (a no-op when it is
/// already at least that wide). Padding never cuts: a caller that needs a fixed
/// column of exactly `cells` calls `truncate()` first, because "pad" silently
/// dropping the tail is how a table cell loses its last digit.
std::string padRight(std::string_view utf8, int cells);

/// Compact tabular numerals for the places a number has to fit a chip or a
/// column: `0`, `999`, `1k`, `1.2k`, `42k`, `128k`, `1M`, `1G`.
///
/// One decimal below ten units and none above, so a value never reads as
/// `10.0k` - the rounded tenths become the unit (`9.95k` is `10k`) - and a carry
/// promotes the unit (`999999` is `1M`, never `1000k`). Rounding is half up, the
/// digits are plain ASCII with no grouping separator and no locale, so a column
/// of these is stable in a monospace grid. Units are the decimal ladder k, M, G,
/// T, P, E (10^3 .. 10^18); the sign of a negative value is kept in front.
std::string formatCount(long long v);

/// `utf8` in codepoints - the "characters" a *rule* counts, as opposed to the cells a
/// renderer draws.
///
/// A byte count is wrong for anything a person types: 5000 CJK characters are 15000
/// bytes, so a byte limit trips at a third of the text it promises (the composer's paste
/// question, design §4.6 / D-PASTE). A codepoint is one character for this purpose even
/// when it is a combining mark (`displayWidth` counts that one as zero cells - the two
/// questions are different and answer differently on purpose).
///
/// The walk is the cheap one: every byte that is not a UTF-8 continuation (`10xxxxxx`) is
/// one codepoint. A malformed sequence therefore counts its lead bytes, never fewer
/// characters than the string claims, which is the safe direction for a limit.
int utf8Count(std::string_view utf8);

/// The width `estimateWrappedLines()` measures at: the design's 80 columns, i.e. the
/// width a block is assumed to have before it has one of its own.
inline constexpr std::size_t kEstimateColumns = 80;

/// A rough count of the lines `utf8` wraps to at `kEstimateColumns`, for the pre-draw
/// height hint.
///
/// Deliberately arithmetic and allocation-free: the thread calls it for *every* message
/// on every rebuild (an appended message, a streamed update), and `wrap()` here built a
/// vector of line views per body - ~29 ms per streamed frame on the 1000-message
/// fixture, more than half the text grid's budget, spent on a hint that only the groups
/// in the window consume.
///
/// The count is rough in both directions and says so: it splits on newlines and divides
/// by the reference width, so a trailing newline counts its empty last line and a wide
/// glyph is one column here. That is enough for a hint: the drawn height replaces it
/// after the first frame (the anchor reads the drawn height, never this number).
int estimateWrappedLines(std::string_view utf8);

} // namespace llmfun::tui::ui
