/// The Ink / Graphite token module: palette, sizes and the one theme application
/// (design §4.15, D-THEME).
///
/// Everything here is data plus a table walk. The colours live in the generated
/// header; this file adds the size scales, the token -> `ImGuiCol_*` map and the
/// depth resolution, so `applyTheme` never contains a colour literal and a view
/// never needs to know which profile it is drawing into.
#include "ui/tokens.h"

#include "ui/tokens_generated.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace llmfun::tui::ui {
namespace {

// ---------------------------------------------------------------------------
// Sizes (design §4.15). The metrics module (task 13) multiplies a step by the
// profile's unit; a view never reads these raw numbers.
// ---------------------------------------------------------------------------

/// The 8 px grid, with the two sub-grid hairlines below it.
constexpr float kSpaceSteps[] = {0.0f, 2.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 24.0f};
constexpr int kSpaceCount = int(sizeof(kSpaceSteps) / sizeof(kSpaceSteps[0]));

/// Square, 4 px, 6 px: identical to the GUI chrome metrics, so the pre-token
/// window and the tokenised one agree while the restyle lands.
constexpr float kRadiusSteps[] = {0.0f, 4.0f, 6.0f};
constexpr int kRadiusCount = int(sizeof(kRadiusSteps) / sizeof(kRadiusSteps[0]));

/// display / title / body / label / meta / code, in pixels (task 20's type
/// scale - the design's GUI column restated in one place). The pixel profile's
/// base size is the body step (see `applyTypeScale`); the text grid ignores all
/// six numbers.
constexpr float kTypeSteps[] = {24.0f, 18.0f, 15.0f, 13.0f, 12.0f, 14.0f};
constexpr int kTypeCount = int(sizeof(kTypeSteps) / sizeof(kTypeSteps[0]));

/// Fast / base / slow state transitions, in milliseconds.
constexpr float kMotionSteps[] = {120.0f, 160.0f, 200.0f};
constexpr int kMotionCount = int(sizeof(kMotionSteps) / sizeof(kMotionSteps[0]));

/// One hover/active step in sRGB. See `stateStep` in the header: 5 % is the
/// largest step that keeps a `textHi` label above the text bar on the brightest
/// derived control fill (measured 4.65:1 on `textDim` + one step).
constexpr float kStateStep = 0.05f;

/// The two font faces the pixel profile asks for (Q-06 settled: keep the assets
/// the build already carries). Both live in rich_md's embedded asset table, so
/// naming them costs no download, no licence review and no binary weight; the
/// family is what a log line or a font config names and the asset is the table
/// path the chain reads, so a post-MVP swap to Inter / JetBrains Mono is these two
/// rows and nothing else.
constexpr FontFace kUiFace{"Roboto-Medium", "fonts/Roboto/Roboto-Medium.ttf"};
constexpr FontFace kMonoFace{"Inconsolata-Medium", "fonts/Inconsolata-Medium.ttf"};

/// A step outside its scale clamps to the nearest end: a view that asks for the
/// eighth spacing step gets `page`, not a read past the table.
float stepOf(const float* steps, int count, int step) {
    if (step < 0)
        step = 0;
    if (step >= count)
        step = count - 1;
    return steps[step];
}

// ---------------------------------------------------------------------------
// The palette
// ---------------------------------------------------------------------------

static_assert(generated::kRowCount == kColorTokenCount,
              "tokens_generated.h is out of date: run tools/gen_tokens.py after changing "
              "ColorToken");

TokenTable buildTable() {
    TokenTable built{};
    for (int i = 0; i < kColorTokenCount; ++i) {
        const generated::TokenRow& row = generated::kGraphite[i];
        built.rgb[i] = ImVec4(row.r, row.g, row.b, row.a);
        // The packed columns are IM_COL32; the vendored imgui's own decoder is
        // the only place the byte order is spelled out, so use it rather than
        // re-deriving shifts here (a mismatch is a silent R/B swap).
        built.xterm256[i] = ImGui::ColorConvertU32ToFloat4(row.xterm256);
        built.xterm256Index[i] = row.xterm256Index;
        built.ansi16[i] = ImGui::ColorConvertU32ToFloat4(row.ansi16);
        built.ansi16Index[i] = row.ansi16Index;
    }
    return built;
}

/// Built once: the table is immutable, so a view's `color()` costs an index.
const TokenTable& builtTable() {
    static const TokenTable table = buildTable();
    return table;
}

bool inRange(ColorToken token) {
    const int index = static_cast<int>(token);
    return index >= 0 && index < kColorTokenCount;
}

int tokenIndex(ColorToken token) {
    // A token outside the table would read a neighbour's colour. `resolve` keeps
    // that clamp (a colour is always needed), while `tokenName` reports the
    // invalid value instead of a neighbour's name.
    return inRange(token) ? static_cast<int>(token) : 0;
}

/// One token as the given depth shows it: the design's value (alpha intact) in
/// true colour, and the generator's pre-image otherwise - the value imtui maps
/// to the entry named in `tokens_generated.h`.
ImVec4 resolve(ColorToken token, ColorDepth depth) {
    const int index = tokenIndex(token);
    const TokenTable& table = builtTable();
    switch (depth) {
    case ColorDepth::Ansi256:
        return table.xterm256[index];
    case ColorDepth::Ansi16:
        return table.ansi16[index];
    case ColorDepth::TrueColor:
    case ColorDepth::Count:
        break;
    }
    return table.rgb[index];
}

// ---------------------------------------------------------------------------
// Token -> ImGuiCol
// ---------------------------------------------------------------------------

/// The one place a colour reaches ImGui. `steps` is how many hover/active steps
/// above the token the slot sits.
///
/// Fill policy, learned from the guards `test_theme_contrast` pins (a fill must
/// stay neutral and may not be a role tint, and a role label must keep 4.5:1 on
/// it):
///
///  * a row or header fill stays inside the ink family (`raised`, `lineSoft`,
///    `inset`, `canvas` and one step above) and never exceeds ~0.09 luminance,
///    because its label is a role colour - a brighter fill drops the label under
///    4.5:1;
///  * a control fill may be brighter (`textDim`) because its label is `textHi`,
///    which clears 5.6:1 there, and `textDim` + one step still clears 4.65:1;
///  * a pressed control goes *darker* (`lineSoft`), not brighter: the brightest
///    legal step is already taken by the hover state;
///  * `selection` (accent at 14 %) is the design's row fill. It is bound where
///    ImGui draws no role label - the editing field and the text selection
///    rectangle - so the "a role may never be a fill" rule stays intact.
///
/// Slots deliberately left at ImGui's stock dark theme: Tab/Table/Plot/TreeLines/
/// DragDrop/CheckMark/NavWindowing* - widgets the workbench does not draw. Naming
/// them here would invent palette entries that no token carries, and task 20 owns
/// the style pass that decides their fate.
constexpr ColorBinding kBindings[] = {
    // Text. `textDim` is never here: it is the one ink below the 4.5:1 bar.
    {ColorToken::textHi, ImGuiCol_Text, 0},
    {ColorToken::textLow, ImGuiCol_TextDisabled, 0},
    // Surfaces. The menu-bar slot is themed too, so a stock ImGui menu bar that
    // appeared would not fall back to its grey.
    {ColorToken::canvas, ImGuiCol_WindowBg, 0},
    {ColorToken::surface, ImGuiCol_ChildBg, 0},
    {ColorToken::surface, ImGuiCol_MenuBarBg, 0},
    {ColorToken::raised, ImGuiCol_PopupBg, 0},
    // Essential boundaries (window/child/menu frames, card edges): the token the
    // design reserves for them, 3.1:1 on the surface it bounds.
    {ColorToken::lineStrong, ImGuiCol_Border, 0},
    // Decorative hairlines drawn by ImGui itself (menu separators). Views draw
    // their own rules from `lineSoft` directly.
    {ColorToken::lineSoft, ImGuiCol_Separator, 0},
    // Disclosure rows: `lineSoft` at rest, one and two steps above while hovered
    // and held. A disclosure header carries a role-coloured label, so its fill has
    // to stay in the dark band (see the fill policy above).
    {ColorToken::lineSoft, ImGuiCol_Header, 0},
    {ColorToken::lineSoft, ImGuiCol_HeaderHovered, 1},
    {ColorToken::lineSoft, ImGuiCol_HeaderActive, 2},
    // Input wells, one step up on hover, and the accent tint while a field is
    // being edited (the design's accent.quiet row fill).
    {ColorToken::inset, ImGuiCol_FrameBg, 0},
    {ColorToken::inset, ImGuiCol_FrameBgHovered, 1},
    {ColorToken::selection, ImGuiCol_FrameBgActive, 0},
    // The text-selection rectangle inside an input. `selection` is a fill and
    // never a role colour, so the "no role fill" scan stays satisfied.
    {ColorToken::selection, ImGuiCol_TextSelectedBg, 0},
    // Actions: `textDim` at rest (a visible chip, 2.6:1 against the surface), one
    // step up on hover, and pressed *into* the surface (`lineSoft`) so the press
    // reads as a move in the opposite direction instead of a second brightening.
    {ColorToken::textDim, ImGuiCol_Button, 0},
    {ColorToken::textDim, ImGuiCol_ButtonHovered, 1},
    {ColorToken::lineSoft, ImGuiCol_ButtonActive, 0},
    // Scrollbars. The track is the canvas so a dark grab reads against it; the
    // grab is `textDim`, the decorative ink (a fill, never a glyph).
    {ColorToken::canvas, ImGuiCol_ScrollbarBg, 0},
    {ColorToken::textDim, ImGuiCol_ScrollbarGrab, 0},
    {ColorToken::textDim, ImGuiCol_ScrollbarGrabHovered, 1},
    {ColorToken::textDim, ImGuiCol_ScrollbarGrabActive, 2},
    // The 1-cell text grid routes the InputText caret and the nav highlight
    // through NavCursor; ImGui's stock NavHighlight aliases it and a backend
    // init resets it to transparent, so it is bound here to a text ink - the
    // caret is a glyph, and `textDim` (2.9:1) would be too faint to be one.
    {ColorToken::textLow, ImGuiCol_NavCursor, 0},
};

/// Theme and depth of the last application, plus the counter the tests read.
/// File scope because the thing it describes - `ImGui::GetStyle()` - is global
/// too: a second theme would already have one style to write into.
struct AppliedTheme {
    ThemeId theme = ThemeId::Graphite;
    ColorDepth depth = ColorDepth::TrueColor;
    unsigned count = 0;
};
AppliedTheme g_applied;

const char* envValue(EnvLookup lookup, const char* name) {
    return lookup != nullptr ? lookup(name) : std::getenv(name);
}

char toLower(char value) {
    return (value >= 'A' && value <= 'Z') ? char(value - 'A' + 'a') : value;
}

bool containsIgnoreCase(const char* haystack, const char* needle) {
    if (haystack == nullptr || needle == nullptr)
        return false;
    const size_t length = std::strlen(needle);
    for (const char* at = haystack; *at != '\0'; ++at) {
        size_t i = 0;
        while (i < length && at[i] != '\0' && toLower(at[i]) == toLower(needle[i]))
            ++i;
        if (i == length)
            return true;
    }
    return false;
}

bool equalsIgnoreCase(const char* value, const char* expected) {
    if (value == nullptr || expected == nullptr)
        return false;
    size_t i = 0;
    while (value[i] != '\0' && expected[i] != '\0' && toLower(value[i]) == toLower(expected[i]))
        ++i;
    return value[i] == '\0' && expected[i] == '\0';
}

} // namespace

