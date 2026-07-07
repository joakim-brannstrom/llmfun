#include "tui.h"

#include "imtui/imtui-impl-ncurses.h"
#include "imtui/imtui-impl-text.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>

namespace llmfun::tui {

// Named key codes for Ctrl shortcuts (ncurses raw key codes)
static constexpr int KEY_CTRL_D = 4;    // Ctrl+D exit
static constexpr int KEY_CTRL_1 = 0x11; // Ctrl+1
static constexpr int KEY_CTRL_2 = 0x12; // Ctrl+2
static constexpr int KEY_CTRL_3 = 0x13; // Ctrl+3

struct Log {
    FILE* logFile;

    template <typename... Args> void operator()(std::string format, Args&&... args) {
        if (logFile != nullptr) {
            std::fprintf(logFile, format.c_str(), std::forward<Args>(args)...);
        }
    }
};

bool isWayland() {
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

bool isWhitespaceOnly(const std::string& s) {
    if (s.empty())
        return true;

    bool allTrue = true;
    for (auto c : s) {
        allTrue = allTrue && (std::isspace(c) || c == '\0');
    }
    return allTrue;
}

size_t countNewLines(const std::string& str) { return std::count(str.begin(), str.end(), '\n'); }

struct StyleColorGuard {
private:
    int count;

public:
    explicit StyleColorGuard(int n) : count(n) {}
    ~StyleColorGuard() { pop(); }

    void pop() {
        for (int i = 0; i < count; ++i) {
            ImGui::PopStyleColor();
        }
        count = 0;
    }

    StyleColorGuard(const StyleColorGuard&) = delete;
    StyleColorGuard& operator=(const StyleColorGuard&) = delete;
    StyleColorGuard(StyleColorGuard&&) = delete;
    StyleColorGuard& operator=(StyleColorGuard&&) = delete;
};

// Number of ImGui style colors pushed per header
static constexpr int HEADER_COLOR_COUNT = 4;

struct HeaderColors {
    ImVec4 base;
    ImVec4 hovered;
    ImVec4 active;
};

static HeaderColors getHeaderColors(ChatMessageType type, const ChatMessageStyle& style) {
    switch (type) {
    case ChatMessageType::User:
        return {style.userColor, style.userColorHover, style.userColorActive};
    case ChatMessageType::Assistant:
        return {style.assistantColor, style.assistantColorHover, style.assistantColorActive};
    case ChatMessageType::ToolCall:
        return {style.toolCallColor, style.toolCallColorHover, style.toolCallColorActive};
    case ChatMessageType::ToolResponse:
        return {style.toolResponseColor, style.toolResponseColorHover,
                style.toolResponseColorActive};
    default:
        // Neutral white fallback for unknown types
        return {ImVec4(0.90f, 0.90f, 0.90f, 1.00f), ImVec4(0.95f, 0.95f, 0.95f, 1.00f),
                ImVec4(1.00f, 1.00f, 1.00f, 1.00f)};
    }
}

void tuiAddLogMessage(TuiState& state, const LogMessage& msg) {
    state.logMessages.push_back(msg);
    if (state.logMessages.size() > state.MaxLogMessages) {
        state.logMessages.pop_front();
    }
}

void tuiAddOutputLine(TuiState& state, const ChatMessage& msg) {
    state.outputLines.push_back(msg);
    if (state.outputLines.size() > state.MaxChatMessages) {
        state.outputLines.pop_front();
    }
}

void tuiClearOutput(TuiState& state) { state.outputLines.clear(); }

void tuiSetStatusText(TuiState& state, const std::string& text) { state.statusText = text; }

std::string tuiGetInput(const TuiState& state) { return state.userQuery.inputBuf; }

void tuiClearInput(TuiState& state) { state.userQuery.inputBuf.clear(); }

bool tuiIsSubmitReady(const TuiState& state) { return state.userQuery.submitReady; }

void tuiResetSubmit(TuiState& state) {
    state.userQuery.submitReady = false;
    state.userQuery.submitQuery.clear();
}

std::string tuiGetSubmitQuery(const TuiState& state) { return state.userQuery.submitQuery; }

void ColoredSeparator(ImU32 color, float thickness = 1.0f, float spacing = 4.0f) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 start = ImGui::GetCursorScreenPos();
    ImVec2 end = ImVec2(start.x + ImGui::GetContentRegionAvail().x, start.y);
    draw_list->AddLine(start, end, color, thickness);
    ImGui::Dummy(ImVec2(0, spacing));
}

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
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    applyTheme();

