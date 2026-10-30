/// The rich_md seam (ui/markdown.h owns the contract).
///
/// This translation unit is the only place in the workbench that talks to
/// rich_md. Every gate the design puts on that library lives here, one function
/// each: `RenderRaw` instead of `Render` (no transclusion file reads), a stable
/// outer id per block, `RichMd::Style` bridged from the Ink tokens, and the
/// per-profile selection policy. A caller that renders model text therefore
/// cannot get any of them wrong on its own.
#include "ui/markdown.h"

#include "ui/metrics.h"
#include "ui/table.h"
#include "ui/text.h"
#include "ui/tokens.h"

#include "imgui/imgui_internal.h"
#include "imgui_rich_md/rich_md.h"

#include <algorithm>
#include <string>
#include <vector>

namespace llmfun::tui::ui {
namespace {

/// The context renders draw through (`setMarkdownContext`). One per process,
/// like rich_md's own current context - a second TuiState in one process is not
/// a configuration the app has, and the wrapper states that instead of pretending
/// otherwise.
RichMd::Context* gContext = nullptr;

/// The theme the context's style was last written from, and whether it is known.
/// Re-applied when a caller passes something else, which is what keeps the
/// parameter path honest without a style write per frame.
MarkdownTheme gAppliedTheme;
bool gApplied = false;

MarkdownCounters gCounters;

/// How many `beginBlock()` scopes are open. Only used to notice a render that no
/// block wrapped; nesting itself needs no record, because `endBlock()` pops the
/// ImGui id stack that `beginBlock()` pushed.
int gBlockDepth = 0;

bool sameColor(const ImVec4& a, const ImVec4& b) {
    // Exact comparison is the right one here: both sides are the same token read
    // through the same accessor, not two independently computed lengths.
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

bool sameTheme(const MarkdownTheme& a, const MarkdownTheme& b) {
    return sameColor(a.link, b.link) && sameColor(a.inlineCode, b.inlineCode) &&
           sameColor(a.quoteBar, b.quoteBar) && sameColor(a.admonitionNote, b.admonitionNote) &&
           sameColor(a.admonitionWarn, b.admonitionWarn) && sameColor(a.markFill, b.markFill) &&
           a.blockGap == b.blockGap && a.headerGap == b.headerGap &&
           a.subSupScale == b.subSupScale && a.textGrid == b.textGrid;
}

/// The context renders use, made current: rich_md keeps its current context in
/// one process-wide slot, and a second context (created by a test, or left over
/// from a backend switch) must not capture this frame's draws.
RichMd::Context* activeContext() {
    if (gContext == nullptr)
        return nullptr;
    if (RichMd::GetCurrentContext() != gContext)
        RichMd::SetCurrentContext(gContext);
    return gContext;
}

// ---------------------------------------------------------------------------
// The table bridge's fenced renderers (design §6.8, task 37)
// ---------------------------------------------------------------------------
//
// The store adapter rewrites a body's top-level tables into fenced blocks
// (`ui::bridgeTables()`); these two renderers are what draws them. Two rules shape both:
//
//   * **no child window.** rich_md's own code block lives in one, which is why it has a
//     4-row minimum and a filled background; a table is text plus rules on the grid, so it
//     is drawn straight into the current window's draw list and the flow is advanced by
//     hand (`ItemSize`). Nothing here begins a window.
//   * **never blank.** A fence that does not parse - a model that writes the magic language
//     itself, a table the parser rejects - draws its own markdown as the verbatim block the
//     `s` toggle shows. The source is always in the block, so the fallback needs no state.

/// Draw `code` as a verbatim block: one row per line, cut to the room (a code block does not
/// wrap - a wrapped table row would be unreadable), in the verbatim voice's ink, with no fill
/// (D-BLOCK). The flow is advanced past the block, which is what keeps the next markdown
/// fragment off it.
void drawSourceLines(std::string_view code, const Metrics& m, ImVec2 start, float availW) {
    const int availCells = static_cast<int>(roundCells(availW));
    if (availCells <= 0) {
        ImGui::SetCursorScreenPos(start);
        ImGui::ItemSize(ImVec2(availW, m.row));
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 ink = ImGui::GetColorU32(color(ColorToken::roleTool));
    int row = 0;
    std::size_t begin = 0;
    while (begin <= code.size()) {
        std::size_t end = code.find('\n', begin);
        if (end == std::string_view::npos)
            end = code.size();
        const std::string shown = truncate(code.substr(begin, end - begin), availCells);
        if (!shown.empty()) {
            dl->AddText(ImVec2(start.x, start.y + static_cast<float>(row) * m.row), ink,
                        shown.c_str(), shown.c_str() + shown.size());
        }
        ++row;
        if (end >= code.size())
            break;
        begin = end + 1;
    }
    ImGui::SetCursorScreenPos(start);
    ImGui::ItemSize(ImVec2(availW, static_cast<float>(row) * m.row));
}

/// Draw one bridged table fence: the grid, or - for the source language, the pixel profile and
/// any fence that does not parse - the verbatim markdown.
void drawTableFence(const std::string& code, bool showSource) {
    const Metrics m = metricsFor();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float availW = std::max(0.0f, ImGui::GetContentRegionAvail().x);
    const int availCells = static_cast<int>(roundCells(availW));
    Table table;
    // The grid is the text grid's business (§6.8): the pixel profile keeps rich_md's native
    // tables and the bridge never writes a fence there, so a fence that reaches this renderer
    // in the GUI is source, not a grid the font cannot line up. `renderTable()` would draw
    // nothing for a pixel rect, which would be a blank block - the fallback is why this test
    // comes first.
    if (showSource || !m.textGrid || availCells <= 0 || !parseGfmTable(code, table) ||
        table.empty()) {
        drawSourceLines(code, m, start, availW);
        return;
    }
    const TableSize size = measureTable(table, availCells);
    // The rect carries the room the flow gave us, not the table's own width: `renderTable()`
    // allocates the columns from `r.w`, so handing it the table's narrower answer would
    // re-decide the layout against a width the table already fits into (a 16-cell table handed
    // a 16-cell rect is below `kMinTableWidth` and would degrade to the key-value layout it had
    // just beaten).
    const float height = static_cast<float>(std::max(1, size.h)) * m.row;
    Rect r{start.x, start.y, static_cast<float>(availCells), height, true};
    renderTable(table, r, m);
    // `renderTable` paints through the draw list and submits no item, so the flow would put the
    // next fragment on the table's first row; this is what advances it (and what lets a table
    // taller than the child scroll instead of overlapping, since a rect is not a clipper).
    ImGui::SetCursorScreenPos(start);
    ImGui::ItemSize(ImVec2(availW, height));
}

} // namespace

MarkdownTheme markdownThemeFromTokens(bool textGrid) {
    // Everything is a token: the five inks come from the palette at the depth the
    // theme applied, and the two gaps are distance tokens over the body step. A
    // gap is em - rich_md multiplies it by the drawn font size - and one em of the
    // grid is one cell, so a token gap of a few pixels stays below a cell there and
    // never adds a blank row between two blocks.
    const float body = typeSize(int(TypeStep::body));
    MarkdownTheme theme;
    theme.link = color(ColorToken::accent);
    theme.inlineCode = color(ColorToken::roleTool);
    theme.quoteBar = color(ColorToken::lineStrong);
    theme.admonitionNote = color(ColorToken::accent);
    theme.admonitionWarn = color(ColorToken::stateWarn);
    // A highlight is a wash under the text, i.e. the same fill the app puts behind
    // a selected row - the token table has one translucent fill and it is this one.
    theme.markFill = color(ColorToken::selection);
    theme.blockGap = space(int(Space::tight)) / body;
    theme.headerGap = space(int(Space::hair)) / body;
    // Sub/superscripts are one type step down in the pixel profile; on the grid a
    // cell has one size, so the fraction would shrink the glyph inside its own
    // cell and the baseline shift would smear it across two rows.
    theme.subSupScale = textGrid ? 1.0f : typeSize(int(TypeStep::meta)) / body;
    theme.textGrid = textGrid;
    return theme;
}

void applyMarkdownTheme(const MarkdownTheme& theme) {
    if (activeContext() == nullptr) {
        // No context to style: forget what was applied, so the next context gets
        // the whole theme instead of a cache hit that describes another context.
        gApplied = false;
        return;
    }
    RichMd::Style& style = RichMd::GetStyle();
    style.linkColor = theme.link;
    style.codeColor = theme.inlineCode;
    style.quoteBar = theme.quoteBar;
    // rich_md's five admonition slots take two classes: Note/Tip/Important are
    // informational, Warning/Caution need attention. The label word carries the
    // severity (D-BLOCK: state via glyph + word), the colour the class.
    style.admonitionColors[0] = theme.admonitionNote;
    style.admonitionColors[1] = theme.admonitionNote;
    style.admonitionColors[2] = theme.admonitionNote;
    style.admonitionColors[3] = theme.admonitionWarn;
    style.admonitionColors[4] = theme.admonitionWarn;
    style.blockGap = theme.blockGap;
    style.headerGapStep = theme.headerGap;
    style.subSupScale = theme.subSupScale;
    // The `==mark==` wash is a colour rich_md uses without a resolve step, so
    // leaving it alone would paint the library's hard-coded amber over model text
    // in both profiles.
    style.markBackground = theme.markFill;
    // Bars: one cell on the grid, strictly left of the content - a 2-3 px bar
    // paints 2-3 whole cell columns starting half a cell left of the text and
    // erases its first cells ("> `Foo` bar" -> "  oo bar"). The pixel profile
    // takes one spacing step, and the admonition bar the step above the quote's,
    // so a warning reads as the stronger mark without becoming a fill.
    style.quoteBarThickness = theme.textGrid ? 1.0f : space(int(Space::hair));
    style.admonitionBarThickness = theme.textGrid ? 1.0f : space(int(Space::tight));
    // The remaining rich_md spacing is stated rather than inherited from a library
    // default that happens to look right: a list item is as tight as the lines of a
    // paragraph, and a fragment adds no padding of its own (the block gap above is
    // the token one). `fragmentGapBottom` keeps rich_md's -1 on purpose - it is not
    // a distance but the "end the line at the last glyph" instruction (any value
    // >= 0 glues the next fragment onto this one's line), so there is no length to
    // take from a token.
    style.listItemGap = space(int(Space::none));
    style.fragmentGapTop = space(int(Space::none));
    gAppliedTheme = theme;
    gApplied = true;
    ++gCounters.styleApplies;
}

bool markdownSelectable(bool textGrid) {
    // The pixel profile keeps rich_md's mouse selection; the text grid turns it off.
    // The grid cannot give the UI `Ctrl+C`: `ISIG` is on, so the line discipline turns
    // the byte into `SIGINT` before ncurses reads it, and the chord is the terminal's
    // own way of quitting (design §4.13/§6.7, C-07, D-05). rich_md's selection is a
    // hit-testing widget that answers `Ctrl+C` itself, so enabling it there would be a
    // second, unreachable copy path that also swallowed drags over the transcript.
    // Copy on the grid is the focused block's `y` and the block menu (task 44).
    //
    // The alternative, if a post-MVP terminal UX wants drag selection, is *not* a flag
    // here: it needs the terminal in raw mode (`cfmakeraw()`, which clears `ISIG`) so
    // `0x03` arrives as a byte the app may answer with "copy the selection, else
    // quit", plus a selection model of our own - mouse tracking is already enabled by
    // the backend (`?1006;1000h`/`?1003h`, measured), so drags already arrive as
    // `ESC[<b;x;yM`; the app would hit-test cells, paint the range, and put the text on
    // the system clipboard itself (OSC 52, or the existing xclip/wl-copy path), with
    // bracketed paste (`?2004h`, which the backend does not enable today) to keep a
    // paste distinct from typing.
    // plan/tui_copy_probe.md records the measurements that decision starts from.
    return !textGrid;
}

void setMarkdownContext(RichMd::Context* context) {
    gContext = context;
    gApplied = false; // a different context has its own style
}

void registerTableFenceRenderers() {
    // Per context, and only where the bridge writes a fence: the text-grid profile registers
    // them from its markdown init (`initMarkdownTextMode`), the pixel profile does not - so a
    // `llmfun-table` written by a model in the GUI still renders as the code block it is,
    // and the GUI's markdown keeps passing through to rich_md unchanged (design §6.8).
    if (activeContext() == nullptr)
        return;
    RichMd::RegisterFencedBlockRenderer(
        kTableFenceLanguage, [](const std::string& code) { drawTableFence(code, false); });
    RichMd::RegisterFencedBlockRenderer(
        kTableSourceFenceLanguage, [](const std::string& code) { drawTableFence(code, true); });
}

RichMd::Context* markdownContext() { return gContext; }

bool markdownReady() { return gContext != nullptr; }

std::size_t beginBlock(std::size_t blockId) {
    // Outside a frame there is no window to push an id onto (ImGui would
    // dereference a null one), and no id to read back: no scope, an answer of 0,
    // and `endBlock()` then leaves the stack alone rather than popping a scope
    // this call never opened.
    if (ImGui::GetCurrentContext() == nullptr || ImGui::GetCurrentWindowRead() == nullptr)
        return 0;
    ImGui::PushID(static_cast<int>(blockId));
    ++gBlockDepth;
    // The scope's own id, read back for the caller and for the id-stability test.
    return static_cast<std::size_t>(ImGui::GetID(""));
}

void endBlock() {
    // An unmatched call would pop someone else's id scope (an ImGui stack
    // underflow at best, a wrong widget id at worst), so it is a no-op: the
    // wrapper only closes a scope it opened.
    if (gBlockDepth == 0)
        return;
    --gBlockDepth;
    ImGui::PopID();
}

void renderModelText(std::string_view text, const MarkdownTheme& theme, bool selectable) {
    if (!gApplied || !sameTheme(gAppliedTheme, theme))
        applyMarkdownTheme(theme);
    // The context is checked before the window because `GetCurrentWindowRead()` is
    // `(*GImGui).CurrentWindow`: it dereferences the ImGui context before it can
    // answer anything, so a "no window" test alone IS the null dereference it is
    // meant to prevent (a rich context can outlive the backend's ImGui context).
    if (activeContext() == nullptr || ImGui::GetCurrentContext() == nullptr ||
        ImGui::GetCurrentWindowRead() == nullptr) {
        // Nothing to draw into: no rich context, no ImGui context, or no frame. A
        // caller with no rich context draws the text itself - it owns the
        // wrapped-plain-text fallback - and drawing nothing here is the honest
        // answer for the rest; the counter makes a skipped `markdownReady()` check
        // visible.
        ++gCounters.droppedRenders;
        return;
    }
    ++gCounters.renders;
    gCounters.lastSelectable = selectable;
    if (gBlockDepth == 0)
        ++gCounters.unscopedRenders;
    // rich_md wraps to the region itself. The caller pushes a wrap position for
    // the plain-text path (maxX - 1); neutralising it here is what stops a
    // markdown paragraph from being wrapped twice.
    ImGui::PushTextWrapPos(0.0f);
    // Per render, not once per context: the profile's default is `markdownSelectable()`,
    // and a caller may still ask for the other answer without a second rich_md context.
    // On the grid this is always `false` - the terminal's `Ctrl+C` must stay its own
    // (`markdownSelectable()` documents the raw-mode alternative).
    RichMd::PushSelectableText(selectable);
    // RenderRaw, never Render: Render resolves `![[...]]` transclusions by
    // reading the named file, and model output must not be able to make the
    // renderer open a file. One string copy per block per frame is the price of
    // rich_md's `const std::string&` signature.
    RichMd::RenderRaw(std::string(text));
    RichMd::PopSelectableText();
    ImGui::PopTextWrapPos();
}

MarkdownCounters markdownCounters() { return gCounters; }

std::string fencedCodeOf(std::string_view body) {
    // A fence is a line that starts with three or more backticks (or tildes); the
    // body is *one* code block when the first non-blank line opens a fence and the
    // closing fence is followed only by blank lines. Anything else - prose around a
    // fence, two blocks, an unterminated fence - is not a code block and answers
    // empty.
    //
    // The scan is deliberately small and literal: it is not a markdown parser and it
    // must not become one. rich_md still owns how a body is *drawn*; this only answers
    // "is this body a single code block, and what is its code", which is what a copy
    // action needs to know.
    const auto lineAt = [body](std::size_t begin) {
        std::size_t end = body.find('\n', begin);
        if (end == std::string_view::npos)
            end = body.size();
        return body.substr(begin, end - begin);
    };
    const auto isBlank = [](std::string_view line) {
        for (const char c : line)
            if (c != ' ' && c != '\t' && c != '\r')
                return false;
        return true;
    };
    const auto fenceMarker = [](std::string_view line) -> std::size_t {
        std::size_t indent = 0;
        while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t'))
            ++indent;
        if (indent >= line.size())
            return 0;
        const char c = line[indent];
        if (c != '`' && c != '~')
            return 0;
        std::size_t run = indent;
        while (run < line.size() && line[run] == c)
            ++run;
        return (run - indent) >= 3 ? run - indent : 0;
    };

    std::size_t firstBegin = 0;
    std::string_view first;
    for (;;) {
        if (firstBegin > body.size())
            return {};
        first = lineAt(firstBegin);
        if (!isBlank(first))
            break;
        if (firstBegin + first.size() >= body.size())
            return {};
        firstBegin += first.size() + 1;
    }
    if (fenceMarker(first) == 0)
        return {};

    // The opening line's marker character: the closing fence must use the same one
    // (a ``` block is not closed by ~~~).
    std::size_t markerAt = 0;
    while (markerAt < first.size() && (first[markerAt] == ' ' || first[markerAt] == '\t'))
        ++markerAt;
    const char marker = first[markerAt];

    const std::size_t codeBegin = firstBegin + first.size() + 1;
    std::size_t lineBegin = codeBegin;
    while (lineBegin <= body.size()) {
        const std::string_view line = lineAt(lineBegin);
        std::size_t other = 0;
        while (other < line.size() && (line[other] == ' ' || line[other] == '\t'))
            ++other;
        const bool closes = other < line.size() && line[other] == marker && fenceMarker(line) > 0;
        if (closes) {
            // Everything after the closing fence must be blank, or this is not a
            // single code block.
            std::size_t restBegin = lineBegin + line.size();
            while (restBegin < body.size()) {
                if (body[restBegin] == '\n') {
                    ++restBegin;
                    continue;
                }
                const std::string_view rest = lineAt(restBegin);
                if (!isBlank(rest))
                    return {};
                restBegin += rest.size();
                if (restBegin < body.size())
                    ++restBegin;
            }
            std::string code(body.substr(codeBegin, lineBegin - codeBegin));
            // A CRLF body keeps its code intact but not its carriage returns: a paste
            // target wants LF, and the CR was the terminal's line discipline, not the
            // code's.
            code.erase(std::remove(code.begin(), code.end(), '\r'), code.end());
            // One trailing newline is the fence's own line break, not the code's: it
            // stays (a pasted block reads better with it), a second one that only
            // separated the code from the closing fence does not.
            while (code.size() >= 2 && code[code.size() - 1] == '\n' &&
                   code[code.size() - 2] == '\n')
                code.pop_back();
            return code;
        }
        if (lineBegin + line.size() >= body.size())
            break;
        lineBegin += line.size() + 1;
    }
    return {}; // unterminated fence
}

std::string copyText(std::string_view body, bool codeBlock) {
    if (!codeBlock)
        return std::string(body);
    const std::string code = fencedCodeOf(body);
    return code.empty() ? std::string(body) : code;
}

namespace {

/// A line's last rendered row, trimmed of its surrounding blanks.
std::string_view trimmedLine(std::string_view line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
        line.remove_suffix(1);
    return line;
}

/// Whether `line` opens or closes a code fence: three or more backticks or tildes, after the
/// line's indentation. The same rule `fencedCodeOf()` scans with.
bool isFenceDelimiter(std::string_view line) {
    const std::string_view text = trimmedLine(line);
    if (text.size() < 3 || (text.front() != '`' && text.front() != '~'))
        return false;
    std::size_t run = 0;
    while (run < text.size() && text[run] == text.front())
        ++run;
    return run >= 3;
}

/// Whether `line` is a thematic break or a setext underline: three or more marks of one kind and
/// nothing else. A thematic break may be spaced (`- - -`, `* * *`), which CommonMark allows and a
/// model occasionally writes; a setext underline is one unbroken run.
bool isRuleLine(std::string_view line) {
    const std::string_view text = trimmedLine(line);
    if (text.size() < 3)
        return false;
    const char mark = text.front();
    if (mark != '-' && mark != '=' && mark != '*' && mark != '_')
        return false;
    std::size_t marks = 0;
    for (const char c : text) {
        if (c == mark)
            ++marks;
        else if (mark == '=' || (c != ' ' && c != '\t'))
            return false;
    }
    return marks >= 3;
}

/// Whether appending `caret` to a body would change which tables `findGfmTables()` reports, given
/// the spans it reports for the body itself.
///
/// Only a caret that is itself a row line can: the ASCII column's `|`, appended to a prose line
/// right after a table, turns that line into one more row (a table runs on while its lines carry
/// a separator), so the sentence would move into the table for as long as the blink is on. The
/// scan is the answer rather than a guess about where a table can end.
bool changesTables(const std::vector<GfmTableSpan>& spans, std::string_view body,
                   std::string_view caret) {
    if (spans.empty() || !ui::isTableRowLine(caret))
        return false;
    std::string appended(body);
    appended.append(caret);
    const std::vector<GfmTableSpan> after = findGfmTables(appended);
    if (spans.size() != after.size())
        return true;
    for (std::size_t i = 0; i < spans.size(); ++i)
        if (spans[i].begin != after[i].begin || spans[i].end != after[i].end)
            return true;
    return false;
}

} // namespace

std::string withStreamingCaret(std::string_view body, std::string_view caret) {
    if (body.empty() || caret.empty())
        return std::string(body);
    std::string out(body);
    // The caret continues the *last rendered line*: trailing line breaks are dropped first, so
    // a body that currently ends with a newline puts the caret at the end of its last line
    // instead of opening an empty one. That is what keeps the drawn height stable while the
    // caret blinks (`./test_streaming` measures it).
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    if (out.empty())
        return std::string(caret);
    std::string_view last(out);
    const std::size_t lineStart = last.rfind('\n');
    if (lineStart != std::string::npos)
        last = last.substr(lineStart + 1);
    const std::string_view text = trimmedLine(last);

    // Three shapes get the caret on a line of its own, because a glyph after them changes what
    // the line *means* - the caret is worth a row, never a different document:
    //
    //   * a fence delimiter - appending would leave the fence unterminated and swallow the rest
    //     of the thread;
    //   * a thematic break / setext underline - `---` plus a glyph is a paragraph and `- - -`
    //     plus one is a list, so a rule would flicker into text;
    //   * a table row the bridge reads as one - a trailing glyph after the row's last pipe
    //     changes how the row and the delimiter row parse, so for one revision the bridge would
    //     stop detecting the table and the text grid would fall back to rich_md's native
    //     renderer (C-05's hazard); inside a grid cell the glyph is dropped anyway.
    //
    // The table question is asked of the scanner, not of "does the line carry a pipe": prose
    // (`ls | grep`) and a code line inside a fence carry one too, and neither is a row. A body
    // with no pipe cannot hold a table at all (`blockHasTable()`'s own guard), so the scan is
    // skipped for the common case.
    const bool fence = isFenceDelimiter(text);
    const bool rule = isRuleLine(text);
    const bool rowLine = ui::isTableRowLine(text);
    const bool caretRow = ui::isTableRowLine(caret);
    std::vector<GfmTableSpan> spans;
    if ((rowLine || caretRow) && out.find('|') != std::string::npos)
        spans = findGfmTables(out);
    const bool tableRow = rowLine && !spans.empty() && spans.back().end == out.size();
    const bool tableBreak =
        !fence && !rule && (tableRow || (!rowLine && changesTables(spans, out, caret)));
    if (!fence && !rule && !tableBreak) {
        out.append(caret);
        return out;
    }
    // A table needs a blank line first: both readers run a table on while its lines look like
    // rows, and md4c takes *any* non-blank line, so a caret directly under a row is an empty row
    // of that table - invisible, and an ASCII `|` would even extend the table over the prose.
    // Nothing else needs one: a fence and a rule end at their own line, so the caret line is a
    // separate block already (inside an open fence it is a code line).
    out.append(tableBreak ? "\n\n" : "\n");
    // The caret line keeps the last line's indentation, so inside a list item or a blockquote it
    // belongs to the container its text does instead of starting a block at the left margin.
    const std::size_t content = last.find_first_not_of(" \t");
    if (content != std::string_view::npos)
        out.append(last.substr(0, content));
    out.append(caret);
    return out;
}

} // namespace llmfun::tui::ui
