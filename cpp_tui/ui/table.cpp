/// The text-grid table renderer (ui/table.h owns the contract and the arithmetic).
///
/// Four stages, in the order the header lists them: detect, parse, allocate, draw. The
/// file is organised the same way, and every stage is a pure function except the last -
/// which is why `./test_table` can pin the parser, the widths and the degradation without
/// a frame, and only the drawing needs the text backend. The bridge (`bridgeTables()`,
/// task 37) is the one step in front of them: it turns a body's tables into fenced blocks,
/// which is text in and text out and needs neither a frame nor a profile.
///
/// The rules that shape the drawing:
///
///   * **glyphs only, never a fill.** A table is text plus rules, so it paints no
///     background at all; the region surface behind it stays the region's (D-BLOCK). The
///     goldens pin that row by row - a filled cell would move a background index.
///   * **cell-exact.** A column is a whole number of cells, a line is drawn at the pen of
///     the cell it starts in, and a text run is never cut mid-glyph: `ui::wrap()` returns
///     lines that fit the column, so the renderer draws and never measures by itself.
///   * **inside the rect.** Everything is clipped to the rect, and a table taller than the
///     rect stops at the last row that fits instead of running into the next block.
///   * **no colour literal.** Every ink is a `ColorToken` resolved through `ui::color()`,
///     which is the rule `./test_metrics`' gate ledger checks for every file under `ui/`.
#include "ui/table.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "imgui/imgui.h"

#include "ui/icons.h"
#include "ui/metrics.h"
#include "ui/text.h"
#include "ui/tokens.h"

namespace llmfun::tui::ui {
namespace {

// ---------------------------------------------------------------------------
// The border glyphs
// ---------------------------------------------------------------------------
//
// Declared here rather than in `ui/icons.h`: they carry no state and have no word beside
// them (icons.h records the handoff). Each is one cell in both columns - a box glyph
// measures one cell in the shared width table, and `+ - |` are ASCII - which is what lets a
// border row be built as a string and drawn in one call.

struct Borders {
    const char* vertical;
    const char* horizontal;
};

const Borders kBoxBorders{"\u2502", "\u2500"}; // │ ─
const Borders kAsciiBorders{"|", "-"};

/// One of the three horizontal rules: the glyph at each end and between two columns.
struct BorderRow {
    const char* left;
    const char* middle;
    const char* right;
};

const BorderRow kTopRow{"\u250C", "\u252C", "\u2510"};    // ┌ ┬ ┐
const BorderRow kHeaderRow{"\u251C", "\u253C", "\u2524"}; // ├ ┼ ┤
const BorderRow kBottomRow{"\u2514", "\u2534", "\u2518"}; // └ ┴ ┘
const BorderRow kTopRowAscii{"+", "+", "+"};
const BorderRow kHeaderRowAscii{"+", "+", "+"};
const BorderRow kBottomRowAscii{"+", "+", "+"};

/// The plain-ASCII column: a terminal that cannot draw the box set (`TERM=linux`, a CJK
/// locale, `LLMFUN_ASCII=1`) gets `+ - |`, which is the arrangement the design calls out.
bool asciiBorders(GlyphSet set) { return set == GlyphSet::Ascii; }

const Borders& bordersFor(GlyphSet set) { return asciiBorders(set) ? kAsciiBorders : kBoxBorders; }

const BorderRow& rowFor(const BorderRow& glyphs, const BorderRow& ascii, GlyphSet set) {
    return asciiBorders(set) ? ascii : glyphs;
}

// ---------------------------------------------------------------------------
// Line and cell scanning, shared by detection and parsing
// ---------------------------------------------------------------------------

bool isSpace(char c) { return c == ' ' || c == '\t'; }

/// `text` without the spaces and tabs at either end. A carriage return is stripped by the
/// line splitter, not here, so a caller that hands a CRLF row still gets a clean cell.
std::string_view trimSpaces(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && isSpace(text[begin]))
        ++begin;
    while (end > begin && isSpace(text[end - 1]))
        --end;
    return text.substr(begin, end - begin);
}

/// One line of a body, and the byte range it occupies in that body.
struct Line {
    std::string_view text;
    std::size_t begin = 0;
    std::size_t end = 0;
};

/// `body` split on newlines, with a trailing carriage return stripped from each line.
///
/// Byte-faithful: `count('\n') + 1` lines, so a body that ends with a newline has a last
/// empty line (the table scanner relies on it to stop at the right place), and each line's
/// `[begin, end)` range is what a span points at.
std::vector<Line> splitLines(std::string_view body) {
    std::vector<Line> lines;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t newline = body.find('\n', pos);
        const std::size_t stop = newline == std::string_view::npos ? body.size() : newline;
        std::size_t end = stop;
        if (end > pos && body[end - 1] == '\r')
            --end;
        Line line;
        line.begin = pos;
        line.end = end;
        line.text = body.substr(pos, end - pos);
        lines.push_back(line);
        if (newline == std::string_view::npos)
            break;
        pos = newline + 1;
    }
    return lines;
}

/// The fence a line opens or closes, if it is a fence line at all.
struct Fence {
    char marker = 0;
    int length = 0;
};

