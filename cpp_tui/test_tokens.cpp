// test_tokens.cpp
//
// Unit test for the Ink/Graphite token module (task 12, D-THEME, NFR-09).
//
// The token header is the one place every colour, size and distance comes from,
// and `applyTheme` is the one place a colour reaches ImGui. This binary pins
// what that contract rests on:
//
//   1. table        the generated table has one row per ColorToken, and each
//                   coarse column really resolves to the palette entry it names:
//                   imtui's own quantizer (mirrored here from
//                   vendor/imtui/src/imtui-impl-text.cpp) maps the applied value
//                   straight back to `xterm256Index`. That round trip is what a
//                   byte-order mistake or a drifting entry breaks - a swapped R
//                   and B is still "some palette colour", so a membership check
//                   would not have caught it;
//   2. hue          a chromatic token keeps its dominant channel at both coarse
//                   depths (a swap turns state.danger red into blue-violet and
//                   accent blue into orange, while every grey stays grey);
//   3. contrast     every text/background pair the theme can draw reaches 4.5:1
//                   and every essential boundary 3:1 (WCAG 2.1 relative
//                   luminance), measured on what the display *shows*: the design
//                   colours in true colour and the palette entries at 256
//                   colours. The pair list is the usage matrix; every exclusion
//                   is named in the header comment below with its number;
//   4. no dim text  `text.dim` is a decorative ink: no text-bearing slot may bind
//                   it, and its measured contrast stays below the text bar;
//   5. mapping      `applyTheme` walks the binding table: every slot holds the
//                   value of its token (base + hover steps) at every depth;
//   6. once per     a frame loop never re-applies the theme (the counter is
//      theme change unchanged across five rendered frames);
//   7. detection    `NO_COLOR` -> `COLORTERM=truecolor|24bit` -> `TERM=*256color*`
//                   -> 16, pinned against an injected environment, plus the glue
//                   that picks the depth for a render profile;
//   8. scales       spacing, radii, type steps and motion durations are the
//                   documented values, monotone, and clamp at both ends;
//   9. type scale   the six GUI steps are applied - the body step is the pixel
//                   profile's base size, a TypeScope moves the size inside a frame
//                   and puts it back, and on the text grid it moves nothing;
//  10. type budget  no view names more than three type steps and every step it
//                   names exists (design §4.6), scanned over views/*.cpp.
//
// Documented exclusions (measured, deliberate - not oversights):
//   * text.low on `raised`: 4.29:1 true / 4.32:1 at 256 colours. Meta text
//     (timestamps, counts) is drawn on the thread surface, the rail and the now
//     bar; a raised card carries body and secondary text. The design's own column
//     puts text.low at 4.6:1 on `surface`, and lifting it to clear `raised` would
//     move the token onto the next grey-ramp step, away from #77808F -> 244.
//   * line.strong on `raised`: 2.95:1 true / 2.79:1 at 256 colours. Essential
//     boundaries bound inputs and cards *on* a surface and split regions on the
//     canvas; a raised card is a fill step, not an outlined box.
//   * text.dim and line.soft are decorative by design (2.5-2.9:1 and 1.2-1.4:1):
//     they are asserted *below* the bars instead of at them.
//   * the ANSI-16 depth carries no contrast matrix: 16 colours cannot express the
//     ink steps (all four surfaces are black), so that depth is checked for
//     structure and hue only.
//
// Exit codes: 0 = every check passed; 1 = at least one failed.
// Run from the build dir:  ./test_tokens [-v]

#include "tui.h"
#include "ui/tokens.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace ui = llmfun::tui::ui;