// ---------------------------------------------------------------------------
// Public accessors
// ---------------------------------------------------------------------------

const TokenTable& tokens() { return builtTable(); }

ResolvedToken token(ColorToken colour, ColorDepth depth) {
    const int index = tokenIndex(colour);
    const TokenTable& table = builtTable();
    ResolvedToken resolved;
    resolved.rgb = resolve(colour, depth);
    switch (depth) {
    case ColorDepth::Ansi256:
        resolved.paletteIndex = table.xterm256Index[index];
        break;
    case ColorDepth::Ansi16:
        resolved.paletteIndex = table.ansi16Index[index];
        break;
    case ColorDepth::TrueColor:
    case ColorDepth::Count:
        resolved.paletteIndex = -1;
        break;
    }
    return resolved;
}

ImVec4 color(ColorToken colour) { return resolve(colour, g_applied.depth); }

const char* tokenName(ColorToken colour) {
    static const char* const kNames[kColorTokenCount] = {
        "canvas",     "surface", "raised",    "inset",       "lineSoft",  "lineStrong", "textHi",
        "textMid",    "textLow", "textDim",   "accent",      "roleYou",   "roleAgent",  "roleTool",
        "roleSystem", "stateOk", "stateWarn", "stateDanger", "selection", "focusRing",
    };
    static_assert(int(sizeof(kNames) / sizeof(kNames[0])) == kColorTokenCount,
                  "tokenName() must name every ColorToken");
    return inRange(colour) ? kNames[static_cast<int>(colour)] : "<invalid>";
}