/// A line's leading whitespace: its width in columns (a tab advances to the next multiple of
/// four, GFM's rule) and the byte count that produces it.
struct Indent {
    int columns = 0;
    std::size_t bytes = 0;
};

Indent leadingIndent(std::string_view text) {
    Indent out;
    std::size_t at = 0;
    while (at < text.size() && isSpace(text[at])) {
        if (text[at] == ' ')
            ++out.columns;
        else
            out.columns += 4 - out.columns % 4;
        ++at;
    }
    out.bytes = at;
    return out;
}

/// A fence line: three or more backticks or tildes, at most three columns past `container` (the
/// content column of the innermost list item, 0 at the top level). Deeper than that it is an
/// indented code block, where the line is text and a table in it is not a table. A backtick fence
/// whose line carries another backtick is inline code, not a fence.
bool fenceAt(std::string_view line, int container, Fence& out) {
    const Indent indent = leadingIndent(line);
    if (indent.columns > container + 3)
        return false;
    const std::string_view text = line.substr(indent.bytes);
    if (text.size() < 3)
        return false;
    const char marker = text.front();
    if (marker != '`' && marker != '~')
        return false;
    std::size_t length = 0;
    while (length < text.size() && text[length] == marker)
        ++length;
    if (length < 3)
        return false;
    if (marker == '`' && text.find('`', length) != std::string_view::npos)
        return false;
    out.marker = marker;
    out.length = static_cast<int>(length);
    return true;
}

/// A list item marker (up to three columns past `container`, a bullet or one to nine digits
/// with a delimiter, then one to four spaces) and the column its content starts at.
struct ListMarker {
    int contentColumn = 0;
};

bool listMarkerAt(std::string_view text, int container, ListMarker& out) {
    const Indent indent = leadingIndent(text);
    if (indent.columns > container + 3)
        return false;
    std::size_t at = indent.bytes;
    if (at >= text.size())
        return false;
    const char first = text[at];
    int markerWidth = 0;
    if (first == '-' || first == '*' || first == '+') {
        markerWidth = 1;
    } else if (std::isdigit(static_cast<unsigned char>(first))) {
        std::size_t digits = 0;
        while (at + digits < text.size() && digits < 9 &&
               std::isdigit(static_cast<unsigned char>(text[at + digits])))
            ++digits;
        if (digits == 0 || at + digits >= text.size() ||
            (text[at + digits] != '.' && text[at + digits] != ')'))
            return false;
        markerWidth = static_cast<int>(digits) + 1;
    } else {
        return false;
    }
    const int markerEnd = indent.columns + markerWidth;
    std::size_t after = at + markerWidth;
    if (after >= text.size()) {
        out.contentColumn = markerEnd + 1; // an empty item: one column past the marker
        return true;
    }
    if (!isSpace(text[after])) // the marker is followed by a space, a tab or the line's end
        return false;
    int column = markerEnd;
    while (after < text.size() && isSpace(text[after])) {
        column += text[after] == ' ' ? 1 : 4 - column % 4;
        ++after;
    }
    const int spaces = column - markerEnd;
    // Five or more spaces means the item's first block is an indented code block; CommonMark
    // then puts the content column one past the marker.
    out.contentColumn = markerEnd + (spaces >= 5 ? 1 : spaces);
    return true;
}

/// The innermost list item whose content column is at most `indent`, or 0 at the top level.
int containerAt(const std::vector<int>& listColumns, int indent) {
    for (auto it = listColumns.rbegin(); it != listColumns.rend(); ++it)
        if (indent >= *it)
            return *it;
    return 0;
}

/// `cells` of one row: the raw (untrimmed) cell views and whether the row carried a
/// top-level separator at all.
struct RawRow {
    std::vector<std::string_view> cells;
    bool separated = false;
};

std::size_t runLength(std::string_view text, std::size_t at, char c) {
    std::size_t length = 0;
    while (at + length < text.size() && text[at + length] == c)
        ++length;
    return length;
}

/// CommonMark's escape rule: a backslash escapes ASCII punctuation only, so `\*` is a
/// literal `*` while `\Users` keeps its backslash. Treating every following byte as
/// escapeable would delete the separators in a `Path` cell (`C:\Users\me`) and,
/// before a multi-byte character, leave half a sequence behind.
bool escapable(char c) {
    return c > 0x20 && c < 0x7F && std::isalnum(static_cast<unsigned char>(c)) == 0;
}

/// The next run of exactly `run` copies of `c` at or after `from`, or npos - the raw scan a
/// code span's closing fence uses.
std::size_t findRun(std::string_view text, std::size_t from, char c, std::size_t run) {
    for (std::size_t at = from; at < text.size();) {
        if (text[at] != c) {
            ++at;
            continue;
        }
        const std::size_t length = runLength(text, at, c);
        if (length == run)
            return at;
        at += length;
    }
    return std::string_view::npos;
}

