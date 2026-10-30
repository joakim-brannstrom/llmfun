/// The feedback presentation (views/feedback.h owns the contract).
#include "views/feedback.h"

#include <algorithm>
#include <cstdio>

#include "imgui/imgui_internal.h"
#include "model/ui_state.h"
#include "ui/primitives.h"

namespace llmfun::tui::views {
namespace {

/// The one action a card can carry: a persistent item's way out that a pointer can see. The
/// design's limit is "at most one action button each", so a warn/error spends it on the answer
/// the item is waiting for, and a transient card - which goes away by itself - spends nothing.
constexpr char kToastDismissLabel[] = "Dismiss";

/// The gap between two stacked cards: one `pad1`, the same gutter the tiling uses between
/// regions.
float toastGap(const ui::Metrics& m) { return m.pad1; }

} // namespace

FeedbackLook feedbackLook(model::ToastKind kind) {
    switch (kind) {
    case model::ToastKind::Info:
        return FeedbackLook{ui::Icon::Info, ui::ColorToken::textMid};
    case model::ToastKind::Success:
        return FeedbackLook{ui::Icon::Ok, ui::ColorToken::stateOk};
    case model::ToastKind::Warn:
        return FeedbackLook{ui::Icon::Warn, ui::ColorToken::stateWarn};
    case model::ToastKind::Error:
        return FeedbackLook{ui::Icon::Error, ui::ColorToken::stateDanger};
    }
    return FeedbackLook{};
}

const model::Toast* nowBarFlashItem(const model::UiState& ui, bool textGrid) {
    return textGrid ? model::topToast(ui) : nullptr;
}

ui::ToastSpec toastSpecFor(const model::Toast& toast) {
    const FeedbackLook look = feedbackLook(toast.kind);
    ui::ToastSpec spec;
    spec.title = toast.title.c_str();
    spec.detail = toast.detail.c_str();
    spec.action = model::toastKindPersists(toast.kind) ? kToastDismissLabel : "";
    spec.mark = look.mark;
    // The severity owns the mark, the rule and nothing else: a card's words are the readable ink
    // (a warning whose text was amber would be a card nobody can read at 16 colours).
    spec.titleInk = ui::ColorToken::textHi;
    spec.detailInk = ui::ColorToken::textMid;
    spec.rule = look.ink;
    return spec;
}

std::vector<ui::Rect> toastStackRects(const model::UiState& ui, const ui::Rect& area,
                                      const ui::Metrics& m) {
    std::vector<ui::Rect> rects;
    if (m.textGrid || area.w <= 0.0f || area.h <= 0.0f)
        return rects;
    const std::size_t shown = std::min(ui.toasts.size(), model::kToastMaxGui);
    const float wantW = kToastWidthGu * m.gu;
    float bottom = area.bottom();
    for (std::size_t i = 0; i < shown; ++i) {
        // `[0]` is the newest: the stack reads upward from the corner, so the newest card is the
        // one nearest it.
        const model::Toast& item = ui.toasts[ui.toasts.size() - 1 - i];
        const ui::ToastSpec spec = toastSpecFor(item);
        const ui::Size size = ui::measureToast(m, spec, ui::Size{wantW, 0.0f});
        if (size.h > bottom - area.y)
            break; // no room left above: the older cards stay off screen, never half drawn
        ui::Rect card;
        // A card that measured wider than the area (a long path, a narrow window at D-GEOM's floor)
        // is clamped to the area instead of sliding out of it: `ui::toast()` clips its strings to
        // the rect it is handed, so the card stays inside and its text is cut - the alternative is
        // a card that starts left of the display and runs under the rail.
        card.w = std::min(size.w, area.w);
        card.h = size.h;
        card.x = area.right() - card.w;
        card.y = bottom - card.h;
        card.cells = false;
        rects.push_back(card);
        bottom = card.y - toastGap(m);
    }
    return rects;
}

void renderToastStack(model::UiState& ui, const ui::Rect& area, const ui::Metrics& m) {
    // The grid's presentation is the now bar's flash (one line), so the stack exists in the pixel
    // profile only. Fail closed rather than draw a second copy of the same item.
    if (m.textGrid)
        return;
    const std::vector<ui::Rect> rects = toastStackRects(ui, area, m);
    if (rects.empty())
        return;
    // The click answers are collected and applied after the loop: dropping an item mid-loop would
    // invalidate the index the next card's rect was measured from.
    std::size_t dismissAt = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < rects.size(); ++i) {
        const std::size_t index = ui.toasts.size() - 1 - i;
        const ui::Rect& card = rects[i];
        // One window per card, positioned at the card and nothing bigger. Two reasons, both
        // measured elsewhere in this tree: a primitive drawn on a *parent* window's list is painted
        // under the thread's child window whatever order the calls run in (the task-18 lesson), so
        // chrome above the tiling has to be its own window; and a window is the smallest scope that
        // can own a click, so the `pad1` between two cards stays the app's - a single window over
        // the whole stack would swallow presses in the gaps.
        char name[32];
        std::snprintf(name, sizeof name, "##toast_stack_%zu", i);
        ImGui::SetNextWindowPos(ImVec2(card.x, card.y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(card.w, card.h), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        // No background: the card paints its own surface, and a window fill would be a second one.
        // No nav focus: a toast is never where the keyboard should go.
        //
        // *Not* `NoBringToFrontOnFocus`: ImGui pushes a window created with that flag to the back
        // of its display order (`CreateNewWindow`), i.e. behind the shell window it exists to cover
        // - measured here, the cards were drawn every frame and nothing reached the framebuffer
        // until the flag was dropped.
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_NoFocusOnAppearing;
        ImGui::Begin(name, nullptr, flags);
        // The style variables are captured by `Begin`; restore them for whatever draws next (the
        // palette's rule).
        ImGui::PopStyleVar(3);
        // Display order is paint order, and a window created later still lands under the shell's
        // children, which the root re-focuses on its own frame (the palette's note, measured there
        // and again here: without this every card was built and drawn and the framebuffer stayed
        // canvas). Bringing each card to the front is what makes the stack visible, and it costs no
        // focus - `NoNavFocus`/`NoFocusOnAppearing` keep the keyboard where it is.
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        const ui::ToastSpec spec = toastSpecFor(ui.toasts[index]);
        ui::toast(spec, m, card);
        // A card's action *is* an answer: a persistent item's `Dismiss`, or whatever a later
        // producer offers, drops the item in the frame it was pressed.
        if (ui::toastDismissPressed(m, card) || ui::toastActionPressed(spec, m, card))
            dismissAt = index;
        ImGui::End();
    }
    if (dismissAt != static_cast<std::size_t>(-1))
        model::dismissToastAt(ui, dismissAt);
}

} // namespace llmfun::tui::views
