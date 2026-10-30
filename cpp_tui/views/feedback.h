/// The feedback presentation (design §5.25, FR-29, task 45): the severity's look, and the pixel
/// profile's toast stack.
///
/// One item, two surfaces. The *queue* is the model's (`model/toast.h`): what a toast says, how
/// long it lives and which item is the newest. This file owns only what a severity looks like and
/// where the stack sits, so the grid's flash (`views/nowbar.cpp`) and the stack draw the same item
/// with the same mark and the same ink - a warning cannot be amber on one row and grey in a card.
///
/// The stack is *not* a tiled region and not an overlay in `ui::OverlayLayer`'s z-order: it is
/// chrome above the tiling with no keyboard of its own (D-OVERLAY's rule for a surface that never
/// takes a key). The pixel profile draws it; the grid's one line is the now bar's flash, so a
/// stack on a cell surface would be a second presentation of one item.
///
/// It is drawn last, after the overlay layer, and each card is its own window: a card therefore
/// paints above a modal that happens to reach into the corner it occupies. That is deliberate - a
/// warning that a dialog covers is a warning the user was never told - and the cost is bounded,
/// because a card takes no key and the pointer only has to reach the `Dismiss` control.
///
/// Layering: `views → ui + model`. Nothing here reads the aggregate's other fields or the display
/// size - the caller hands the area the stack may occupy and the metrics it draws with.
#pragma once

#include <cstddef>
#include <vector>

#include "model/toast.h"
#include "ui/icons.h"
#include "ui/metrics.h"
#include "ui/tokens.h"

namespace llmfun::tui::model {
struct UiState;
} // namespace llmfun::tui::model

namespace llmfun::tui::ui {
struct ToastSpec;
} // namespace llmfun::tui::ui

namespace llmfun::tui::views {

/// The mark and the ink a severity draws with - one mapping, so the stack and the now bar's flash
/// cannot disagree:
///
///     info    ◇ / i   `textMid`     (a plain answer: a copy, a saved path)
///     success ✓ / +   `stateOk`
///     warn    ▲ / !   `stateWarn`
///     error   ✕ / x   `stateDanger`
///
/// The mark is always beside the word (NFR-16), so a 16-colour or ASCII surface keeps the meaning.
struct FeedbackLook {
    ui::Icon mark = ui::Icon::Info;
    ui::ColorToken ink = ui::ColorToken::textMid;
};

FeedbackLook feedbackLook(model::ToastKind kind);

/// The item the now bar's flash may show in this profile: the newest one on the text grid, and
/// **nothing** in the pixel profile - where the stack is the presentation (design §5.25: "GUI: max
/// 3 stacked" / "TUI: a single-line flash in the now bar").
///
/// One rule, stated once, because two surfaces showing the same item is a duplicate (a copy would
/// appear in the row *and* as a card), and the row's hints (`Ctrl+K commands`) would be displaced
/// for the item's whole lifetime. The frame derives `NowBarInput::flash` from this answer, and `x`
/// (`model::dismissShownToast`) still answers the item the stack draws nearest the corner - the
/// same newest item this function would have named on the grid.
const model::Toast* nowBarFlashItem(const model::UiState& ui, bool textGrid);

/// The width the design gives a toast: 360 px at 100 % (`gu` x 45), i.e. the card the stack is
/// laid out with. A longer title grows the card rather than being cut.
inline constexpr float kToastWidthGu = 45.0f;

/// The cards of the stack, newest first, inside `area`.
///
/// The newest card sits at the bottom - a toast grows upward from the corner it belongs to, so a
/// new item never pushes the one the user is reading off the screen. `area` is the band the caller
/// allows (the pixel profile: the display, above the composer); the stack is right-aligned in it
/// and stops at the top edge instead of drawing outside. At most `kToastMaxGui` cards.
///
/// Pure, so a harness can assert the geometry with no window at all: the rect a card is drawn in
/// and the rect its controls answer at are the same numbers.
std::vector<ui::Rect> toastStackRects(const model::UiState& ui, const ui::Rect& area,
                                      const ui::Metrics& m);

/// Draw the stack and apply what its controls answered: a dismiss cross (or the `Dismiss` action a
/// persistent item carries) drops that item, in the frame of the press.
///
/// A no-op on the text grid: the grid's presentation is the now bar's one-line flash, and drawing
/// cards on a cell surface would be a second presentation of the same item.
void renderToastStack(model::UiState& ui, const ui::Rect& area, const ui::Metrics& m);

/// The card's spec: the severity's look, and the one action a persistent item offers (`Dismiss`).
/// Exposed for the tests and for any other presentation that wants the same anatomy; `toast()` is
/// still the only thing that draws it.
ui::ToastSpec toastSpecFor(const model::Toast& toast);

} // namespace llmfun::tui::views
