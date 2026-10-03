/// Definition of renderButton for the shared TUI render widgets (tui_widgets.h).
#include "tui_widgets.h"

namespace llmfun::tui {

bool renderButton(const std::string& label, int width, bool active, ImVec4 colorActive) {
    bool result = false;

    ImGui::PushID(label.c_str());
    const auto p0 = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##but", ImVec2(width, 1))) {
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
    ImGui::SetCursorScreenPos(ImVec2(p0.x + ImGui::GetStyle().FramePadding.x, p0.y));
    ImGui::Text("%s", label.c_str());
    ImGui::PopStyleColor(npop);
    ImGui::PopID();

    return result;
}

} // namespace llmfun::tui