float space(int step) { return stepOf(kSpaceSteps, kSpaceCount, step); }
float radius(int step) { return stepOf(kRadiusSteps, kRadiusCount, step); }
float typeSize(int step) { return stepOf(kTypeSteps, kTypeCount, step); }
float motionMs(int step) { return stepOf(kMotionSteps, kMotionCount, step); }

FontFace uiFontFace() { return kUiFace; }
FontFace monoFontFace() { return kMonoFace; }
const char* uiFontFamily() { return kUiFace.family; }
const char* monoFontFamily() { return kMonoFace.family; }

/// The code face the chain installed. File scope because the thing it describes -
/// the live font atlas - is global too.
ImFont* g_codeFont = nullptr;

void setCodeFont(ImFont* font) { g_codeFont = font; }
ImFont* codeFont() { return g_codeFont; }

/// The base the type scale was applied with.
///
/// Not `ImGuiStyle::FontSizeBase`: `PushFont()` writes the size it pushed into that field (ImGui's
/// `UpdateCurrentFontSize`), so inside any scope the style answers "the step that is open" rather
/// than "the size the profile draws at". A scope that read the style would scale a step by a step -
/// `label` inside `label` came out at 13 * 13 / 15 = 11 px, which is how the top bar measured
/// every plain-text box 15 % narrower than it drew it and clipped the last glyph of the wordmark
/// and of the palette hint.
float g_appliedBase = 0.0f;

