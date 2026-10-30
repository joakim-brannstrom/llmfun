/// Ink / Graphite design tokens (D-THEME, design §4.15): the one source of every
/// colour, size and distance the workbench draws.
///
/// A view never writes a colour. It names a semantic token — `canvas`, `raised`,
/// `roleAgent`, `stateDanger` — and the token module answers with the value the
/// *active colour depth* can show, so the same call site is correct on a true
/// colour GUI, on a 256-colour terminal and on a 16-colour one.
///
///     ImDrawList::AddRectFilled(a, b, ui::color(ColorToken::raised));
///
/// The palette is the design's Graphite table (plan/appendix_modern_ui.md §4.2),
/// restated as data in `tokens_generated.h` by `tools/gen_tokens.py`. That file
/// is the only place a colour literal exists; `applyTheme()` is the only place a
/// colour reaches `ImGuiCol_*`.
///
/// Layering: this header depends on imgui.h and nothing else in the project,
/// which is what lets `ui/` stay below `views/` and `model/` in the dependency
/// graph and lets the token tests link the module on its own.
///
/// Parchment and Native exist in the design but are not selectable in the MVP
/// (D-02), so `ThemeId` names Graphite only: a theme that cannot be chosen needs
/// no branch, and the enum is where P5 adds them.
#pragma once

#include "imgui/imgui.h"

