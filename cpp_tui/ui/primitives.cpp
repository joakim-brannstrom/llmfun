/// The primitive library (design §4.15, plan §8.6): fourteen widgets, each with a
/// `measure()` and a renderer that draws from the same numbers.
///
/// `primitives.h` states the contract; this file holds the numbers and the two
/// renderings. Three rules shape every function below:
///
///   * **one size arithmetic.** A `measure*()` computes the size; the renderer reads the
///     rect it is handed and never recomputes a width. Where both need the same
///     intermediate value (the meter's segment pitch, the button's label pen) it comes
///     from one shared helper, so a measure and its render cannot drift apart without a
///     test failing.
///   * **no colour, ever.** Every ink and fill is a `ColorToken` resolved through
///     `ui::color()`; a hover is `ui::stateStep()` of a token. `./test_primitives` reads
///     this file and fails on a colour literal - the `ImVec4` constructor or a packed
///     `IM_COL32` value - so the token rule is enforced rather than trusted. A resolved
///     `ImU32` local is not a literal: it holds what `ui::color()` answered.
///   * **inside the rect.** A fill is `AddRectFilled` on whole-unit coordinates, a border
///     is drawn *inside* the rect (a stroked rectangle lands on the pixel boundary above
///     its rect in the text backend), and every label and glyph is clipped to the
///     primitive's rect and, on the grid, additionally cut with `ui::truncate()` so it
///     cannot claim a cell it was not measured for. A caller may hand a primitive a rect
///     narrower than its measure; it then paints the part that fits and nothing else.
///
/// The cell grid gets its geometry from the pen mapping measured against the real text
/// backend: `AddText(pos, …)` lands in cell `(int(pos.x), int(pos.y))`, a filled rect
/// `[x1,x2) x [y1,y2)` paints exactly those cells and clears their character, and
/// `PushClipRect` cuts on cell boundaries. So "draw at a cell" is literal here - no
/// rounding fudge is needed anywhere.
#include "ui/primitives.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// Two ImGui internals, both for layout *neutrality* rather than for rendering:
//
//   * `ImHashData` - a button's identity hashes the exact float position, because the
//     GUI is a float layout and two rects that differ by less than a pixel are two
//     buttons (a truncated `long` key would silently merge them);
//   * `DC.CursorMaxPos` - a primitive placed by an absolute rect must claim no flow
//     space. An item's own contribution to a container's content extent is its position
//     *plus the scroll offset*, so a scrolled region containing absolute items would grow
//     by its own scroll every frame and could never settle.
//
// Both are still imgui; no app knowledge reaches `ui/`.
#include "imgui/imgui_internal.h"

#include "ui/text.h"

namespace llmfun::tui::ui {
namespace {

// ---------------------------------------------------------------------------
// Profile answers and the shared unit arithmetic
// ---------------------------------------------------------------------------

/// A guard on loop counts that come from a caller: a meter with a million segments is a
/// bug, not a layout. The design's bar is ten.
constexpr int kMaxMeterSegments = 1024;

/// Which context and frame a cached answer belongs to.
///
/// The frame counter alone is not enough: two ImGui contexts in one process each start
/// counting at 1, so a cache keyed on the frame number could answer with a record from a
/// context that no longer exists.
struct FrameKey {
    ImGuiContext* context = nullptr;
    int frame = -1;