float scaledTypeSize(TypeStep step, float basePx) {
    const float body = typeSize(int(TypeStep::body));
    const float stepPx = typeSize(int(step));
    // 0 (or a nonsense negative) means "the type scale owns the size", which is
    // also the answer a caller gets if it asks before `applyTypeScale` ran.
    if (!(basePx > 0.0f))
        return stepPx;
    // The ratio, not the absolute size, so a configured base keeps the hierarchy:
    // a 30 px base renders a title at 36 px, not at 18.
    const float scaled = basePx * (stepPx / body);
    // A sub-pixel size would bake an unreadable atlas entry; one pixel is the
    // floor ImGui itself uses for a usable glyph.
    return scaled < 1.0f ? 1.0f : scaled;
}

void applyTypeScale(float basePx) {
    // No context (a unit test that never made one) is not a reason to write into
    // a null style: the call is start-up plumbing, not a state change a caller
    // can observe.
    if (ImGui::GetCurrentContext() == nullptr)
        return;
    g_appliedBase = scaledTypeSize(TypeStep::body, basePx);
    ImGui::GetStyle().FontSizeBase = g_appliedBase;
}

TypeScope::TypeScope(TypeStep step, bool textGrid) {
    // Two reasons to do nothing, both normal: the text grid (one cell, one size)
    // and a caller outside a frame (there is no current font to scale, and
    // PushFont() asserts on one).
    if (textGrid || ImGui::GetCurrentContext() == nullptr || ImGui::GetFont() == nullptr)
        return;
    // A step is absolute, so the size comes from the applied base and never from the scope that
    // happens to be open: `label` inside `label` is still 13 px, and `meta` inside `title` is still
    // 12 px rather than 12 * 18 / 15. Nesting is therefore free rather than wrong - the bar
    // measures itself inside its own label scope (see `topBarMeasure`).
    ImFont* face = step == TypeStep::code ? g_codeFont : nullptr;
    const float size = scaledTypeSize(step, g_appliedBase);
    // Already drawing this step (a nested scope, or the body step at the base): a second push of
    // the same size would be invisible, so it is skipped and the frame keeps its stack shallow.
    if (std::fabs(size - ImGui::GetFontSize()) < 0.5f &&
        (face == nullptr || face == ImGui::GetFont()))
        return;
    ImGui::PushFont(face, size);
    pushed_ = true;
}