/// Split one table row into cells.
///
/// The outer pipes are optional; `\|` is an escaped pipe and stays in the cell (the plain-text
/// pass unescapes it); a pipe inside a backtick run is text, because inline code is opaque. A
/// run of N backticks opens a code span that only a run of exactly N closes - and a run that
/// nothing closes is *literal*, so a lone backtick in a cell cannot swallow the row's
/// separators (which would fold the remaining columns into it, and in a header row would make
/// the delimiter look mismatched and the whole table disappear).
RawRow splitRow(std::string_view line) {
    RawRow row;
    const std::string_view text = trimSpaces(line);
    std::size_t pos = 0;
    if (!text.empty() && text.front() == '|') {
        row.separated = true;
        pos = 1;
    }
    std::size_t start = pos;
    std::size_t codeRun = 0;
    std::size_t lastSeparator = std::string_view::npos;
    while (pos < text.size()) {
        const char c = text[pos];
        if (c == '\\' && pos + 1 < text.size() && escapable(text[pos + 1])) {
            pos += 2;
            continue;
        }
        if (c == '`') {
            const std::size_t run = runLength(text, pos, '`');
            if (codeRun == 0) {
                if (findRun(text, pos + run, '`', run) != std::string_view::npos)
                    codeRun = run;
            } else if (codeRun == run) {
                codeRun = 0;
            }
            pos += run;
            continue;
        }
        if (c == '|' && codeRun == 0) {
            row.cells.push_back(text.substr(start, pos - start));
            row.separated = true;
            lastSeparator = pos;
            start = pos + 1;
        }
        ++pos;
    }
    row.cells.push_back(text.substr(start));
    // A row that ends with its closing pipe does not have an empty last cell.
    if (lastSeparator != std::string_view::npos && lastSeparator + 1 == text.size())
        row.cells.pop_back();
    if (row.cells.empty())
        row.cells.emplace_back();
    return row;
}

/// The alignment a delimiter cell spells, and whether it spells a legal one at all:
/// `:?-+:?`. A cell with no hyphen is not a delimiter, and a single `:` is neither.
bool delimiterAlign(std::string_view cell, int& align) {
    const std::string_view text = trimSpaces(cell);
    if (text.empty())
        return false;
    const bool left = text.front() == ':';
    const bool right = text.back() == ':';
    std::string_view hyphens = text;
    if (left)
        hyphens.remove_prefix(1);
    if (right && !hyphens.empty())
        hyphens.remove_suffix(1);
    if (hyphens.empty())
        return false;
    for (const char c : hyphens)
        if (c != '-')
            return false;
    align = left && right ? kAlignCenter : (right ? kAlignRight : kAlignLeft);
    return true;
}

/// The delimiter row of a `columns`-wide table: one legal alignment cell per column, and a
/// top-level separator (so a setext heading's `---` is not a one-column delimiter).
bool parseDelimiterRow(std::string_view line, std::size_t columns, std::vector<int>& align) {
    const RawRow row = splitRow(line);
    if (!row.separated || row.cells.size() != columns)
        return false;
    align.assign(columns, kAlignLeft);
    for (std::size_t c = 0; c < columns; ++c)
        if (!delimiterAlign(row.cells[c], align[c]))
            return false;
    return true;
}

// ---------------------------------------------------------------------------
// Plain cell text (MVP: inline styling is P3)
// ---------------------------------------------------------------------------

bool wordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

/// GFM's intraword rule for `_`: an underscore delimiter must not sit inside a word, which
/// is what keeps `read_file` a tool name instead of italic fragments.
bool flanked(std::string_view text, std::size_t at, std::size_t run) {
    const bool before = at == 0 || !wordChar(text[at - 1]);
    const bool after = at + run >= text.size() || !wordChar(text[at + run]);
    return before && after;
}

/// The run of `c` starting at `at` opens emphasis, and only a run with content around it
/// closes it, when content sits between them: `**bold**` opens before `b` and closes after
/// `d`, while `* a *` is text. That is GFM's left/right flanking rule reduced to the one
/// thing a table cell needs from it. `_` additionally keeps GFM's intraword rule on both
/// sides, which is what keeps `read_file` a tool name instead of italic fragments.
bool opensMark(std::string_view text, std::size_t at, std::size_t run) {
    return at + run < text.size() && !isSpace(text[at + run]);
}

/// The next run of exactly `run` copies of `c` at or after `from` that closes emphasis, or
/// npos.
std::size_t findClose(std::string_view text, std::size_t from, char c, std::size_t run) {
    for (std::size_t at = from; at < text.size();) {
        if (text[at] != c) {
            ++at;
            continue;
        }
        const std::size_t length = runLength(text, at, c);
        const bool closes = length == run && at > from && !isSpace(text[at - 1]) &&
                            (c != '_' || flanked(text, at, run));
        if (closes)
            return at;
        at += length;
    }
    return std::string_view::npos;
}

/// The `)` that closes a link destination opened at `from`, honouring nested parentheses:
/// `[x](https://e/x_(y))` ends at the outer `)`, not at the `)` inside the URL. npos when the
/// destination is not closed on this line, which leaves the construct as plain text.
std::size_t closingParen(std::string_view text, std::size_t from) {
    int depth = 1;
    for (std::size_t at = from; at < text.size(); ++at) {
        // An escaped paren is part of the destination, the same way `plainInline` reads escapes.
        if (text[at] == '\\' && at + 1 < text.size() && escapable(text[at + 1])) {
            ++at;
            continue;
        }
        if (text[at] == ')' && --depth == 0)
            return at;
        if (text[at] == '(')
            ++depth;
    }
    return std::string_view::npos;
}