    bool matches(const FrameKey& other) const {
        return context == other.context && frame == other.frame;
    }
};

FrameKey currentFrame() {
    if (ImGui::GetCurrentContext() == nullptr)
        return FrameKey{};
    return FrameKey{ImGui::GetCurrentContext(), ImGui::GetFrameCount()};
}

/// The glyph set a surface draws with, resolved once per context-frame.
///
/// `primitives.h` explains why this is derived from the metrics instead of being a
/// parameter: the contract's signatures carry no `GlyphSet`. The memo is what keeps that
/// honest - a glyph-per-cell loop on the text grid would otherwise read the environment
/// five times per drawn mark - and it refreshes with the frame counter, so a changed
/// `LLMFUN_ASCII` still takes effect on the next frame (the answer follows the
/// environment rather than needing an invalidation story).
GlyphSet glyphSetFor(const Metrics& m) {
    static FrameKey cachedKey;
    static GlyphSet cachedText = GlyphSet::Glyphs;
    static GlyphSet cachedPixel = GlyphSet::Ascii;
    const FrameKey key = currentFrame();
    if (key.context == nullptr || !cachedKey.matches(key)) {
        cachedKey = key;
        cachedText = textGlyphSet();
        cachedPixel = pixelGlyphSet();
    }
    return m.textGrid ? cachedText : cachedPixel;
}

/// True when the rect speaks the unit the metrics measured in. Mixed units are the bug
/// `Rect.cells` exists to catch: 28 cells read as 28 px is a legal-looking number that
/// lays out wrong, and it would draw a *plausible* primitive over the wrong cells. So a
/// mixed pair asserts in a debug build and paints nothing in a release build - fail
/// closed, never guess. The comparison is returned (not only asserted) because `IM_ASSERT`
/// is compiled out of the shipped build.
bool unitMatches(const Metrics& m, const Rect& r) {
    IM_ASSERT(r.cells == m.textGrid &&
              "Rect.cells must state the unit the Metrics were measured in");
    return r.cells == m.textGrid;
}

/// How wide `text` is in the profile's own unit: cells on the grid (the renderer's own
/// table, so a wide glyph counts two) and pixels in the GUI (the atlas, so a kerning pair
/// counts once). This is the *only* way a measure learns a string's width, which is what
/// makes "the measure and the render agree" an identity rather than a coincidence.
float textUnits(const Metrics& m, std::string_view text) {
    if (text.empty())
        return 0.0f;
    if (m.textGrid)
        return static_cast<float>(displayWidth(text));
    // The context alone is not enough: `CalcTextSize` reads the *current font*, which a
    // fresh context does not have until the first `NewFrame`. A measure is a pure query
    // the layout may run before the first frame, so the answer is "the text contributes
    // nothing" rather than a crash in `ImFont::GetFontBaked` (and rather than an assert,
    // which would turn a legal early layout pass into a debug-only abort).
    if (ImGui::GetCurrentContext() == nullptr || ImGui::GetFont() == nullptr)
        return 0.0f;
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

/// The draw list a primitive paints on: the current window's.
///
/// ImGui always has a window in a frame - it opens its own implicit fallback window
/// (`Debug##Default`) at the end of `NewFrame`, exactly so that a stray `Text()` cannot
/// crash - so a primitive never has to look for a list. A caller that wants its own
/// geometry opens its own window (the shell does; so does the golden harness), and a
/// caller that forgets gets ImGui's fallback window, which is the loud-but-alive answer.
/// `panel()` and `card()` are the two that also take an explicit list, for a surface that
/// draws somewhere else on purpose. Outside a frame there is no window and this asserts,
/// which is the right answer for a primitive asked to draw outside a frame.
ImDrawList* targetList() { return ImGui::GetWindowDrawList(); }

ImU32 inkOf(ColorToken token) { return ImGui::GetColorU32(color(token)); }

void fillRect(ImDrawList* dl, const Rect& r, ColorToken token, float radius) {
    if (dl == nullptr || r.w <= 0.0f || r.h <= 0.0f)
        return;
    dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), inkOf(token), radius);
}

/// The same fill with an already-resolved colour (a hover step is derived from a token
/// rather than named by one). It carries the radius as well: a hovered button that lost
/// its rounding would visibly pop its corners as the pointer enters.
void fillRect(ImDrawList* dl, const Rect& r, ImU32 colour, float radius) {
    if (dl == nullptr || r.w <= 0.0f || r.h <= 0.0f)
        return;
    dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), colour, radius);
}

/// A border drawn *inside* the rect.
///
/// In the GUI that is one stroked rectangle inset by half its thickness, so the border
/// follows the fill's radius instead of framing a rounded fill in a square (which is what
/// a set of edge bars would do, and it would make every rounded primitive read as a
/// square).
///
/// On the text grid it is four filled bars, because a stroke there is the one way a
/// border escapes the measured size: the text backend paints a 1-unit `AddRect` on the
/// row *above* its rect. And a grid rect with no interior unit gets no border at all -
/// two bars would meet and repaint the whole shape, so a one-cell control's border would
/// *become* its fill and its fill would vanish. The shape is then the fill, which is what
/// a cell renderer can draw, and the reason the design's focus ring is a GUI affordance on
/// a one-row control (the same accepted hole as "no focus-ring animation in the TUI").
void hairlineInside(ImDrawList* dl, const Metrics& m, const Rect& r, ColorToken token,
                    float thickness) {
    if (dl == nullptr || thickness <= 0.0f || r.w <= 0.0f || r.h <= 0.0f)
        return;
    const ImU32 col = inkOf(token);
    if (!m.textGrid) {
        const float half = thickness * 0.5f;
        dl->AddRect(ImVec2(r.x + half, r.y + half), ImVec2(r.right() - half, r.bottom() - half),
                    col, std::max(0.0f, m.radius - half), 0, thickness);
        return;
    }
    const float t = std::min(thickness, std::min(r.w, r.h) * 0.5f);
    if (r.w <= 2.0f * t || r.h <= 2.0f * t)
        return;
    const ImVec2 topLeft(r.x, r.y);
    const ImVec2 botRight(r.right(), r.bottom());
    dl->AddRectFilled(topLeft, ImVec2(botRight.x, r.y + t), col);
    dl->AddRectFilled(ImVec2(topLeft.x, botRight.y - t), botRight, col);
    dl->AddRectFilled(ImVec2(topLeft.x, r.y + t), ImVec2(r.x + t, botRight.y - t), col);
    dl->AddRectFilled(ImVec2(botRight.x - t, r.y + t), ImVec2(botRight.x, botRight.y - t), col);
}

/// The vertical pen for a row of text or a one-row mark centred in a taller rect.
///
/// Vertical centring uses the *font's own* line height, not the token row: on the grid the
/// two are the same number (one cell), so the pen lands exactly on the rect's row, and in
/// the GUI a 13 px line box is centred in a 24 px row instead of hugging its top. A font's
/// ink does not scale with a layout token; the row does.
float rowPenY(const Rect& r) {
    const float lineH = ImGui::GetCurrentContext() != nullptr ? ImGui::GetTextLineHeight() : 1.0f;
    return r.y + std::floor((r.h - lineH) * 0.5f);
}

