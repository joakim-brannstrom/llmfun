// test_theme_contrast.cpp
//
// Headless contrast / neutral-surface test for the thread theme (task 5).
//
// The thread is one neutral surface: a role is a rule plus a label in the role
// colour and never a filled row, and every fill inside the thread is a neutral
// step (D-BLOCK). This binary pins the properties that contract rests on,
// against the real theme application in both rendering profiles:
//
//   1. contrast      every text/fill pair the thread, the composer and the
//                    disclosure headers actually draw reaches 4.5:1 (WCAG 2.1
//                    relative luminance); the composer placeholder is a dim
//                    affordance and is measured against the 3:1 bar instead.
//                    The role rule is the label colour, so a role pair covers
//                    both the text and that non-text boundary;
//   2. no role fill  no ImGui colour slot carries a role colour - neither a
//                    bit-exact copy nor a same-hue derivative (other alpha or
//                    brightness), which is how the deleted `*Bg` fills were
//                    built. Both role sources are the oracle: the six label
//                    colours the thread still draws and the palette's four token
//                    roles. `role.system` shares its hue corridor with the ink
//                    family by construction; that one carve-out is recorded and
//                    measured at `kThreadFills`;
//   3. palette       every fill the thread paints is a token value (or a step
//                    of one) - the palette is the only vocabulary - and the two
//                    surfaces are dark, opaque and ordered (canvas darker than
//                    surface);
//   4. distinguishable  the role colours stay pairwise different - with the
//                    fills gone, colour is the only thing telling roles apart.
//
// The two profiles are measured differently, because they render differently:
// the pixel profile alpha-composites a translucent fill over the surface, while
// the text backend cannot blend - it scales the fill's channels by alpha and
// then quantizes to an ANSI-256 index (imtui-impl-text.cpp:131-165), and glyphs
// are quantized without alpha. The grid numbers below are therefore the colours
// a terminal actually shows, not the floats that fed it (the palette entry is
// reconstructed from the index).
//
// The session rail's row labels are measured here as well (task 23). The v4 panel's
// active-row pair - #666673 as the label on an `ImGuiCol_Button` (#545D6B) fill,
// 1.18:1 in the pixel profile and 1.98:1 on the text grid - was the one pre-existing
// pair below the 4.5:1 bar, and it is gone with the panel: the rail draws its labels
// from the token table (`railRowInks()`), so this binary can measure every row state
// from the same function the view draws with. The row
// inks live in `views/rail.cpp`; a state that dims its label, or that reaches for
// `textDim`, fails here.
//
// Exit codes: 0 = all checks pass; 1 = at least one failed.
// Run from the build dir:  ./test_theme_contrast [-v]
// (-v prints the whole measured table, also when every pair passes.)

#include "tui.h"
#include "views/rail.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace rail = llmfun::tui::views;

