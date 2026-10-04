/// Ncurses/ImTui text backend behind the Backend seam.
/// Owns the terminal screen and the text-grid lifecycle previously inlined in
/// tui.cpp; the popen clipboard stays text-only (the GUI backend uses the GLFW
/// platform clipboard instead).
#include "tui_backend.h"

#include "imtui/imtui-impl-ncurses.h"
#include "imtui/imtui-impl-text.h"

#include "imgui/imgui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace llmfun::tui {

static bool isWayland() {
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    const char* session = std::getenv("XDG_SESSION_TYPE");
    return wayland != nullptr && wayland[0] != '\0' ||
           (session != nullptr && strcmp(session, "wayland") == 0);
}

static void SetClipboardText(void*, const char* text) {
    const char* cmd = isWayland() ? "wl-copy" : "xclip -selection clipboard -i";
    FILE* f = popen(cmd, "w");
    if (f) {
        fputs(text, f);
        pclose(f);
    }
}

static const char* GetClipboardText(void*) {
    static char buf[8192];
    const char* cmd = isWayland() ? "wl-paste -n" : "xclip -selection clipboard -o";
    FILE* f = popen(cmd, "r");
    if (f) {
        if (fread(buf, 1, sizeof(buf) - 1, f) <= 0) {
            return "";
        }
        buf[sizeof(buf) - 1] = '\0';
        pclose(f);
    }
    return buf;
}

namespace {

class NcursesBackend final : public Backend {
public:
    bool init(std::string& error) override {
        // mouseSupport=true, fps_active=60.0, fps_idle=3.0 (save CPU when idle)
        screen = ImTui_ImplNcurses_Init(true, 60.0f, 3.0f);
        if (!screen) {
            error = "Failed to initialize ncurses terminal";
            return false;
        }

        ImTui_ImplText_Init();

        ImGuiIO& io = ImGui::GetIO();
        io.GetClipboardTextFn = GetClipboardText;
        io.SetClipboardTextFn = SetClipboardText;

        return true;
    }

    void newFrame() override {
        ImTui_ImplNcurses_NewFrame();
        ImTui_ImplText_NewFrame();
        // fprintf(stderr, "[style@draw] NavCursor=%08x ScrollbarGrab=%08x Button=%08x\n",
        // ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_NavCursor]),
        // ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_ScrollbarGrab]),
        // ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_Button]));
        ImGui::NewFrame();
    }

    void renderFrame() override {
        ImGui::Render();
        ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), screen);
        ImTui_ImplNcurses_DrawScreen();
    }

    void shutdown() override {
        if (screen) {
            ImTui_ImplText_Shutdown();
            ImTui_ImplNcurses_Shutdown();
            screen = nullptr;
        }
    }

    BackendKind kind() const override { return BackendKind::Tui; }

    bool shouldClose() const override { return false; }

private:
    ImTui::TScreen* screen = nullptr;
};

} // namespace

std::unique_ptr<Backend> makeNcursesBackend() { return std::make_unique<NcursesBackend>(); }

} // namespace llmfun::tui