/// One line of text inside `r`: `penX` is the left edge of the room, `room` its width in
/// the profile's unit, and the line is vertically centred in the rect.
///
/// On the grid the line is cut to `room` cells with the surface's own ellipsis
/// (`ui::truncate` measures the marker it is given, so a three-cell `...` fallback still
/// fits) - a cell cannot be half drawn, so the cut is part of the measurement contract. In
/// the GUI the text is drawn whole and the clip rect cuts it, which is why the room
/// argument is a *clip* there and a *budget* here, and why the GUI path allocates nothing.
void drawLine(ImDrawList* dl, const Metrics& m, const Rect& r, std::string_view text, ColorToken fg,
              float penX, float room) {
    if (dl == nullptr || text.empty() || room <= 0.0f)
        return;
    const ImVec2 pen(penX, rowPenY(r));
    const ImU32 col = inkOf(fg);
    dl->PushClipRect(ImVec2(penX, r.y), ImVec2(penX + room, r.bottom()), true);
    if (m.textGrid) {
        const std::string fitted =
            truncate(text, static_cast<int>(room), icon(Icon::Ellipsis, glyphSetFor(m)));
        IM_ASSERT(displayWidth(fitted) <= static_cast<int>(room) &&
                  "a grid label must fit the cells it was measured for");
        dl->AddText(pen, col, fitted.c_str(), fitted.c_str() + fitted.size());
    } else {
        dl->AddText(pen, col, text.data(), text.data() + text.size());
    }
    dl->PopClipRect();
}

/// A glyph at a whole-unit pen, clipped to its primitive's rect: a mark that does not fit
/// is cut off at the edge instead of running into the neighbour (a caller may hand a
/// primitive a rect narrower than its measure, e.g. a segment count that does not fit the
/// budget a breakpoint left).
void drawGlyph(ImDrawList* dl, const Rect& r, float penX, float penY, const char* glyph,
               ColorToken fg) {
    if (dl == nullptr || glyph == nullptr || glyph[0] == '\0' || r.w <= 0.0f || r.h <= 0.0f)
        return;
    dl->PushClipRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), true);
    dl->AddText(ImVec2(penX, penY), inkOf(fg), glyph);
    dl->PopClipRect();
}

/// Where a label of width `textW` starts when it is centred in `r`, kept at least a
/// hairline away from either edge. A half unit rounds down (a grid cannot spend a fraction
/// of a cell), so the label sits one cell left of exact centre when the slack is odd - the
/// same bias `roundCells` gives every other length. A label with no room to spare starts at
/// the left edge and is cut by the clip, which is where a too-long label belongs.
float centredPenX(const Metrics& m, const Rect& r, float textW) {
    const float slack = r.w - textW;
    if (slack <= 0.0f)
        return r.x;
    return r.x + std::max(m.border, roundCells(slack * 0.5f));
}

/// The room a label has once its pen is fixed and the far edge is kept `inset` clear of
/// the primitive's edge: `inset` is a hairline inside a button, `pad1` inside a chip or a
/// pill, and nothing when the label ends where the rect does.
float roomFrom(float penX, const Rect& r, float inset) { return r.right() - inset - penX; }

// ---------------------------------------------------------------------------
// The button's hit box
// ---------------------------------------------------------------------------

/// One recorded button, for the current context-frame only.
///
/// `button()` submits a real `ImGui::InvisibleButton` over the measured rect and stores
/// what it learned here; `buttonPressed()` reads it back. The record is keyed by
/// context-frame *and* id so a button that moved, disappeared or was never drawn cannot
/// answer for another one.
struct HitRecord {
    FrameKey key;
    ImGuiID id = 0;
    bool pressed = false;
};

std::vector<HitRecord> g_hits;

/// A hard bound on the record list: one entry per drawn button per frame, so this is a
/// sanity cap (a frame counter that jumped would otherwise grow the vector once per
/// button).
constexpr std::size_t kMaxHits = 4096;

/// The button's identity: its label and its exact position.
///
/// Two "Delete" buttons in two rows are two buttons, and so are two whose rects differ by
/// a fraction of a pixel - the GUI is a float layout, so hashing the position's bits is the
/// only honest key. `ImGui::GetID` folds in the window and ID stack, so the same label at
/// the same place in two windows is two buttons as well.
ImGuiID buttonId(const ButtonSpec& spec, const Rect& r) {
    const ImGuiID labelId = ImGui::GetID(spec.label != nullptr ? spec.label : "");
    const float position[2] = {r.x, r.y};
    return ImHashData(position, sizeof position, labelId);
}

HitRecord* findHit(const FrameKey& key, ImGuiID id) {
    // A linear scan of one frame's buttons: a view draws tens of them, and the thread's
    // bound (last N groups, task 25) is what keeps that number small.
    for (HitRecord& hit : g_hits) {
        if (hit.id == id && hit.key.matches(key))
            return &hit;
    }
    return nullptr;
}