TypeScope::~TypeScope() {
    if (pushed_)
        ImGui::PopFont();
}

const ColorBinding* colorBindings(int& count) {
    count = int(sizeof(kBindings) / sizeof(kBindings[0]));
    return kBindings;
}

ImVec4 stateStep(ImVec4 base, int steps) {
    if (steps <= 0)
        return base;
    const float amount = kStateStep * float(steps);
    return ImVec4(std::min(base.x + amount, 1.0f), std::min(base.y + amount, 1.0f),
                  std::min(base.z + amount, 1.0f), base.w);
}

void applyTheme(ThemeId theme, ColorDepth depth) {
    // One theme ships (D-02): the argument names the choice the settings will
    // eventually carry, and anything else falls back to Graphite rather than
    // leaving the UI half-themed.
    if (theme != ThemeId::Graphite)
        theme = ThemeId::Graphite;
    if (depth != ColorDepth::Ansi256 && depth != ColorDepth::Ansi16)
        depth = ColorDepth::TrueColor;

    // A defined base for the slots the token map does not name (see kBindings).
    ImGui::StyleColorsDark();

    ImVec4* colors = ImGui::GetStyle().Colors;
    int count = 0;
    const ColorBinding* bindings = colorBindings(count);
    for (int i = 0; i < count; ++i) {
        const ColorBinding& binding = bindings[i];
        const ImVec4 base = resolve(binding.token, depth);
        colors[binding.slot] = binding.steps == 0 ? base : stateStep(base, binding.steps);
    }

    g_applied.theme = theme;
    g_applied.depth = depth;
    // No early-out on a repeated (theme, depth): a backend init and a test
    // harness both reset style colours, so the caller asking again must get the
    // theme again. The frame loop simply never asks (the counter says so).
    ++g_applied.count;
}

ColorDepth detectColorDepth(EnvLookup lookup) {
    // Design §4.15 order, one reason per line:
    //   1. NO_COLOR asks for no colour at all, so it wins over any capability
    //      (the MVP maps it to the coarsest palette instead of attribute-only
    //      rendering - see the gap recorded in tokens.h);
    //   2. COLORTERM is the only widely-set true-colour advertisement;
    //   3. a 256-colour TERM;
    //   4. everything else, including TERM=linux and an empty environment.
    const char* noColor = envValue(lookup, "NO_COLOR");
    if (noColor != nullptr && *noColor != '\0')
        return ColorDepth::Ansi16;
    const char* colorTerm = envValue(lookup, "COLORTERM");
    if (equalsIgnoreCase(colorTerm, "truecolor") || equalsIgnoreCase(colorTerm, "24bit"))
        return ColorDepth::TrueColor;
    if (containsIgnoreCase(envValue(lookup, "TERM"), "256color"))
        return ColorDepth::Ansi256;
    return ColorDepth::Ansi16;
}

ColorDepth detectColorDepth() { return detectColorDepth(nullptr); }

ThemeId currentTheme() { return g_applied.theme; }

ColorDepth currentColorDepth() { return g_applied.depth; }

unsigned themeApplyCount() { return g_applied.count; }

} // namespace llmfun::tui::ui