namespace {

int g_failures = 0;
bool g_verbose = false;

/// WCAG 2.1 AA for body text.
constexpr float MinContrast = 4.5f;
/// WCAG 1.4.11 for incidental/dim affordances (the composer placeholder).
constexpr float MinIncidentalContrast = 3.0f;
/// A surface that reads as neutral: no channel may lead by more than this. The
/// two surfaces measure 0.020 (ink.canvas #0B0D10) and 0.039 (ink.surface
/// #12161C), so 0.08 leaves room for a near-identical design tweak and still
/// refuses a visible tint. Fills are not measured against it - they must be token
/// values bit-exact (see `checkThreadFills`) - so this bound can never be widened
/// to admit a fill.
constexpr float MaxChannelSpread = 0.08f;
/// A dark surface: relative luminance below 0.05 (mid grey already measures 0.18).
constexpr float MaxSurfaceLuminance = 0.05f;

float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

/// Source-over of a (possibly translucent) colour onto an opaque one. This is
/// what the pixel profile does; the text backend cannot blend.
ImVec4 compositeOver(ImVec4 fg, ImVec4 bg) {
    const float a = fg.w;
    return ImVec4(fg.x * a + bg.x * (1.0f - a), fg.y * a + bg.y * (1.0f - a),
                  fg.z * a + bg.z * (1.0f - a), 1.0f);
}

float relativeLuminance(ImVec4 c) {
    return 0.2126f * srgbToLinear(c.x) + 0.7152f * srgbToLinear(c.y) + 0.0722f * srgbToLinear(c.z);
}

/// Both arguments must already be the opaque colour a reader sees.
float contrastRatio(ImVec4 a, ImVec4 b) {
    const float la = relativeLuminance(a);
    const float lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

int channel255(float v) { return int(std::lround(std::max(0.0f, std::min(1.0f, v)) * 255.0f)); }

/// The text backend's colour mapping, reproduced from
/// `vendor/imtui/src/imtui-impl-text.cpp:131-165`: greys go through the 232-255
/// ramp, everything else through the 6x6x6 cube, and a *fill* has its channels
/// scaled by its alpha first (`doAlpha`), while a glyph keeps them.
int ansi256Index(ImVec4 colour, bool scaleByAlpha) {
    int r = channel255(colour.x);
    int g = channel255(colour.y);
    int b = channel255(colour.z);
    const float alpha = scaleByAlpha ? std::max(0.0f, std::min(1.0f, colour.w)) : 1.0f;
    if (r == g && g == b) {
        const float grey = float(r) * alpha;
        if (grey < 8.0f)
            return 16;
        if (grey > 248.0f)
            return 231;
        return int(std::lround((grey - 8.0f) / 247.0f * 24.0f)) + 232;
    }
    r = int(std::lround(float(r) * alpha));
    g = int(std::lround(float(g) * alpha));
    b = int(std::lround(float(b) * alpha));
    return 16 + 36 * int(std::lround(float(r) / 255.0f * 5.0f)) +
           6 * int(std::lround(float(g) / 255.0f * 5.0f)) +
           int(std::lround(float(b) / 255.0f * 5.0f));
}

/// The RGB of an xterm-256 palette entry (16 = the cube's black, 232-255 = the
/// grey ramp). Indices 0-15 are the terminal's own palette; treat them as black.
ImVec4 ansi256Color(int index) {
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

ImVec4 styleColor(ImGuiCol slot) { return ImGui::GetStyle().Colors[slot]; }

struct Pair {
    std::string what;
    ImVec4 text;
    ImVec4 fill;
    float min = MinContrast;
};

/// What a reader sees for one (text, fill) pair in the given profile.
void renderPair(const Pair& pair, ImVec4 under, bool grid, ImVec4& text, ImVec4& fill) {
    if (grid) {
        text = ansi256Color(ansi256Index(pair.text, /*scaleByAlpha=*/false));
        fill = ansi256Color(ansi256Index(pair.fill, /*scaleByAlpha=*/true));
        return;
    }
    fill = compositeOver(pair.fill, under);
    text = compositeOver(pair.text, fill);
}

void fail(const std::string& message) {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    ++g_failures;
}

void checkContrast(const std::vector<Pair>& pairs, ImVec4 under, bool grid) {
    for (const Pair& pair : pairs) {
        ImVec4 text;
        ImVec4 fill;
        renderPair(pair, under, grid, text, fill);
        const float ratio = contrastRatio(text, fill);
        if (g_verbose)
            std::fprintf(stderr, "  %-40s %6.2f:1 (need %.1f) %s\n", pair.what.c_str(), ratio,
                         pair.min, ratio >= pair.min ? "ok" : "FAIL");
        if (ratio < pair.min) {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "%s contrasts %.2f:1 on its fill (need %.1f:1)",
                          pair.what.c_str(), ratio, pair.min);
            fail(buffer);
        }
    }
}

/// One role foreground, plus whether the same-hue scan applies to it. Both role
/// sources are live: the thread's labels still come from `ChatMessageStyle`, and
/// the palette's four roles are what the rest of the UI draws - a fill has to be
/// clear of either. The token rows carry the *design* RGB, because at the coarse
/// depths both a fill and `role.system` are flattened to pure greys, where a hue
/// comparison has nothing left to measure.
struct Role {
    const char* name;
    ImVec4 colour;
    /// False for a role that is itself a near-grey: its hue is the ink family's
    /// corridor, so "same hue as this" is true of every neutral fill and is not
    /// evidence of a role tint (the carve-out measured at `kThreadFills`).
    bool hueScan = true;
};

/// Every role foreground the header can draw, in one table: the labels, the
/// times and the collapsed previews all use the same role colour, and so does
/// the rule (a non-text boundary, measured at the same ratio).
std::vector<Role> roleColors(const llmfun::tui::ChatMessageStyle& s) {
    const llmfun::tui::ui::TokenTable& table = llmfun::tui::ui::tokens();
    const auto fromTable = [&table](llmfun::tui::ui::ColorToken token) {
        return table.rgb[static_cast<int>(token)];
    };
    return {
        {"role YOU", s.userFg},
        {"role AGENT", s.assistantFg},
        {"role TOOL call", s.toolCallFg},
        {"role TOOL response", s.toolResponseFg},
        {"role SYSTEM", s.systemFg},
        {"role ANSWER", s.finalAnswerFg},
        {"token role.you", fromTable(llmfun::tui::ui::ColorToken::roleYou)},
        {"token role.agent", fromTable(llmfun::tui::ui::ColorToken::roleAgent)},
        {"token role.tool", fromTable(llmfun::tui::ui::ColorToken::roleTool)},
        {"token role.system", fromTable(llmfun::tui::ui::ColorToken::roleSystem),
         /*hueScan=*/false},
    };
}

/// Same hue, i.e. the two colours differ only in brightness or alpha. The
/// channel ratios survive a small additive change (a brighten or a darken), so
/// this catches a *derived* role fill as well as a copied one. It is only
/// meaningful for a role with a hue of its own - see `Role::hueScan`.
bool sameHue(ImVec4 a, ImVec4 b) {
    constexpr float tolerance = 0.015f;
    const float sumA = a.x + a.y + a.z;
    const float sumB = b.x + b.y + b.z;
    if (sumA <= 0.0f || sumB <= 0.0f)
        return false;
    return std::fabs(a.x / sumA - b.x / sumB) <= tolerance &&
           std::fabs(a.y / sumA - b.y / sumB) <= tolerance &&
           std::fabs(a.z / sumA - b.z / sumB) <= tolerance;
}

/// The fills the thread paints. Each must be a token value bit-exact (see
/// `checkThreadFills`), and none may be a same-hue derivative of a role colour.
/// (Slots the thread never uses - ImGuiCol_FrameBgActive, the Tab/Plot greys -
/// are out of scope; the widgets here take FrameBg even while a field is focused.
/// The dividing lines - `Border`, `TableBorder*` - are not fills; they are covered
/// by the exact-match scan below.)
///
/// Recorded carve-out: `textDim` is accepted as a *control* fill although,
/// measured against the token roles, it and its hover step sit inside
/// `role.system`'s hue corridor (0.0095 and 0.0051 against the 0.015 tolerance).
/// No bound separates them: `role.system` #8A94A6 is itself near-grey (channel
/// spread 0.11 against the ink family's 0.03-0.09), and *every* +5 % step of
/// *every* ink lands inside that corridor - the base inks are 2-3x clear, each
/// step is not - so a same-hue test on the corridor would reject the ink fill
/// vocabulary as a whole rather than one bad fill. The fills are therefore pinned
/// exactly (a fill can only be a palette value), which is the part that can be
/// enforced; the design itself sanctions one tinted fill (`selection` is the
/// accent at 14 %, and the accent *is* `role.agent`). Consequence, and it is a
/// rule: no `role.system`-coloured label may ever be drawn on a Button - it
/// measures 2.18:1 on `textDim`, far below the text bar (task 23 must not do it
/// when it re-tones the rail).
const ImGuiCol kThreadFills[] = {
    ImGuiCol_WindowBg,      ImGuiCol_ChildBg,      ImGuiCol_Header,         ImGuiCol_HeaderHovered,
    ImGuiCol_HeaderActive,  ImGuiCol_FrameBg,      ImGuiCol_FrameBgHovered, ImGuiCol_Button,
    ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive,
};

/// role = foreground + rule, never a fill: a role colour may not appear as a
/// style slot (bit-exact, any slot), and no thread fill may be a same-hue
/// derivative of one (the deleted `*Bg` fills were the role colour darkened and
/// given an alpha, so equality alone would have missed them).
void checkNoRoleFill(const std::vector<Role>& roles) {
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        const ImVec4 slot = ImGui::GetStyle().Colors[i];
        if (slot.w <= 0.0f)
            continue; // fully transparent paints nothing
        for (const Role& role : roles) {
            if (slot.x == role.colour.x && slot.y == role.colour.y && slot.z == role.colour.z &&
                slot.w == role.colour.w) {
                fail(std::string("style slot ") + ImGui::GetStyleColorName(i) + " carries the " +
                     role.name + " colour: a role may never be a fill");
            }
        }
    }
    for (const ImGuiCol slot : kThreadFills) {
        const ImVec4 fill = ImGui::GetStyle().Colors[slot];
        for (const Role& role : roles) {
            if (role.hueScan && sameHue(fill, role.colour)) {
                fail(std::string("thread fill ") + ImGui::GetStyleColorName(slot) + " is a " +
                     role.name + " derivative: a role may never be a fill");
            }
        }
    }
}

void checkNeutral(const ImVec4& colour, const std::string& what) {
    const float lo = std::min({colour.x, colour.y, colour.z});
    const float hi = std::max({colour.x, colour.y, colour.z});
    if (hi - lo > MaxChannelSpread) {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%s is tinted: channel spread %.3f > %.3f",
                      what.c_str(), hi - lo, MaxChannelSpread);
        fail(buffer);
    }
}