void recordHit(const FrameKey& key, ImGuiID id, bool pressed) {
    if (g_hits.size() >= kMaxHits)
        g_hits.clear();
    if (!g_hits.empty() && !g_hits.front().key.matches(key))
        g_hits.clear();
    if (HitRecord* existing = findHit(key, id)) {
        existing->pressed = existing->pressed || pressed;
        return;
    }
    g_hits.push_back(HitRecord{key, id, pressed});
}

// ---------------------------------------------------------------------------
// Meter and spinner geometry, shared by measure and render
// ---------------------------------------------------------------------------

/// A bar's segment pitch and total size.
///
/// One `gu` per segment - a cell on the grid, the design's 8 px in the GUI - and a hairline
/// of gap in the GUI only: a cell cannot be subdivided, so the grid's segments butt
/// together and a 10-segment bar is exactly 10 cells (design §5.14's `██████░░░░`). The
/// height is three quarters of a unit (6 px at 100 %) but never less than one hairline,
/// which is the cell the grid draws it in.
struct MeterGeometry {
    float segW = 0.0f;
    float gap = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

MeterGeometry meterGeometry(const Metrics& m, int segments) {
    const int count = std::clamp(segments, 1, kMaxMeterSegments);
    MeterGeometry g;
    g.segW = m.gu;
    g.gap = m.textGrid ? 0.0f : m.border;
    g.w = static_cast<float>(count) * g.segW + static_cast<float>(count - 1) * g.gap;
    g.h = std::max(m.border, roundCells(m.gu * 0.75f));
    return g;
}

/// The spinner's four frames: the design's `◐ ◓ ◑ ◒` (appendix §4.8) and the ASCII column
/// `| / - \` (§4.8's "bar, slash, dash, backslash").
///
/// The cycle lives here, not in `ui/icons.h`, because only frame 0 is a *state mark* - the
/// table's `Running`/`Spinner` row, the mark that sits beside a word - and the table's rule
/// is one row per mark. A reduced-motion surface draws frame 0, which is exactly that
/// row's glyph, so the two agree by construction.
///
/// Recorded deviation: task 16's note says "4-frame braille cycle", while design
/// §4.8/§4.9 specify this cycle at 150 ms. The design wins, and the decisive point is the
/// fallback column: a braille frame has no ASCII form, so a `TERM=linux` console would have
/// nothing to draw for a braille cycle. It also keeps the static mark (frame 0) identical
/// to the glyph table's `◐`.
constexpr const char* kSpinnerGlyphs[4] = {"\xE2\x97\x90", "\xE2\x97\x93", "\xE2\x97\x91",
                                           "\xE2\x97\x92"}; // ◐ ◓ ◑ ◒
constexpr const char* kSpinnerAscii[4] = {"|", "/", "-", "\\"};

constexpr const char* kMeterFilled = "\xE2\x96\x88"; // █
constexpr const char* kMeterEmpty = "\xE2\x96\x91";  // ░
constexpr const char* kMeterFilledAscii = "#";
constexpr const char* kMeterEmptyAscii = ".";

std::string repeatGlyph(const char* glyph, int count) {
    std::string out;
    for (int i = 0; i < count; ++i)
        out += glyph;
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------

Size measurePanel(const Metrics& m, Size want) {
    // The chrome a panel cannot shrink below: two hairlines, `pad2` of inner padding on
    // each side, and one unit that can hold the first glyph.
    const float minW = 2.0f * m.border + 2.0f * m.pad2 + m.gu;
    const float minH = 2.0f * m.border + 2.0f * m.pad2 + m.row;
    return Size{std::max(roundCells(want.w), minW), std::max(roundCells(want.h), minH)};
}

void panel(ImDrawList* dl, const Metrics& m, const Rect& r, ColorToken fill, bool hairline) {
    if (!unitMatches(m, r))
        return;
    fillRect(dl, r, fill, m.radius);
    if (hairline)
        hairlineInside(dl, m, r, ColorToken::lineSoft, m.border);
}

Size measureCard(const Metrics& m, Size want) { return measurePanel(m, want); }

void card(ImDrawList* dl, const Metrics& m, const Rect& r, ColorToken fill) {
    if (!unitMatches(m, r))
        return;
    fillRect(dl, r, fill, m.radius);
    hairlineInside(dl, m, r, ColorToken::lineSoft, m.border);
}

Size measureRule(const Metrics& m, float length, bool horizontal) {
    const float thin = std::max(m.border, 0.0f);
    const float len = std::max(roundCells(length), 0.0f);
    return horizontal ? Size{len, thin} : Size{thin, len};
}

void rule(const Metrics& m, const Rect& r, ColorToken token) {
    if (!unitMatches(m, r))
        return;
    fillRect(targetList(), r, token, 0.0f);
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

Size measureButton(const Metrics& m, const ButtonSpec& spec) {
    const float textW = textUnits(m, spec.label != nullptr ? spec.label : "");
    return Size{roundCells(textW + 2.0f * m.pad2), m.row};
}

void button(const ButtonSpec& spec, const Metrics& m, const Rect& r) {
    if (!unitMatches(m, r))
        return;
    const std::string_view label =
        spec.label != nullptr ? std::string_view(spec.label) : std::string_view();

    // The hit box first: a real item covering exactly the measured rect, so the whole
    // button is clickable rather than the label inside it. Three details matter:
    //
    //   * the item is submitted at an absolute position (a primitive draws where the
    //     layout put it) and the cursor is left advanced, the way any ImGui widget leaves
    //     it - restoring the cursor instead makes ImGui report a cursor that moved
    //     without an item;
    //   * its id is the button's own identity pushed onto the ID stack, so two buttons
    //     with the same label are two *items* and not one shared id (ImGui's own rule:
    //     `InvisibleButton` would otherwise hash the label alone);
    //   * the content extent is restored afterwards, so an absolutely placed primitive
    //     claims no flow space. Without that, a scrolled container's content size grows by
    //     its own scroll offset every frame and can never settle.
    bool hovered = false;
    if (r.w > 0.0f && r.h > 0.0f && ImGui::GetCurrentWindow() != nullptr) {
        const FrameKey key = currentFrame();
        const ImGuiID id = buttonId(spec, r);
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImVec2 contentExtent = window->DC.CursorMaxPos;
        ImGui::SetCursorScreenPos(ImVec2(r.x, r.y));
        ImGui::PushID(static_cast<int>(id & 0x7fffffff));
        ImGui::BeginDisabled(!spec.enabled);
        ImGui::InvisibleButton("##button", ImVec2(r.w, r.h));
        hovered = ImGui::IsItemHovered();
        const bool pressed = spec.enabled && ImGui::IsItemClicked();
        ImGui::EndDisabled();
        ImGui::PopID();
        window->DC.CursorMaxPos = contentExtent;
        recordHit(key, id, pressed);
        // The item is the last item, so a caller can read `ui::tooltip()` straight after
        // `button()` - including for a disabled button, which is the one that needs one.
    }

    // Chrome. A disabled button is the darkest surface with the dimmest ink that still
    // clears 4.5:1 (`textLow` on `inset`, 4.75:1) - `textDim` is barred from carrying a
    // glyph by NFR-09.
    ImDrawList* dl = targetList();
    ColorToken fill = ColorToken::raised;
    ColorToken ink = ColorToken::textHi;
    if (!spec.enabled) {
        fill = ColorToken::inset;
        ink = ColorToken::textLow;
    } else if (spec.primary) {
        fill = ColorToken::accent;
        ink = ColorToken::canvas; // dark ink on the accent fill: 7.9:1
    }
    if (hovered && spec.enabled)
        fillRect(dl, r, ImGui::GetColorU32(stateStep(color(fill), 1)), m.radius);
    else
        fillRect(dl, r, fill, m.radius);

    // A primary button's fill *is* its edge - an accent border on an accent fill would be
    // four invisible rects - so only the quiet variants carry a hairline.
    if (!spec.primary)
        hairlineInside(dl, m, r, ColorToken::lineStrong, m.border);
    if (spec.focused) {
        // The design's ring is accent at 60 %, which cannot read on an accent fill; there
        // the ring flips to the fill's own ink (canvas on accent, 7.9:1).
        hairlineInside(dl, m, r, spec.primary ? ColorToken::canvas : ColorToken::focusRing,
                       m.border);
    }

    const float textW = textUnits(m, label);
    const float penX = centredPenX(m, r, textW);
    drawLine(dl, m, r, label, ink, penX, roomFrom(penX, r, m.border));
}

bool buttonPressed(const ButtonSpec& spec, const Metrics& m, const Rect& r) {
    if (!spec.enabled || !unitMatches(m, r))
        return false;
    if (ImGui::GetCurrentContext() == nullptr || ImGui::GetCurrentWindow() == nullptr)
        return false;
    const HitRecord* hit = findHit(currentFrame(), buttonId(spec, r));
    return hit != nullptr && hit->pressed;
}

Size measureIconButton(const Metrics& m) { return Size{m.row, m.row}; }

void iconButton(const Metrics& m, const Rect& r, Icon which, bool focused) {
    if (!unitMatches(m, r))
        return;
    ImDrawList* dl = targetList();
    fillRect(dl, r, ColorToken::raised, m.radius);
    hairlineInside(dl, m, r, ColorToken::lineStrong, m.border);
    if (focused)
        hairlineInside(dl, m, r, ColorToken::focusRing, m.border);

    const char* mark = icon(which, glyphSetFor(m));
    const float markW = textUnits(m, mark);
    const float penX = centredPenX(m, r, markW);
    drawGlyph(dl, r, penX, rowPenY(r), mark, ColorToken::textMid);
}

// ---------------------------------------------------------------------------
// Chips, pills, rows
// ---------------------------------------------------------------------------

Size measureChip(const Metrics& m, std::string_view text) {
    return Size{roundCells(textUnits(m, text) + 2.0f * m.pad1), m.row};
}

void chip(const Metrics& m, const Rect& r, std::string_view text, ColorToken fg, ColorToken bg) {
    if (!unitMatches(m, r))
        return;
    ImDrawList* dl = targetList();
    fillRect(dl, r, bg, m.radius);
    hairlineInside(dl, m, r, ColorToken::lineSoft, m.border);
    drawLine(dl, m, r, text, fg, r.x + m.pad1, r.w - 2.0f * m.pad1);
}

Size measurePill(const Metrics& m, std::string_view text) {
    // [pad1][glyph: gu][pad1][text][pad1]
    return Size{roundCells(m.gu + m.pad1 + textUnits(m, text) + 2.0f * m.pad1), m.row};
}

void pill(const Metrics& m, const Rect& r, Icon which, std::string_view text, ColorToken fg,
          ColorToken bg) {
    if (!unitMatches(m, r))
        return;
    ImDrawList* dl = targetList();
    fillRect(dl, r, bg, m.radius);
    hairlineInside(dl, m, r, ColorToken::lineSoft, m.border);
    const float glyphX = r.x + m.pad1;
    drawGlyph(dl, r, glyphX, rowPenY(r), icon(which, glyphSetFor(m)), fg);
    const float penX = glyphX + m.gu + m.pad1;
    drawLine(dl, m, r, text, fg, penX, roomFrom(penX, r, m.pad1));
}

Size measureRow(const Metrics& m, std::string_view text) {
    return Size{roundCells(textUnits(m, text) + 2.0f * m.pad1), m.row};
}

void row(const Metrics& m, const Rect& r, std::string_view text, bool selected, bool enabled) {
    if (!unitMatches(m, r))
        return;
    ImDrawList* dl = targetList();
    if (selected)
        fillRect(dl, r, ColorToken::selection, 0.0f);
    const ColorToken ink =
        !enabled ? ColorToken::textLow : (selected ? ColorToken::textHi : ColorToken::textMid);
    drawLine(dl, m, r, text, ink, r.x + m.pad1, r.w - 2.0f * m.pad1);
}

// ---------------------------------------------------------------------------
// Marks and progress
// ---------------------------------------------------------------------------

Size measureIconMark(const Metrics& m) { return Size{m.gu, m.row}; }

void iconMark(const Metrics& m, const Rect& r, Icon which, ColorToken fg) {
    if (!unitMatches(m, r))
        return;
    drawGlyph(targetList(), r, r.x, rowPenY(r), icon(which, glyphSetFor(m)), fg);
}

Size measureMeter(const Metrics& m, int segments) {
    const MeterGeometry g = meterGeometry(m, segments);
    return Size{g.w, g.h};
}

void meter(const Metrics& m, const Rect& r, int segments, float fraction, ColorToken fill) {
    if (!unitMatches(m, r))
        return;
    const MeterGeometry g = meterGeometry(m, segments);
    const int count = std::clamp(segments, 1, kMaxMeterSegments);
    const float clamped = std::isfinite(fraction) ? std::clamp(fraction, 0.0f, 1.0f) : 0.0f;
    // A segment lights only when it is full, so a 99 % bar reports 9 of 10 rather than
    // claiming a segment it has not filled.
    const int filled = std::clamp(static_cast<int>(clamped * static_cast<float>(count)), 0, count);
    const int empty = count - filled;

    ImDrawList* dl = targetList();
    // Clipped to its own rect: a caller may have less room than the measured width (a
    // breakpoint budget), and a bar that ran past it would paint over its neighbour.
    dl->PushClipRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), true);
    if (m.textGrid) {
        const bool ascii = glyphSetFor(m) == GlyphSet::Ascii;
        const char* fullGlyph = ascii ? kMeterFilledAscii : kMeterFilled;
        const char* emptyGlyph = ascii ? kMeterEmptyAscii : kMeterEmpty;
        const float penY = rowPenY(r);
        if (filled > 0)
            drawGlyph(dl, r, r.x, penY, repeatGlyph(fullGlyph, filled).c_str(), fill);
        if (empty > 0)
            drawGlyph(dl, r, r.x + static_cast<float>(filled) * g.segW, penY,
                      repeatGlyph(emptyGlyph, empty).c_str(), ColorToken::lineStrong);
    } else {
        const float y = r.y + std::floor((r.h - g.h) * 0.5f);
        for (int i = 0; i < count; ++i) {
            Rect segment;
            segment.x = r.x + static_cast<float>(i) * (g.segW + g.gap);
            segment.y = y;
            segment.w = g.segW;
            segment.h = g.h;
            segment.cells = r.cells;
            fillRect(dl, segment, i < filled ? fill : ColorToken::lineStrong, m.border);
        }
    }
    dl->PopClipRect();
}

int spinnerFrame(double phase) {
    if (!(phase > 0.0) || !std::isfinite(phase))
        return 0; // the static mark: idle, reduced motion, and a poisoned phase
    const int frame = static_cast<int>(std::fmod(phase / kSpinnerFrameSeconds, 4.0));
    return std::clamp(frame, 0, 3);
}

Size measureSpinner(const Metrics& m) { return Size{m.gu, m.row}; }

void spinner(const Metrics& m, const Rect& r, double phase) {
    if (!unitMatches(m, r))
        return;
    const int frame = spinnerFrame(phase);
    const char* mark =
        glyphSetFor(m) == GlyphSet::Ascii ? kSpinnerAscii[frame] : kSpinnerGlyphs[frame];
    drawGlyph(targetList(), r, r.x, rowPenY(r), mark, ColorToken::accent);
}

Size measureDisclosure(const Metrics& m, std::string_view label) {
    // [glyph: gu][pad1][label]
    return Size{roundCells(m.gu + m.pad1 + textUnits(m, label)), m.row};
}

void disclosure(const Metrics& m, const Rect& r, std::string_view label, bool open) {
    if (!unitMatches(m, r))
        return;
    ImDrawList* dl = targetList();
    const Icon mark = open ? Icon::DisclosureOpen : Icon::DisclosureClosed;
    drawGlyph(dl, r, r.x, rowPenY(r), icon(mark, glyphSetFor(m)), ColorToken::textLow);
    const float penX = r.x + m.gu + m.pad1;
    drawLine(dl, m, r, label, ColorToken::textHi, penX, roomFrom(penX, r, 0.0f));
}

Size measureSkeleton(const Metrics& m, Size want) {
    return Size{std::max(roundCells(want.w), m.row), m.row};
}

void skeleton(const Metrics& m, const Rect& r, float t01) {
    if (!unitMatches(m, r))
        return;
    ImDrawList* dl = targetList();
    fillRect(dl, r, ColorToken::inset, m.radius);
    if (m.textGrid) {
        // A cell renderer cannot shimmer without a redraw; the design asks for the static
        // cut mark here (design §5.26).
        const std::string mark = icon(Icon::Ellipsis, glyphSetFor(m));
        if (displayWidth(mark) <= static_cast<int>(r.w))
            drawGlyph(dl, r, r.x, rowPenY(r), mark.c_str(), ColorToken::textLow);
        return;
    }
    const float t = std::isfinite(t01) ? std::clamp(t01, 0.0f, 1.0f) : 0.0f;
    const float bandW = std::max(m.gu, r.w * 0.3f);
    const float bandX = r.x + (r.w + bandW) * t - bandW;
    dl->PushClipRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), true);
    Rect band;
    band.x = bandX;
    band.y = r.y;
    band.w = bandW;
    band.h = r.h;
    band.cells = r.cells;
    fillRect(dl, band, ColorToken::surface, 0.0f);
    dl->PopClipRect();
}