    ImGui::GetIO().IniFilename = nullptr;

    // mouseSupport=true, fps_active=60.0, fps_idle=3.0 (save CPU when idle)
    *screen = ImTui_ImplNcurses_Init(true, 60.0f, 3.0f);
    if (!*screen) {
        std::fprintf(stderr, "Failed to initialize ncurses terminal. Aborting.\n");
        ImGui::DestroyContext(); // clean up context on failure
        return false;
    }

    ImTui_ImplText_Init();

    // In your Init function:
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

static void renderSeparator(const char* prefix, const char* ch, const char* suffix, int n,
                            bool disabled) {
    static char buf[256];
    if (n > 255)
        n = 255;
    if (n < 0)
        n = 0;
    for (int i = 0; i < n; i++) {
        buf[i] = ch[0];
    }
    buf[n] = 0;
    if (disabled) {
        ImGui::TextDisabled("%s%s%s", prefix, buf, suffix);
    } else {
        ImGui::Text("%s%s%s", prefix, buf, suffix);
    }
}

int InputResizeCallback(ImGuiInputTextCallbackData* data) {
    auto* udata = reinterpret_cast<std::string*>(data->UserData);

    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        udata->resize(data->BufSize);
        data->Buf = udata->data();
    }

    return 0;
}

void renderTabChat(TuiState& state, Log& log) {
    auto io = ImGui::GetIO();
    ImVec2 DisplaySize = io.DisplaySize;
    const auto inputBufLines =
        std::min(20, std::max(2, static_cast<int>(countNewLines(state.userQuery.inputBuf))));

    bool focusInput{false};

    auto outputArea = [&state, &log, &inputBufLines, &DisplaySize, &focusInput]() {
        // Clamp height to avoid negative values on very small terminals
        ImVec2 outPos(0, 1.0f);
        ImVec2 outSize(DisplaySize.x, std::max(1.0f, DisplaySize.y - 3 - inputBufLines));
        ImGui::SetCursorPos(outPos);
        ImGuiWindowFlags outFlags = ImGuiWindowFlags_HorizontalScrollbar;

        ImGui::BeginChild("llm_output", outSize, false, outFlags);

        const bool lastMsgIsTool = [&state]() {
            if (state.outputLines.size() > 0) {
                const auto& lastEntry = state.outputLines.back();
                return (lastEntry.type == ChatMessageType::ToolCall ||
                        lastEntry.type == ChatMessageType::ToolResponse);
            }
            return false;
        }();

        for (size_t i = 0; i < state.outputLines.size(); ++i) {
            // DefaultOpen if within last 10 messages
            const bool isRecent =
                (static_cast<int>(i) >= static_cast<int>(state.outputLines.size()) - 10);
            const auto flags = isRecent ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;

            const auto& entry = state.outputLines[i];
            auto s = entry.summary;
            s.append("##");
            s.append(std::to_string(i));

            const auto& colors = getHeaderColors(entry.type, state.chatStyle);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, colors.base);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, colors.hovered);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, colors.active);
            StyleColorGuard guard{HEADER_COLOR_COUNT};
            if (ImGui::CollapsingHeader(s.c_str(), flags)) {
                guard.pop();
                ImGui::PushTextWrapPos(DisplaySize.x - 4);
                ImGui::TextUnformatted(entry.text.c_str());
                ImGui::PopTextWrapPos();

                // Render thinking as nested collapsible (closed by default)
                // Open by default only if this is the last message AND it's a tool message
                if (!entry.thinking.empty()) {
                    std::string thinkId = "Model reasoning ##" + std::to_string(i) + "_thinking";
                    bool isLastMsg = (i == state.outputLines.size() - 1);
                    auto thinkFlags = (isLastMsg && lastMsgIsTool) ? ImGuiTreeNodeFlags_DefaultOpen
                                                                   : ImGuiTreeNodeFlags_None;

                    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.2f, 0.25f, 0.8f));
                    StyleColorGuard thinkGuard{1};
                    const bool thinkOpened = ImGui::CollapsingHeader(thinkId.c_str(), thinkFlags);
                    if (thinkOpened) {
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextUnformatted(entry.thinking.c_str());
                        ImGui::PopTextWrapPos();
                    }
                }

                // Copy button for main content
                std::string buttonId = " [c] ##" + std::to_string(i);
                if (ImGui::Button(buttonId.c_str())) {
                    ImGui::SetClipboardText(entry.text.c_str());
                    ImGui::SetTooltip("Copied");
                } else if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Copy to clipboard");
                    ImGui::EndTooltip();
                }
            }
        }

        if (!state.readyStatus) {
            static std::string fullIndicator{"...."};
            std::string indicator =
                fullIndicator.substr(state.busyIndicatorState % fullIndicator.size());
            ImGui::Text("%s%s", "Processing query", indicator.c_str());
            if (std::chrono::system_clock::now() > state.nextIndicatorIncr) {
                state.busyIndicatorState++;
                state.nextIndicatorIncr =
                    std::chrono::system_clock::now() + std::chrono::seconds{1};
            }
        }

        if (state.autoScroll) {
            ImGui::SetScrollHereY(1.0f);
        }

        // Auto-scroll detection: capture scroll state BEFORE EndChild so we
        // read the "output" child's actual scroll values.
        const float scrollY = ImGui::GetScrollY();
        const float scrollMax = ImGui::GetScrollMaxY();
        if (scrollMax > 0.0f) {
            state.autoScroll = (scrollY >= scrollMax - 1.0f);
        }

        static std::string dummyBuf{" "};
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImGui::InputText("##llm_output_tab_focus", dummyBuf.data(), dummyBuf.size());
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemActive()) {
            focusInput = true;
        }

        ImGui::EndChild();
    };

    auto inputHistory = [&state = state.userQuery, &log, &io](bool isPageDown, bool isPageUp) {
        if (state.inputHistory.empty()) {
            return;
        }

        // this code do not work until ImGui::ImGui::ClearActiveID() is available
        // if (!ImGui::IsItemActive() || state.inputHistory.empty()) {
        //     return;
        // }
        // auto& io = ImGui::GetIO();
        // const bool isPageUp = ImGui::IsItemActive() && io.KeysDown[io.KeyMap[ImGuiKey_PageUp]];
        // const bool isPageDown = ImGui::IsItemActive() &&
        // io.KeysDown[io.KeyMap[ImGuiKey_PageDown]];
        if (!(isPageUp || isPageDown)) {
            return;
        }

        bool setInput = false;
        if (isPageUp) {
            if (state.historyPos == -1) {
                // First press: save draft and push current input to history.
                state.draftBuf = state.inputBuf;
            }
            state.historyPos =
                std::min(state.historyPos + 1, static_cast<int>(state.inputHistory.size()) - 1);
            if (state.historyPos >= 0 && state.historyPos < state.inputHistory.size()) {
                setInput = true;
            }
        } else if (isPageDown && state.historyPos >= 0) {
            state.historyPos--;
            if (state.historyPos >= 0 && state.historyPos < state.inputHistory.size()) {
                setInput = true;
            } else {
                state.newInputBufString = state.draftBuf;
                state.draftBuf.clear();
            }
        }
        if (setInput) {
            state.newInputBufString =
                state.inputHistory[state.inputHistory.size() - state.historyPos - 1];
        }

        if (state.inputHistory.size() > state.MAX_HISTORY) {
            state.inputHistory.erase(state.inputHistory.begin());
        }
    };

    auto inputArea = [&state, &inputBufLines, &DisplaySize, &inputHistory, &log, &focusInput,
                      &io]() {
        // Estimate "button" width (text + padding)
        float buttonWidth =
            ImGui::CalcTextSize("Send   ").x + ImGui::GetStyle().FramePadding.x * 2.0f;

        // Input field width = remaining space
        float inputWidth =
            ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x;
        inputWidth = std::max(0.0f, inputWidth);

        // Height for text (including frame padding)
        float lineHeight = 1.0f + ImGui::GetTextLineHeight() * inputBufLines;
        float framePaddingY = ImGui::GetStyle().FramePadding.y;
        float inputHeight = lineHeight + framePaddingY * 2.0f;

        if (!state.userQuery.newInputBufString.empty()) {
            state.userQuery.inputBuf = state.userQuery.newInputBufString;
        }

        if (state.userQuery.isSubmitted || focusInput) {
            state.userQuery.isSubmitted = false;
            ImGui::SetKeyboardFocusHere();
        }

        ImGui::InputTextMultiline(
            "##user_input", state.userQuery.inputBuf.data(), state.userQuery.inputBuf.size() + 1,
            ImVec2(inputWidth, inputHeight), ImGuiInputTextFlags_CallbackResize,
            InputResizeCallback, &state.userQuery.inputBuf);
        if (!state.userQuery.newInputBufString.empty()) {
            state.userQuery.newInputBufString.clear();
        }

        // inputHistory();

        ImGui::SameLine();
        ImGui::BeginGroup();
        static std::string buttonText("Send");
        // using an InputText field to simulate a button because otherwise
        // moving to the widget do not work with tab in imtui
        state.userQuery.isSubmitted =
            ImGui::InputText("##llm_send", const_cast<char*>(buttonText.c_str()), 4,
                             ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_EnterReturnsTrue);
        state.userQuery.isSubmitted = state.userQuery.isSubmitted || ImGui::IsItemActive();
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("Send the query to the LLM for processing");
            ImGui::EndTooltip();
        }
        bool historyPrev = ImGui::Button("Prev");
        bool historyNext = ImGui::Button("Next");
        ImGui::EndGroup();

        inputHistory(historyNext, historyPrev);

        if (state.userQuery.isSubmitted) {
            std::string query = state.userQuery.inputBuf;
            // ImGui may insert a trailing for example when ctrl+enter.
            if (!query.empty() && query.back() == '\n') {
                query.pop_back();
            }
            if (!isWhitespaceOnly(query)) {
                state.userQuery.submitReady = true;
                state.userQuery.submitQuery = query;
                state.userQuery.inputHistory.push_back(query);
            }
            state.userQuery.inputBuf.clear();
            state.userQuery.historyPos = -1;
        }
    };

    auto statusLine = [&state, &DisplaySize]() {
        static constexpr std::string_view defaultStatus =
            "Context: 0/0 tokens | Model: none | Ready";

        if (state.statusText.empty()) {
            ImGui::TextUnformatted(defaultStatus.data());
        } else {
            ImGui::TextUnformatted(state.statusText.c_str());
        }
    };

    outputArea();
    inputArea();
    statusLine();
}

