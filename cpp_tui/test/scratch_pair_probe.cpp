// Does a [down,up] pair queued in ONE batch survive imgui 1.92 trickle+dedup?
#include "imgui.h"
#include <cstdio>
static void run(const char* name, bool pair, bool trickle) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigInputTrickleEventQueue = trickle;
    io.DisplaySize = ImVec2(80, 24);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
    unsigned char* px = nullptr;
    int fw = 0, fh = 0;
    io.Fonts->GetTexDataAsAlpha8(&px, &fw, &fh);
    printf("== %s (trickle=%d) ==\n", name, trickle);
    for (int frame = 0; frame < 6; ++frame) {
        if (frame == 0) {
            io.AddKeyEvent(ImGuiKey_Enter, true);
            if (pair)
                io.AddKeyEvent(ImGuiKey_Enter, false);
        }
        ImGui::NewFrame();
        printf("  frame %d: IsKeyPressed(Enter)=%d\n", frame, ImGui::IsKeyPressed(ImGuiKey_Enter));
        ImGui::EndFrame();
    }
    ImGui::DestroyContext();
}
int main() {
    run("pair [down,up] one batch", true, true);
    run("down only", false, true);
    run("pair, trickle OFF", true, false);
    return 0;
}