// ---------------------------------------------------------------------------
// Tooltip
// ---------------------------------------------------------------------------

Size measureTooltip(const Metrics& m, std::string_view text) {
    return Size{roundCells(textUnits(m, text) + 2.0f * m.pad2), roundCells(m.row + 2.0f * m.pad1)};
}

void tooltip(std::string_view text) {
    if (text.empty())
        return;
    // `DelayNormal` is the design's "normal" hover delay; `AllowWhenDisabled` keeps a
    // disabled control's explanation reachable, which is the whole reason a disabled
    // control carries one.
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled))
        return;
    ImGui::SetTooltip("%.*s", static_cast<int>(text.size()), text.data());
}

// ---------------------------------------------------------------------------
// Feedback (design §5.25, FR-29, task 45)
// ---------------------------------------------------------------------------

namespace {

/// The dismiss control's identity. Not a label a user sees: it is hashed with the card's rect
/// (`buttonId`) so two cards' crosses are two items, and the glyph drawn in it is the table's
/// `Error` mark (`×` / `x`), never a literal.
constexpr char kToastDismissId[] = "##toast_dismiss";

/// Submit an invisible hit box and record it, drawing no chrome.
///
/// The button's hit-box half (`button()`), without the surface: the toast's dismiss cross is a
/// *mark* inside the card it already painted, and a raised square behind it would read as a
/// second card. The record is the same one `buttonPressed()` reads, under the same identity, so
/// "was it pressed" answers exactly like a button's.
void submitHitBox(const char* label, const Rect& r) {
    if (r.w <= 0.0f || r.h <= 0.0f || ImGui::GetCurrentWindow() == nullptr)
        return;
    const FrameKey key = currentFrame();
    const ImGuiID id = buttonId(ButtonSpec{label}, r);
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 contentExtent = window->DC.CursorMaxPos;
    ImGui::SetCursorScreenPos(ImVec2(r.x, r.y));
    ImGui::PushID(static_cast<int>(id & 0x7fffffff));
    ImGui::InvisibleButton("##hit", ImVec2(r.w, r.h));
    const bool pressed = ImGui::IsItemClicked();
    ImGui::PopID();
    window->DC.CursorMaxPos = contentExtent;
    recordHit(key, id, pressed);
}

/// The dismiss control's box: one row square in the card's top-right corner - the smallest box
/// that holds a glyph at every scale (24 px in the pixel profile, one cell on the grid).
Rect toastDismissRect(const Metrics& m, const Rect& r) {
    Rect box;
    box.w = std::max(m.row, 0.0f);
    box.h = std::max(m.row, 0.0f);
    box.x = r.right() - m.pad2 - box.w;
    box.y = r.y + m.pad2;
    box.cells = r.cells;
    return box;
}

/// The card's parts. One derivation for the measure, the draw and the two hit boxes, so the rect
/// a control answers at is the rect it was drawn in.
struct ToastLayout {
    float ruleW = 0.0f;
    Rect rule;
    Rect mark;
    Rect title;
    Rect detail;
    Rect action;
    Rect dismiss;
};

ToastLayout toastLayout(const Metrics& m, const ToastSpec& spec, const Rect& r) {
    ToastLayout out;
    // 4 units of state rule: 4 px in the pixel profile (the design's "4 px state rule") and four
    // cells on the grid, where the smallest drawable bar is a cell.
    out.ruleW = 4.0f * std::max(m.border, 0.0f);
    out.rule = Rect{r.x, r.y, out.ruleW, r.h, r.cells};

    const float markW = std::max(m.gu, 0.0f);
    const float textX = r.x + out.ruleW + m.pad2 + markW + m.pad1;
    out.dismiss = toastDismissRect(m, r);

    out.mark = Rect{r.x + out.ruleW + m.pad2, r.y + m.pad2, markW, m.row, r.cells};
    const float textRoom =
        std::max(0.0f, out.dismiss.x - m.pad1 - textX); // the cross owns the title row's right
    out.title = Rect{textX, r.y + m.pad2, textRoom, m.row, r.cells};
    if (spec.detail != nullptr && spec.detail[0] != '\0')
        out.detail = Rect{textX, r.y + m.pad2 + m.row, textRoom, m.row, r.cells};
    if (spec.action != nullptr && spec.action[0] != '\0') {
        const float actionW = roundCells(textUnits(m, spec.action) + 2.0f * m.pad2);
        out.action = Rect{r.right() - m.pad2 - actionW, r.bottom() - m.pad2 - m.row, actionW, m.row,
                          r.cells};
    }
    return out;
}

} // namespace