/// The text a grid cell draws: the inline markdown a model writes reduced to what one cell
/// can hold.
///
/// `**bold**`, `*em*`, `__bold__`, `_em_` and `~~struck~~` lose their markers and keep their
/// content; `` `code` `` keeps its content verbatim (pipes and all - that is what "opaque"
/// means for the parser too); `[label](url)` keeps the label (and `![alt](src)` the alt); a
/// backslash before ASCII punctuation keeps that punctuation (`\*` is `*`, `C:\Users` stays a
/// path). A marker that has no partner is text: a lone `*` in `read_file(*.cpp)` or a single
/// `~` stays, because dropping it would corrupt a name the user has to read.
///
/// Nesting is resolved by recursion, so `**a *b* c**` keeps both layers; the depth is bounded
/// by the number of unclosed brackets in a cell, never by the input length.
std::string plainInline(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size()) {
        const char c = text[at];
        if (c == '\\' && at + 1 < text.size() && escapable(text[at + 1])) {
            out += text[at + 1];
            at += 2;
            continue;
        }
        if (c == '`') {
            const std::size_t run = runLength(text, at, '`');
            const std::size_t close = findRun(text, at + run, '`', run);
            if (close != std::string_view::npos) {
                out.append(text.substr(at + run, close - (at + run)));
                at = close + run;
                continue;
            }
            out.append(text.substr(at, run));
            at += run;
            continue;
        }
        if (c == '[') {
            const std::size_t close = text.find(']', at + 1);
            if (close != std::string_view::npos && close + 1 < text.size() &&
                text[close + 1] == '(') {
                const std::size_t end = closingParen(text, close + 2);
                if (end != std::string_view::npos) {
                    out += plainInline(text.substr(at + 1, close - at - 1));
                    at = end + 1;
                    continue;
                }
            }
        }
        if (c == '!' && at + 1 < text.size() && text[at + 1] == '[') {
            // An image keeps its alt text; the `[` below does the work.
            ++at;
            continue;
        }
        if (c == '*' || c == '_' || c == '~') {
            const std::size_t run = runLength(text, at, c);
            const bool marked = opensMark(text, at, run) && (c != '_' || flanked(text, at, run)) &&
                                (c != '~' || run == 2);
            if (marked) {
                const std::size_t close = findClose(text, at + run, c, run);
                if (close != std::string_view::npos) {
                    out += plainInline(text.substr(at + run, close - (at + run)));
                    at = close + run;
                    continue;
                }
            }
            out.append(text.substr(at, run));
            at += run;
            continue;
        }
        out += c;
        ++at;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Draw helpers
// ---------------------------------------------------------------------------

/// `glyph` repeated `times` times. Appending with a count would cut a multi-byte box glyph
/// into bytes, so the run is built from whole copies.
void appendRepeat(std::string& out, const char* glyph, int times) {
    const std::string_view copy(glyph);
    for (int i = 0; i < times; ++i)
        out += copy;
}

/// One horizontal rule, `widthCells` wide: `[left][run][middle][run]...[right]`.
///
/// Built as a string because every border glyph is exactly one cell in both columns, so the
/// rule is one `AddText` instead of one per cell.
std::string borderLine(const Borders& borders, const BorderRow& row,
                       const std::vector<int>& colCells) {
    std::string line(row.left);
    for (std::size_t c = 0; c < colCells.size(); ++c) {
        appendRepeat(line, borders.horizontal, colCells[c] + 2 * kCellPadding);
        line += c + 1 < colCells.size() ? row.middle : row.right;
    }
    return line;
}

} // namespace

// ---------------------------------------------------------------------------
// Detect
// ---------------------------------------------------------------------------

