/// Printable input (ui/input.h owns the contract).
#include "ui/input.h"

#include "imgui/imgui.h"

namespace llmfun::tui::ui {

bool takeCharacterAfterRelease(char lower) {
    ImGuiIO& io = ImGui::GetIO();
    const ImWchar wanted = static_cast<ImWchar>(static_cast<unsigned char>(lower));
    const ImWchar wantedUpper =
        (lower >= 'a' && lower <= 'z') ? static_cast<ImWchar>(wanted - 'a' + 'A') : wanted;
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar c = io.InputQueueCharacters[i];
        if (c != wanted && c != wantedUpper)
            continue;
        io.InputQueueCharacters.erase(io.InputQueueCharacters.Data + i);
        return true;
    }
    return false;
}

bool takeCharacter(char lower) {
    ImGuiIO& io = ImGui::GetIO();
    // A text field owns the keyboard while it edits (D-KEYMAP's plain-key rule).
    if (io.WantTextInput)
        return false;
    return takeCharacterAfterRelease(lower);
}

} // namespace llmfun::tui::ui