void renderTabLog(TuiState& state, Log& log) {
    ImVec2 DisplaySize = ImGui::GetIO().DisplaySize;
    ImVec2 childPos(0, 1.0f);
    ImVec2 childSize(DisplaySize.x, DisplaySize.y);
    ImGui::SetCursorPos(childPos);
    ImGuiWindowFlags outFlags = ImGuiWindowFlags_HorizontalScrollbar;

    ImGui::BeginChild("llm_log", childSize, false, outFlags);

    for (size_t i = 0; i < state.logMessages.size(); ++i) {
        const auto flags = (static_cast<int>(i) >= static_cast<int>(state.logMessages.size()) - 5)
                               ? ImGuiTreeNodeFlags_DefaultOpen
                               : ImGuiTreeNodeFlags_None;
        if (ImGui::CollapsingHeader(state.logMessages[i].summary.c_str(), flags)) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(state.logMessages[i].text.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    if (state.autoScroll) {
        ImGui::SetScrollHereY(1.0f);
    }

    // Auto-scroll detection: capture scroll state BEFORE EndChild so we
    // read the "output" child's actual scroll values.
    const float scrollY = ImGui::GetScrollY();
    const float scrollMax = ImGui::GetScrollMaxY();
    if (scrollMax > 0.0f) {
        state.autoScroll = (scrollY >= scrollMax - 1.0f);
    }

    ImGui::EndChild();
}

void renderMainWindow(TuiState& state, Log& log) {
    ImVec2 DisplaySize = ImGui::GetIO().DisplaySize;

    static int activeTab = 0;

    bool showChat{false};
    bool showLog{false};
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Tab")) {
            ImGui::MenuItem("Chat", nullptr, &showChat);
            ImGui::MenuItem("Log", nullptr, &showLog);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    if (showChat) {
        activeTab = 0;
    }
    if (showLog) {
        activeTab = 1;
    }

    switch (activeTab) {
    case 0:
        renderTabChat(state, log);
        break;
    case 1:
        renderTabLog(state, log);
        break;
    }
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

void tuiSetIniFilename(TuiState& state, const std::string& filename) {
    // TODO: this doesn't work. It ends up creating junk files.
    // state.iniFilename = filename;
    // ImGui::GetIO().IniFilename = state.iniFilename.c_str();
}

} // namespace llmfun::tui
