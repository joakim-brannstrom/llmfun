/// The feedback queue's rules (design §5.25, task 45) - `model/toast.h` owns the contract.
#include "model/toast.h"

#include <algorithm>

#include "model/ui_state.h"

namespace llmfun::tui::model {
namespace {

/// The pixel profile's two lifetimes (design §5.25: info/success 4 s, warnings 8 s). The task's
/// table moves the 8 s to `success` and makes both warning kinds persist, which is what the code
/// implements - the design's own "errors persist until dismissed" is extended to warnings because
/// a warning about a failed write is exactly as unanswered as an error.
constexpr std::int64_t kInfoMs = 4000;
constexpr std::int64_t kSuccessMs = 8000;

/// The grid's one number: the now bar's flash is 2 s for both transient kinds. A row that kept a
/// copy answer for 8 s would be a line the user reads long after they stopped caring.
constexpr std::int64_t kFlashMs = 2000;

} // namespace

std::int64_t toastLifetimeMs(ToastKind kind, bool textGrid) {
    switch (kind) {
    case ToastKind::Info:
        return textGrid ? kFlashMs : kInfoMs;
    case ToastKind::Success:
        return textGrid ? kFlashMs : kSuccessMs;
    case ToastKind::Warn:
    case ToastKind::Error:
        return 0; // persist until acknowledged, in both profiles
    }
    return 0;
}

std::size_t toastCapacity(bool textGrid) { return textGrid ? kToastMaxGrid : kToastMaxGui; }

void raiseToast(UiState& ui, Toast toast, std::int64_t nowMs, bool textGrid) {
    std::deque<Toast>& queue = ui.toasts;
    const std::int64_t lifetime = toastLifetimeMs(toast.kind, textGrid);
    toast.expiresAtMs = lifetime > 0 ? nowMs + lifetime : 0;

    // A repeat of the newest item is the same answer, refreshed. Two presses of one control must
    // not stack two identical cards, and on the grid they cannot even be told apart.
    if (!queue.empty()) {
        Toast& newest = queue.back();
        if (newest.kind == toast.kind && newest.title == toast.title &&
            newest.detail == toast.detail) {
            newest.expiresAtMs = toast.expiresAtMs;
            return;
        }
    }

    const std::size_t cap = std::max<std::size_t>(toastCapacity(textGrid), 1);
    if (queue.size() >= cap) {
        // Make room by evicting the oldest item that may go. A transient never pushes out an
        // unacknowledged warning: on the grid the incoming item is then dropped instead, because
        // the row holds one line and the warning has not been answered.
        const auto victim = std::find_if(queue.begin(), queue.end(), [](const Toast& item) {
            return !toastKindPersists(item.kind);
        });
        if (victim != queue.end())
            queue.erase(victim);
        else if (toastKindPersists(toast.kind))
            queue.erase(queue.begin());
        else
            return;
    }
    queue.push_back(std::move(toast));
}

void expireToasts(UiState& ui, std::int64_t nowMs) {
    std::deque<Toast>& queue = ui.toasts;
    queue.erase(std::remove_if(queue.begin(), queue.end(),
                               [nowMs](const Toast& item) {
                                   return item.expiresAtMs != 0 && nowMs >= item.expiresAtMs;
                               }),
                queue.end());
}

bool dismissShownToast(UiState& ui) {
    if (ui.toasts.empty())
        return false;
    ui.toasts.pop_back(); // the newest: the item `topToast()` names
    return true;
}

bool dismissToastAt(UiState& ui, std::size_t index) {
    if (index >= ui.toasts.size())
        return false;
    ui.toasts.erase(ui.toasts.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

const Toast* topToast(const UiState& ui) { return ui.toasts.empty() ? nullptr : &ui.toasts.back(); }

} // namespace llmfun::tui::model
