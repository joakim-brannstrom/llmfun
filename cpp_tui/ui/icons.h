/// The glyph table (design §4.15 "Theme and primitives"; the marks themselves are
/// `plan/appendix_modern_ui.md` §4.8, their degradation §4.9 and §7.2): one home
/// for every *state mark* the workbench draws, each with a single-cell Unicode form
/// and a plain ASCII fallback.
///
///     draw(ui::icon(Icon::Ok, glyphs));                // "✓" or "+"
///     draw(ui::icon(Icon::DisclosureClosed, glyphs));  // "▸" / ">": this row opens
///     draw(ui::icon(Icon::Rule, glyphs));              // "▌" / "|": the role rule
///
/// Scope: *state marks*, not shapes. The text-grid table renderer (task 36) needs
/// box-drawing borders (`│ ─ ┌ ┬ ┐ └ ┴ ┘ ├ ┼ ┤`) and its own ASCII fallback; those
/// are geometry, they have no state and no word beside them, so they are declared
/// where they are drawn rather than here - recorded as task 36's handoff so a later
/// reader does not come looking for `BorderTop` in this enum.
///
/// What the table exists to keep is that a glyph is a *cell*, never a pictogram. A
/// state mark sits in a fixed column beside its label, so a glyph that renders two
/// cells wide - a CJK ideograph, an emoji, or an "ambiguous width" symbol on a
/// terminal that decides to draw it double - pushes that label one column right and
/// breaks the alignment the whole grid is built on. So every `glyph()` is exactly
/// one cell per `ui::displayWidth()` (the same table the ncurses backend renders
/// with), every `glyph()` is a single codepoint, and `test_icons` also reads the
/// rendered cell back through the real text backend to check that the measure and
/// the render agree rather than only the table and the measure.
///
/// No emoji, ever: they are double-width in most terminals and draw as colour
/// cartoons in the GUI's text font. `test_icons` pins that by measuring in the
/// renderer's default emoji mode, where an emoji-presentation codepoint measures two
/// cells and fails the one-cell rule.
///
/// A glyph is never the only carrier of meaning (NFR-16): every state renders as
/// glyph + word + colour, so a reader on an ASCII terminal, at 16 colours or
/// colour-blind loses the mark and keeps the meaning. That is also what makes the
/// fallback a *safe* answer everywhere the marks cannot be drawn, which is why the
/// choice is a value each surface resolves once (`GlyphSet`, below) instead of a
/// guess per glyph.
///
/// ## Two surfaces, two reasons to fall back
///
/// The text grid draws codepoints with the *terminal's* font, so the question is
/// what the terminal can render: `TERM=linux`, a CJK locale or `LLMFUN_ASCII=1` all
/// mean "not these marks" (`textGlyphSet()`).
///
/// The pixel GUI draws through the ImGui font atlas, so the question is what the
/// *atlas* carries - and ImGui substitutes a missing codepoint with the font's
/// fallback glyph, so a mark the atlas lacks is drawn as "?" (the bitmap font's
/// fallback) and not as the mark. Measured on this tree (`test_icons` re-measures
/// it): the shipped GUI font, the embedded ProggyClean bitmap, carries **0 of the
/// 12** distinct marks, and Roboto-Medium - the design's UI font, reachable through
/// `LLMFUN_GUI_FONT` - carries **2** (`•`, `…`). So the GUI draws the ASCII column
/// today (`pixelGlyphSet()`), and task 20 (font) flips it once a symbol-capable font
/// is in the atlas; task 19's GUI golden then has to contain one of each icon, so a
/// "?" cannot pass as a golden.
///
/// ## Which fallback form is chosen
///
///   * `LLMFUN_ASCII=1` forces it and `LLMFUN_ASCII=0` turns it off - the escape
///     hatch for a CJK locale whose terminal draws the ambiguous glyphs narrow. An
///     empty value means unset and any other value is ignored, so a stray
///     `LLMFUN_ASCII=yes` never silently changes the screen;
///   * `TERM=linux*` - the Linux console font has nothing beyond the ACS
///     line-drawing set, so every mark there would paint as "?";
///   * a CJK locale - `LC_ALL`, else `LC_CTYPE`, else `LANG` (the POSIX cascade),
///     matching `zh*`, `ja*` or `ko*` - because those terminals commonly draw the
///     East-Asian *ambiguous* characters (`▸ ● ○ ✓ ▲ ◇ …`) two cells wide, which is
///     precisely the one-column shift the table exists to prevent. When the width is
///     uncertain the glyph is not used, which is what keeps the alignment-critical
///     columns exactly one cell wide everywhere.
///
/// Layering: this header includes nothing and `icons.cpp` includes `<cstddef>`,
/// `<cstdlib>` and `<cstring>` only, so `ui/`, `model/`, `views/` and the store
/// adapter may all include it - the module is below every one of them, and `ui/`'s
/// own rule (no `tui.h`, no view header) is trivially kept.
///
/// Cost: `textGlyphSet()` reads the environment on every call - five `getenv`s at
/// most - so the answer follows a changed process environment instead of needing an
/// invalidation story. `icon()` is therefore a few string comparisons per drawn
/// glyph; a shell resolves the set once per surface and hands it down, and a view
/// that draws a glyph per cell in a wide loop keeps that value.
///
/// Consumers: `ui/primitives.*` (task 16's chip, disclosure and spinner) and, from
/// task 22a on, the views - top bar, rail, thread, block, now bar, inspector,
/// activity, Raw Log - plus the text-grid table renderer. Every one of them calls
/// `icon()` and never writes a glyph literal, which is what keeps this table the
/// single source; the design's rule that a state is drawn as glyph + word + colour
/// only holds together if there is one place the glyph comes from.
///
/// Recorded deviations from the design's mark table (`appendix_modern_ui.md` §4.8),
/// each deliberate:
///   * `Warn` is "▲" / "!" - the appendix writes "!" for *both* columns of Warning,
///     and a pair whose two forms are identical cannot fall back at all. The obvious
///     alternative, "⚠" (U+26A0), was rejected: terminals that ship a colour emoji
///     font draw it as one, and some draw it double-width - the exact failure the
///     table exists to prevent. The word ("Warn") carries the meaning either way, and
///     "!" stays the ASCII form, which is what the Activity tab's severity column
///     asks for.
///   * `Info` is "◇" / "i" - the Activity rows draw "i"; the mark is the hollow
///     diamond, which leaves the solid "◆" (§4.8's Tool mark, not in this table) free
///     for a later row. Both always sit beside their word.
///   * `Rest` and `DisclosureClosed` share "▸": both mean "this row opens" - the
///     rail's rest marker (§4.3: `▸` rest / `●` active, ASCII `>` / `o`) and §4.8's
///     collapsed disclosure.
///   * `Running` and `Spinner` share "◐": the second name exists so reduced motion
///     (§4.9: "no spinners, static ◐") has a mark of its own; the *animated*
///     indicator is `ui::spinner()`'s (task 16), which owns its frame cycle.
///   * `Ellipsis` falls back to "..." - the one form that is not one cell. It is a cut
///     marker handed to `ui::truncate()`, which measures the marker it is given
///     (`truncate(text, cells)` is `truncate(text, cells, icon(Icon::Ellipsis))`), so
///     three dots still cannot overflow the budget they exist to respect.
#pragma once

