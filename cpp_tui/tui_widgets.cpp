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
    ImGui::SetCursorScreenPos(p0);
    ImGui::Text("%s", label.c_str());
    ImGui::PopStyleColor(npop);
    ImGui::PopID();

    return result;
}

} // namespace llmfun::tui