std::vector<GfmTableSpan> findGfmTables(std::string_view body) {
    std::vector<GfmTableSpan> spans;
    const std::vector<Line> lines = splitLines(body);
    bool inFence = false;
    char fenceMarker = 0;
    int fenceLength = 0;
    // The list items the walk is inside, innermost last. Their content columns are what an
    // indented code block is counted from, so a table nested in a list item is still a table.
    std::vector<int> listColumns;
    bool afterBlank = true; // the body's start is a block start

    std::size_t index = 0;
    while (index < lines.size()) {
        const std::string_view text = lines[index].text;
        const Indent indent = leadingIndent(text);
        // The container this line starts in, before the line changes the stack: a marker's own
        // content column is where its children start, not where the marker itself sits.
        const int container = containerAt(listColumns, indent.columns);
        Fence fence;
        if (inFence) {
            if (fenceAt(text, container, fence) && fence.marker == fenceMarker &&
                fence.length >= fenceLength)
                inFence = false;
            afterBlank = !inFence;
            ++index;
            continue;
        }
        if (fenceAt(text, container, fence)) {
            // A fence is a block start: it closes every item its indent leaves behind.
            while (!listColumns.empty() && indent.columns < listColumns.back())
                listColumns.pop_back();
            inFence = true;
            fenceMarker = fence.marker;
            fenceLength = fence.length;
            afterBlank = false;
            ++index;
            continue;
        }
        if (trimSpaces(text).empty()) {
            afterBlank = true;
            ++index;
            continue;
        }
        ListMarker marker;
        if (listMarkerAt(text, container, marker)) {
            // A marker at a smaller indent is a sibling or a parent, not a child.
            while (!listColumns.empty() && indent.columns < listColumns.back())
                listColumns.pop_back();
            listColumns.push_back(marker.contentColumn);
        } else if (afterBlank) {
            // A non-blank line after a blank line is a block start; a paragraph's lazy
            // continuation is not, so it keeps the item open (the safe direction: bridge).
            while (!listColumns.empty() && indent.columns < listColumns.back())
                listColumns.pop_back();
        }
        afterBlank = false;
        if (indent.columns >= container + 4) {
            // Four columns past the container is an indented code block: a table-like line in
            // it is code, and the lines after it are scanned one by one like this one.
            ++index;
            continue;
        }
        if (index + 1 >= lines.size())
            break;
        const RawRow header = splitRow(text);
        std::vector<int> align;
        if (!header.separated || header.cells.empty() ||
            !parseDelimiterRow(lines[index + 1].text, header.cells.size(), align)) {
            ++index;
            continue;
        }
        // The delimiter row is a paragraph continuation for md4c, so it may be indented
        // further; it must still belong to the container (a delimiter behind the item ends
        // the table before it starts).
        const Indent delimiter = leadingIndent(lines[index + 1].text);
        if (delimiter.columns < container) {
            ++index;
            continue;
        }
        // The body runs while a line still looks like a row in this container; a blank line,
        // another fence and anything without a top-level separator end it.
        std::size_t last = index + 1;
        for (std::size_t row = index + 2; row < lines.size(); ++row) {
            if (trimSpaces(lines[row].text).empty())
                break;
            const Indent rowIndent = leadingIndent(lines[row].text);
            if (rowIndent.columns < container || rowIndent.columns >= container + 4)
                break;
            Fence unexpected;
            if (fenceAt(lines[row].text, container, unexpected))
                break;
            if (!splitRow(lines[row].text).separated)
                break;
            last = row;
        }
        // The range ends after the last line's carriage return when the body is CRLF, so a caller
        // that replaces the range cannot strand a CR in front of what it wrote.
        std::size_t end = lines[last].end;
        if (end < body.size() && body[end] == '\r')
            ++end;
        spans.push_back(GfmTableSpan{lines[index].begin, end, container});
        index = last + 1;
        afterBlank = true; // a table is a whole block: the next line starts a new one
    }
    return spans;
}

// ---------------------------------------------------------------------------
// Bridge
// ---------------------------------------------------------------------------

bool bridgeTables(std::string_view body, bool showSource, std::string& out) {
    const std::vector<GfmTableSpan> spans = findGfmTables(body);
    if (spans.empty())
        return false;
    const char* language = showSource ? kTableSourceFenceLanguage : kTableFenceLanguage;
    std::string text;
    // A fence costs its two delimiter lines, the language name and the container's indent; the
    // tables themselves are copied as they are, so the estimate is the body plus that per table.
    text.reserve(body.size() + spans.size() * (std::strlen(language) + 16));
    std::size_t copied = 0;
    std::size_t rewritten = 0;
    for (const GfmTableSpan& span : spans) {
        // A span list is only replayable while it is in document order and inside the body;
        // anything else is a caller's bug, and appending the rest verbatim is the harmless
        // answer (the guard exists so a malformed span cannot read out of bounds).
        if (span.begin < copied || span.end < span.begin || span.end > body.size())
            continue;
        text.append(body.substr(copied, span.begin - copied));
        // The fence sits at the container's content column. At column zero it would end the
        // list item, and a following indented paragraph would become an indented code block.
        const std::size_t indent =
            span.containerIndent > 0 ? static_cast<std::size_t>(span.containerIndent) : 0;
        text.append(indent, ' ');
        text.append("```");
        text.append(language);
        text.push_back('\n');
        // The table's own lines. A carriage return is a line ending, never content: the fence
        // is written with LF and `parseGfmTable()` reads either, so a CRLF body bridges into
        // the same fence a LF body does.
        for (std::size_t i = span.begin; i < span.end; ++i)
            if (body[i] != '\r')
                text.push_back(body[i]);
        // The span ends at the last line's last cell (no trailing newline), so this one ends
        // the table's last line and the next `push_back`-ed fence is on a line of its own.
        text.push_back('\n');
        text.append(indent, ' ');
        text.append("```");
        copied = span.end;
        ++rewritten;
    }
    if (rewritten == 0)
        return false; // a malformed span list: the body is untouched
    text.append(body.substr(copied));
    out = std::move(text);
    return true;
}

bool bridgeBodyForProfile(bool textGrid, std::string_view body, bool showSource, std::string& out) {
    // The bridge is a text-grid correction (design §6.8, D-TABLE): the GUI keeps rich_md's
    // native tables, so its markdown must pass through byte-identical. The caller measured the
    // profile (`ui/` cannot read it), and `bodyHasTable()` takes the same flag.
    if (!textGrid)
        return false;
    // `out` is written by `bridgeTables()` alone, and only when it rewrote something: the
    // caller's copy is the transform's output, never a copy it has to make first.
    return bridgeTables(body, showSource, out);
}

