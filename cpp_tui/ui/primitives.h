/// The widget vocabulary (design §4.15 "Primitives", plan §8.6): one function per
/// primitive, each with a `measure()` that answers the size it draws in.
///
///     const ui::Size s = ui::measureButton(m, spec);
///     ui::button(spec, m, {x, y, s.w, s.h, m.textGrid});
///     if (ui::buttonPressed(spec, m, sameRect)) accept();
///
/// Why a measure at all: the layout engine (task 17) has to place a primitive
/// *before* it is drawn, and the two numbers must not be able to disagree. So the
/// size arithmetic lives in `measure*()`, the renderer takes the rect that came out
/// of it, and neither recomputes the other's half. `./test_primitives` proves it
/// the only way it can be proven: it renders each primitive into a cell grid at
/// exactly its measured rect and asserts that nothing at all is painted outside
/// that rect, and - for button, chip, meter and disclosure - that the rect the
/// renderer actually occupies is the measured one, cell for cell.
///
/// Units. A `Rect` is cells on the text grid and pixels in the GUI (`Rect.cells`
/// says which, and every primitive asserts it against `Metrics::textGrid`), and a
/// `Size` is in the same unit. Nothing here converts: the profile decision was made
/// once, in `ui::metricsFor()`, and every length below is a multiple of
/// `Metrics::gu`/`row`/`pad*`/`border`.
///
/// Colour. No primitive writes a colour. Each one names a `ColorToken` (its own
/// default, or a parameter the caller passes) and resolves it through
/// `ui::color()`, so the same call site is correct at true colour, 256 and 16
/// colours. That rule is checked mechanically: `./test_primitives` reads this
/// module's own source and fails if a colour literal (the `ImVec4` constructor or a
/// packed `IM_COL32` value) appears in it, and asserts that every ink/fill pair the
/// primitives can produce clears the design's contrast bar (4.5:1 for text, 3:1 for
/// a boundary) - the same bar `test_tokens` holds the theme to. A resolved `ImU32`
/// local is not a literal: it holds what `ui::color()` answered.
///
/// Two renderings, one anatomy. On the text grid a primitive draws glyphs into
/// cells (`AddText` at whole-cell coordinates, truncated with `ui::truncate()` so a
/// label can never claim a cell it was not measured for); in the GUI it draws
/// rects with the tokens' radii - a fill with `AddRectFilled`, a border as one
/// inset stroked rectangle so it follows that radius instead of framing it in a
/// square. The *anatomy* - what sits where, how wide the padding is, how tall a row
/// is - is one set of numbers, which is what makes the two profiles the same design
/// rather than two designs that resemble each other.
///
/// Inside the rect. Every primitive paints only inside the rect it is handed, even
/// when the caller hands it less room than the measure asked for (a breakpoint
/// budget, a shrunken container): labels and glyphs are clipped, a cut is marked
/// with the profile's ellipsis on the grid, and the bar runs are clipped to the
/// bar's own rect. A primitive is a geometry taker, so "less room" is a legal call
/// and not a licence to overrun the neighbour.
///
/// Layer: `ui/` only - `ui/metrics.h`, `ui/tokens.h`, `ui/text.h`, `ui/icons.h`
/// and imgui. No `tui.h`, no `ChatMessage`, no view header: a primitive knows
/// nothing about the app, which is what lets it be tested on its own and lets the
/// same code serve the shell and the thread's block chrome.
#pragma once

#include <string_view>

#include "imgui/imgui.h"

#include "ui/icons.h"
#include "ui/metrics.h"
#include "ui/tokens.h"