namespace {

int g_failures = 0;
bool g_verbose = false;

/// WCAG 2.1 AA for body text (NFR-09).
constexpr float MinTextContrast = 4.5f;
/// WCAG 1.4.11 for boundaries and other non-text affordances.
constexpr float MinBoundaryContrast = 3.0f;

void check(bool condition, const std::string& what) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

std::string describe(ui::ColorToken token, ui::ColorDepth depth) {
    const char* name = depth == ui::ColorDepth::TrueColor ? "truecolour"
                       : depth == ui::ColorDepth::Ansi256 ? "256 colours"
                                                          : "16 colours";
    return std::string(ui::tokenName(token)) + " at " + name;
}

// ---------------------------------------------------------------------------
// Colour measurement
// ---------------------------------------------------------------------------

float srgbToLinear(float channel) {
    return channel <= 0.04045f ? channel / 12.92f : std::pow((channel + 0.055f) / 1.055f, 2.4f);
}

float relativeLuminance(const ImVec4& colour) {
    return 0.2126f * srgbToLinear(colour.x) + 0.7152f * srgbToLinear(colour.y) +
           0.0722f * srgbToLinear(colour.z);
}

float contrastRatio(const ImVec4& a, const ImVec4& b) {
    const float la = relativeLuminance(a);
    const float lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

/// Source-over, i.e. what the pixel profile does with a translucent fill. The
/// coarse rows are already flattened by the generator (a terminal cannot blend),
/// so this is only used at TrueColor.
ImVec4 compositeOver(const ImVec4& fg, const ImVec4& bg) {
    const float alpha = fg.w;
    return ImVec4(fg.x * alpha + bg.x * (1.0f - alpha), fg.y * alpha + bg.y * (1.0f - alpha),
                  fg.z * alpha + bg.z * (1.0f - alpha), 1.0f);
}

int channel8(float value) {
    return int(std::lround(std::max(0.0f, std::min(1.0f, value)) * 255.0f));
}

/// imtui's `rgbToAnsi256`, mirrored from
/// `vendor/imtui/src/imtui-impl-text.cpp:131-165` (glyph path: no alpha scaling).
/// This is the quantizer the terminal applies to every style colour, so it is the
/// oracle for "what does the glass show".
int quantizeAnsi256(const ImVec4& colour) {
    int r = channel8(colour.x);
    int g = channel8(colour.y);
    int b = channel8(colour.z);
    if (r == g && g == b) {
        if (r < 8)
            return 16;
        if (r > 248)
            return 231;
        return int(std::lround((float(r - 8) / 247.0f) * 24.0f)) + 232;
    }
    return 16 + 36 * int(std::lround(float(r) / 255.0f * 5.0f)) +
           6 * int(std::lround(float(g) / 255.0f * 5.0f)) +
           int(std::lround(float(b) / 255.0f * 5.0f));
}

/// The RGB of an xterm-256 palette entry (16 = the cube's black, 232-255 = the
/// grey ramp). Indices 0-15 are the terminal's own palette; treat them as black.
ImVec4 palette256(int index) {
    static const float levels[6] = {0.0f, 95.0f, 135.0f, 175.0f, 215.0f, 255.0f};
    if (index >= 232) {
        const float v = float(8 + 10 * (index - 232)) / 255.0f;
        return ImVec4(v, v, v, 1.0f);
    }
    if (index >= 16) {
        const int i = index - 16;
        return ImVec4(levels[i / 36] / 255.0f, levels[(i / 6) % 6] / 255.0f, levels[i % 6] / 255.0f,
                      1.0f);
    }
    return ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
}

/// The 16 base colours as xterm ships them.
ImVec4 palette16(int index) {
    static const int table[16][3] = {
        {0, 0, 0},       {128, 0, 0},   {0, 128, 0},   {128, 128, 0},
        {0, 0, 128},     {128, 0, 128}, {0, 128, 128}, {192, 192, 192},
        {128, 128, 128}, {255, 0, 0},   {0, 255, 0},   {255, 255, 0},
        {0, 0, 255},     {255, 0, 255}, {0, 255, 255}, {255, 255, 255},
    };
    const int slot = (index >= 0 && index < 16) ? index : 0;
    return ImVec4(float(table[slot][0]) / 255.0f, float(table[slot][1]) / 255.0f,
                  float(table[slot][2]) / 255.0f, 1.0f);
}

/// What a token renders as at a depth, with `under` behind it when it is
/// translucent. At the coarse depths this answers with the palette entry the
/// terminal paints (`palette256`/`palette16`), not with the applied pre-image.
ImVec4 rendered(ui::ColorToken token, ui::ColorDepth depth, ui::ColorToken under) {
    if (depth == ui::ColorDepth::Ansi256)
        return palette256(ui::token(token, depth).paletteIndex);
    if (depth == ui::ColorDepth::Ansi16)
        return palette16(ui::token(token, depth).paletteIndex);
    const ImVec4 value = ui::token(token, depth).rgb;
    if (value.w >= 1.0f)
        return ImVec4(value.x, value.y, value.z, 1.0f);
    return compositeOver(value, rendered(under, depth, under));
}

/// 0 = red leads, 1 = green, 2 = blue. A grey has no dominant channel, which is
/// why the hue check below only covers chromatic tokens.
int dominantChannel(const ImVec4& colour) {
    if (colour.x >= colour.y && colour.x >= colour.z)
        return 0;
    return colour.y >= colour.z ? 1 : 2;
}

float spread(const ImVec4& colour) {
    const float lo = std::min({colour.x, colour.y, colour.z});
    const float hi = std::max({colour.x, colour.y, colour.z});
    return hi - lo;
}

// ---------------------------------------------------------------------------
// 1. Table integrity and the palette round trip
// ---------------------------------------------------------------------------

void checkTable() {
    check(ui::kColorTokenCount == 20, "the Graphite palette has 20 semantic colours (it has " +
                                          std::to_string(ui::kColorTokenCount) + ")");
    const ui::TokenTable& table = ui::tokens();
    for (int i = 0; i < ui::kColorTokenCount; ++i) {
        const ui::ColorToken token = static_cast<ui::ColorToken>(i);
        const std::string name = ui::tokenName(token);

        const ImVec4 rgb = table.rgb[i];
        for (const float channel : {rgb.x, rgb.y, rgb.z})
            check(channel >= 0.0f && channel <= 1.0f, name + ": every RGB channel is inside 0..1");
        check(rgb.w > 0.0f && rgb.w <= 1.0f, name + ": alpha is inside 0..1");
        check(rgb.w == 1.0f || token == ui::ColorToken::selection ||
                  token == ui::ColorToken::focusRing,
              name + ": only the two view-level fills are translucent (alpha " +
                  std::to_string(rgb.w) + ")");

        // The 256-colour column: the applied value must quantize straight back to
        // the entry the table names. This is the assertion a byte-order swap or a
        // drifting palette entry fails, and a membership check would not.
        const int index = table.xterm256Index[i];
        check(index >= 16 && index <= 255,
              name + ": the xterm-256 index is 16..255 (it is " + std::to_string(index) + ")");
        const int roundTrip = quantizeAnsi256(table.xterm256[i]);
        check(roundTrip == index, name + ": the applied 256-colour value must quantize to entry " +
                                      std::to_string(index) + " (imtui would show " +
                                      std::to_string(roundTrip) +
                                      ") - regenerate tokens_generated.h");
        check(spread(table.xterm256[i]) == 0.0f || token == ui::ColorToken::selection ||
                  token == ui::ColorToken::focusRing || spread(rgb) > 0.12f,
              name + ": an ink's 256-colour value is a pure grey (imtui takes the grey ramp "
                     "only for equal channels)");

        // The 16-colour column: the applied value is the base colour it names.
        const int base = table.ansi16Index[i];
        check(base >= 0 && base <= 15,
              name + ": the ANSI-16 index is 0..15 (it is " + std::to_string(base) + ")");
        const ImVec4 baseRgb = palette16(base);
        check(channel8(table.ansi16[i].x) == channel8(baseRgb.x) &&
                  channel8(table.ansi16[i].y) == channel8(baseRgb.y) &&
                  channel8(table.ansi16[i].z) == channel8(baseRgb.z),
              name + ": the 16-colour value is the base colour it names");
    }

    // The two view-level fills are the accent at two alphas (design §4.2:
    // accent.quiet for a selected row, a 60 % accent ring).
    const ui::ColorToken accent = ui::ColorToken::accent;
    for (const ui::ColorToken fill : {ui::ColorToken::selection, ui::ColorToken::focusRing}) {
        check(table.rgb[int(fill)].x == table.rgb[int(accent)].x &&
                  table.rgb[int(fill)].y == table.rgb[int(accent)].y &&
                  table.rgb[int(fill)].z == table.rgb[int(accent)].z,
              std::string(ui::tokenName(fill)) + " is the accent colour");
    }
    check(table.rgb[int(ui::ColorToken::selection)].w < table.rgb[int(ui::ColorToken::focusRing)].w,
          "the selection fill is quieter than the focus ring");
}

// ---------------------------------------------------------------------------
// 2. Hue survives the coarse depths
// ---------------------------------------------------------------------------

void checkHue(ui::ColorDepth depth) {
    const ui::TokenTable& table = ui::tokens();
    for (int i = 0; i < ui::kColorTokenCount; ++i) {
        const ui::ColorToken token = static_cast<ui::ColorToken>(i);
        const ImVec4 design = table.rgb[i];
        // A grey has no hue to preserve, and a translucent fill is composited to a
        // grey on purpose (a terminal cannot blend the accent tint).
        if (spread(design) <= 0.12f || design.w < 1.0f)
            continue;
        const ImVec4 shown = rendered(token, depth, ui::ColorToken::surface);
        check(dominantChannel(shown) == dominantChannel(design),
              describe(token, depth) + " keeps its dominant channel (design " +
                  std::to_string(dominantChannel(design)) + ", shown " +
                  std::to_string(dominantChannel(shown)) + ") - a swapped R and B shows up here");
    }
    // The two colours an operator reads as "this failed" and "this is a link":
    // red and blue are the hallmarks of a byte-order mistake.
    check(dominantChannel(rendered(ui::ColorToken::stateDanger, depth, ui::ColorToken::canvas)) ==
              0,
          std::string("state.danger is red at ") +
              (depth == ui::ColorDepth::Ansi256 ? "256 colours" : "16 colours"));
    check(dominantChannel(rendered(ui::ColorToken::accent, depth, ui::ColorToken::canvas)) == 2,
          std::string("accent is blue at ") +
              (depth == ui::ColorDepth::Ansi256 ? "256 colours" : "16 colours"));
}

// ---------------------------------------------------------------------------
// 3. Contrast matrix
// ---------------------------------------------------------------------------

struct SurfaceRule {
    ui::ColorToken token;
    const char* name;
    unsigned bit;
};

/// One bit per surface, so a rule states exactly where a token is drawn and an
/// exclusion is visible in the table rather than implied by an index.
enum SurfaceBit : unsigned {
    kCanvas = 1u << 0,
    kSurface = 1u << 1,
    kRaised = 1u << 2,
    kInset = 1u << 3,
};
constexpr unsigned kEverySurface = kCanvas | kSurface | kRaised | kInset;

const SurfaceRule kAllSurfaces[] = {
    {ui::ColorToken::canvas, "canvas", kCanvas},
    {ui::ColorToken::surface, "surface", kSurface},
    {ui::ColorToken::raised, "raised", kRaised},
    {ui::ColorToken::inset, "inset", kInset},
};

struct ContrastRule {
    ui::ColorToken text;
    float minimum;
    /// The surfaces the token is drawn on (SurfaceBit mask).
    unsigned surfaces;
    /// Measure it on a selected row as well (the `selection` fill over surface).
    bool onSelection;
};

/// The text tokens and where they are drawn. Body text everywhere; meta text
/// everywhere except a raised card (see the header's measured exclusion); role
/// and state colours on every surface, because a role label sits on the thread
/// surface *and* on a raised card.
const ContrastRule kTextRules[] = {
    {ui::ColorToken::textHi, MinTextContrast, kEverySurface, true},
    {ui::ColorToken::textMid, MinTextContrast, kEverySurface, true},
    {ui::ColorToken::textLow, MinTextContrast, kCanvas | kSurface | kInset, false},
    {ui::ColorToken::accent, MinTextContrast, kEverySurface, true},
    {ui::ColorToken::roleYou, MinTextContrast, kEverySurface, true},
    {ui::ColorToken::roleAgent, MinTextContrast, kEverySurface, true},
    {ui::ColorToken::roleTool, MinTextContrast, kEverySurface, true},
    {ui::ColorToken::roleSystem, MinTextContrast, kEverySurface, true},
    {ui::ColorToken::stateOk, MinTextContrast, kEverySurface, true},
    {ui::ColorToken::stateWarn, MinTextContrast, kEverySurface, true},
    {ui::ColorToken::stateDanger, MinTextContrast, kEverySurface, true},
};

/// Boundaries: the essential border clears 3:1 on the surfaces it bounds (see the
/// header's exclusion for `raised`); the focus ring is drawn around a control on
/// any surface.
const ContrastRule kBoundaryRules[] = {
    {ui::ColorToken::lineStrong, MinBoundaryContrast, kCanvas | kSurface | kInset, false},
    {ui::ColorToken::focusRing, MinBoundaryContrast, kEverySurface, false},
};

void checkRule(const ContrastRule& rule, ui::ColorDepth depth, bool boundary) {
    const ImVec4 value = rendered(rule.text, depth, ui::ColorToken::surface);
    for (const SurfaceRule& surface : kAllSurfaces) {
        if ((rule.surfaces & surface.bit) == 0)
            continue;
        const ImVec4 fill = rendered(surface.token, depth, surface.token);
        const float ratio = contrastRatio(value, fill);
        if (g_verbose)
            std::fprintf(stderr, "  %-11s on %-7s %6.2f:1 (need %.1f) %s\n",
                         ui::tokenName(rule.text), surface.name, ratio, rule.minimum,
                         ratio >= rule.minimum ? "ok" : "FAIL");
        check(ratio >= rule.minimum, describe(rule.text, depth) + " contrasts " +
                                         std::to_string(ratio) + ":1 on " + surface.name +
                                         (boundary ? " as a boundary" : "") + " (needs " +
                                         std::to_string(rule.minimum) + ":1)");
    }
    if (rule.onSelection) {
        const ImVec4 fill = rendered(ui::ColorToken::selection, depth, ui::ColorToken::surface);
        const float ratio = contrastRatio(value, fill);
        check(ratio >= rule.minimum, describe(rule.text, depth) + " contrasts " +
                                         std::to_string(ratio) + ":1 on a selected row");
    }
}

void checkMatrix(ui::ColorDepth depth) {
    for (const ContrastRule& rule : kTextRules)
        checkRule(rule, depth, /*boundary=*/false);
    for (const ContrastRule& rule : kBoundaryRules)
        checkRule(rule, depth, /*boundary=*/true);

    // The decorative inks stay below the bars, measured rather than declared.
    const ImVec4 canvas = rendered(ui::ColorToken::canvas, depth, ui::ColorToken::canvas);
    const ImVec4 surface = rendered(ui::ColorToken::surface, depth, ui::ColorToken::surface);
    for (const ui::ColorToken ink : {ui::ColorToken::textDim, ui::ColorToken::lineSoft}) {
        const ImVec4 value = rendered(ink, depth, ui::ColorToken::surface);
        const float onCanvas = contrastRatio(value, canvas);
        const float onSurface = contrastRatio(value, surface);
        if (g_verbose)
            std::fprintf(stderr, "  %-11s decorative: %6.2f:1 canvas, %6.2f:1 surface\n",
                         ui::tokenName(ink), onCanvas, onSurface);
        check(onCanvas < MinBoundaryContrast && onSurface < MinBoundaryContrast,
              describe(ink, depth) + " is decorative: it must stay below " +
                  std::to_string(MinBoundaryContrast) + ":1 on every surface");
    }
}

// ---------------------------------------------------------------------------
// 4. text.dim never carries a glyph
// ---------------------------------------------------------------------------

bool isTextSlot(ImGuiCol slot) {
    // The slots ImGui draws a glyph through: the body text, the disabled or
    // placeholder text, and the nav cursor - which is the InputText caret and the
    // nav highlight rectangle on the 1-cell grid.
    return slot == ImGuiCol_Text || slot == ImGuiCol_TextDisabled || slot == ImGuiCol_NavCursor;
}

/// The class a token belongs to: its text rule, or nullptr when the token is not a
/// text colour (a surface, a line, a fill). A text-bearing slot must carry a token
/// with a rule, and is only measured on the surfaces that rule names.
const ContrastRule* ruleFor(ui::ColorToken token) {
    for (const ContrastRule& rule : kTextRules) {
        if (rule.text == token)
            return &rule;
    }
    return nullptr;
}

void checkDimIsNotText(ui::ColorDepth depth) {
    int count = 0;
    const ui::ColorBinding* bindings = ui::colorBindings(count);
    check(count > 0, "the theme publishes its token -> ImGuiCol map");

    bool dimBound = false;
    for (int i = 0; i < count; ++i) {
        const ui::ColorBinding& binding = bindings[i];
        if (binding.token == ui::ColorToken::textDim)
            dimBound = true;
        if (!isTextSlot(binding.slot))
            continue;
        check(binding.token != ui::ColorToken::textDim,
              std::string("text.dim is bound to ") + ImGui::GetStyleColorName(binding.slot) +
                  ", which draws text: text.dim is a decorative ink (NFR-09)");
        // Whatever else a text slot receives must clear the text bar on the
        // surfaces that token is drawn on - the half of the rule a future palette
        // edit trips, measured rather than declared.
        const ContrastRule* rule = ruleFor(binding.token);
        check(rule != nullptr, std::string(ImGui::GetStyleColorName(binding.slot)) + " carries " +
                                   ui::tokenName(binding.token) + ", which is not a text token");
        if (rule == nullptr)
            continue;
        const ImVec4 text = rendered(rule->text, depth, ui::ColorToken::surface);
        for (const SurfaceRule& surface : kAllSurfaces) {
            if ((rule->surfaces & surface.bit) == 0)
                continue;
            const ImVec4 fill = rendered(surface.token, depth, surface.token);
            const float ratio = contrastRatio(text, fill);
            check(ratio >= MinTextContrast, std::string(ImGui::GetStyleColorName(binding.slot)) +
                                                " carries " + ui::tokenName(binding.token) +
                                                " at " + std::to_string(ratio) + ":1 on " +
                                                surface.name + " (a text slot needs " +
                                                std::to_string(MinTextContrast) + ":1)");
        }
    }
    check(dimBound, "text.dim is used by the theme (a token nothing binds is dead weight)");

    // And the palette half: the dim ink is genuinely dim, so it cannot be a text
    // token that merely *looks* acceptable today.
    const float onCanvas = contrastRatio(
        rendered(ui::ColorToken::textDim, ui::ColorDepth::TrueColor, ui::ColorToken::surface),
        rendered(ui::ColorToken::canvas, ui::ColorDepth::TrueColor, ui::ColorToken::canvas));
    check(onCanvas < MinTextContrast,
          "text.dim stays below the text bar on the canvas (it measures " +
              std::to_string(onCanvas) + ":1) - if it now clears it, decide whether it is text");
}

// ---------------------------------------------------------------------------
// 5. applyTheme walks the binding table
// ---------------------------------------------------------------------------

void checkMapping(ui::ColorDepth depth) {
    ui::applyTheme(ui::ThemeId::Graphite, depth);
    check(ui::currentTheme() == ui::ThemeId::Graphite, "applyTheme records the theme it applied");
    check(ui::currentColorDepth() == depth, "applyTheme records the depth it applied");

    int count = 0;
    const ui::ColorBinding* bindings = ui::colorBindings(count);
    for (int i = 0; i < count; ++i) {
        const ui::ColorBinding& binding = bindings[i];
        const ImVec4 base = ui::token(binding.token, depth).rgb;
        const ImVec4 expected = binding.steps == 0 ? base : ui::stateStep(base, binding.steps);
        const ImVec4 actual = ImGui::GetStyle().Colors[binding.slot];
        check(actual.x == expected.x && actual.y == expected.y && actual.z == expected.z &&
                  actual.w == expected.w,
              std::string(ImGui::GetStyleColorName(binding.slot)) + " must hold " +
                  ui::tokenName(binding.token) + " (" + describe(binding.token, depth) +
                  (binding.steps == 0 ? ")" : " + " + std::to_string(binding.steps) + " step(s))"));
    }

    const ui::TokenTable& table = ui::tokens();
    check(ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x ==
              ui::token(ui::ColorToken::canvas, depth).rgb.x,
          "the window background is the canvas token");
    check(table.rgb[int(ui::ColorToken::canvas)].w == 1.0f,
          "the canvas is opaque (a translucent root would let the desktop through)");

    // What a view asks for is the depth-resolved value, not the design's RGB: it
    // must be exactly the colour the depth table states, or a view would draw a
    // colour the display cannot show.
    for (int i = 0; i < ui::kColorTokenCount; ++i) {
        const ui::ColorToken token = static_cast<ui::ColorToken>(i);
        const ImVec4 fromView = ui::color(token);
        const ImVec4 fromDepth = ui::token(token, depth).rgb;
        check(fromView.x == fromDepth.x && fromView.y == fromDepth.y && fromView.z == fromDepth.z &&
                  fromView.w == fromDepth.w,
              std::string("ui::color(") + ui::tokenName(token) + ") is the depth-resolved value");
        check((ui::token(token, depth).paletteIndex >= 0) == (depth != ui::ColorDepth::TrueColor),
              std::string(ui::tokenName(token)) +
                  ": true colour has no palette index, the ANSI depths do");
    }

    // The 16-colour fallback keeps the app's structure: black surfaces, readable
    // inks. (No contrast matrix: 16 colours cannot express the ink steps.)
    if (depth == ui::ColorDepth::Ansi16) {
        for (const SurfaceRule& surface : kAllSurfaces) {
            const ImVec4 fill = rendered(surface.token, depth, surface.token);
            check(fill.x == 0.0f && fill.y == 0.0f && fill.z == 0.0f,
                  std::string(surface.name) + " falls back to black at 16 colours");
        }
        for (const ui::ColorToken ink : {ui::ColorToken::textHi, ui::ColorToken::textMid,
                                         ui::ColorToken::textLow, ui::ColorToken::lineStrong}) {
            const int base = table.ansi16Index[int(ink)];
            check(base >= 7, describe(ink, depth) + " picks a bright base colour (index " +
                                 std::to_string(base) + ")");
        }
    }
}

// ---------------------------------------------------------------------------
// 6. A frame loop does not re-apply the theme
// ---------------------------------------------------------------------------

ImTui::TScreen g_screen;

void frame(llmfun::tui::TuiState& state) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    llmfun::tui::tuiRender(state);
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
}

/// The glue `applyUiTheme()` owns: the pixel profile keeps the design colours,
/// the text grid asks the terminal. Runs on the grid the harness already has up,
/// so it does not initialize the text backend a second time.
void checkProfileGlue() {
    check(ImTui_TextEncodingActive, "the harness is on the text grid");
    // The glue promises equality in both directions: whatever the terminal says
    // (NO_COLOR -> 16, a 256-colour TERM -> Ansi256, a true-colour one -> the
    // design values) is what the grid applies. The implication this replaces
    // ("not true colour unless detected") also held when applyUiTheme() picked the
    // coarsest depth unconditionally.
    check(ui::currentColorDepth() == ui::detectColorDepth(),
          std::string("the text grid applies the terminal's colour depth (applied ") +
              std::to_string(int(ui::currentColorDepth())) + ", detected " +
              std::to_string(int(ui::detectColorDepth())) + ")");
}

void checkOncePerThemeChange() {
    llmfun::tui::TuiState state;
    state.iniFilename = "";
    ImTui_ImplText_Init();
    ImGui::GetIO().DisplaySize = ImVec2(100.0f, 30.0f);

    // The application's order: backend init, then the theme.
    llmfun::tui::applyUiTheme();
    const unsigned afterApply = ui::themeApplyCount();
    check(afterApply >= 1, "the theme was applied once by applyUiTheme()");
    checkProfileGlue();
    const ImVec4 applied = ImGui::GetStyle().Colors[ImGuiCol_Text];

    for (int i = 0; i < 5; ++i)
        frame(state);

    check(ui::themeApplyCount() == afterApply,
          "a frame loop never re-applies the theme (5 frames moved themeApplyCount from " +
              std::to_string(afterApply) + " to " + std::to_string(ui::themeApplyCount()) + ")");
    const ImVec4 after = ImGui::GetStyle().Colors[ImGuiCol_Text];
    check(after.x == applied.x && after.y == applied.y && after.z == applied.z,
          "the text colour survives five frames unchanged");

    ImTui_ImplText_Shutdown();
}

// ---------------------------------------------------------------------------
// 7. Depth detection
// ---------------------------------------------------------------------------

std::map<std::string, std::string> g_env;

const char* testEnv(const char* name) {
    const auto found = g_env.find(name);
    return found == g_env.end() ? nullptr : found->second.c_str();
}

void checkDetection() {
    struct Case {
        const char* what;
        std::map<std::string, std::string> env;
        ui::ColorDepth expected;
    };
    const std::vector<Case> cases = {
        {"an empty environment is a 16-colour terminal", {}, ui::ColorDepth::Ansi16},
        {"NO_COLOR wins over a true-colour advertisement",
         {{"NO_COLOR", "1"}, {"COLORTERM", "truecolor"}},
         ui::ColorDepth::Ansi16},
        {"an empty NO_COLOR means unset (the standard's letter)",
         {{"NO_COLOR", ""}, {"COLORTERM", "truecolor"}},
         ui::ColorDepth::TrueColor},
        {"COLORTERM=truecolor", {{"COLORTERM", "truecolor"}}, ui::ColorDepth::TrueColor},
        {"COLORTERM=24bit", {{"COLORTERM", "24bit"}}, ui::ColorDepth::TrueColor},
        {"COLORTERM is matched case-insensitively",
         {{"COLORTERM", "TrueColor"}},
         ui::ColorDepth::TrueColor},
        {"TERM=*-256color", {{"TERM", "xterm-256color"}}, ui::ColorDepth::Ansi256},
        {"true colour beats a 256-colour TERM",
         {{"TERM", "xterm-256color"}, {"COLORTERM", "truecolor"}},
         ui::ColorDepth::TrueColor},
        {"TERM=linux is a 16-colour console", {{"TERM", "linux"}}, ui::ColorDepth::Ansi16},
        {"an unknown COLORTERM value falls through to TERM",
         {{"COLORTERM", "yes"}, {"TERM", "screen-256color"}},
         ui::ColorDepth::Ansi256},
        {"a plain xterm has no 256-colour advertisement",
         {{"TERM", "xterm"}},
         ui::ColorDepth::Ansi16},
    };
    for (const Case& item : cases) {
        g_env = item.env;
        const ui::ColorDepth got = ui::detectColorDepth(testEnv);
        check(got == item.expected, std::string("depth detection: ") + item.what);
    }
    g_env.clear();
}

// ---------------------------------------------------------------------------
// 8. Size scales
// ---------------------------------------------------------------------------

void checkScales() {
    // Spacing: 0, 2, 4, 6, 8, 12, 16, 24 - strictly increasing after 0.
    const float expectSpace[] = {0.0f, 2.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 24.0f};
    for (int step = 0; step < 8; ++step) {
        check(ui::space(step) == expectSpace[step],
              "space(" + std::to_string(step) + ") is " + std::to_string(expectSpace[step]));
        if (step > 0)
            check(ui::space(step) > ui::space(step - 1), "spacing grows with the step");
    }
    check(ui::space(-1) == 0.0f, "a negative spacing step clamps to space(0)");
    check(ui::space(99) == 24.0f, "a spacing step past the scale clamps to space(7)");
    check(ui::space(int(ui::Space::comfy)) == 12.0f, "Space::comfy is 12");
    check(ui::space(int(ui::Space::page)) == 24.0f, "Space::page is 24");

    const float expectRadius[] = {0.0f, 4.0f, 6.0f};
    for (int step = 0; step < 3; ++step)
        check(ui::radius(step) == expectRadius[step],
              "radius(" + std::to_string(step) + ") is " + std::to_string(expectRadius[step]));
    check(ui::radius(-4) == 0.0f && ui::radius(7) == 6.0f, "radius clamps at both ends");
    check(ui::radius(int(ui::Radius::small)) == 4.0f, "Radius::small is 4 px");

    // Type: display 24 > title 18 > body 15 > code 14 > label 13 > meta 12. The
    // order is asserted, not just the values: the hierarchy is the contract (a
    // heading larger than body, meta smaller than a label), and these are the one
    // place a size may live (a view names a step, never a number).
    const float expectType[] = {24.0f, 18.0f, 15.0f, 13.0f, 12.0f, 14.0f};
    for (int step = 0; step < 6; ++step)
        check(ui::typeSize(step) == expectType[step],
              "typeSize(" + std::to_string(step) + ") is " + std::to_string(expectType[step]));
    check(ui::typeSize(int(ui::TypeStep::body)) == 15.0f, "the body step is 15 px");
    check(ui::typeSize(99) == 14.0f, "the type scale clamps at the code step");
    check(ui::typeSize(int(ui::TypeStep::display)) > ui::typeSize(int(ui::TypeStep::title)) &&
              ui::typeSize(int(ui::TypeStep::title)) > ui::typeSize(int(ui::TypeStep::body)) &&
              ui::typeSize(int(ui::TypeStep::body)) > ui::typeSize(int(ui::TypeStep::label)) &&
              ui::typeSize(int(ui::TypeStep::label)) > ui::typeSize(int(ui::TypeStep::meta)),
          "display > title > body > label > meta");
    check(ui::typeSize(int(ui::TypeStep::code)) == ui::typeSize(int(ui::TypeStep::label)) + 1.0f,
          "code sits one pixel above a label, so a code span in a label row still fits");

    const float expectMotion[] = {120.0f, 160.0f, 200.0f};
    for (int step = 0; step < 3; ++step)
        check(ui::motionMs(step) == expectMotion[step],
              "motionMs(" + std::to_string(step) + ") is " + std::to_string(expectMotion[step]));
    check(ui::motionMs(int(ui::Motion::fast)) == 120.0f, "Motion::fast is 120 ms");
    check(ui::motionMs(5) == 200.0f, "the motion scale clamps at Motion::slow");
}

// ---------------------------------------------------------------------------
// 9. Typography: the GUI type scale, and the three-steps-per-view rule
// ---------------------------------------------------------------------------

/// The families the pixel profile asks for. Q-06 is settled as "keep Roboto for
/// the MVP", so these two names are the assets the build already carries; the two
/// post-MVP candidates must not appear, because a font added without the licence
/// and size review is exactly what that review gates.
///
/// The *asset* travels with the family: a swap that renames the family but keeps
/// loading Roboto would pass a name check and change nothing on screen, so both
/// halves are pinned here.
void checkFontFamilies() {
    const ui::FontFace uiFace = ui::uiFontFace();
    const ui::FontFace monoFace = ui::monoFontFace();
    check(std::string(uiFace.family) == "Roboto-Medium", "the UI family is Roboto-Medium");
    check(std::string(uiFace.asset) == "fonts/Roboto/Roboto-Medium.ttf",
          "the UI family's asset is the embedded Roboto-Medium");
    check(std::string(monoFace.family) == "Inconsolata-Medium",
          "the mono family is Inconsolata-Medium");
    check(std::string(monoFace.asset) == "fonts/Inconsolata-Medium.ttf",
          "the mono family's asset is the embedded Inconsolata-Medium");
    const std::string names = std::string(uiFace.family) + monoFace.family;
    check(names.find("Inter") == std::string::npos, "no Inter without the Q-06 licence review");
    check(names.find("JetBrains") == std::string::npos,
          "no JetBrains Mono without the Q-06 licence review");
}

/// The type scale reaching the style. `applyTypeScale()` is the one place the
/// pixel profile's base size is set; a `TypeScope` moves the size for one draw and
/// puts it back, and on the text grid it must not move at all.
///
/// Runs on a bare ImGui context - no backend, no text grid, no window on screen -
/// because the pixel profile's size is `style.FontSizeBase` and `GetFontSize()` is
/// exactly that value times the two global scales (100 % here). A frame is still
/// needed for `GetFontSize()`: outside one there is no current font.
void checkTypeApplication() {
    ImGuiStyle& style = ImGui::GetStyle();
    ui::applyTypeScale();
    check(style.FontSizeBase == 15.0f, "applyTypeScale() sets the body step (15 px) as the base");
    ui::applyTypeScale();
    check(style.FontSizeBase == 15.0f, "applyTypeScale() is idempotent");

    // The operator's size: a configured base has to reach the drawn size (the
    // regression a dropped `FontSizeBase` write would cause) and must scale the
    // hierarchy with it instead of flattening every step to the base.
    check(ui::scaledTypeSize(ui::TypeStep::body, 0.0f) == 15.0f,
          "an unconfigured base is the body step");
    check(ui::scaledTypeSize(ui::TypeStep::meta, 30.0f) == 24.0f,
          "a 30 px base draws meta at 24 px (12 / 15 of the base)");
    check(ui::scaledTypeSize(ui::TypeStep::display, 7.0f) >= 1.0f,
          "a tiny base never yields a sub-pixel size");
    ui::applyTypeScale(30.0f);
    check(style.FontSizeBase == 30.0f, "applyTypeScale(30) sets a 30 px base");
    ui::applyTypeScale();
    check(style.FontSizeBase == 15.0f, "applyTypeScale() (no argument) returns to the body step");

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(320.0f, 200.0f);
    io.DeltaTime = 1.0f / 60.0f;
    // `ImGuiBackendFlags_RendererHasTextures` is what lets a 1.92 backend build its
    // atlas itself; without it (and without a real renderer's
    // GetTexDataAsRGBA32) `NewFrame()` has no atlas builder to talk to. This harness
    // uploads nothing, so the flag only has to survive the one frame.
    const ImGuiBackendFlags backendFlags = io.BackendFlags;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    // Two distinct families, so "the code step pushes the code face" is
    // measurable: the first one added becomes the UI face (`Fonts[0]`), the second
    // is a face nothing else would pick. The atlas is still unlocked here.
    ImFont* const uiDefault = io.Fonts->AddFontDefaultVector();
    ImFont* const codeFace = io.Fonts->AddFontDefaultBitmap();
    ImGui::NewFrame();
    ImGui::Begin("typography");
    // Read inside the frame: outside one there is no current font yet.
    ImFont* const uiFace = ImGui::GetFont();
    check(uiFace == uiDefault, "the frame draws with the first added face");
    check(codeFace != nullptr && codeFace != uiFace, "the harness has two distinct faces to push");
    check(ImGui::GetFontSize() == 15.0f,
          "the frame draws at body size (got " + std::to_string(ImGui::GetFontSize()) + ")");
    {
        const ui::TypeScope display(ui::TypeStep::display, /*textGrid=*/false);
        check(display.active(), "a type scope is active in the pixel profile");
        check(ImGui::GetFontSize() == 24.0f,
              "a display step draws at 24 px (got " + std::to_string(ImGui::GetFontSize()) + ")");
    }
    check(ImGui::GetFontSize() == 15.0f, "closing a type scope restores the body size");
    {
        // The code step is monospaced: it pushes the face the chain installed, and
        // every other step keeps the UI family.
        ui::setCodeFont(codeFace);
        {
            const ui::TypeScope code(ui::TypeStep::code, /*textGrid=*/false);
            check(ImGui::GetFont() == codeFace, "a code step draws with the code face");
            check(ImGui::GetFontSize() == 14.0f, "a code step draws at 14 px");
        }
        check(ImGui::GetFont() == uiFace, "closing a code scope restores the UI face");
        ui::setCodeFont(nullptr);
        {
            const ui::TypeScope label(ui::TypeStep::label, /*textGrid=*/false);
            check(ImGui::GetFont() == uiFace, "a label step keeps the UI face");
            check(ImGui::GetFontSize() == 13.0f, "a label step draws at 13 px");

            // A step is absolute, so scopes nest without scaling a step by a step. The top bar
            // measured itself inside its own label scope: with a relative scope that measure came
            // out at 13 * 13 / 15 = 11 px, so every plain-text box was 15 % narrower than the
            // text drawn into it and the last glyph of the wordmark was clipped.
            const float widthAt13 = ImGui::CalcTextSize("llmfun").x;
            {
                const ui::TypeScope innerLabel(ui::TypeStep::label, /*textGrid=*/false);
                check(ImGui::GetFontSize() == 13.0f, "a nested label scope keeps 13 px (got " +
                                                         std::to_string(ImGui::GetFontSize()) +
                                                         ")");
                check(!innerLabel.active(), "a nested scope of the same step pushes nothing");
                check(ImGui::CalcTextSize("llmfun").x == widthAt13,
                      "a nested scope measures the same text width");
            }
            {
                const ui::TypeScope innerMeta(ui::TypeStep::meta, /*textGrid=*/false);
                check(ImGui::GetFontSize() == 12.0f,
                      "meta inside a label scope is still 12 px (got " +
                          std::to_string(ImGui::GetFontSize()) + ")");
            }
        }
    }
    {
        const ui::TypeScope label(ui::TypeStep::label, /*textGrid=*/true);
        check(!label.active(), "a type scope is inert on the text grid");
        check(ImGui::GetFontSize() == 15.0f, "the text grid keeps its own size");
    }
    ImGui::End();
    ImGui::Render();
    io.BackendFlags = backendFlags;
    // `Render()` leaves the style's base size where the last `SetCurrentFont` put
    // it - the body step again; the next frame reads it back from there.
    check(ImGui::GetStyle().FontSizeBase == 15.0f,
          "the frame ends on the body step, not on a step a view left open");

    // A configured base, in its own frame: the operator's size is read by
    // `applyTypeScale()` before the frame and the hierarchy is applied as a ratio.
    ui::applyTypeScale(30.0f);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGui::NewFrame();
    ImGui::Begin("typography-scaled");
    check(ImGui::GetFontSize() == 30.0f, "a 30 px base draws body text at 30 px (got " +
                                             std::to_string(ImGui::GetFontSize()) + ")");
    {
        const ui::TypeScope meta(ui::TypeStep::meta, /*textGrid=*/false);
        check(ImGui::GetFontSize() == 24.0f, "with a 30 px base a meta step draws at 24 px (got " +
                                                 std::to_string(ImGui::GetFontSize()) + ")");
    }
    ImGui::End();
    ImGui::Render();
    io.BackendFlags = backendFlags;
    ui::applyTypeScale();
}

/// A view's source with both comment forms blanked out: `/* ... */` first (it can
/// span lines, so only its non-newline characters are replaced, which keeps every
/// later line number where a reader expects it), then a `//` tail per line. A
/// comment that mentions a step or a forbidden call must not count as code.
std::vector<std::string> stripComments(const std::string& text) {
    std::string code = text;
    for (std::size_t i = 0; (i = code.find("/*", i)) != std::string::npos;) {
        const std::size_t end = code.find("*/", i + 2);
        const std::size_t stop = end == std::string::npos ? code.size() : end + 2;
        for (std::size_t k = i; k < stop; ++k)
            if (code[k] != '\n')
                code[k] = ' ';
        i = stop;
    }

    std::vector<std::string> lines;
    std::string line;
    for (const char c : code) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
            continue;
        }
        line += c;
    }
    if (!line.empty())
        lines.push_back(line);
    for (std::string& entry : lines) {
        const std::size_t comment = entry.find("//");
        if (comment != std::string::npos)
            entry.resize(comment);
    }
    return lines;
}