namespace llmfun::tui::ui {

/// The 20 semantic colours (design §4.2). Declaration order is the generated
/// table's row order — the generator comments the row with its token name, so a
/// mismatch is visible in the diff and caught by `kColorTokenCount`.
enum class ColorToken : int {
    // Surfaces, darkest first: the app canvas, the region surface (rail,
    // inspector, thread), a raised card/composer, and an inset well (inputs,
    // code blocks).
    canvas = 0,
    surface,
    raised,
    inset,
    // Hairlines. `lineSoft` is decorative (region separators), `lineStrong` is an
    // essential boundary (input and card edges) and must clear 3:1.
    lineSoft,
    lineStrong,
    // Ink, brightest first. `textDim` is decorative by design: it is the only
    // ink below the 4.5:1 text bar and must never carry a glyph (NFR-09).
    textHi,
    textMid,
    textLow,
    textDim,
    // One interactive colour: primary action, active row, links.
    accent,
    // Roles are foreground + rule, never a fill (D-BLOCK).
    roleYou,
    roleAgent,
    roleTool,
    roleSystem,
    // State glyphs and words.
    stateOk,
    stateWarn,
    stateDanger,
    // Fills drawn by views rather than by an ImGui slot: a selected row
    // (`accent` at 14 %, the design's accent.quiet) and the 2 px focus ring
    // (`accent` at 60 %).
    selection,
    focusRing,
    Count
};

/// 20, spelled out so the generated table can be static-asserted against it.
inline constexpr int kColorTokenCount = static_cast<int>(ColorToken::Count);

/// The palette the UI renders with. Only Graphite ships (D-02).
enum class ThemeId : int { Graphite = 0, Count };

/// What the display can show. TrueColor keeps the design's exact values; at the
/// two ANSI depths the value to *draw with* is a pre-image of the palette entry
/// the terminal then paints, because imtui quantizes the style colour again at
/// render time. `tokens_generated.h` states the derivation, including how a
/// translucent token is flattened.
///
/// Ansi16 is a fallback, not a target profile: 16 colours cannot carry four
/// surface steps or four ink steps, so that depth keeps the app's *structure*
/// (black surfaces, bright inks, a bright-black fill for a selected row) and
/// collapses the rest. Nothing in the MVP detects into it except an explicit
/// `NO_COLOR` or a terminal that advertises neither true colour nor 256.
///
/// `ansi16Index` names the base colour a token *means*, not a colour pair ncurses
/// can be handed: imtui quantizes every style colour to a 256-colour index
/// (`rgbToAnsi256` never answers 0-15) and passes it straight to `init_pair`, so on
/// a genuine 16-colour terminal (`COLORS == 16`) the call is refused and the cell
/// keeps the default pair. Ansi16 therefore means "apply the base RGBs"; drawing
/// *with* palette 0-15 is not implemented.
///
/// MVP gap (recorded, not hidden): the design's §4.5 rule is "`NO_COLOR` ->
/// attributes only", and this module maps it to the 16-colour depth instead, so a
/// `NO_COLOR` user still gets coloured roles and states. The attribute-only
/// rendering path belongs to the task that owns attribute drawing; until then a
/// `NO_COLOR` terminal gets the coarsest palette rather than no theme at all.
enum class ColorDepth : int { TrueColor = 0, Ansi256, Ansi16, Count };

/// The generated palette, resolved once per token.
///
/// `rgb` is the design's colour. The two coarse pairs are "the colour to apply"
/// plus "the palette entry the terminal then paints": imtui quantizes the style
/// colour again at render time, so the applied value is a pre-image of the entry
/// (a palette entry is not a fixed point of that formula) and the pair is what
/// makes the round trip provable. `tokens_generated.h` states the derivation.
struct TokenTable {
    ImVec4 rgb[kColorTokenCount];
    ImVec4 xterm256[kColorTokenCount];
    int xterm256Index[kColorTokenCount];
    ImVec4 ansi16[kColorTokenCount];
    int ansi16Index[kColorTokenCount];
};

/// The Graphite table as built at start-up (independent of the active depth).
const TokenTable& tokens();

/// One token at a colour depth: the RGB to draw with, plus the palette index the
/// depth resolves to (a 256-colour entry at Ansi256, a base colour at Ansi16, and
/// -1 at TrueColor, which has no palette). `applyTheme` maps the RGB; a view only
/// needs `color()`.
struct ResolvedToken {
    ImVec4 rgb;
    int paletteIndex;
};
ResolvedToken token(ColorToken token, ColorDepth depth);

/// The colour of a token at the depth the theme applied last — the accessor
/// every view uses. Before any `applyTheme` call it answers in true colour.
ImVec4 color(ColorToken token);

/// The token's name, for failure messages and the UI log ("token: roleAgent").
/// `Count` (or anything outside the enum) answers "<invalid>" rather than a
/// neighbour's name: a computed `Count` must not read as a real token.
const char* tokenName(ColorToken token);

// ---------------------------------------------------------------------------
// Sizes. One scale each, in design units: the metrics module (task 13) turns a
// step into cells or pixels, so no view multiplies by hand (D-THEME).
// ---------------------------------------------------------------------------

/// Spacing on the 8 px grid, plus the two hairlines below it: 0, 2, 4, 6, 8, 12,
/// 16, 24.
float space(int step);
/// Corner radii: 0 (square), 4 px, 6 px. Matches the GUI chrome metrics so the
/// pre-token window and the tokenised one agree.
float radius(int step);
/// Type scale, GUI px: display 24, title 18, body 15, label 13, meta 12, code
/// 14. The text grid ignores the size - a cell has one size - and uses the step
/// for emphasis only, so every number here is a pixel-profile length.
float typeSize(int step);
/// Motion durations in milliseconds: 120 (state change), 160 (overlay), 200
/// (panel). `0` disables motion for the text grid.
float motionMs(int step);

/// Named steps, so a call site reads `space(int(Space::comfy))` instead of a
/// bare number and a later density setting has one place to move.
enum class Space : int { none = 0, hair, tight, compact, base, comfy, section, page };
enum class Radius : int { none = 0, small, large };
enum class TypeStep : int { display = 0, title, body, label, meta, code };
enum class Motion : int { fast = 0, base, slow };

// ---------------------------------------------------------------------------
// Typography (pixel profile only: a cell has one size)
// ---------------------------------------------------------------------------

/// The UI font family the pixel profile draws with, the monospaced one for code,
/// and the embedded asset each one names. Q-06 (the licence/size review) is
/// settled as *keep Roboto for the MVP*: the build already carries Roboto-Medium
/// and Inconsolata-Medium inside rich_md's embedded asset table, so no font file
/// is added, no download happens at run time and NFR-06's 1.5 MB allowance is not
/// claimed.
///
/// Family and asset travel together on purpose: a post-MVP swap to Inter and
/// JetBrains Mono changes the two rows of this pair and nothing else. `family` is
/// what the log line and a font config name; `asset` is the path inside the
/// embedded table the chain reads.
struct FontFace {
    const char* family;
    const char* asset;
};
FontFace uiFontFace();
FontFace monoFontFace();

/// The family names alone, for a caller that does not load a font (a settings
/// field, a test message).
const char* uiFontFamily();
const char* monoFontFamily();

/// Apply the type scale to the pixel profile: `ImGuiStyle::FontSizeBase` becomes
/// `basePx`, or the body step when `basePx` is 0, so every widget that does not
/// ask for a step of its own draws at that size. The text grid is untouched (a
/// cell has one size, and the grid pins its own font), so this is the pixel
/// backend's init call, next to `applyGuiChromeMetrics()`.
///
/// The value is also remembered (`TypeScope` reads it rather than the style): `PushFont()` writes
/// the size it pushed into `Style.FontSizeBase`, so the style field only answers "the step that is
/// open" once a scope is on the stack.
///
/// `basePx` is the operator's font size (`LLMFUN_GUI_FONT_SIZE`). A file font's
/// own size is only a raster hint in 1.92 - the drawn size comes from here - so a
/// configured size can only reach the screen through the base; 0 (the default)
/// means the type scale owns the size.
///
/// Called once per start-up and never per frame: a frame that moves the base size
/// would re-lay out every widget of that frame.
void applyTypeScale(float basePx = 0.0f);

/// The size a step draws at *relative to the applied base*: `typeSize(step)` at
/// the default base, and `basePx * typeSize(step) / typeSize(body)` when the
/// operator configured a size - a bigger base keeps the hierarchy instead of
/// flattening it to one size. Pure (it needs no ImGui context), so the arithmetic
/// can be tested on its own.
float scaledTypeSize(TypeStep step, float basePx);

/// The monospaced face the code step draws with, as installed by the pixel
/// backend's font chain; null until then. `TypeScope` pushes it for
/// `TypeStep::code` and the current font for every other step, so a code span is
/// monospaced even where the view draws no text widget of its own.
void setCodeFont(ImFont* font);
ImFont* codeFont();

/// One type step for one draw: `PushFont(face, scaledTypeSize(step, base))` on
/// entry and a matching pop on exit, so a heading, a label and a meta row can sit
/// next to each other without any of them editing the shared base size. `face` is
/// the code font for `TypeStep::code` (when the chain installed one) and the
/// current font for every other step.
///
/// A step is *absolute*: the size comes from the base `applyTypeScale()` was given, not from the
/// scope that happens to be open, so scopes nest for free. `label` inside `label` stays 13 px and
/// `meta` inside `title` stays 12 px - a scope that read `Style.FontSizeBase` instead would scale
/// a step by a step (`label` inside `label` = 13 * 13 / 15 = 11 px) and a view that measures
/// inside its own scope would answer numbers the draw does not use. Pushing the size the frame
/// already draws at is skipped, so a nested scope costs nothing.
///
/// `textGrid` is the live profile (`Metrics::textGrid`), passed as data for the
/// same reason every other geometry decision is: a pixel size on the text grid
/// would multiply each cell by the size - a 15 px glyph in a 1-cell grid draws
/// outside its cell - so on the grid the scope is a no-op and the step means
/// emphasis only.
///
/// RAII rather than a push/pop pair: a view has several early returns (a region
/// that measured to zero, a clipped label), and one leaked push would leave the
/// rest of the frame at the wrong size.
class TypeScope {
public:
    TypeScope(TypeStep step, bool textGrid);
    ~TypeScope();
    TypeScope(const TypeScope&) = delete;
    TypeScope& operator=(const TypeScope&) = delete;

