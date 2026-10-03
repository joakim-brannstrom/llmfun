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

void tuiClearOutput(TuiState& state) {
    state.chat.outputLines.clear();
    state.chat.outputLineOpen.clear();
    tuiStreamChatMessageClear(state);
}

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
    // ImGui draws the nav cursor (RenderNavCursor) on the focused widget using
    // ImGuiCol_NavCursor; with the 1px font the InputText caret and the
    // scrollbar grab both route through that slot, rendering as the blue
    // default (0.26,0.59,0.98) instead of the themed Text/ScrollbarGrab
    // colors. Route NavCursor to the scrollbar grab color so the grab is
    // visible and the caret matches the text color.
    colors[ImGuiCol_NavCursor] = colors[ImGuiCol_ScrollbarGrab];
    // The ImGui default Button blue (0.26,0.59,0.98,0.40) alpha-multiplies to
    // (26,60,100) which quantizes to ANSI 60 - the same index as the plain row
    // background - so 1-row-tall buttons (title tabs, Send, Prev, Next) render
    // invisible. Use the themed grab gray (quantizes to 236, distinct from the
    // surrounding 60/102/235 backgrounds) for all three button states.
    colors[ImGuiCol_Button] = colors[ImGuiCol_ScrollbarGrab];
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.42f, 0.42f, 0.42f, 0.60f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.50f, 0.50f, 0.50f, 0.65f);
    // Scrollbars: the base theme's colors are semi-transparent near-blacks
    // (bg 0.05 @ 0.54 alpha, grab 0.34 @ 0.54) which blend into the black
    // background, so the chat log's vertical scrollbar was invisible on
    // screen. Use solid greys that stay visible on black; assigned AFTER the
    // Button/NavCursor aliases above so those keep their dimmer look.
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.55f, 0.55f, 0.55f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.65f, 0.65f, 0.65f, 1.00f);
    // One cell is a dot on a tall track; three cells read as a thumb.
    ImGui::GetStyle().GrabMinSize = 3.0f;
    // fprintf(stderr, "[style] NavCursor=%08x ScrollbarGrab=%08x\n",
    // ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_NavCursor]),
    // ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_ScrollbarGrab]));
}

bool tuiInit(ImTui::TScreen** screen) {
    setlocale(LC_ALL, "");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGui::GetIO().IniFilename = nullptr;

    // mouseSupport=true, fps_active=60.0, fps_idle=3.0 (save CPU when idle)
    *screen = ImTui_ImplNcurses_Init(true, 60.0f, 3.0f);
    if (!*screen) {
        std::fprintf(stderr, "Failed to initialize ncurses terminal. Aborting.\n");
        ImGui::DestroyContext();
        return false;
    }

    ImTui_ImplText_Init();

    // Apply the theme AFTER the backend inits: ImTui_ImplText_Init() resets
    // several style colors (notably Colors[ImGuiCol_NavHighlight] = (0,0,0,0),
    // which aliases ImGuiCol_NavCursor in ImGui 1.91.4+), so applying the
    // theme before the backends lets the backend undo the NavCursor override.
    applyTheme();

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
    // fprintf(stderr, "[style@draw] NavCursor=%08x ScrollbarGrab=%08x Button=%08x\n",
    // ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_NavCursor]),
    // ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_ScrollbarGrab]),
    // ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_Button]));
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

    if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_C))) {
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
    // The root window is an absolute-positioned terminal grid; it must never
    // scroll. On 1.92, content that ends exactly at the inner bottom edge
    // leaves one row of scrollable range, and something during Begin/nav
    // scrolls the window by that 1 row on every frame after the first
    // (observed: GetScrollY() == 1 from frame 2 on), shifting every
    // SetCursorPos-based position up one row. SetScrollY(0) below re-asserts
    // each frame after Begin's scroll calc, so it wins.
    ImGui::SetScrollY(0.0f);

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