/// One step name after `TypeStep::`, or an empty string. The scan reads the name
/// out of the source, so it has to accept `TypeStep::label`, `ui::TypeStep::meta`
/// and a line that names two steps.
std::string typeStepNamedIn(const std::string& code, std::size_t at) {
    const std::string prefix = "TypeStep::";
    const std::size_t start = code.find(prefix, at);
    if (start == std::string::npos)
        return std::string();
    std::size_t end = start + prefix.size();
    while (end < code.size() && std::isalpha(static_cast<unsigned char>(code[end])) != 0)
        ++end;
    return code.substr(start + prefix.size(), end - start - prefix.size());
}

/// Design §4.6's typography rule, made measurable: a view names at most three type
/// steps, every step it names exists, and it never sizes a font itself. A view that
/// reached for a fourth step - or for a `PushFont(..., 20.0f)`, a `typeSize()` call
/// or a multiplier - would be inventing the scale again, and the rule is what keeps
/// a region from turning into a ransom note.
///
/// The scan is source-level because the alternative is a runtime tally that only a
/// frame can fill: a view draws in several branches, so "which steps it *can* use"
/// is a property of the file, not of one frame. Both comment forms are stripped
/// first, so a comment that mentions a step does not count as a usage, and a
/// `/* ... */` that lists all six does not fail the budget.
///
/// The budget is per file, which today is per view (`shell.cpp` hosts the five
/// region stubs until their own files arrive); the message says "named in this
/// file" so a split that changes the granularity does not read as a regression.
void checkViewTypeSteps() {
    namespace fs = std::filesystem;
    const fs::path views{TEST_TOKENS_VIEWS_DIR};
    std::error_code error;
    check(fs::exists(views, error) && fs::is_directory(views, error),
          "views/: the scan directory exists (" + views.string() + ")");
    if (error)
        return;

    const char* const known[] = {"display", "title", "body", "label", "meta", "code"};
    int files = 0;
    for (fs::directory_iterator it(views, error), end; it != end; it.increment(error)) {
        if (error || !it->is_regular_file())
            continue;
        if (it->path().extension() != ".cpp")
            continue;
        ++files;
        const std::string name = it->path().filename().string();
        std::ifstream in(it->path());
        check(in.good(), "views/: " + name + " is readable");
        const std::string raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        const std::vector<std::string> code = stripComments(raw);

        std::vector<std::string> steps;
        std::vector<std::string> offenders;
        for (std::size_t i = 0; i < code.size(); ++i) {
            const std::string& line = code[i];
            for (std::size_t at = 0; (at = line.find("TypeStep::", at)) != std::string::npos;) {
                const std::string step = typeStepNamedIn(line, at);
                at += 1;
                if (!step.empty() && std::find(steps.begin(), steps.end(), step) == steps.end())
                    steps.push_back(step);
            }
            // A view may not size a font itself: `typeSize()` hands it a number, and
            // a multiplier or a literal is the scale being reinvented. Neither is
            // visible in the step count above, so each is an offence of its own.
            if (line.find("PushFont(") != std::string::npos)
                offenders.push_back("line " + std::to_string(i + 1) + " calls PushFont");
            if (line.find("typeSize(") != std::string::npos)
                offenders.push_back("line " + std::to_string(i + 1) + " calls typeSize");
        }

        std::string named;
        for (const std::string& step : steps)
            named += (named.empty() ? "" : ", ") + step;
        check(steps.size() <= 3, name + ": at most three type steps named in this file (" +
                                     std::to_string(steps.size()) + ": " + named + ")");
        std::string where;
        for (const std::string& offence : offenders)
            where += (where.empty() ? "" : ", ") + offence;
        check(offenders.empty(),
              name + ": no view sizes a font itself - a step goes through TypeScope" +
                  (where.empty() ? std::string() : " (found: " + where + ")"));
        for (const std::string& step : steps)
            check(std::find_if(std::begin(known), std::end(known),
                               [&step](const char* candidate) { return step == candidate; }) !=
                      std::end(known),
                  name + ": '" + step + "' is a TypeStep");
    }
    check(files > 0, "views/: the scan saw at least one view");
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0)
            g_verbose = true;
        else {
            std::fprintf(stderr, "usage: %s [-v]\n", argv[0]);
            return 2;
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    checkTable();
    for (const ui::ColorDepth depth : {ui::ColorDepth::Ansi256, ui::ColorDepth::Ansi16})
        checkHue(depth);

    // The matrix is measured on what each depth shows: the design's exact colours
    // and the xterm-256 entries a terminal paints.
    for (const ui::ColorDepth depth : {ui::ColorDepth::TrueColor, ui::ColorDepth::Ansi256}) {
        ui::applyTheme(ui::ThemeId::Graphite, depth);
        checkMatrix(depth);
    }

    for (const ui::ColorDepth depth :
         {ui::ColorDepth::TrueColor, ui::ColorDepth::Ansi256, ui::ColorDepth::Ansi16})
        checkMapping(depth);

    ui::applyTheme(ui::ThemeId::Graphite, ui::ColorDepth::TrueColor);
    checkDimIsNotText(ui::ColorDepth::TrueColor);

    checkDetection();
    checkScales();
    checkFontFamilies();
    checkTypeApplication();
    checkViewTypeSteps();
    checkOncePerThemeChange();

    ImGui::DestroyContext();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d token check(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_tokens: all checks passed (%d colours, %.1f:1 text bar)\n",
                 ui::kColorTokenCount, MinTextContrast);
    return 0;
}