namespace llmfun::tui::ui {

/// Every state mark the workbench draws. Declaration order is the table's row
/// order: `test_icons` walks it and pins both the order and the count, so adding an
/// icon without a row (or a row without an icon) is a build-and-test failure, not a
/// neighbour's glyph appearing where a new one was meant to be.
enum class Icon : int {
    /// A session row at rest: "this row opens" (§4.3's rail marker). "▸" / ">".
    Rest = 0,
    /// The live session: the rail's active marker (§4.3). "●" / "o".
    Active,
    /// A run in flight: the static state mark beside the word. "◐" / "*".
    Running,
    /// An update the user has not seen (D-UNREAD): a filled bullet. "•" / ".".
    Unread,
    /// Queued or waiting: an empty ring, a slot nothing has filled yet. "○" / "-".
    Pending,
    /// Success (§4.8 Success). "✓" / "+".
    Ok,
    /// Warning. §4.8 writes "!" for both of its columns; the glyph is a triangle so
    /// that the pair can differ at all (see the header note). "▲" / "!".
    Warn,
    /// Failure (§4.8 Error). "✕" / "x".
    Error,
    /// An informational detail (the Activity tab's severity column). "◇" / "i".
    Info,
    /// The busy mark drawn when motion is off (§4.9 reduced motion). "◐" / "*".
    Spinner,
    /// An expanded disclosure (§4.8 Collapsed/expand). "▾" / "v".
    DisclosureOpen,
    /// A collapsed disclosure - the same mark as `Rest`. "▸" / ">".
    DisclosureClosed,
    /// The role rule beside a message header: a filled rect in the GUI, this glyph in
    /// a cell renderer. "▌" / "|".
    Rule,
    /// The cut marker `ui::truncate()` puts where text was removed. "…" / "...".
    Ellipsis,
    /// Not a glyph: the row count. Keep last.
    Count
};

/// 14, spelled out so the table can be static-asserted against it.
inline constexpr int kIconCount = static_cast<int>(Icon::Count);

/// Which of the two columns a surface draws.
///
/// A shell resolves this once per surface and hands it to its views (D-METRICS:
/// views take capability from the shell, they do not probe the environment per
/// glyph), which is what keeps a shared `views/*` file correct on both backends.
enum class GlyphSet : int {
    /// The Unicode column: a surface that can actually draw the marks.
    Glyphs = 0,
    /// The ASCII column: the surface cannot, or asked not to.
    Ascii,
};

/// The text grid's set: `Ascii` when the terminal cannot render the marks
/// (`useAscii()`), else `Glyphs`.
GlyphSet textGlyphSet();

/// The pixel GUI's set: `Ascii` until the GUI's font atlas carries the marks (see
/// the header note - the shipped bitmap font has 0 of the 12, Roboto-Medium 2, and
/// ImGui draws a missing codepoint as the font's fallback instead of the mark).
///
/// Task 20 (GUI style / fonts) owns the flip to `Glyphs`; `test_icons` measures the
/// atlas and asserts that this answer *equals* the measurement, so the constant
/// cannot go stale in either direction without failing a run.
GlyphSet pixelGlyphSet();

/// The Unicode form: exactly one cell, one codepoint, never an emoji.
///
/// The string is a literal with static storage duration and lives as long as the
/// process; a caller borrows it and never frees it. An icon outside the table answers
/// the first row, so a bad cast misdraws a mark but cannot read past the table.
const char* glyph(Icon icon);

/// The plain ASCII form: printable ASCII, one cell - except `Ellipsis`, which is
/// "..." and is measured by whoever draws it (see the header comment).
///
/// This is what a surface that cannot render the mark gets: `icon()` returns this,
/// so a caller never needs a branch of its own.
const char* asciiFallback(Icon icon);

/// The form a surface draws, for a surface that has resolved its `GlyphSet`.
///
/// This is the call a *shared* view makes: two backends, one view file, and the
/// shell's set is the only thing that differs.
const char* icon(Icon icon, GlyphSet set);

/// The text-grid answer, `icon(icon, textGlyphSet())` - for a caller that is on the
/// text grid by construction (the store adapter, a terminal-only path, a test).
///
/// A view that both surfaces share calls the two-argument form: on the pixel profile
/// this one would return marks the GUI cannot draw.
const char* icon(Icon icon);

/// `getenv`-shaped lookup: the same seam `ui/tokens.h` uses (`detectColorDepth`),
/// redeclared here so this header stays free of `imgui.h`. Pass `nullptr` or use the
/// overload below to read the real environment.
using EnvLookup = const char* (*)(const char* name);

/// Whether the ASCII column is the one a *terminal* draws, as decided by an
/// environment the caller supplies - the test seam that pins the detection order
/// without mutating the process environment.
bool useAscii(EnvLookup lookup);

/// The same decision for the real process environment, read at every call.
/// `textGlyphSet()` is this answer as a `GlyphSet`.
bool useAscii();

} // namespace llmfun::tui::ui
