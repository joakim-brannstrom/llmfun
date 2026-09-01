/// TUI frame-loop lifecycle and public state accessors.
/// Owns terminal init/shutdown, the render entry point, and the tui* accessors.
#include "tui.h"
#include "tui_chat.h"
#include "tui_common.h"

#include "imtui/imtui-impl-ncurses.h"
#include "imtui/imtui-impl-text.h"

#include "imgui/imgui_internal.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

void tuiAddOutputLine(TuiState& state, const ChatMessage& msg) {
    state.chat.outputLines.push_back(msg);
    if (state.chat.outputLines.size() > state.chat.MaxChatMessages) {
        state.chat.outputLines.pop_front();
    }
    tuiStreamChatMessageClear(state);
}

void tuiAddLogMessage(TuiState& state, const LogMessage& msg) {
    state.logMessages.push_back(msg);
    if (state.logMessages.size() > state.MaxLogMessages) {
        state.logMessages.pop_front();
    }
}

void tuiClearOutput(TuiState& state) { state.chat.outputLines.clear(); }

void tuiUpdateStreamChatMessage(TuiState& state, const ChatMessage& msg) {
    state.chat.streamMsg = msg;
    state.chat.hasStreamMsg = true;
}

void tuiStreamChatMessageClear(TuiState& state) {
    state.chat.streamMsg = ChatMessage{};
    state.chat.hasStreamMsg = false;
}

void tuiSetStatusText(TuiState& state, const std::string& text) { state.statusText = text; }

std::string tuiGetInput(const TuiState& state) { return state.userQuery.inputBuf; }

void tuiClearInput(TuiState& state) { state.userQuery.inputBuf.clear(); }

bool tuiIsSubmitReady(const TuiState& state) { return state.userQuery.submitReady; }

void tuiResetSubmit(TuiState& state) {
    state.userQuery.submitReady = false;
    state.userQuery.submitQuery.clear();
}

std::string tuiGetSubmitQuery(const TuiState& state) { return state.userQuery.submitQuery; }

void applyTheme() {
    // Start with StyleColorsDark as a consistent base for all ~35 color slots,
    // then override the specific colors that differ from the defaults.
    ImGui::StyleColorsDark();

    ImVec4* colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_Text] = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.06f, 0.06f, 0.06f, 0.00f);
    colors[ImGuiCol_Border] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.59f, 0.98f, 0.65f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.05f, 0.05f, 0.05f, 0.54f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.34f, 0.34f, 0.34f, 0.54f);
}

bool tuiInit(ImTui::TScreen** screen) {
    setlocale(LC_ALL, "");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    applyTheme();

    ImGui::GetIO().IniFilename = nullptr;

    // mouseSupport=true, fps_active=60.0, fps_idle=3.0 (save CPU when idle)
    *screen = ImTui_ImplNcurses_Init(true, 60.0f, 3.0f);
    if (!*screen) {
        std::fprintf(stderr, "Failed to initialize ncurses terminal. Aborting.\n");
        ImGui::DestroyContext();
        return false;
    }

    ImTui_ImplText_Init();

    ImGuiIO& io = ImGui::GetIO();
    io.GetClipboardTextFn = GetClipboardText;
    io.SetClipboardTextFn = SetClipboardText;

    return true;
}

void tuiShutdown(ImTui::TScreen* screen) {
    if (screen) {
        ImTui_ImplText_Shutdown();
        ImTui_ImplNcurses_Shutdown();
    }
    ImGui::DestroyContext();
}

void tuiNewFrame() {
    ImTui_ImplNcurses_NewFrame();
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
}

void tuiRenderFrame(ImTui::TScreen* screen) {
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), screen);
    ImTui_ImplNcurses_DrawScreen();
}

bool tuiRender(TuiState& state) {
    auto logFile = [&state]() {
        if (state.isLogActive)
            return fopen("llmfun_ui_log.txt", "a");
        return static_cast<FILE*>(nullptr);
    }();
    Log log{logFile};

    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImVec2 DisplaySize = ImGui::GetIO().DisplaySize;

    if (state.maxWidth > 0 && DisplaySize.x > state.maxWidth) {
        DisplaySize.x = static_cast<float>(state.maxWidth);
        ImGui::GetIO().DisplaySize = DisplaySize; // propagate to grid sizing
    }

    static constexpr float MIN_TERMINAL_WIDTH = 40.0f;
    static constexpr float MIN_TERMINAL_HEIGHT = 15.0f;

    if (DisplaySize.x < MIN_TERMINAL_WIDTH || DisplaySize.y < MIN_TERMINAL_HEIGHT) {
        ImGui::Begin("Error");
        ImGui::Text("Terminal too small! Minimum size: 40x15");
        ImGui::End();
        return true;
    }

    ImGuiIO& io = ImGui::GetIO();

    if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_C)))) {
        return false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End)) {
        state.autoScroll = true;
    }

    // Required: BeginChild calls must be nested inside a Begin/End block.
    // Without a parent window, BeginChild creates an implicit window whose
    // auto-positioning offsets the layout, making the TUI unusable.
    ImGuiWindowFlags parentFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_MenuBar;
    static bool noClose = true;
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(DisplaySize, ImGuiCond_Always);
    ImGui::Begin("##TuiRoot", &noClose, parentFlags);

    renderMainWindow(state, log);

    ImGui::End();

    if (logFile != nullptr)
        fclose(logFile);

    return true;
}

void tuiSetLogging(TuiState& state, bool onOff) { state.isLogActive = onOff; }

void tuiSetMaxWidth(TuiState& state, int maxWidth) { state.maxWidth = maxWidth; }

void tuiSetIniFilename(TuiState& state, const std::string& filename) {
    // TODO: this doesn't work. It ends up creating junk files.
    // state.iniFilename = filename;
    // ImGui::GetIO().IniFilename = state.iniFilename.c_str();
}

void tuiInitQueryHistory(TuiState& state, const std::vector<std::string>& history) {
    state.userQuery.inputHistory = history;
    state.userQuery.historyPos = -1;
}

} // namespace llmfun::tui
