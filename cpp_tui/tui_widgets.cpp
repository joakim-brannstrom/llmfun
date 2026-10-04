/// Definition of renderButton for the shared TUI render widgets (tui_widgets.h).
#include "tui_widgets.h"
#include "tui.h"

namespace llmfun::tui {

bool renderButton(const std::string& label, int width, bool active, ImVec4 colorActive) {
    bool result = false;

    ImGui::PushID(label.c_str());
    const auto p0 = ImGui::GetCursorScreenPos();
    // audit: pixel profile — the button height is one grid cell in the
    // text backend but a full frame height in the GUI: a 1 px tall hit box is
    // not mouse-usable. The overlaid label is vertically centered in the
    // button (the grid formula reduces to the historical label-at-p0).
    const float buttonH = tuiIsTextGrid() ? 1.0f : ImGui::GetFrameHeight();
    if (ImGui::Button("##but", ImVec2(static_cast<float>(width), buttonH))) {
        result = true;
    }

    int npop = 0;
    if (ImGui::IsItemHovered() || active) {
        ImGui::PushStyleColor(ImGuiCol_Text, colorActive);
        ++npop;
    }
    // Draw the label inset by FramePadding.x, like ImGui's own buttons. The
    // cell mapping places text on the cell containing its pen, so this is what
    // gives left-aligned labels their one-cell inset from the button edge
    // (and keeps callers' width = label + 2*FramePadding symmetric).
    const float labelY = p0.y + (buttonH - ImGui::GetTextLineHeight()) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + ImGui::GetStyle().FramePadding.x, labelY));
    ImGui::Text("%s", label.c_str());
    ImGui::PopStyleColor(npop);
    ImGui::PopID();

    return result;
}

} // namespace llmfun::tui