namespace llmfun::tui::ui {

/// The size a primitive draws in: cells on the text grid, pixels in the GUI.
///
/// Not a rect - it has no position and no unit flag. The unit is the `Metrics` it
/// was measured with, and the caller turns it into a `Rect` (setting `cells` from
/// `Metrics::textGrid`), which is where the unit becomes checkable again.
struct Size {
    float w = 0.0f;
    float h = 0.0f;
};

/// What a button says and how it looks. `label` is borrowed for the call only and
/// is also the button's identity (with its rect): two buttons with the same label
/// at different rects are different buttons, which is what a list of rows needs.
struct ButtonSpec {
    const char* label = "";
    /// A disabled button is drawn in the darkest surface with `textLow` ink and is
    /// not a hit target at all.
    bool enabled = true;
    /// Draws the focus ring. The ring is a state, not an animation: the text grid
    /// gets no focus transition (design §4.9, "accepted hole").
    bool focused = false;
    /// The primary action: `accent` fill with `canvas` ink (7.9:1), which is the
    /// one pair in the vocabulary that inverts the ink/fill relationship.
    bool primary = false;
};

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------

/// A panel's size: the wish, rounded to whole units and grown (never shrunk) so
/// the chrome fits - two hairlines plus `pad2` of inner padding plus one unit of
/// content in each axis. A panel with nothing in it still has to be a panel.
Size measurePanel(const Metrics& m, Size want);

/// A panel: fill, then - when asked - a hairline *inside* the rect on all four
/// edges, drawn as four filled bars rather than a stroked rectangle. A stroke in
/// this renderer sits on the pixel boundary above the rect (a known imtui
/// behaviour), which would paint outside the measured size; four bars cannot.
void panel(ImDrawList* dl, const Metrics& m, const Rect& r, ColorToken fill, bool hairline);

/// A card: the raised step of the surface ladder with a hairline, i.e. a panel
/// with the design's elevation instead of a caller-chosen fill. Same minimum size
/// as a panel; "no nested cards" is the caller's rule (design §4.7).
Size measureCard(const Metrics& m, Size want);
void card(ImDrawList* dl, const Metrics& m, const Rect& r, ColorToken fill);

/// A rule: a filled bar one hairline thick in its short axis. `length` is the long
/// axis, in the profile's unit.
Size measureRule(const Metrics& m, float length, bool horizontal = true);

/// A rule fills its whole rect with one token - no rounding, no hairline. That is
/// the difference from `panel(fill, true)`: a rule *is* the line, whether it
/// separates two regions or marks a role (the design's 2 px role rule is a rule
/// two units thick, and still one fill).
void rule(const Metrics& m, const Rect& r, ColorToken token);

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

/// A button's size: the label plus `pad2` on each side, one row (24 px / 1 cell)
/// tall. The row height *is* the design's 1-row/24 px hit box - the target is the
/// measured size, not an invisible margin around it.
Size measureButton(const Metrics& m, const ButtonSpec& spec);

/// Draw a button and submit its hit box.
///
/// This is the primitive that replaces the v4 blank-button-plus-
/// overlaid-text: the chrome, the label and the interactive item are one call. The
/// item is a real `ImGui::InvisibleButton` covering exactly `r`, which is what
/// makes the whole measured rect clickable (and hoverable) instead of a label the
/// user has to hit; the label is drawn by this primitive, cell-exactly on the
/// grid, so a caller that wants its own label can still draw over it.
///
/// The click is *recorded*, not returned - `button()` returns nothing so that a
/// caller can draw a row of buttons and then act on the answered one. Ask
/// `buttonPressed()` for the result in the same frame.
///
/// States. `primary` fills with `accent` and inks its label with `canvas`; a primary
/// button carries no hairline (its fill is its edge - an accent border on an accent
/// fill would be invisible), and its focus ring flips to `canvas` because the
/// design's accent-at-60 % ring cannot read on an accent fill. A quiet button has a
/// `lineStrong` hairline and a `focusRing` ring. A disabled one is `inset` with
/// `textLow` ink and is not a hit target at all.
///
/// Layout. The item is submitted at an absolute position and claims no flow space
/// (the caller's content extent is restored), so a scrolled container does not grow
/// by its own scroll offset every frame; a view inside a scroller pins its content
/// height itself. The item is nevertheless the *last item*, so `ui::tooltip()` (or
/// `ImGui::IsItemHovered()`) can be called straight after `button()` - including for
/// a disabled button, which is the one that needs an explanation.
void button(const ButtonSpec& spec, const Metrics& m, const Rect& r);

/// Whether the button drawn by `button()` at `(spec, r)` was clicked this frame.
///
/// Read it in the same frame, in the same window and on the same ID stack as the
/// `button()` call: the answer is stored under the button's identity, and that
/// identity is the label hashed with its exact position inside the current ImGui
/// window. True on the press frame exactly once per click (the `IsItemClicked`
/// semantic, not press-and-release), false for a disabled button, and false when
/// nothing was drawn at that rect this frame - so a stale rect cannot answer for a
/// button that moved or disappeared.
bool buttonPressed(const ButtonSpec& spec, const Metrics& m, const Rect& r);

/// A square icon button: one row on each side (24 x 24 px, one cell on the grid).
Size measureIconButton(const Metrics& m);

/// An icon button draws the raised surface, a hairline and the glyph, centred.
/// It submits no item: its caller owns the click (a rail row, the top bar's
/// settings entry) because the item and its neighbours are one list.
///
/// On the grid a one-cell-tall rect has no interior, so the hairline and the focus
/// ring are omitted and the shape is its fill: a cell cannot hold a box around a
/// single row (see `hairlineInside` in `primitives.cpp`).
void iconButton(const Metrics& m, const Rect& r, Icon which, bool focused);

// ---------------------------------------------------------------------------
// Chips, pills, rows
// ---------------------------------------------------------------------------

/// A chip's size: the text plus `pad1` on each side, one row (24 px / 1 cell) tall.
Size measureChip(const Metrics& m, std::string_view text);

/// A chip: a small labelled surface (`bg`), hairline, `fg` label. The pair is the
/// caller's - a chip is how a view says "this mode", and the mode's colours are
/// the view's decision - but the geometry is this file's, so the label can never
/// spill over the fill.
///
/// The corner radius is the metrics' one radius (6 px at 100 %, 0 on the grid). The
/// design's smaller 4 px chip radius is a style refinement rather than a layout one:
/// `Metrics` carries a single radius, and task 20 owns the radius table.
void chip(const Metrics& m, const Rect& r, std::string_view text, ColorToken fg, ColorToken bg);

/// A pill: a leading state glyph, one unit of air, then the text - the design's
/// `● running 00:12` (design §5.1). Same surface as a chip, one glyph wider.
Size measurePill(const Metrics& m, std::string_view text);
void pill(const Metrics& m, const Rect& r, Icon which, std::string_view text, ColorToken fg,
          ColorToken bg);

/// A list row's size: the text plus `pad1` on each side, one row tall.
Size measureRow(const Metrics& m, std::string_view text);

/// A row: one line of a list. `selected` fills with `selection` (flattened to a
/// solid step by the text backend, which cannot draw a 14 % fill) and lifts the
/// ink to `textHi`; a disabled row keeps `textLow` - the dimmest ink that still
/// clears 4.5:1, never `textDim` (NFR-09: it carries no glyph).
void row(const Metrics& m, const Rect& r, std::string_view text, bool selected, bool enabled);

// ---------------------------------------------------------------------------
// Marks and progress
// ---------------------------------------------------------------------------

/// An icon slot: one unit wide, one row tall.
Size measureIconMark(const Metrics& m);

/// One glyph from the table (`ui/icons.h`), left-aligned in its slot. The set is
/// the surface's own answer (`textGlyphSet()` / `pixelGlyphSet()`), derived from
/// the metrics rather than passed in, because the contract for this primitive
/// carries no `GlyphSet` - and derived, not guessed: both answers *are* what the
/// surface can draw (see `ui/icons.h`). A glyph wider than the rect is clipped to
/// it rather than drawn over the neighbour.
void iconMark(const Metrics& m, const Rect& r, Icon which, ColorToken fg);

/// A meter's size for `segments` segments: one `gu` per segment, one hairline of
/// gap in the GUI and none on the grid (a cell cannot be subdivided), three
/// quarters of a `gu` tall and never thinner than one hairline - 6 px / 1 cell at
/// 100 %, which is the design's 10-segment bar.
Size measureMeter(const Metrics& m, int segments);

/// A segmented bar. Segments fill left to right and a segment lights only when it
/// is *full* (`floor(fraction * segments)`, clamped), so a 99 % bar reports 9 of 10
/// and cannot claim space it has not used. The empty half is `lineStrong` - a
/// boundary at 3:1, not decoration - and the value is always also in text next to
/// it, so the colour is redundant (design §5.14). The whole bar is clipped to `r`:
/// a caller that has less room than the measured width gets the segments that fit,
/// never a bar running into its neighbour.
void meter(const Metrics& m, const Rect& r, int segments, float fraction, ColorToken fill);

/// One spinner frame lasts 150 ms and there are four of them (design §4.8/§4.9):
/// `◐ ◓ ◑ ◒`, ASCII `| / - \`.
///
/// The cycle lives here rather than in `ui/icons.h` on purpose: only its first
/// frame is a *state mark* (the table's `Running`/`Spinner` row, beside the word),
/// and the table's rule is one row per mark. A caller that must not animate - the
/// idle shell, reduced motion - passes a negative phase and gets frame 0.
constexpr double kSpinnerFrameSeconds = 0.150;

/// The frame index for a phase in seconds: `phase < 0`, NaN and infinity all answer
/// 0 (the static mark). Pure, so a frame loop can compare this frame's index with
/// the last one and request no frame at all while it is unchanged (NFR-01: motion
/// never raises the idle frame rate).
int spinnerFrame(double phase);

/// The animated mark's slot: one unit wide, one row tall.
Size measureSpinner(const Metrics& m);

/// The busy mark for `phase` seconds, drawn in `accent` - the app's one live signal,
/// and the token the design's run pill uses for the same meaning. `phase` is a
/// monotonic clock reading, not a frame count, so the cycle keeps its 150 ms cadence
/// whatever the frame rate.
void spinner(const Metrics& m, const Rect& r, double phase);

/// A disclosure's size: the glyph, `pad1` of air, then the label - one row tall.
Size measureDisclosure(const Metrics& m, std::string_view label);

/// A disclosure header: `▸`/`▾` (or `>`/`v`) and a label. The mark is the one the
/// table gives `DisclosureClosed`/`DisclosureOpen`, so a collapsed row and a rail
/// row at rest read the same. Both the mark and the label are clipped to `r`.
void disclosure(const Metrics& m, const Rect& r, std::string_view label, bool open);

/// A skeleton's size: at least one row wide, one row tall.
Size measureSkeleton(const Metrics& m, Size want);

/// A placeholder for content that has not arrived (design §5.26). In the GUI it is
/// an `inset` bar with a `surface` band sweeping across it, positioned by
/// `t01` in [0,1] (clamped); on the grid it is the static cut mark, because a cell
/// renderer cannot shimmer without a redraw and the design says so ("TUI static
/// `…`").
void skeleton(const Metrics& m, const Rect& r, float t01);

// ---------------------------------------------------------------------------
// Tooltip
// ---------------------------------------------------------------------------

/// A tooltip's size: the text plus `pad2` on each side, one row plus `pad1` of
/// vertical padding. Reported for the layout's benefit; the GUI tooltip itself is
/// sized by ImGui's own window, which has the same anatomy.
Size measureTooltip(const Metrics& m, std::string_view text);

/// Show a tooltip for the item that was submitted just before this call - the
/// ImGui convention (`IsItemHovered` on the previous item). Hover delay is the
/// design's "normal" delay, and a disabled item still explains itself (that is the
/// point of a disabled tooltip).
void tooltip(std::string_view text);

// ---------------------------------------------------------------------------
// Feedback (design §5.25, FR-29, task 45)
// ---------------------------------------------------------------------------

/// What a toast says and how it is inked.
///
/// One line (the title), an optional second line (the detail) and an optional single action -
/// the design's "title + one line + optional action". The severity is *not* a parameter here:
/// the caller resolves it to a mark and two tokens (`views/feedback.h` does that once for the
/// stack and the now bar's flash), because `ui/` knows no app vocabulary and the primitive must
/// keep its "no colour decision" rule.
struct ToastSpec {
    const char* title = "";
    /// The second line. "" is the ordinary case - a toast is an answer, not a paragraph.
    const char* detail = "";
    /// One action, at most (the design's limit): a label drawn as a quiet button on the card's
    /// last row. "" means the card carries no action and `toastActionPressed()` answers false.
    const char* action = "";
    /// The severity's mark, drawn beside the title.
    Icon mark = Icon::Info;
    ColorToken titleInk = ColorToken::textHi;
    ColorToken detailInk = ColorToken::textMid;
    /// The 4-unit state rule on the leading edge - the one colour a toast spends on a surface,
    /// exactly as the now bar's row spends it on its phase rule.
    ColorToken rule = ColorToken::accent;
};

/// A toast's size: the title, the detail and the action each measured, plus the rule, the mark,
/// the paddings and the dismiss control.
///
/// `want` is the caller's preferred width and is grown, never shrunk (the panel rule): the pixel
/// profile asks for the design's 360 px card and a long path widens it instead of being cut. The
/// height is the rows the card carries - title, optional detail, optional action row.
Size measureToast(const Metrics& m, const ToastSpec& spec, Size want = {});

/// Draw a toast and submit its three hit boxes: the card itself is not one, the action button
/// and the dismiss control are.
///
/// Anatomy: the `raised` surface with a hairline (the design's card, minus the shadow - this
/// renderer has no shadow token), the severity's rule on the leading edge, the mark and the title
/// on the first row, the detail under it, and the action and the dismiss mark on the card's right.
/// Every string is clipped to the card: a toast is never a reason to paint over its neighbour.
void toast(const ToastSpec& spec, const Metrics& m, const Rect& r);

/// Whether the action drawn by `toast()` at `(spec, r)` was pressed this frame - the `button()`
/// contract, one control over (`true` on the press frame, once, and false when the card carries
/// no action or was not drawn).
bool toastActionPressed(const ToastSpec& spec, const Metrics& m, const Rect& r);

/// Whether the dismiss control was pressed this frame. It is offered on every card, because a
/// persistent item (a warning, an error) has no other way out in the pixel profile than the
/// pointer.
bool toastDismissPressed(const Metrics& m, const Rect& r);

} // namespace llmfun::tui::ui