/// Every thread fill must be a token value, bit-exact: no heuristic and no magic
/// bound. The palette plus `stateStep` is the whole vocabulary a fill may come
/// from, so anything else is a hand-written colour or a stock ImGui default that
/// nobody themed. The step range is read from the binding table rather than
/// written as a literal, so this cannot be widened to admit a new fill the way a
/// channel-spread bound can.
void checkThreadFills() {
    const llmfun::tui::ui::ColorDepth depth = llmfun::tui::ui::currentColorDepth();
    int bindingCount = 0;
    const llmfun::tui::ui::ColorBinding* bindings = llmfun::tui::ui::colorBindings(bindingCount);
    int maxSteps = 0;
    for (int i = 0; i < bindingCount; ++i)
        maxSteps = std::max(maxSteps, bindings[i].steps);

    for (const ImGuiCol slot : kThreadFills) {
        const ImVec4 fill = styleColor(slot);
        bool isToken = false;
        for (int i = 0; i < llmfun::tui::ui::kColorTokenCount && !isToken; ++i) {
            const auto token = static_cast<llmfun::tui::ui::ColorToken>(i);
            const ImVec4 base = llmfun::tui::ui::token(token, depth).rgb;
            for (int steps = 0; steps <= maxSteps && !isToken; ++steps) {
                const ImVec4 value = steps == 0 ? base : llmfun::tui::ui::stateStep(base, steps);
                isToken = fill.x == value.x && fill.y == value.y && fill.z == value.z &&
                          fill.w == value.w;
            }
        }
        if (!isToken) {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer),
                          "thread fill %s is not a token value (one of %d tokens, at most %d "
                          "state steps): a fill must come from the palette",
                          ImGui::GetStyleColorName(slot), llmfun::tui::ui::kColorTokenCount,
                          maxSteps);
            fail(buffer);
        }
    }
}

