/// TUI core: the widget render entry point and the public state accessors.
/// Backend lifecycle (init/frames/shutdown) is driven by the C API layer and
/// owned per state (TuiState::backend); this file renders the widgets.
#include "tui.h"
#include "tui_chat.h"
#include "tui_common.h"

#include "imgui/imgui_internal.h"

#include <cstdio>
#include <string>

namespace llmfun::tui {

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
    // audit: gated - the cell-tuned grab is a text-grid metric; the
    // GUI keeps imgui's default (12.0) so its scrollbar thumb is not clamped
    // to a 3 px length. Caveat: harnesses that apply the theme before
    // ImTui_ImplText_Init() (applyTheme-then-init order, e.g.
    // test_tui_maxwidth's harnessInit) run this line while the guard is still
    // false and keep the imtui 1.0 value instead; no harness asserts the grab
    // metric.
    if (tuiIsTextGrid())
        ImGui::GetStyle().GrabMinSize = 3.0f;
    // fprintf(stderr, "[style] NavCursor=%08x ScrollbarGrab=%08x\n",
    // ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_NavCursor]),
    // ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_ScrollbarGrab]));
}

bool tuiIsTextGrid() { return ImTui_TextEncodingActive; }

bool tuiRender(TuiState& state) {
    auto logFile = [&state]() {
        if (state.isLogActive)
            return fopen("llmfun_ui_log.txt", "a");
        return static_cast<FILE*>(nullptr);
    }();
    Log log{logFile};

    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImVec2 DisplaySize = ImGui::GetIO().DisplaySize;

    // audit: gated - maxWidth is a terminal-column cap (D validates
    // 40..10000); the GUI's DisplaySize is in pixels, so applying the clamp
    // would read a column value as pixels and truncate the window to a
    // sliver. The text backend keeps the exact clamp.
    if (tuiIsTextGrid() && state.maxWidth > 0 && DisplaySize.x > state.maxWidth) {
        DisplaySize.x = static_cast<float>(state.maxWidth);
        ImGui::GetIO().DisplaySize = DisplaySize; // propagate to grid sizing
    }

    static constexpr float MIN_TERMINAL_WIDTH = 40.0f;
    static constexpr float MIN_TERMINAL_HEIGHT = 15.0f;

    // audit: gated - 40x15 is the text grid's minimum cell area; a
    // 40x15-pixel window is not "too small" for the GUI, and blanking the UI
    // to an error page when the user shrinks the window would be wrong.
    if (tuiIsTextGrid() &&
        (DisplaySize.x < MIN_TERMINAL_WIDTH || DisplaySize.y < MIN_TERMINAL_HEIGHT)) {
        ImGui::Begin("Error");
        ImGui::Text("Terminal too small! Minimum size: 40x15");
        ImGui::End();
        return true;
    }

    ImGuiIO& io = ImGui::GetIO();

    // audit: accepted for BOTH modes - Ctrl+C exits the app in the
    // GUI too (terminal parity: the key is documented as quit). GUI copy is
    // unaffected: it goes through the [c] buttons / markdown links calling
    // ImGui::SetClipboardText (platform clipboard), and Ctrl+V paste is
    // handled by the input widget, not by this binding.
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
    // audit: pixel profile — the grid intentionally draws no root
    // background (the terminal's own background shows through), while the GUI
    // has a real framebuffer and the backend never clears it (no glClear):
    // without a background, pixels the children do not repaint keep stale
    // frame content (visible after a resize or an early-frame layout change).
    // Let the root window paint the themed background in the GUI only.
    if (!tuiIsTextGrid())
        parentFlags &= ~ImGuiWindowFlags_NoBackground;
    static bool noClose = true;
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(DisplaySize, ImGuiCond_Always);
    // The root window is an absolute-positioned layout; it must never scroll.
    // Two ways it otherwise can: the content overshoots the bottom edge (27 px
    // of scrollable range in the pixel profile), and a nav focus request made
    // while a child window is Appearing (the input row at startup) asks for a
    // centered scroll. SetNextWindowScroll() pins the scroll to 0 inside this
    // same Begin — unlike SetScrollY(0), whose target the *next* Begin applies
    // (1.92 semantics), so the startup request used to leave one frame
    // scrolled and the whole layout slid up before settling.
    // audit: accepted for BOTH modes — the absolute SetCursorPos
    // layout depends on the root window never scrolling, and the pixel
    // profile wants the same invariant (a stale scroll offset would slide
    // the whole UI); gating this would risk the drift described above.
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
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

bool tuiHasMoreEvents() {
    auto ctx = ImGui::GetCurrentContext();
    return ctx != nullptr && ctx->InputEventsTrail.Size > 0;
}

} // namespace llmfun::tui