bool bodyHasTable(bool textGrid, std::string_view body) {
    // The detector is the answer, not the transform: it is the same span list `bridgeTables()`
    // rewrites, so a toggle can never offer a body the renderer would not rewrite (and pays a
    // scan rather than building the bridged copy). A verbatim body answers false here because
    // its caller never asks - the row is drawn as markdown only when the body is markdown.
    return textGrid && !findGfmTables(body).empty();
}

// ---------------------------------------------------------------------------
// Parse
// ---------------------------------------------------------------------------

bool isTableRowLine(std::string_view line) {
    // One reader of "what a row is" (`splitRow`): a top-level separator, a `|` outside inline
    // code. The table search above and the live answer's caret both come through here.
    return splitRow(line).separated;
}

bool parseGfmTable(std::string_view block, Table& out) {
    const std::vector<Line> lines = splitLines(block);
    std::size_t first = 0;
    std::size_t last = lines.size();
    while (first < last && trimSpaces(lines[first].text).empty())
        ++first;
    while (last > first && trimSpaces(lines[last - 1].text).empty())
        --last;
    if (last - first < 2)
        return false;

    const RawRow header = splitRow(lines[first].text);
    std::vector<int> align;
    if (!header.separated || header.cells.empty())
        return false;
    if (!parseDelimiterRow(lines[first + 1].text, header.cells.size(), align))
        return false;

    const std::size_t columns = header.cells.size();
    Table table;
    table.rows.reserve(last - first - 1);
    std::vector<TableCell> headRow;
    headRow.reserve(columns);
    for (std::size_t c = 0; c < columns; ++c)
        headRow.push_back(TableCell{plainInline(trimSpaces(header.cells[c])), true, align[c]});
    table.rows.push_back(std::move(headRow));

    for (std::size_t index = first + 2; index < last; ++index) {
        // An interior blank line means the block is a document with a table in it, not a
        // table: the caller asked the wrong question and gets "no".
        if (trimSpaces(lines[index].text).empty())
            return false;
        const RawRow row = splitRow(lines[index].text);
        std::vector<TableCell> cells;
        cells.reserve(columns);
        for (std::size_t c = 0; c < columns; ++c) {
            const std::string_view raw =
                c < row.cells.size() ? trimSpaces(row.cells[c]) : std::string_view{};
            cells.push_back(TableCell{plainInline(raw), false, align[c]});
        }
        table.rows.push_back(std::move(cells));
    }
    out = std::move(table);
    return true;
}

// ---------------------------------------------------------------------------
// Allocate
// ---------------------------------------------------------------------------

TableWidths allocateTableWidths(const Table& table, int availCells) {
    TableWidths widths;
    const int columns = table.columns();
    if (table.empty() || columns <= 0 || columns > kMaxTableColumns)
        return widths;
    if (availCells < kMinTableWidth)
        return widths;
    const int chrome = kTableBorderWidth(columns);
    const int content = availCells - chrome;
    if (content < columns * kMinColumnWidth)
        return widths;
    const int cap = std::max(kMinColumnWidth, content / columns);

    std::vector<int> natural(static_cast<std::size_t>(columns), kMinColumnWidth);
    for (const std::vector<TableCell>& row : table.rows) {
        // A `Table` is a public aggregate, so a hand-built row can be shorter than the header
        // (the parser never produces one): clamp instead of reading past it.
        const std::size_t cells = std::min(row.size(), static_cast<std::size_t>(columns));
        for (std::size_t c = 0; c < cells; ++c)
            natural[c] = std::max(natural[c], displayWidth(row[c].text));
    }

    std::vector<int> assigned(static_cast<std::size_t>(columns), 0);
    int spare = content;
    for (int c = 0; c < columns; ++c) {
        const std::size_t at = static_cast<std::size_t>(c);
        assigned[at] = std::min(natural[at], cap);
        spare -= assigned[at];
    }
    // The cap protects a column's neighbours, so once the neighbours have what they need the
    // room belongs to the columns that still wrap. A table whose columns all fit keeps its
    // spare: a table is not stretched to the width of the block that holds it.
    while (spare > 0) {
        int pick = -1;
        int pickNeed = 0;
        for (int c = 0; c < columns; ++c) {
            const std::size_t at = static_cast<std::size_t>(c);
            const int need = natural[at] - assigned[at];
            if (need > pickNeed) {
                pick = c;
                pickNeed = need;
            }
        }
        if (pick < 0)
            break;
        ++assigned[static_cast<std::size_t>(pick)];
        --spare;
    }

    widths.fits = true;
    widths.colCells = std::move(assigned);
    widths.widthCells = chrome;
    for (const int cells : widths.colCells)
        widths.widthCells += cells;
    return widths;
}