void runProfile(const char* profile, bool grid, const llmfun::tui::ChatMessageStyle& style) {
    if (g_verbose)
        std::fprintf(stderr, "profile %s\n", profile);

    const ImVec4 canvas = styleColor(ImGuiCol_WindowBg);
    const ImVec4 surface = styleColor(ImGuiCol_ChildBg);
    const ImVec4 text = styleColor(ImGuiCol_Text);
    const ImVec4 header = styleColor(ImGuiCol_Header);
    const ImVec4 headerHovered = styleColor(ImGuiCol_HeaderHovered);
    const ImVec4 headerActive = styleColor(ImGuiCol_HeaderActive);

    const std::vector<Role> roles = roleColors(style);

    // 1. Contrast -------------------------------------------------------------
    std::vector<Pair> pairs;
    for (const Role& role : roles) {
        pairs.push_back({std::string(role.name) + " on canvas", role.colour, canvas});
        pairs.push_back({std::string(role.name) + " on surface", role.colour, surface});
    }
    pairs.push_back({"body text on canvas", text, canvas});
    pairs.push_back({"body text on surface", text, surface});
    // The composer: the field fill, the action button and its states, and the
    // placeholder (a dim affordance, so the 3:1 incidental bar applies).
    pairs.push_back({"composer field text", text, styleColor(ImGuiCol_FrameBg)});
    pairs.push_back({"composer placeholder", styleColor(ImGuiCol_TextDisabled),
                     styleColor(ImGuiCol_FrameBg), MinIncidentalContrast});
    pairs.push_back({"action label", text, styleColor(ImGuiCol_Button)});
    pairs.push_back({"action label (hovered)", text, styleColor(ImGuiCol_ButtonHovered)});
    pairs.push_back({"action label (active)", text, styleColor(ImGuiCol_ButtonActive)});
    // The framed disclosure headers (streamed message = role-coloured label,
    // thinking node and log entries = body text) in all three paint states. A
    // per-widget `Header` push used to leave Hovered/Active at the stock blue.
    pairs.push_back({"stream header (rest)", style.assistantFg, header});
    pairs.push_back({"stream header (hovered)", style.assistantFg, headerHovered});
    pairs.push_back({"stream header (held)", style.assistantFg, headerActive});
    pairs.push_back({"disclosure label (rest)", text, header});
    pairs.push_back({"disclosure label (hovered)", text, headerHovered});
    pairs.push_back({"disclosure label (held)", text, headerActive});
    // The rail's rows (task 23): every state's title and preview against its own row
    // fill, measured from `views::railRowInks()` - the one table the view draws with.
    // The fill is composited over the rail's surface (the active row's `selection` is
    // translucent), and on the grid both sides are quantized exactly as the terminal
    // does it.
    const llmfun::tui::ui::ColorDepth depth = llmfun::tui::ui::currentColorDepth();
    for (int i = 0; i < static_cast<int>(rail::RailRowState::Count); ++i) {
        const auto state = static_cast<rail::RailRowState>(i);
        const rail::RailRowInks inks = rail::railRowInks(state);
        const std::string name = std::string("rail row (") + rail::railRowStateName(state) + ") ";
        const ImVec4 rowFill = llmfun::tui::ui::token(inks.fill, depth).rgb;
        pairs.push_back({name + "title", llmfun::tui::ui::token(inks.title, depth).rgb, rowFill});
        pairs.push_back(
            {name + "preview", llmfun::tui::ui::token(inks.preview, depth).rgb, rowFill});
        // NFR-09: the decorative ink never carries a glyph, so a rail label may not be
        // `textDim` in any state - the recipe task 23 was handed, checked as a rule.
        if (inks.title == llmfun::tui::ui::ColorToken::textDim ||
            inks.preview == llmfun::tui::ui::ColorToken::textDim)
            fail(name + "inks a label with textDim: the decorative ink carries no glyph");
    }
    checkContrast(pairs, surface, grid);

    // 2. No role colour may be a fill anywhere in the theme.
    checkNoRoleFill(roles);

    // 3. Every fill is a palette value, and the two surfaces are dark + opaque +
    //    ordered (a translucent child would let an unknown background through -
    //    the old alpha-0 hide).
    checkThreadFills();
    checkNeutral(canvas, "window background (ink.canvas)");
    checkNeutral(surface, "thread surface (ink.surface)");
    for (const auto& pair :
         {std::make_pair("canvas", canvas), std::make_pair("surface", surface)}) {
        if (relativeLuminance(pair.second) > MaxSurfaceLuminance) {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "%s is not a dark surface (luminance %.3f)",
                          pair.first, relativeLuminance(pair.second));
            fail(buffer);
        }
    }
    if (surface.w < 1.0f)
        fail("the thread surface is translucent: an unknown background would show through");
    if (relativeLuminance(canvas) >= relativeLuminance(surface))
        fail("the canvas is not darker than the thread surface");

    // 4. The roles must stay distinguishable from each other: role is the only
    //    colour signal left on a row, so two equal roles would erase one.
    for (size_t i = 0; i < roles.size(); ++i) {
        for (size_t j = i + 1; j < roles.size(); ++j) {
            const ImVec4& a = roles[i].colour;
            const ImVec4& b = roles[j].colour;
            if (a.x == b.x && a.y == b.y && a.z == b.z)
                fail(std::string(roles[i].name) + " and " + roles[j].name +
                     " share one colour: the roles would be indistinguishable");
        }
    }
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

    const llmfun::tui::ChatMessageStyle style;
    for (int profile = 0; profile < 2; ++profile) {
        // The flag is what tuiIsTextGrid() reads and what the text backend sets
        // in ImTui_ImplText_Init(); the theme is applied after the backend init
        // in the application (tui_api.cpp), so the test flips the same switch.
        // The depth is pinned per profile rather than detected: this binary
        // measures the palette, so "a 256-colour terminal" must not depend on
        // the TERM of whoever runs the tests (which is what applyUiTheme()
        // would read for the grid).
        ImTui_TextEncodingActive = (profile == 1);
        llmfun::tui::ui::applyTheme(llmfun::tui::ui::ThemeId::Graphite,
                                    profile == 1 ? llmfun::tui::ui::ColorDepth::Ansi256
                                                 : llmfun::tui::ui::ColorDepth::TrueColor);
        runProfile(profile == 1 ? "text grid" : "pixel", profile == 1, style);
    }
    ImTui_TextEncodingActive = false;

    ImGui::DestroyContext();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d contrast/neutral-surface check(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_theme_contrast: all checks passed (both profiles, %.1f:1)\n",
                 MinContrast);
    return 0;
}