    /// False when the scope did nothing (the text grid, or an ImGui context
    /// without a current font). Read by the tests; a view never needs it.
    bool active() const { return pushed_; }

private:
    bool pushed_ = false;
};

/// One slot binding: which token fills which `ImGuiCol`, and how many hover
/// steps above the token (`0` = the token itself). `applyTheme` walks exactly
/// this table, and `test_tokens` reads it back to prove that a text-bearing slot
/// only ever receives a token that clears 4.5:1.
struct ColorBinding {
    ColorToken token;
    ImGuiCol slot;
    int steps;
};
const ColorBinding* colorBindings(int& count);

/// One hover/active step above a fill token: +5 % per step in sRGB, clamped to
/// white. The 20 semantic colours carry no hover variants — a hover is a state
/// of a widget, not a semantic colour — so the theme derives them from the fill
/// and every value still comes from a token.
///
/// 5 % is the largest step that leaves a margin: a `textHi` label on the
/// *brightest* derived fill (`textDim` + one step, the button hover) measures
/// 4.67:1 in the pixel profile and 5.26:1 on the text grid. A 6 % step puts that
/// pair at 4.497:1 - under the bar by 0.003 (4.52:1 if the fill is quantized to
/// 8 bits) - so the step is measured against the bar, not chosen by taste.
ImVec4 stateStep(ImVec4 base, int steps);

/// Apply a theme at a colour depth: map the token table onto `ImGuiCol_*`.
/// Called once after the backend initialized (a backend init resets several
/// style colours) and never per frame; `themeApplyCount()` is the counter the
/// tests assert on.
void applyTheme(ThemeId theme, ColorDepth depth);

/// `NO_COLOR` → `COLORTERM=truecolor|24bit` → `TERM=*256color*` → ANSI-16, in
/// that order (design §4.15). The optional lookup is the test seam: it answers
/// as `getenv` would, so the detection order is pinned without mutating the
/// process environment.
using EnvLookup = const char* (*)(const char* name);
ColorDepth detectColorDepth(EnvLookup lookup);
ColorDepth detectColorDepth();

/// What the last `applyTheme` applied. A view that must know (the text grid's
/// cell metrics) asks here rather than re-reading the environment.
ThemeId currentTheme();
ColorDepth currentColorDepth();

/// How many times `applyTheme` has done its work. Test seam: a frame loop must
/// not move this number, and the counter is the only way to see that from
/// outside ("runs once per theme change, not per frame").
unsigned themeApplyCount();

} // namespace llmfun::tui::ui