Size measureToast(const Metrics& m, const ToastSpec& spec, Size want) {
    const auto width = [&m](const char* text) { return textUnits(m, text != nullptr ? text : ""); };
    const float actionW = (spec.action != nullptr && spec.action[0] != '\0')
                              ? roundCells(width(spec.action) + 2.0f * m.pad2)
                              : 0.0f;
    // [rule][pad2][mark: gu][pad1][text ... cross: row][pad2] - the cross sits on the title row,
    // so the text's room stops one `pad1` left of it.
    const float body = std::max(std::max(width(spec.title), width(spec.detail)), actionW);
    const float contentW = 4.0f * std::max(m.border, 0.0f) + m.pad2 + m.gu + m.pad1 + body +
                           m.pad1 + std::max(m.row, 0.0f) + m.pad2;
    const bool hasDetail = spec.detail != nullptr && spec.detail[0] != '\0';
    const bool hasAction = actionW > 0.0f;
    const float contentH =
        2.0f * m.pad2 + m.row + (hasDetail ? m.row : 0.0f) + (hasAction ? m.pad1 + m.row : 0.0f);
    return Size{std::max(roundCells(contentW), roundCells(want.w)),
                std::max(roundCells(contentH), roundCells(want.h))};
}

void toast(const ToastSpec& spec, const Metrics& m, const Rect& r) {
    if (!unitMatches(m, r))
        return;
    ImDrawList* dl = targetList();
    // The card: the raised step with a hairline. No shadow - `ui/tokens.h` has no shadow token and
    // this renderer has no blur, and a fake one under a card is a second surface the design never
    // asked for.
    fillRect(dl, r, ColorToken::raised, m.radius);
    hairlineInside(dl, m, r, ColorToken::lineSoft, m.border);

    const ToastLayout layout = toastLayout(m, spec, r);
    rule(m, layout.rule, spec.rule);
    drawGlyph(dl, layout.mark, layout.mark.x, rowPenY(layout.mark), icon(spec.mark, glyphSetFor(m)),
              spec.rule);
    drawLine(dl, m, layout.title, spec.title != nullptr ? spec.title : "", spec.titleInk,
             layout.title.x, layout.title.w);
    if (layout.detail.w > 0.0f && layout.detail.h > 0.0f)
        drawLine(dl, m, layout.detail, spec.detail, spec.detailInk, layout.detail.x,
                 layout.detail.w);
    if (layout.action.w > 0.0f && layout.action.h > 0.0f)
        button(ButtonSpec{spec.action}, m, layout.action);

    // The dismiss cross last, so it is the topmost item of the card: a pointer on the corner
    // answers the cross and not the card behind it.
    submitHitBox(kToastDismissId, layout.dismiss);
    const char* cross = icon(Icon::Error, glyphSetFor(m));
    drawGlyph(dl, layout.dismiss, centredPenX(m, layout.dismiss, textUnits(m, cross)),
              rowPenY(layout.dismiss), cross, ColorToken::textLow);
}

bool toastActionPressed(const ToastSpec& spec, const Metrics& m, const Rect& r) {
    if (spec.action == nullptr || spec.action[0] == '\0' || !unitMatches(m, r))
        return false;
    const ToastLayout layout = toastLayout(m, spec, r);
    if (layout.action.w <= 0.0f || layout.action.h <= 0.0f)
        return false;
    return buttonPressed(ButtonSpec{spec.action}, m, layout.action);
}

bool toastDismissPressed(const Metrics& m, const Rect& r) {
    if (!unitMatches(m, r))
        return false;
    if (ImGui::GetCurrentContext() == nullptr || ImGui::GetCurrentWindow() == nullptr)
        return false;
    const Rect box = toastDismissRect(m, r);
    const HitRecord* hit = findHit(currentFrame(), buttonId(ButtonSpec{kToastDismissId}, box));
    return hit != nullptr && hit->pressed;
}

} // namespace llmfun::tui::ui
