/// The rich_md seam (design §4.13, D-RAW): one wrapper owns everything rich_md
/// needs from the app, so no view touches the library directly.
///
/// Five things live here and nowhere else:
///
///  * **Raw rendering.** `RenderRaw` for every piece of model output. `Render`
///    resolves `![[...]]` transclusions by reading files from disk, so a model
///    that emits `![[/etc/passwd]]` would make the renderer read it. The wrapper
///    never calls `Render`.
///  * **Stable ids.** rich_md pushes `ID(fragmentCounter++)` around each fragment
///    it draws, i.e. an id that follows frame render order. `beginBlock(blockId)`
///    puts a stable outer scope around a block's fragments (the group's or the
///    message's own id, never a frame counter), so paging, clipping and a
///    collapsed-then-expanded group cannot remap selection or expansion state.
///  * **Token theming.** `markdownThemeFromTokens()` resolves the Ink tokens for
///    one profile and `applyMarkdownTheme()` writes them into `RichMd::Style` -
///    links, inline code, the quote bar, the admonition classes, the block and
///    header gaps, sub/sup scaling and the bar thicknesses. Mapping `ImGuiCol_*`
///    alone does not theme markdown, which is why the bridge exists at all.
///  * **The selection policy.** `markdownSelectable()`: the pixel profile keeps
///    rich_md's mouse selection, the text grid does not - `Ctrl+C` is the quit
///    chord there, so a hit-testing selection would steal its meaning. Copy on
///    the grid goes through a block's own action, never a mouse-only path.
///  * **The table bridge's fenced renderers.** `llmfun-table` and
///    `llmfun-table-source` (design §6.8, task 37) are drawn here, on the text
///    grid: the grid itself is `ui/table.*`, and a fenced renderer is a rich_md
///    extension, so it belongs in the one translation unit allowed to name the
///    library (`registerTableFenceRenderers()`, called by the text-grid profile).
///
/// Layering: this header depends on imgui.h (for `ImVec4`) and `<string_view>`
/// only, and `markdown.cpp` is the one translation unit that includes rich_md -
/// the seam scan in `test_rich_markdown_text` proves that `views/`, `model/` and
/// `store/` name no `RichMd` symbol at all.
#pragma once

#include "imgui/imgui.h"

#include <cstddef>
#include <string>
#include <string_view>

// Forward declaration: the caller (tui.h's TuiState) owns the context, and a
// rich_md include here would drag the library into every consumer of the seam.
namespace RichMd {
struct Context;
}

