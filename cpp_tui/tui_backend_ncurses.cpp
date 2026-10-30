/// Ncurses/ImTui text backend behind the Backend seam.
/// Owns the terminal screen and the text-grid lifecycle previously inlined in
/// tui.cpp; the popen clipboard stays text-only (the GUI backend uses the GLFW
/// platform clipboard instead).
#include "tui_backend.h"

#include "imtui/imtui-impl-ncurses.h"
#include "imtui/imtui-impl-text.h"

#include "imgui/imgui.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <termios.h>
#include <unistd.h>

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
    static std::array<char, 8192> buf{};
    const char* cmd = isWayland() ? "wl-paste -n" : "xclip -selection clipboard -o";
    FILE* f = popen(cmd, "r");
    if (f) {
        if (fread(buf.data(), 1, buf.size() - 1, f) <= 0) {
            return "";
        }
        buf[buf.size() - 1] = '\0';
        pclose(f);
    }
    return buf.data();
}

namespace {

class NcursesBackend final : public Backend {
public:
    bool init(std::string& error) override {
        // Snapshot the terminal *before* ncurses touches it: shutdown() restores
        // this struct, and `ImTui_ImplNcurses_Init()` calls
        // initscr()/cbreak()/noecho() (vendor/imtui/src/imtui-impl-ncurses.cpp),
        // so a snapshot taken after it would capture ncurses' own modes.
        hasSavedTermios = tcgetattr(STDIN_FILENO, &savedTermios) == 0;

        // mouseSupport=true, fps_active=60.0, fps_idle=3.0 (save CPU when idle)
        screen = ImTui_ImplNcurses_Init(true, 60.0f, 3.0f);
        if (!screen) {
            error = "Failed to initialize ncurses terminal";
            return false;
        }

        ImTui_ImplText_Init();

        // Ctrl+Q is the quit chord, and the line discipline was eating it:
        // `cbreak()` clears ICANON/ECHO but leaves IXON on, so the terminal's
        // flow control answered the XON byte (0x11) itself and ncurses never saw
        // a key (plan/key_gate.md §4/F3: `Ctrl+Q` -> nothing at all). Clear it on
        // the *current* modes - the saved struct above is the shell's and still
        // has ICANON/ECHO on, so writing it back here would undo cbreak() - and
        // put the saved settings back on shutdown. Ctrl+C is unaffected: ISIG
        // stays on, so it remains the terminal's SIGINT (design §6.7, C-07).
        struct termios current{};
        if (tcgetattr(STDIN_FILENO, &current) == 0) {
            current.c_iflag &= ~static_cast<tcflag_t>(IXON);
            if (tcsetattr(STDIN_FILENO, TCSANOW, &current) == 0)
                ixonCleared = true;
        }

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
            // Restore the struct captured before ncurses touched the terminal.
            // endwin() restores the *shell* modes it saved itself; putting back
            // exactly what we changed is the property the caller of a UI should be
            // able to rely on, whatever ncurses decides to restore on its own.
            if (hasSavedTermios && ixonCleared) {
                tcsetattr(STDIN_FILENO, TCSANOW, &savedTermios);
                ixonCleared = false;
            }
        }
    }

    BackendKind kind() const override { return BackendKind::Tui; }

    bool shouldClose() const override { return false; }

private:
    ImTui::TScreen* screen = nullptr;
    /// Terminal settings captured before `ImTui_ImplNcurses_Init()` touched the
    /// tty; restored in shutdown() when `ixonCleared`.
    struct termios savedTermios{};
    bool hasSavedTermios = false;
    bool ixonCleared = false;
};

} // namespace

std::unique_ptr<Backend> makeNcursesBackend() { return std::make_unique<NcursesBackend>(); }

} // namespace llmfun::tui
