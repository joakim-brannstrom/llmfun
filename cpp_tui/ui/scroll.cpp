/// Follow-the-newest (ui/scroll.h owns the contract and the three rules).
#include "scroll.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

namespace llmfun::tui::ui {

bool pointerTookOverScroll() {
    // The scrollbar owns ImGui's ActiveId while it is dragged, and the default
    // IsWindowHovered() answers false when any item is active. Without this flag the
    // follower believed the pointer was not over the scrollbar, re-pinned the view to
    // the bottom and made a scrollbar drag impossible while following.
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        return false;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)
        return true;
    const ImRect scrollbar = ImGui::GetWindowScrollbarRect(ImGui::GetCurrentWindow(), ImGuiAxis_Y);
    // The scrollbar sits outside the child's content ClipRect, so the default
    // IsMouseHoveringRect() clip would reject every point on it. The rect is already
    // the window's own scrollbar, so it needs no content clip.
    return ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
           ImGui::IsMouseHoveringRect(scrollbar.Min, scrollbar.Max, /*clip=*/false);
}

bool followNewest(bool follow) {
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    if (pointerTookOverScroll())
        return atBottom;

    if (!follow)
        return false;

    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float contentH = window->ContentSize.y + 2.0f * window->WindowPadding.y;
    const bool contentBelow = contentH > window->ScrollMax.y + window->InnerRect.GetHeight() + 0.5f;
    if (contentBelow || !atBottom)
        ImGui::SetScrollHereY(1.0f);
    return true;
}

} // namespace llmfun::tui::ui