namespace llmfun::tui::ui {

/// One profile's resolved markdown look: the colours and gaps the Ink tokens
/// answer with, plus the profile they were resolved for.
///
/// The colours are foregrounds (D-BLOCK: markdown never paints a role
/// background) plus the one wash that is a fill by nature - `==mark==` highlights
/// the text it sits behind, so it takes the same translucent token a selected row
/// does.
struct MarkdownTheme {
    /// Links: the one interactive colour (`accent`).
    ImVec4 link{};
    /// Inline code: the verbatim voice (`roleTool`); the pixel profile also draws
    /// it in the code face, the grid in the same cell font.
    ImVec4 inlineCode{};
    /// The quote bar: an essential boundary, not a decorative one (`lineStrong`) -
    /// `lineSoft` is invisible against `surface` on a 256-colour terminal.
    ImVec4 quoteBar{};
    /// Admonitions come in two classes the token set can carry: the informational
    /// one (Note, Tip, Important) and the attention one (Warning, Caution). The
    /// word carries the severity, the colour carries the class.
    ImVec4 admonitionNote{};
    ImVec4 admonitionWarn{};
    /// The `==mark==` wash behind highlighted text: the same fill the app puts
    /// behind a selected row (`selection`, accent at 14 %). rich_md's default is a
    /// hard-coded amber, and it is used without a resolve step, so leaving it
    /// unbridged would paint a non-token colour in both profiles.
    ImVec4 markFill{};
    /// Vertical gap between two markdown blocks, in em (rich_md multiplies it by
    /// the drawn font size): a distance token over the body step, so the grid
    /// (1 em = 1 cell) stays below one cell and a gap never eats a row.
    float blockGap = 0.0f;
    /// Extra gap above a heading, in em: `(7 - level) * headerGap`, so the h1 step
    /// is the widest one and the one the grid budget has to survive.
    float headerGap = 0.0f;
    /// Sub/superscript size as a fraction of the body: one type step down in the
    /// pixel profile, exactly 1.0 on the grid (a cell has one size).
    float subSupScale = 0.0f;
    /// The profile this theme was resolved for. It decides the grid's cell
    /// corrections: sub/sup stay one cell tall, and the quote/admonition bars are
    /// one cell wide and sit strictly left of the content, where a 2-3 px bar
    /// would paint over the content's first cells.
    bool textGrid = false;
};

/// The tokens resolved for one profile. Pure: it reads the token table (and the
/// colour depth the theme applied last), never the live profile - the caller
/// states the profile (`ui::Metrics::textGrid`), so this module never asks which
/// renderer is up.
MarkdownTheme markdownThemeFromTokens(bool textGrid);

/// Write a theme into the current rich_md context's `RichMd::Style` and record it
/// as applied: called once when the context is created (and on a theme change),
/// never per frame - `markdownCounters().styleApplies` is the number a test reads
/// to prove that. A no-op (and no recorded theme) when there is no context, since
/// `RichMd::GetStyle()` needs one.
void applyMarkdownTheme(const MarkdownTheme& theme);

/// Whether model text can be selected in this profile (design §4.13): the pixel
/// profile keeps rich_md's selection, the text grid does not, because `Ctrl+C` is
/// the quit chord there.
bool markdownSelectable(bool textGrid);

/// The rich_md context every render draws through - the caller's `TuiState`
/// owns it and hands it over here once created; `nullptr` when there is none.
///
/// Setting a context invalidates the recorded theme, so the first render into a
/// fresh context (a backend switch makes one) re-applies the style instead of
/// trusting a cache that described the old one.
void setMarkdownContext(RichMd::Context* context);
RichMd::Context* markdownContext();

/// Register the table bridge's fenced renderers on the installed context (design §6.8, task
/// 37): `llmfun-table` draws the text-grid table, `llmfun-table-source` its verbatim markdown.
///
/// Called by the **text-grid** profile's markdown init and by nothing else. The GUI keeps
/// rich_md's native tables, so its context registers neither language and a `llmfun-table` a
/// model happened to write still renders as the code block it is - the GUI's markdown passes
/// through to rich_md unchanged. A no-op without a context (a headless state never has one).
void registerTableFenceRenderers();

/// True when a context is installed, i.e. when `renderModelText()` can render.
/// False for a headless state that never called `tuiInit()`: the caller draws the
/// text itself there (a wrapped-plain-text fallback), which only it can do.
bool markdownReady();

/// Open one block's id scope and return the ImGui id the scope entered.
///
/// `blockId` is the *group's* or the *message's* own id - monotonic, never a frame
/// counter and never a position in a capped list - so rich_md's per-frame fragment
/// ids are keyed to the block. Two frames that open the same block answer the same
/// number and two blocks answer different ones; that identity is what
/// `test_rich_markdown_text` asserts, and it is the whole reason the scope cannot
/// be left to the caller's own `PushID`.
///
/// Outside a frame (no ImGui context, or a context with no current window) there is
/// nothing to hash against: no scope is pushed and the answer is 0, which makes the
/// matching `endBlock()` a no-op as well.
std::size_t beginBlock(std::size_t blockId);

/// Close the scope `beginBlock()` opened.
void endBlock();

/// Draw one piece of model text through rich_md.
///
/// `theme` is the theme the caller resolved for the live profile: the wrapper
/// re-applies it when it is not the one the context was styled with, so a caller
/// can never draw with a style that disagrees with the theme it passed. A theme
/// that did not change costs one field comparison, not a style write.
/// `selectable` is the caller's policy answer (`markdownSelectable(theme.textGrid)`),
/// so one block may differ from the profile default.
///
/// The call belongs inside `beginBlock()/endBlock()`; a render outside a scope
/// still draws (rich_md scopes itself per frame), it is just not stable across
/// frames, and `markdownCounters().unscopedRenders` counts it so a missing scope
/// is a test failure instead of an invisible drift. A render with nothing to draw
/// into - no rich context, no ImGui context, or no frame - draws nothing and is
/// counted in `droppedRenders` rather than dereferencing a null window (a missing
/// ImGui context included: `GetCurrentWindowRead()` dereferences it).
void renderModelText(std::string_view text, const MarkdownTheme& theme, bool selectable);

/// The code a body is, when the body is exactly one fenced code block: the fence
/// lines, the info string and the blank lines around them removed, everything
/// between them kept byte for byte. Empty when `body` is anything else (prose, two
/// code blocks, an unterminated fence) - "no code" is an empty answer, never a
/// best guess at what the user meant.
///
/// Why the wrapper owns this: a code block's *content* is a markdown fact, and this
/// is the one translation unit allowed to know markdown. Task 26's copy path needs
/// it twice - the adapter asks once per message to classify a block as `Code`, and
/// `copyText()` asks again when the user copies a block - so the scan lives here
/// and neither caller parses a fence.
std::string fencedCodeOf(std::string_view body);

/// What a block's copy action puts on the clipboard (task 26, S-13: `y` on the
/// focused block, and the row's own copy control).
///
/// A `Code` block copies its *code* (`fencedCodeOf`) - that is what the TUI needs a
/// keyboard path for, and copying the fence markers with it would make the paste
/// wrong. Everything else copies its body verbatim. A code block whose fences do not
/// parse falls back to the whole body: an exotic body is still content the user asked
/// for, and an empty clipboard is worse than an unfenced one.
std::string copyText(std::string_view body, bool codeBlock);

/// `body` with the live answer's caret appended to its last rendered line (design FR-08,
/// §4.5; task 38).
///
/// `caret` is the glyph the profile draws - `ui::icon(Icon::Rule, …)`, i.e. "▌" on a surface
/// that carries the marks and "|" on the ASCII one - resolved by the caller, because which
/// column a surface can draw is the icon table's business and not markdown's. The *off* half
/// of the blink is a space of the same width, so the caret costs the same cells on and off
/// and the row's height cannot oscillate with the blink.
///
/// Trailing line breaks are dropped before the append, so the caret sits at the end of the
/// content's last line rather than on an empty line of its own. Three shapes get a line of their
/// own instead, because a glyph after them would change what the body *means*: a fence delimiter
/// (appending would leave the fence unterminated), a rule line (`---`, `- - -`), and a table row
/// (a table runs on while its lines look like rows, so the caret would land in a cell - and the
/// caret line needs a blank line before it to end the table, because the ASCII column's `|` is a
/// separator itself). The caret line copies the last line's indentation, so it stays in the
/// container the text belongs to. An empty body has no caret at all - the streaming row's header
/// says `writing` before the first character arrives, and a caret with no text under it would
/// claim otherwise.
std::string withStreamingCaret(std::string_view body, std::string_view caret);

/// What the wrapper has done since the process started - the seam a test reads
/// instead of poking at rich_md's internals:
///
///   styleApplies     how often `RichMd::Style` was written (once per theme)
///   renders          `RenderRaw` calls
///   droppedRenders   renders that drew nothing (no rich context, no ImGui context, or
///                    no frame)
///   unscopedRenders  renders outside `beginBlock()` (ids not stable)
///   lastSelectable   the `selectable` the last render ran with
struct MarkdownCounters {
    unsigned styleApplies = 0;
    unsigned renders = 0;
    unsigned droppedRenders = 0;
    unsigned unscopedRenders = 0;
    bool lastSelectable = false;
};
MarkdownCounters markdownCounters();

} // namespace llmfun::tui::ui