namespace {

/// One row's drawn lines and the height they need: `lines[c]` is what `ui::wrap()` answered for
/// column `c` at that column's width (at least one line, so an empty cell still occupies its
/// row) and `height` is the tallest of them.
///
/// One call answers both, so the measure and the draw wrap each cell once - the two used to wrap
/// it separately, and a caller that measured before drawing paid for four wraps per cell.
struct RowLines {
    std::vector<std::vector<std::string_view>> lines;
    int height = 1;
};

RowLines wrappedRow(const std::vector<TableCell>& row, const std::vector<int>& colCells) {
    RowLines out;
    out.lines.resize(colCells.size());
    // A `Table` is a public aggregate: a hand-built row may be shorter than the header (the
    // parser never produces one), and reading past it would be a crash, not a layout.
    const std::size_t cells = std::min(row.size(), colCells.size());
    for (std::size_t c = 0; c < cells; ++c)
        out.lines[c] = wrap(row[c].text, colCells[c]);
    for (const std::vector<std::string_view>& column : out.lines)
        out.height = std::max(out.height, static_cast<int>(column.size()));
    return out;
}

/// The cell offset a line starts at inside its column, from the column's alignment and the
/// line's own width. Slack is never negative (`ui::wrap()` guarantees it) and a half cell
/// rounds down, like every other length on the grid.
int alignOffset(int alignment, int columnCells, std::string_view line) {
    const int slack = columnCells - displayWidth(line);
    if (slack <= 0)
        return 0;
    if (alignment == kAlignRight)
        return slack;
    if (alignment == kAlignCenter)
        return slack / 2;
    return 0;
}

} // namespace

TableSize measureTable(const Table& table, int availCells) {
    const TableWidths widths = allocateTableWidths(table, availCells);
    if (!widths.fits) {
        const std::vector<std::string> lines = tableKeyValueLines(table);
        int widest = 0;
        for (const std::string& line : lines)
            widest = std::max(widest, displayWidth(line));
        return TableSize{std::min(availCells, widest), static_cast<int>(lines.size())};
    }
    int height = 2; // the top and bottom rules
    for (const std::vector<TableCell>& row : table.rows)
        height += wrappedRow(row, widths.colCells).height;
    if (table.rows.size() > 1)
        ++height; // the header rule
    return TableSize{widths.widthCells, height};
}

std::vector<std::string> tableKeyValueLines(const Table& table) {
    std::vector<std::string> lines;
    const int columns = table.columns();
    if (columns <= 0)
        return lines;
    const std::vector<TableCell>& header = table.rows.front();
    if (table.rows.size() == 1) {
        // No body: the header cells are all the table has, so the fallback is never empty.
        for (const TableCell& cell : header)
            lines.push_back(cell.text);
        return lines;
    }
    for (std::size_t r = 1; r < table.rows.size(); ++r) {
        if (r > 1)
            lines.emplace_back(); // a blank line between rows keeps two rows readable
        // Same clamp as the grid: a hand-built row may be shorter than the header, and a
        // missing cell is an empty value, not a read past the row.
        const std::vector<TableCell>& row = table.rows[r];
        const std::size_t have = std::min(row.size(), static_cast<std::size_t>(columns));
        for (int c = 0; c < columns; ++c) {
            const std::string& key = header[static_cast<std::size_t>(c)].text;
            std::string line;
            if (!key.empty()) {
                line += key;
                line += ": ";
            }
            if (static_cast<std::size_t>(c) < have)
                line += row[static_cast<std::size_t>(c)].text;
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

namespace {

ImU32 inkOf(ColorToken token) { return ImGui::GetColorU32(color(token)); }

/// The cell offset of every column boundary: `left[c]` is the column's left border, and
/// `left[columns]` the right one, so the table is `left[columns] + 1` cells wide.
std::vector<int> columnOffsets(const std::vector<int>& colCells) {
    std::vector<int> left(colCells.size() + 1, 0);
    for (std::size_t c = 0; c < colCells.size(); ++c)
        left[c + 1] = left[c] + 1 + kCellPadding + colCells[c] + kCellPadding;
    return left;
}

/// One horizontal rule at `row` (an offset from the rect's top), drawn in one `AddText`: every
/// border glyph is one cell, so a whole rule is one string.
void drawRule(ImDrawList* dl, const Rect& r, int row, const std::string& line, ImU32 ink) {
    dl->AddText(ImVec2(r.x, r.y + static_cast<float>(row)), ink, line.c_str(),
                line.c_str() + line.size());
}

/// The glyph set this surface draws with, resolved once per context-frame.
///
/// `textGlyphSet()` reads up to five environment variables per call (`icons.h` says so, and a
/// table resolves it for every glyph it draws), so the answer is memoized the way
/// `ui/primitives.cpp` memoizes it: keyed on the ImGui context *and* the frame counter, so a
/// changed `LLMFUN_ASCII` still takes effect on the next frame and a second context cannot
/// answer with the first one's set. It is local rather than shared with `ui/primitives.h` to
/// keep this module's include surface at `ui/{icons,text,tokens,metrics}.h` - pulling in the
/// widget vocabulary for one resolver would be the larger coupling.
GlyphSet gridGlyphs() {
    static ImGuiContext* cachedContext = nullptr;
    static int cachedFrame = -1;
    static GlyphSet cached = GlyphSet::Glyphs;
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr)
        return textGlyphSet();
    const int frame = ImGui::GetFrameCount();
    if (context != cachedContext || frame != cachedFrame) {
        cachedContext = context;
        cachedFrame = frame;
        cached = textGlyphSet();
    }
    return cached;
}

void drawKeyValue(const Table& table, const Rect& r, int availCells, int budgetRows,
                  ImDrawList* dl) {
    const std::vector<std::string> lines = tableKeyValueLines(table);
    const GlyphSet glyphs = gridGlyphs();
    const ImU32 ink = inkOf(ColorToken::textHi);
    for (int index = 0; index < static_cast<int>(lines.size()) && index < budgetRows; ++index) {
        if (lines[static_cast<std::size_t>(index)].empty())
            continue;
        const std::string fitted = truncate(lines[static_cast<std::size_t>(index)], availCells,
                                            icon(Icon::Ellipsis, glyphs));
        dl->AddText(ImVec2(r.x, r.y + static_cast<float>(index)), ink, fitted.c_str(),
                    fitted.c_str() + fitted.size());
    }
}

void drawGrid(const Table& table, const TableWidths& widths, const Rect& r, int budgetRows,
              ImDrawList* dl) {
    const GlyphSet glyphs = gridGlyphs();
    const Borders& borders = bordersFor(glyphs);
    const ImU32 borderInk = inkOf(ColorToken::lineStrong);
    const ImU32 headerInk = inkOf(ColorToken::accent);
    const ImU32 bodyInk = inkOf(ColorToken::textHi);
    const std::vector<int> left = columnOffsets(widths.colCells);
    const int columns = table.columns();

    int row = 0;
    drawRule(dl, r, row,
             borderLine(borders, rowFor(kTopRow, kTopRowAscii, glyphs), widths.colCells),
             borderInk);
    ++row;

    for (std::size_t index = 0; index < table.rows.size(); ++index) {
        const std::vector<TableCell>& cells = table.rows[index];
        const RowLines rowLines = wrappedRow(cells, widths.colCells);
        const std::vector<std::vector<std::string_view>>& lines = rowLines.lines;
        const int height = rowLines.height;
        // The header is `rows[0]`, not "the row whose first cell says header": a hand-built
        // row can be empty, and `front()` on an empty row is a crash.
        const ImU32 ink = index == 0 ? headerInk : bodyInk;
        for (int line = 0; line < height; ++line) {
            if (row >= budgetRows)
                return;
            for (int c = 0; c < columns; ++c) {
                const std::vector<std::string_view>& column = lines[static_cast<std::size_t>(c)];
                // A column a short row does not have has no lines, so this `continue` is what
                // keeps the `cells[c]` read below safe on a hand-built ragged table.
                if (line >= static_cast<int>(column.size()))
                    continue;
                const std::string_view text = column[static_cast<std::size_t>(line)];
                if (text.empty())
                    continue;
                const int columnCells = widths.colCells[static_cast<std::size_t>(c)];
                const int offset =
                    alignOffset(cells[static_cast<std::size_t>(c)].align, columnCells, text);
                const int x = left[static_cast<std::size_t>(c)] + 1 + kCellPadding + offset;
                dl->AddText(
                    ImVec2(r.x + static_cast<float>(x), r.y + static_cast<float>(row + line)), ink,
                    text.data(), text.data() + text.size());
            }
            for (int boundary = 0; boundary <= columns; ++boundary)
                dl->AddText(
                    ImVec2(r.x + static_cast<float>(left[static_cast<std::size_t>(boundary)]),
                           r.y + static_cast<float>(row + line)),
                    borderInk, borders.vertical);
        }
        row += height;
        if (index == 0 && table.rows.size() > 1) {
            if (row >= budgetRows)
                return;
            drawRule(
                dl, r, row,
                borderLine(borders, rowFor(kHeaderRow, kHeaderRowAscii, glyphs), widths.colCells),
                borderInk);
            ++row;
        }
    }
    if (row < budgetRows)
        drawRule(dl, r, row,
                 borderLine(borders, rowFor(kBottomRow, kBottomRowAscii, glyphs), widths.colCells),
                 borderInk);
}

} // namespace

void renderTable(const Table& table, const Rect& r, const Metrics& m) {
    IM_ASSERT(r.cells == m.textGrid &&
              "Rect.cells must state the unit the Metrics were measured in");
    // The pixel profile keeps rich_md's native tables (design section 6.8): a cell renderer
    // handed a pixel rect would draw a grid the font cannot line up, so it draws nothing.
    if (!r.cells || !m.textGrid || r.w <= 0.0f || r.h <= 0.0f)
        return;
    const int availCells = static_cast<int>(roundCells(r.w));
    const int budgetRows = static_cast<int>(roundCells(r.h));
    if (availCells <= 0 || budgetRows <= 0)
        return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (dl == nullptr)
        return;
    const TableWidths widths = allocateTableWidths(table, availCells);
    dl->PushClipRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), true);
    if (widths.fits)
        drawGrid(table, widths, r, budgetRows, dl);
    else
        drawKeyValue(table, r, availCells, budgetRows, dl);
    dl->PopClipRect();
}

} // namespace llmfun::tui::ui
