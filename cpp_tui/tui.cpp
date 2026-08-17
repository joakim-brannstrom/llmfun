#include "tui.h"
#include "session_fuzzy.h"

#include "imtui/imtui-impl-ncurses.h"
#include "imtui/imtui-impl-text.h"

#include "imgui/imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>

namespace llmfun::tui {

void AgentStream::finished() {
    if (stream.content.empty() && stream.thinking.empty()) {
        stream = AgentStreamMessage{};
        return;
    }

    updateCnt++;
    messages.emplace_back(stream);
    if (messages.size() > MaxMessages) {
        messages.erase(messages.begin());
    }
    stream = AgentStreamMessage{};
}

struct Log {
    FILE* logFile;

    template <typename... Args> void operator()(std::string format, Args&&... args) {
        if (logFile != nullptr) {
            if constexpr (sizeof...(Args) == 0) {
                std::fputs(format.c_str(), logFile);
            } else {
                std::fprintf(logFile, format.c_str(), std::forward<Args>(args)...);
            }
        }
    }
};

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

// Session row title button with filter-match highlighting (R25). Same
// button and widget id as renderButton (id = the full label), same hover /
// active color, but the label bytes covered by `runs` (label offsets [begin,
// end)) are over-drawn in `matchColor`. The overdraw happens on top of the
// just-drawn base label, so an empty `runs` leaves the frame exactly as
// renderButton draws it (one Text call, no extra items). `runs` must stay
// within the title portion of the label so the ellipsis and the " [N]"
// count suffix are never highlighted (R25).
bool renderTitleButton(const std::string& label, int width, bool active, ImVec4 colorActive,
                       const ImVec4& matchColor,
                       const std::vector<std::pair<std::size_t, std::size_t>>& runs) {
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
    if (!runs.empty()) {
        const char* lbl = label.c_str();
        for (const auto& run : runs) {
            const float x = p0.x + ImGui::CalcTextSize(lbl, lbl + run.first).x;
            ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
            ImGui::PushStyleColor(ImGuiCol_Text, matchColor);
            ImGui::TextUnformatted(lbl + run.first, lbl + run.second);
            ImGui::PopStyleColor();
        }
        // The overdraw runs are real items, so after the loop SameLine's
        // anchor (DC.CursorPosPrevLine - each item's right edge, published
        // by ItemSize) is the last run's right edge, not the label's.
        // Restore both cursor positions to the label's right edge so
        // sameLineAfterButton places the del button where it sits without a
        // filter, instead of pulling it left over the title.
        const ImVec2 anchor(p0.x + ImGui::CalcTextSize(label.c_str()).x, p0.y);
        ImGui::SetCursorScreenPos(anchor);
        ImGui::GetCurrentWindow()->DC.CursorPosPrevLine = anchor;
        // ImGui::SetCursorScreenPos(p0);
    }
    ImGui::PopID();

    return result;
}

static std::string makeUniqueId(const std::string& base, const std::string& suffix, int i) {
    auto s = base;
    s.append("##");
    s.append(suffix);
    s.append(std::to_string(i));
    return s;
}

static bool isWayland() {
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    const char* session = std::getenv("XDG_SESSION_TYPE");
    return wayland != nullptr && wayland[0] != '\0' ||
           (session != nullptr && strcmp(session, "wayland") == 0);
}

static void renderSeparator(const std::string& prefix, std::string_view ch, int n, bool disabled) {
    static char buf[256];
    if (n > 255)
        n = 255;
    if (n < 0)
        n = 0;
    if (n > prefix.size())
        n -= prefix.size();
    for (int i = 0; i < n; i++) {
        buf[i] = ch[0];
    }
    buf[n] = 0;
    if (disabled) {
        ImGui::TextDisabled("%s%s", prefix.c_str(), buf);
    } else {
        ImGui::Text("%s%s", prefix.c_str(), buf);
    }
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
        // Cast to unsigned char: std::isspace is UB for negative values,
        // and titles may contain arbitrary UTF-8 bytes.
        allTrue = allTrue && (std::isspace(static_cast<unsigned char>(c)) || c == '\0');
    }
    return allTrue;
}

void textUnformattedMultiline(std::string_view text) {
    auto begin = text.begin();
    auto end = text.end();
    while (begin != end) {
        auto next = std::find(begin, end, '\n');
        ImGui::TextUnformatted(begin, next);
        begin = (next != end) ? next + 1 : end;
    }
}

void textUnformattedMultiline(ImGui::MarkdownConfig& mdConfig, std::string_view text,
                              bool renderMarkdown = true) {
    if (renderMarkdown) {
        ImGui::Markdown(text.data(), text.length(), mdConfig);
    } else {
        textUnformattedMultiline(text);
    }
}

bool renderAsMarkdown(const ChatMessageType type) {
    if (type == ChatMessageType::ToolCall || type == ChatMessageType::ToolResponse)
        return false;
    return true;
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

static constexpr int HEADER_COLOR_COUNT = 4;

struct HeaderColors {
    ImVec4 bgColor;
    ImVec4 bgHovered;
    ImVec4 bgActive;
    ImVec4 textColor;
};

static HeaderColors getHeaderColors(ChatMessageType type, const ChatMessageStyle& style) {
    switch (type) {
    case ChatMessageType::User:
        return {style.userBg, style.userBgHover, style.userBgActive, style.darkText};
    case ChatMessageType::Vision:
        return {style.userBg, style.userBgHover, style.userBgActive, style.darkText};
    case ChatMessageType::Assistant:
        return {style.systemBg, style.systemBgHover, style.systemBgActive, style.assistantFg};
    case ChatMessageType::ToolCall:
        return {style.systemBg, style.systemBgHover, style.systemBgActive, style.toolCallFg};
    case ChatMessageType::ToolResponse:
        return {style.systemBg, style.systemBgHover, style.systemBgActive, style.toolResponseFg};
    case ChatMessageType::System:
        return {style.systemBg, style.systemBgHover, style.systemBgActive, style.systemFg};
    case ChatMessageType::FinalAnswer:
        return {style.finalAnswerBg, style.finalAnswerBgHover, style.finalAnswerBgActive,
                style.darkText};
    default:
        return {style.systemBg, style.systemBgHover, style.systemBgActive, style.systemFg};
    }
}

static bool isAssistantWork(ChatMessageType type) {
    return type == ChatMessageType::Assistant || type == ChatMessageType::ToolCall ||
           type == ChatMessageType::ToolResponse || type == ChatMessageType::System;
}

static bool isTopLevel(ChatMessageType type) {
    return type == ChatMessageType::User || type == ChatMessageType::Vision ||
           type == ChatMessageType::FinalAnswer;
}

static void buildRenderGroups(ChatTab& chat) {
    if (chat.outputLines.empty()) {
        chat.renderGroups.clear();
        return;
    }
    if (chat.outputLines.front().id == chat.renderGroupFirstId &&
        chat.outputLines.back().id == chat.renderGroupLastId) {
        return;
    }
    chat.renderGroups.clear();

    size_t i = 0;
    while (i < chat.outputLines.size()) {
        if (isTopLevel(chat.outputLines[i].type)) {
            GroupKind kind = (chat.outputLines[i].type == ChatMessageType::FinalAnswer)
                                 ? GroupKind::FinalAnswer
                                 : GroupKind::UserQuery;
            chat.renderGroups.push_back({i, i + 1, kind});
            ++i;
        } else {
            size_t start = i;
            while (i < chat.outputLines.size() && isAssistantWork(chat.outputLines[i].type)) {
                ++i;
            }
            chat.renderGroups.push_back({start, i, GroupKind::AssistantWork});
        }
    }

    chat.renderGroupFirstId = chat.outputLines.front().id;
    chat.renderGroupLastId = chat.outputLines.back().id;
}

static bool isAssistantGroupOpen(const RenderGroup& grp, const std::vector<RenderGroup>& groups,
                                 size_t index) {
    if (grp.kind != GroupKind::AssistantWork)
        return true;
    return index == groups.size() - 1;
}

static void renderAgentStream(TuiState& state, const int agentIndex, const int64_t updateCount,
                              const std::string agentId, const AgentStreamMessage& msg) {
    auto headerTmp = agentId;
    if (!msg.role.empty()) {
        headerTmp.append(" [");
        headerTmp.append(msg.role);
        headerTmp.append("]");
    }
    if (!msg.status.empty()) {
        headerTmp.append(" [");
        headerTmp.append(msg.status);
        headerTmp.append("]");
    }
    headerTmp.append(" [");
    headerTmp.append(std::to_string(updateCount));
    headerTmp.append("]");

    const auto headerId = makeUniqueId(headerTmp, "streamagent_msg_header", agentIndex);
    const auto colors = getHeaderColors(ChatMessageType::Assistant, state.chat.style);
    ImGui::PushStyleColor(ImGuiCol_Text, colors.textColor);
    StyleColorGuard guard{1};
    if (ImGui::CollapsingHeader(headerId.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        guard.pop();
        ImGui::Indent();

        if (!msg.thinking.empty()) {
            std::string thinkId =
                makeUniqueId("Model reasoning", "_agent_stream_think_", agentIndex);
            auto thinkFlags = ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen;

            ImGui::PushStyleColor(ImGuiCol_Header, state.left.thinkingNodeBg);
            StyleColorGuard thinkGuard{1};
            if (ImGui::TreeNodeEx(thinkId.c_str(), thinkFlags)) {
                thinkGuard.pop();
                ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 1);
                textUnformattedMultiline(state.mdConfig, msg.thinking);
                ImGui::PopTextWrapPos();
                renderSeparator("End ", "-", ImGui::GetContentRegionMax().x - 4, true);
                ImGui::TreePop();
            }
        }

        ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 1);
        textUnformattedMultiline(state.mdConfig, msg.content);
        ImGui::PopTextWrapPos();

        ImGui::Unindent();
    }
}

static void renderNestedMessage(TuiState& state, size_t i, ImVec2 displaySize) {
    if (i >= state.chat.outputLines.size())
        return;
    const auto& entry = state.chat.outputLines[i];

    const bool showOpen = (i + 5) > state.chat.outputLines.size();
    const auto openFlags = ImGuiTreeNodeFlags_Framed |
                           (showOpen ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None);
    auto treeId = makeUniqueId(entry.summary, "", entry.id);
    ImGui::PushStyleColor(ImGuiCol_Header, state.chat.nestedAssistNodeBg);
    StyleColorGuard topNodeGuard{1};
    if (!ImGui::TreeNodeEx(treeId.c_str(), openFlags)) {
        return;
    }
    topNodeGuard.pop();

    if (!entry.thinking.empty()) {
        auto thinkId = makeUniqueId("Model reasoning", "_assist_think_", entry.id);
        ImGui::PushStyleColor(ImGuiCol_Header, state.chat.thinkingNodeBg);
        StyleColorGuard thinkGuard{1};
        if (ImGui::TreeNodeEx(thinkId.c_str(), openFlags)) {
            thinkGuard.pop();
            ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 1);
            textUnformattedMultiline(state.mdConfig, entry.thinking);
            ImGui::PopTextWrapPos();
            renderSeparator("End ", "-", ImGui::GetContentRegionMax().x - 4, true);
            ImGui::TreePop();
        }
    }

    ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 1);
    textUnformattedMultiline(state.mdConfig, entry.text, renderAsMarkdown(entry.type));
    ImGui::PopTextWrapPos();

    ImGui::PushID((int)(i + state.chat.outputLines.size()));
    if (ImGui::Button(" [c] ")) {
        ImGui::SetClipboardText(entry.text.c_str());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Copy to clipboard");
    }
    ImGui::PopID();

    ImGui::TreePop();
}

static bool renderSingleHeader(TuiState& state, const ChatMessage& entry, ImVec2 displaySize,
                               bool forceOpen, bool showHeader, bool showThinking,
                               bool allowMarkdown = true) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_None;
    if (forceOpen) {
        flags = ImGuiTreeNodeFlags_DefaultOpen;
    }

    const auto headerId = makeUniqueId(entry.summary, "msg_header", entry.id);
    const auto colors = getHeaderColors(entry.type, state.chat.style);
    if (showHeader) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.textColor);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    }
    ImGui::PushStyleColor(ImGuiCol_Header, colors.bgColor);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, colors.bgHovered);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, colors.bgActive);
    StyleColorGuard guard{HEADER_COLOR_COUNT};
    const bool userOpen = ImGui::CollapsingHeader(headerId.c_str(), flags);
    if (userOpen) {
        guard.pop();
        ImGui::Indent();

        if (!entry.thinking.empty()) {
            std::string thinkId = makeUniqueId("Model reasoning", "_primary_think_", entry.id);
            auto thinkFlags =
                ImGuiTreeNodeFlags_Framed |
                (showThinking ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None);

            ImGui::PushStyleColor(ImGuiCol_Header, state.chat.thinkingNodeBg);
            StyleColorGuard thinkGuard{1};
            if (ImGui::TreeNodeEx(thinkId.c_str(), thinkFlags)) {
                thinkGuard.pop();
                ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 1);
                textUnformattedMultiline(state.mdConfig, entry.thinking);
                ImGui::PopTextWrapPos();
                renderSeparator("End ", "-", ImGui::GetContentRegionMax().x - 4, true);
                ImGui::TreePop();
            }
        }

        ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 1);
        textUnformattedMultiline(state.mdConfig, entry.text,
                                 allowMarkdown && renderAsMarkdown(entry.type));
        ImGui::PopTextWrapPos();

        std::string buttonId = " [c] ##" + std::to_string(entry.id);
        if (ImGui::Button(buttonId.c_str())) {
            ImGui::SetClipboardText(entry.text.c_str());
        } else if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("Copy to clipboard");
            ImGui::EndTooltip();
        }
        ImGui::Unindent();
    }
    return userOpen;
}

static void renderSingleHeader(TuiState& state, size_t i, ImVec2 displaySize,
                               bool forceOpen = false, bool lastMsgIsTool = false) {
    if (i >= state.chat.outputLines.size())
        return;

    const auto& entry = state.chat.outputLines[i];
    const bool isRecent =
        (static_cast<int>(i) >= static_cast<int>(state.chat.outputLines.size()) - 10);
    const bool showHeader = state.chat.outputLineOpen.count(i) == 0;
    const bool showThinking = (i >= state.chat.outputLines.size() - 3);

    if (renderSingleHeader(state, entry, displaySize, forceOpen || isRecent, showHeader,
                           showThinking)) {
        state.chat.outputLineOpen.insert(i);
    } else {
        state.chat.outputLineOpen.erase(i);
    }
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

void markdownFormatCallback(const ImGui::MarkdownFormatInfo& markdownFormatInfo_, bool start_) {
    // TODO: This must be a runtime parameter because it is different depending on backend
#ifdef IMGUI_HAS_TEXTURES
    ImGui::defaultMarkdownFormatCallback(markdownFormatInfo_, start_);
#else
    auto* userData =
        reinterpret_cast<::llmfun::tui::TuiState*>(markdownFormatInfo_.config->userData);
    auto& style = userData->mdStyle;

    switch (markdownFormatInfo_.type) {
    case ImGui::MarkdownFormatType::HEADING:
        if (start_) {
            const auto idx =
                std::min((markdownFormatInfo_.level > 0 ? markdownFormatInfo_.level : 1) - 1, 3);
            ImGui::PushStyleColor(ImGuiCol_Text, style.heading[idx]);
            ImGui::TextUnformatted(style.headingPrefix[idx].c_str());
            ImGui::SameLine(0.0f, 0.0f);
        } else {
            ImGui::PopStyleColor();
        }
        break;
    case ImGui::MarkdownFormatType::EMPHASIS: {
        const char* marker;
        ImVec4 color;
        if (markdownFormatInfo_.level == 1) {
            marker = "_";
            color = style.italic;
        } else {
            marker = "**";
            color = style.strong;
        }
        if (start_) {
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(marker);
            ImGui::SameLine(0.0f, 0.0f);
        } else {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(marker);
            ImGui::PopStyleColor();
        }
    } break;
    // Terminal has no monospace face: wrap the span in literal backticks,
    // same trick as the EMPHASIS markers above.
    case ImGui::MarkdownFormatType::INLINE_CODE:
        if (start_) {
            ImGui::PushStyleColor(ImGuiCol_Text, style.inlineCode);
            ImGui::TextUnformatted("`");
            ImGui::SameLine(0.0f, 0.0f);
        } else {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted("`");
            ImGui::PopStyleColor();
        }
        break;
    case ImGui::MarkdownFormatType::LINK:
        if (start_) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
        } else {
            ImGui::PopStyleColor();
            if (markdownFormatInfo_.itemHovered) {
                ImGui::UnderLine(ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
            }
        }
        break;
    default:
        ImGui::defaultMarkdownFormatCallback(markdownFormatInfo_, start_);
        break;
    }
#endif
}

void markdownLinkCallback(ImGui::MarkdownLinkCallbackData data_) {
    std::string url(data_.link, data_.linkLength);
    if (!data_.isImage) {
        SetClipboardText(nullptr, url.c_str());
    }
}

void initMarkdownConfig(TuiState& state) {
    auto& mdConfig = state.mdConfig;

    mdConfig.linkCallback = &markdownLinkCallback;

    // TODO: This must be a runtime parameter because it is different depending on backend
#ifdef IMGUI_HAS_TEXTURES // used to detect dynamic font capability
    mdConfig.headingFormats[0] = {nullptr, true, fontSize * 1.1f};
    mdConfig.headingFormats[1] = {nullptr, true, fontSize};
    mdConfig.headingFormats[2] = {nullptr, false, fontSize};
#else
    mdConfig.headingFormats[0] = {nullptr, true};
    mdConfig.headingFormats[1] = {nullptr, true};
    mdConfig.headingFormats[2] = {nullptr, false};
#endif
    mdConfig.userData = &state;
    mdConfig.formatCallback = &markdownFormatCallback;
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

int InputResizeCallback(ImGuiInputTextCallbackData* data) {
    auto* udata = reinterpret_cast<UserQueryState*>(data->UserData);

    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        udata->inputBuf.resize(data->BufSize);
        data->Buf = udata->inputBuf.data();
    }

    return 0;
}

void renderTabChatLeftPanel(ChatTabLeftPanel& panel, Log& log) {
    if (panel.agents.empty()) {
        panel.panelW = 0;
        return;
    } else if (panel.panelW == 0) {
        panel.panelW = panel.PanelWActivated;
    }

    ImGui::BeginChild("Child window", ImVec2(panel.panelW - 1, 0), true);
    const auto panelWClosed = 8;
    if (panel.panelOpen) {
        if (renderButton("Close", panelWClosed, false, panel.activeButton)) {
            panel.activeAgent = -1;
            panel.panelOpen = false;
            panel.panelW = panelWClosed;
        }
        ImGui::SameLine();
        if (renderButton("Clear", 5, false, panel.activeButton)) {
            panel.agents.clear();
            panel.activeAgent = -1;
            panel.panelW = 0;
        }
    } else {
        panel.panelW = panelWClosed;
        if (renderButton("Open", panelWClosed, false, panel.activeButton)) {
            panel.activeAgent = -1;
            panel.panelOpen = true;
            panel.panelW = panel.PanelWActivated;
        }
        ImGui::EndChild();
        return;
    }

    if (renderButton("Main Agent", ImGui::GetContentRegionMax().x, false, panel.activeButton)) {
        panel.activeAgent = -1;
    }
    renderSeparator("Pipeline ", "-", ImGui::GetContentRegionMax().x, true);
    for (size_t i; i < panel.agents.size(); ++i) {
        auto s = panel.agents[i].agentId;
        s.append(" [");
        s.append(std::to_string(panel.agents[i].updateCnt));
        s.append("]");
        if (renderButton(s.c_str(), ImGui::GetContentRegionMax().x, !panel.agents[i].activity,
                         panel.activeButton)) {
            panel.activeAgent = i;
            panel.agents[i].activity = false;
            panel.autoScroll = true;
        }
    }
    ImGui::EndChild();
}

static bool isUtf8Continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// Word separator for the R25 highlight runs: the same four bytes the fuzzy
// matcher treats as word boundaries (A21, fuzzyIsBoundary). Separators are
// always whole ASCII characters, so walking over them never splits a
// multi-byte character.
static bool isWordSeparator(char c) { return c == ' ' || c == '-' || c == '_' || c == '/'; }

// Title bytes shown in a session row of width `rowWidth` (UTF-8-safe
// truncation, ellipsis only when something was cut). Shared by
// sessionRowLabel and the match highlight (R25) so the displayed prefix and
// the highlight clip never disagree.
static std::size_t sessionTitlePortionLen(const SessionEntry& entry, int rowWidth) {
    const std::string count = " [" + std::to_string(entry.messageCount) + "]";
    const int titleBudget = rowWidth - static_cast<int>(count.size());
    if (titleBudget <= 0) {
        return 0;
    }
    if (static_cast<int>(entry.title.size()) <= titleBudget) {
        return entry.title.size();
    }
    // Reserve 3 cells for the ellipsis when there is room for it.
    // titleBudget == 3 fits the ellipsis alone (the title text then
    // contributes nothing); smaller budgets drop the ellipsis entirely.
    int n = titleBudget >= 3 ? titleBudget - 3 : 0;
    while (n > 0 && isUtf8Continuation(entry.title[n - 1])) {
        --n;
    }
    return static_cast<std::size_t>(n);
}

static std::string sessionRowLabel(const SessionEntry& entry, int rowWidth) {
    const std::string count = " [" + std::to_string(entry.messageCount) + "]";
    const std::size_t portion = sessionTitlePortionLen(entry, rowWidth);
    if (portion >= entry.title.size()) {
        return entry.title + count;
    }
    std::string title = entry.title.substr(0, portion);
    // The ellipsis is kept whenever there is room for it or any title text survived.
    const int titleBudget = rowWidth - static_cast<int>(count.size());
    if (portion > 0 || titleBudget >= 3) {
        title += "...";
    }
    return title + count;
}

static void titleMatchRuns(const std::string& query, const std::string& title, std::size_t portion,
                           std::vector<std::pair<std::size_t, std::size_t>>& runs) {
    runs.clear();
    std::vector<std::size_t> pos;
    if (!fuzzyMatchPositions(query, title, pos)) {
        return;
    }
    const std::size_t n = title.size();
    for (std::size_t i = 0; i < pos.size();) {
        std::size_t s = pos[i];
        std::size_t e = s;
        while (i < pos.size() && pos[i] == e) {
            ++e;
            ++i;
        }
        // Snap to the enclosing character(s): a character is highlighted iff
        // any of its bytes matched (no mid-character splits, N7). A matched
        // continuation byte walks s back to its character start; a matched
        // byte whose character continues walks e forward to the end.
        while (s > 0 && isUtf8Continuation(title[s])) {
            --s;
        }
        while (e < n && isUtf8Continuation(title[e])) {
            ++e;
        }
        // The highlight covers the whole word enclosing the matched
        // character(s) (word = maximal span between the A21 separator
        // bytes), so an ASCII prefix match lights an accented word whole
        // ("caf" -> "Café") instead of leaving the accented tail unlit
        // (S13). The walks stop only on separator bytes - always whole
        // ASCII characters - so s/e remain at character starts (N7).
        while (s > 0 && !isWordSeparator(title[s - 1])) {
            --s;
        }
        while (e < n && !isWordSeparator(title[e])) {
            ++e;
        }
        if (s >= portion) {
            continue; // matched only beyond the displayed prefix
        }
        if (e > portion) {
            e = portion; // clip to the character-boundary prefix
        }
        // Two raw runs can snap into the same character; merge on touch.
        if (!runs.empty() && s <= runs.back().second) {
            runs.back().second = std::max(runs.back().second, e);
        } else {
            runs.emplace_back(s, e);
        }
    }
}

// One dimmed preview line: the first user message truncated to the row width
// with an ellipsis. Returns empty for an empty preview so the row stays
// single-line. Control bytes are collapsed to spaces as a backstop on top of
// the D-side normalization, so a preview can never break the two-line layout.
static std::string previewRowLabel(const SessionEntry& entry, int rowWidth) {
    if (entry.preview.empty() || rowWidth <= 0) {
        return std::string();
    }
    std::string preview = entry.preview;
    for (char& c : preview) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7f) {
            c = ' ';
        }
    }
    if (static_cast<int>(preview.size()) <= rowWidth) {
        return preview;
    }
    // Reserve 3 cells for the ellipsis when there is room for it.
    // rowWidth == 3 fits the ellipsis alone; smaller budgets drop it.
    int n = rowWidth >= 3 ? rowWidth - 3 : 0;
    while (n > 0 && isUtf8Continuation(preview[n - 1])) {
        --n;
    }
    std::string out;
    out.assign(preview, 0, n);
    if (n > 0 || rowWidth >= 3) {
        out += "...";
    }
    return out;
}

// Queue one sidebar action for the D side to poll (A2/A7).
static void queueSessionAction(ChatTabSessionPanel& panel, SessionActionType type,
                               const std::string& id, const std::string& title) {
    panel.actions.push_back(SessionAction{type, id, title});
}

// Continue a button row past the previous button's click rect.
// renderButton draws its label at the button origin, so ImGui::SameLine
// anchors on the TEXT item - a label shorter than the button would pull
// the next button left, inside the previous button's rect, making the
// second button unclickable (the first button owns the hover id). Move the
// cursor to the end of the previous button rect plus one spacing instead.
static void sameLineAfterButton(float buttonWidth, float labelWidth) {
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + buttonWidth - labelWidth +
                         ImGui::GetStyle().ItemSpacing.x);
}

// Initialize the rename buffer from a title. A title that does not fit the
// 128-byte buffer initializes the buffer empty - no silent truncation (L4).
static void initRenameBuf(ChatTabSessionPanel& panel, const std::string& title) {
    const size_t bufSize = sizeof(panel.renameBuf);
    if (title.size() < bufSize) {
        std::strncpy(panel.renameBuf, title.c_str(), bufSize - 1);
        panel.renameBuf[bufSize - 1] = '\0';
    } else {
        panel.renameBuf[0] = '\0';
    }
}

// Programmatic filter clear (A23): reset the buffer and bump filterSeq so
// the InputText id changes and the widget re-reads the now-empty user
// buffer instead of its stale internal edit state (C10).
static void clearFilter(ChatTabSessionPanel& panel) {
    panel.filterBuf.fill('\0');
    ++panel.filterSeq;
}

static void renderTabChatSessionPanel(TuiState& state, Log& log) {
    auto& panel = state.sessionPanel;

    // Pending-switch flush (A12/R14): a session row clicked while the
    // agent was busy becomes an ordinary Select on the first ready frame.
    // This runs at the VERY TOP, before every early return (mutual
    // exclusion below and the collapsed-state branch further down, M8),
    // so the queued switch applies even when the pipeline panel currently
    // owns the left slot or the panel was collapsed after the click; if
    // the panel is not rendered at all, the slot simply survives until it
    // is. The slot is cleared here, so the flush fires exactly once.
    if (!panel.pendingSelectId.empty() && state.readyStatus) {
        if (panel.pendingSelectId != panel.activeId) {
            queueSessionAction(panel, SessionActionType::Select, panel.pendingSelectId, "");
            log("session panel: flushed pending select %s\n", panel.pendingSelectId.c_str());
        } else {
            // The queued target became the active session in the meantime
            // (e.g. a slash /switch); the flush must not re-select it.
            log("session panel: pending select %s already active; dropped\n",
                panel.pendingSelectId.c_str());
        }
        panel.pendingSelectId.clear();
    }

    // Mutual exclusion (R6/A6/H1): the pipeline panel renders whenever it
    // has agents - open or collapsed - so the session panel must not. Panel
    // state is preserved so the session panel reappears when the pipeline
    // clears.
    if (!state.left.agents.empty())
        return;

    // First open: never offset the output area by 0 on the first frame (F5).
    if (panel.panelW == 0)
        panel.panelW = panel.PanelWActivated;

    // The rename input is bound to the active row; when the active row is
    // absent from the snapshot (deleted session), close the box - an open
    // box bound to a missing row is dead state.
    if (panel.renameActive) {
        auto it = std::find_if(panel.sessions.begin(), panel.sessions.end(),
                               [&panel](const SessionEntry& e) { return e.id == panel.activeId; });
        if (it == panel.sessions.end()) {
            panel.renameActive = false;
            panel.renameFocus = false;
        } else if (panel.renameRowId != panel.activeId) {
            // The active row changed but still exists: re-initialize the
            // buffer from the new active row (L3) and rebind the box.
            panel.renameRowId = panel.activeId;
            initRenameBuf(panel, it->title);
        }
    }

    const auto panelWClosed = 8;
    const bool canQueue = state.readyStatus;

    // Outer child: panel border/background, id unchanged (A15). The header
    // (Close/Open, New, separator) stays fixed; the rows live in their own
    // scrollable child sized to the remaining panel height (R15).
    ImGui::BeginChild("Session child window", ImVec2(panel.panelW - 1, 0), true);
    if (!panel.panelOpen) {
        panel.panelW = panelWClosed;
        if (renderButton("Open", panelWClosed, false, panel.activeButton)) {
            panel.pendingDeleteId.clear();
            panel.panelOpen = true;
            panel.panelW = panel.PanelWActivated;
            log("session panel: open\n");
        }
        ImGui::EndChild();
        return;
    }

    if (renderButton("Close", panelWClosed, false, panel.activeButton)) {
        panel.pendingDeleteId.clear();
        panel.renameActive = false;
        panel.renameFocus = false;
        panel.panelOpen = false;
        panel.panelW = panelWClosed;
        log("session panel: close\n");
    }
    sameLineAfterButton(panelWClosed, ImGui::CalcTextSize("Close").x);
    if (renderButton("New", 3, false, panel.activeButton)) {
        panel.pendingDeleteId.clear();
        if (canQueue) {
            queueSessionAction(panel, SessionActionType::New, "", "");
            log("session panel: queued new session\n");
        } else {
            log("session panel: new skipped (busy)\n");
        }
    }

    renderSeparator("Sessions ", "-", ImGui::GetContentRegionMax().x, true);

    // Filter input (A19): a single-line InputText fixed in the header,
    // between the separator and the rows child, so it stays put while the
    // rows scroll (A15). Click-to-focus only (C11): no
    // SetKeyboardFocusHere, so the always-rendered input never steals
    // keyboard focus from the main query input. The id is suffixed by
    // filterSeq so a programmatic clear (Escape, A23) can force a fresh InputText
    // state (C10). EnterReturnsTrue: the return value is handled after the
    // visible list is computed below (Enter selects the top match, A22/R23).
    // S11: snapshot for the rename-Esc compensation at the end of the rows
    // loop. 1.81's InputText treats Escape as cancel_edit: an ACTIVE input
    // reverts its buffer to the value at activation (InitialTextA,
    // imgui_widgets.cpp:4260-4275). While the rename box is open, Escape
    // must close the box only - if the filter input is the focused one,
    // its same-frame revert would wipe the query along with the box.
    const std::string filterPreFrame(panel.filterBuf.data());
    bool renameEscClosedThisFrame = false;

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const std::string filterId = "##session_filter_" + std::to_string(panel.filterSeq);
    const bool filterEnter =
        ImGui::InputText(filterId.c_str(), panel.filterBuf.data(), panel.filterBuf.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue);

    // Real-time filter + ranking (A20/A21/A27/A28): compute the visible
    // (filtered + ranked) list each frame from the local snapshot. A
    // whitespace-only filter is "no filter" (A19): all entries in snapshot
    // order, no reordering. Otherwise keep the entries whose title OR preview
    // matches (fuzzyScoreFields >= 0, R26) and rank them by descending score;
    // stable ties keep snapshot order.
    // Per-frame local (N5, no caching): store an index into panel.sessions
    // plus the score, not a copy of the SessionEntry (three std::strings)
    // per match per frame.
    struct VisibleRow {
        std::size_t index; // index into panel.sessions
        int score;         // fuzzyScoreFields (0 when unfiltered)
    };
    std::vector<VisibleRow> visible;
    const std::string filterQuery(panel.filterBuf.data());
    if (isWhitespaceOnly(filterQuery)) {
        visible.reserve(panel.sessions.size());
        for (std::size_t i = 0; i < panel.sessions.size(); ++i) {
            visible.push_back(VisibleRow{i, 0});
        }
    } else {
        for (std::size_t i = 0; i < panel.sessions.size(); ++i) {
            const SessionEntry& e = panel.sessions[i];
            // Multi-field match (R26): title + preview, title weighted
            // 2x. A session shows if either field matches.
            const int s = fuzzyScoreFields(filterQuery, e.title, e.preview);
            if (s >= 0) {
                visible.push_back(VisibleRow{i, s});
            }
        }
        std::stable_sort(
            visible.begin(), visible.end(),
            [](const VisibleRow& a, const VisibleRow& b) { return a.score > b.score; });
    }

    // A28: if the filter hides the active row while the rename box is open,
    // close the box - an open box bound to a hidden row is dead state
    // (mirrors the tuiSetSessionList "active row absent" rule). This also
    // keeps the two Esc branches disjoint: the rename's own Esc check (inside
    // the row loop, active row only) is the only live rename-Esc path, and it
    // runs only when the active row is visible.
    if (panel.renameActive) {
        const bool activeVisible =
            std::any_of(visible.begin(), visible.end(), [&panel](const VisibleRow& v) {
                return panel.sessions[v.index].id == panel.activeId;
            });
        if (!activeVisible) {
            panel.renameActive = false;
            panel.renameFocus = false;
            log("session panel: rename closed (active row filtered out, A28)\n");
        }
    }

    // Escape clears the filter (A23): the filter input has no key binding
    // of its own (InputText only drops focus on Escape), so the key is
    // caught here, before the row loop. The rename box owns Escape while
    // open: its own check runs in the row loop (active row only), and
    // renameActive is read AFTER the A28 close above, so the frame where
    // A28 closed the box (the active row was filtered out by a keystroke in
    // this very input) still clears the filter, while a frame where the box
    // is still live only closes the box - the two Escape paths stay
    // disjoint. A whitespace-only query is already "no filter" (A19), so it
    // never triggers the clear (no pointless id churn). While the input is
    // ACTIVE, 1.81's cancel_edit (NavUpdate Cancel -> ClearActiveID,
    // imgui.cpp:9010-9016) reverts the buffer to its activation value
    // during NewFrame - before this code runs - so "was the query real" is
    // also read from the end-of-last-frame snapshot: a query that was
    // non-empty last frame and is empty now counts as the clear, which
    // keeps C10's filterSeq bump firing on the Esc frame (S18).
    const bool filterRevertedEmpty = panel.filterNonEmptyLastFrame && isWhitespaceOnly(filterQuery);
    if (!panel.renameActive && (!isWhitespaceOnly(filterQuery) || filterRevertedEmpty) &&
        ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape))) {
        clearFilter(panel);
        log("session panel: filter cleared (Escape)\n");
    }

    // Enter selects the top visible match (A22/R23/A24): with a non-empty
    // visible list, Enter in the filter input selects visible[0]'s id. A
    // non-empty filter ranks best-first, so visible[0] is the best match; an
    // empty filter keeps snapshot order, so visible[0] is the first session.
    // Reuses the existing Select path: queue when ready, else defer in the
    // single pending slot (A12, last action wins; it flushes as an ordinary
    // Select on the first ready frame at the top of this function). Selecting
    // the already-active session is a no-op (log only, no action). An empty
    // visible list is a no-op. The filter clears at selection time
    // (clearFilter), independent of the async switch. Enter only fires while the
    // filter input itself is active (EnterReturnsTrue), so it cannot race the
    // rename input's own Enter handling.
    if (filterEnter && !visible.empty()) {
        const std::string& topId = panel.sessions[visible[0].index].id;
        if (topId == panel.activeId) {
            log("session panel: filter select %s skipped (already active)\n", topId.c_str());
        } else if (canQueue) {
            queueSessionAction(panel, SessionActionType::Select, topId, "");
            log("session panel: filter queued select %s\n", topId.c_str());
        } else {
            panel.pendingSelectId = topId;
            log("session panel: filter pending select %s (busy)\n", topId.c_str());
        }
        clearFilter(panel);
    }

    // The rows live in a child of their own, sized to the remaining panel
    // height, so ImGui 1.81 enables the scrollbar when the list overflows
    // (R15). The id is stable, so the scroll position survives snapshot
    // refreshes (the full-replace list does not reset it).
    ImGui::BeginChild("session_rows", ImVec2(panel.panelW - 1, ImGui::GetContentRegionAvail().y),
                      true);

    const int delWidth = 4;
    // Row width budget from the actual clip rect, not the content size. The
    // rows child is bordered and nested inside the bordered panel child, and
    // each border level costs one clipped column, so the draw clip rect is
    // two cells narrower than GetContentRegionAvail().x (29 vs 27 at the
    // default panel width). The vendored imtui backend also drops the last
    // clip column (text vertices clamp at ClipRect.Max - 1 and cells at
    // >= Max are discarded), so a del label sized against the content width
    // loses its last character. ClipRect.Max stays put when a scrollbar
    // appears (the WorkRect shrinks instead), so rows narrow with the
    // scrollbar instead of clipping. The trailing -1 keeps the del button
    // one cell short of the clip edge, like before.
    const int usableWidth = static_cast<int>(std::floor(ImGui::GetContentRegionMax().x)) -
                            static_cast<int>(std::floor(ImGui::GetCursorScreenPos().x)) - 1;
    const int rowWidth =
        usableWidth - delWidth - static_cast<int>(ImGui::GetStyle().ItemSpacing.x) - 1;
    for (const auto& row : visible) {
        const SessionEntry& entry = panel.sessions[row.index];
        const bool isActive = (entry.id == panel.activeId);
        const bool delPending = (panel.pendingDeleteId == entry.id);
        const std::string label = sessionRowLabel(entry, rowWidth);
        const float labelWidth = ImGui::CalcTextSize(label.c_str()).x;
        std::vector<std::pair<std::size_t, std::size_t>> matchRuns;
        if (!isWhitespaceOnly(filterQuery)) {
            titleMatchRuns(filterQuery, entry.title, sessionTitlePortionLen(entry, rowWidth),
                           matchRuns);
        }
        const ImVec4& rowColor =
            (entry.id == panel.pendingSelectId) ? panel.pendingButton : panel.activeButton;
        // The session id keeps row widgets unique even when titles collide.
        ImGui::PushID(entry.id.c_str());
        if (renderTitleButton(label, rowWidth, isActive, rowColor, panel.matchColor, matchRuns)) {
            panel.pendingDeleteId.clear(); // non-delete control (L1)
            if (canQueue && !isActive) {
                queueSessionAction(panel, SessionActionType::Select, entry.id, "");
                log("session panel: queued select %s\n", entry.id.c_str());
            } else if (!canQueue && !isActive) {
                // Busy (A12): defer the switch in the single pending slot
                // (last click wins); it flushes as an ordinary Select on
                // the first ready frame (top of this function). Selecting
                // is safe to defer - the in-flight query completes in the
                // session it was typed in; New/Rename/Delete stay
                // guard-and-skip.
                panel.pendingSelectId = entry.id;
                log("session panel: pending select %s (busy)\n", entry.id.c_str());
            } else {
                // Active-row click: no-op, and any pending target stays
                // armed (A12).
                log("session panel: select %s skipped (already active)\n", entry.id.c_str());
            }
            clearFilter(panel);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(entry.title.c_str());
            if (!entry.preview.empty()) {
                ImGui::TextUnformatted(entry.preview.c_str());
            }
            ImGui::EndTooltip();
        }
        sameLineAfterButton(rowWidth, labelWidth);
        if (renderButton(delPending ? "del?" : "del", delWidth, false, rowColor)) {
            if (canQueue) {
                if (delPending) {
                    // Second press: confirmed (A5).
                    queueSessionAction(panel, SessionActionType::Delete, entry.id, "");
                    panel.pendingDeleteId.clear();
                    log("session panel: queued delete %s\n", entry.id.c_str());
                } else {
                    // First press - or the pending target moves to this row (L1).
                    panel.pendingDeleteId = entry.id;
                    log("session panel: delete armed %s\n", entry.id.c_str());
                }
            } else {
                log("session panel: delete %s skipped (busy)\n", entry.id.c_str());
            }
        }
        if (isActive) {
            // Rename toggle + input on the active row only (R4/L8).
            if (renderButton("Rename", 6, panel.renameActive, panel.activeButton)) {
                panel.pendingDeleteId.clear(); // non-delete control (L1)
                panel.renameActive = !panel.renameActive;
                if (panel.renameActive) {
                    panel.renameRowId = entry.id;
                    initRenameBuf(panel, entry.title);
                    panel.renameFocus = true;
                    ++panel.renameSeq; // fresh InputText state per open
                    log("session panel: rename opened %s\n", entry.id.c_str());
                }
            }
            if (panel.renameActive) {
                ImGui::SameLine();
                if (panel.renameFocus) {
                    ImGui::SetKeyboardFocusHere();
                    panel.renameFocus = false;
                }
                // Fill the rest of the row (the default 0.65 * content width
                // would clip the input's right edge in the narrow panel).
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                const std::string inputId = "##session_rename_" + std::to_string(panel.renameSeq);
                const bool enter =
                    ImGui::InputText(inputId.c_str(), panel.renameBuf, sizeof(panel.renameBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue);

                // The vendored InputText clears its own ActiveId on Escape
                // (1.81 behavior, no EscapeClearsAll flag needed), so
                // IsItemActive() is false by the time we check - gate on
                // the key alone. Trade-off: Escape closes the rename box
                // even when another widget holds the keyboard focus (e.g.
                // the query input); acceptable, because Escape has no other
                // TUI binding - the filter clear (A23) is gated on
                // !renameActive - and canceling the rename is the safe action.
                const bool esc = ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape));
                if (esc) {
                    panel.renameActive = false;
                    panel.renameFocus = false;
                    renameEscClosedThisFrame = true;
                    log("session panel: rename cancelled\n");
                } else if (enter) {
                    const std::string newTitle(panel.renameBuf);
                    if (isWhitespaceOnly(newTitle)) {
                        // Empty titles are rejected here and again in D (L4).
                        log("session panel: empty rename rejected\n");
                    } else if (canQueue) {
                        queueSessionAction(panel, SessionActionType::Rename, entry.id, newTitle);
                        panel.renameActive = false;
                        panel.renameFocus = false;
                        log("session panel: queued rename %s\n", entry.id.c_str());
                    } else {
                        log("session panel: rename skipped (busy)\n");
                    }
                }
            }
        }
        // The rename input stays inside the row's PushID scope so its widget
        // id changes with the row: when the active row changes, the InputText
        // re-reads the (re-initialized) user buffer instead of its stale
        // internal edit state (L3).
        // Line 2: dimmed non-interactive preview of the first user message
        // (A14). Skipped entirely when the preview is empty so rows without
        // one stay single-line; non-interactive (no button, no hover
        // action, no tooltip of its own).
        const std::string preview = previewRowLabel(entry, rowWidth);
        if (!preview.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, panel.previewColor);
            ImGui::TextUnformatted(preview.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    // S11 compensation: when the box just closed via Escape above, the
    // filter widget (if it was the active input) reverted its buffer to
    // InitialTextA on the same frame (cancel_edit, see snapshot above).
    // Restore the pre-frame query and bump filterSeq so the next frame
    // re-initializes a fresh InputText state from the restored buffer
    // (C10 pattern); without the bump the deactivated widget's stale stb
    // (holding the reverted text) would resurface on the next
    // click-to-focus.
    if (renameEscClosedThisFrame && strcmp(panel.filterBuf.data(), filterPreFrame.c_str()) != 0) {
        panel.filterBuf.fill('\0');
        std::copy_n(filterPreFrame.begin(),
                    std::min<std::size_t>(filterPreFrame.size(), panel.filterBuf.size() - 1),
                    panel.filterBuf.begin());
        ++panel.filterSeq;
        log("session panel: filter restored (rename Esc frame)\n");
    }

    // No-match indicator (A26): when a non-empty filter matches nothing
    // but the snapshot is non-empty, render a single dimmed "no matches"
    // line instead of a blank area. The !panel.sessions.empty() clause
    // keeps this consistent with the edge-case table: an empty snapshot
    // renders no rows and no indicator regardless of the filter.
    if (!isWhitespaceOnly(filterQuery) && visible.empty() && !panel.sessions.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, panel.previewColor);
        ImGui::TextUnformatted("no matches");
        ImGui::PopStyleColor();
    }
    // End-of-frame snapshot of the rendered query for A23's revert
    // detection next frame (filterNonEmptyLastFrame); reads the final
    // buffer so the S11 rename-Esc restore is counted.
    panel.filterNonEmptyLastFrame = !isWhitespaceOnly(std::string(panel.filterBuf.data()));
    ImGui::EndChild(); // session_rows
    ImGui::EndChild(); // Session child window
}

int leftPanelWidth(const TuiState& s) {
    // One left-panel slot (A6/H1): the pipeline panel wins whenever it has
    // agents - open or collapsed; otherwise the session panel renders when
    // open (30 wide) and stays an 8-wide "Open" strip when closed, so the
    // output area never covers the panel's Open button (mirrors the
    // pipeline panel's collapsed width).
    return !s.left.agents.empty() ? s.left.panelW
                                  : (s.sessionPanel.panelOpen ? s.sessionPanel.panelW : 8);
}

void renderTabChat(TuiState& state, bool focusInput_, Log& log) {
    auto io = ImGui::GetIO();
    ImVec2 DisplaySize = io.DisplaySize;
    const auto inputBufLines =
        std::min(20, std::max(2, static_cast<int>(countNewLines(state.userQuery.inputBuf))));

    if (state.readyStatus)
        state.startProcesssingTime = std::chrono::system_clock::now();
    bool focusInput{focusInput_};

    auto outputArea = [&state, &log, &inputBufLines, &DisplaySize, &focusInput]() {
        // Clamp height to avoid negative values on very small terminals
        const int lpw = leftPanelWidth(state);
        ImVec2 outPos(lpw, 1.0f);
        ImVec2 outSize(DisplaySize.x - lpw - 1, std::max(1.0f, DisplaySize.y - 3 - inputBufLines));
        ImGui::SetCursorPos(outPos);
        ImGuiWindowFlags outFlags = ImGuiWindowFlags_HorizontalScrollbar;

        ImGui::BeginChild("llm_output", outSize, false, outFlags);

        const bool lastMsgIsTool = [&state]() {
            if (state.chat.outputLines.size() > 0) {
                const auto& lastEntry = state.chat.outputLines.back();
                return (lastEntry.type == ChatMessageType::ToolCall ||
                        lastEntry.type == ChatMessageType::ToolResponse);
            }
            return false;
        }();

        buildRenderGroups(state.chat);

        for (size_t gi = 0; gi < state.chat.renderGroups.size(); ++gi) {
            const auto& g = state.chat.renderGroups[gi];

            switch (g.kind) {
            case GroupKind::UserQuery:
                renderSingleHeader(state, g.start, DisplaySize, false, lastMsgIsTool);
                break;

            case GroupKind::FinalAnswer:
                renderSingleHeader(state, g.start, DisplaySize, true, lastMsgIsTool);
                break;

            case GroupKind::AssistantWork: {
                size_t msgCount = g.end - g.start;
                assert(msgCount > 0 && "AssistantWork group must contain at least one message");

                static std::string groupLabelBuf;
                groupLabelBuf.clear();
                groupLabelBuf = "Assistant: ";
                groupLabelBuf.append(std::to_string(msgCount));
                groupLabelBuf.append("##");
                groupLabelBuf.append(std::to_string(g.start));

                bool shouldOpen = isAssistantGroupOpen(g, state.chat.renderGroups, gi);
                auto flags = shouldOpen ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;

                const bool showHeader = state.chat.outputLineOpen.count(g.start) == 0;

                const auto colors = getHeaderColors(ChatMessageType::Assistant, state.chat.style);
                if (showHeader) {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.textColor);
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                }
                ImGui::PushStyleColor(ImGuiCol_Header, colors.bgColor);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, colors.bgHovered);
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, colors.bgActive);
                StyleColorGuard guard{HEADER_COLOR_COUNT};

                if (ImGui::CollapsingHeader(groupLabelBuf.c_str(), flags)) {
                    guard.pop();
                    state.chat.outputLineOpen.insert(g.start);

                    for (size_t i = g.start; i < g.end; ++i) {
                        ImGui::PushID(groupLabelBuf.c_str());
                        renderNestedMessage(state, i, DisplaySize);
                        ImGui::PopID();
                    }
                } else {
                    state.chat.outputLineOpen.erase(g.start);
                }
                break;
            }
            }
        }

        if (state.chat.hasStreamMsg) {
            renderSingleHeader(state, state.chat.streamMsg, DisplaySize, true, false, true, true);
        }

        bool autoScroll = state.autoScroll;

        if (state.left.activeAgent != -1 && state.left.activeAgent < state.left.agents.size()) {
            auto& agent = state.left.agents[state.left.activeAgent];
            size_t count{1};
            for (auto& msg : agent.messages) {
                renderAgentStream(state, count, agent.updateCnt + count - agent.messages.size(),
                                  agent.agentId, msg);
                ++count;
            }
            renderAgentStream(state, count, agent.updateCnt + 1, agent.agentId, agent.stream);
            autoScroll = state.left.autoScroll;
        }

        if (!state.readyStatus) {
            ImGui::Text("%s%ds", "Thinking ",
                        (std::chrono::system_clock::now() - state.startProcesssingTime).count() /
                            1000000000);
            ImGui::TextUnformatted("");
        }

        if (autoScroll) {
            ImGui::SetScrollHereY(1.0f);
        }

        // Auto-scroll detection: capture scroll state BEFORE EndChild so we
        // read the "output" child's actual scroll values.
        const float scrollY = ImGui::GetScrollY();
        const float scrollMax = ImGui::GetScrollMaxY();
        if (scrollMax > 0.0f) {
            bool x = (scrollY >= scrollMax - 1.0f);
            state.autoScroll = x;
            state.left.autoScroll = x;
        }

        ImGui::EndChild();
    };

    auto inputHistory = [&state = state.userQuery, &log, &io](bool isPageDown, bool isPageUp) {
        if (state.inputHistory.empty()) {
            return;
        }

        if (!(isPageUp || isPageDown)) {
            return;
        }

        bool setInput = false;
        if (isPageUp) {
            if (state.historyPos == -1) {
                // First press: save the draft so PageDown can restore it.
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

    auto inputArea = [&state, &inputBufLines, &inputHistory, &focusInput]() {
        float buttonWidth =
            ImGui::CalcTextSize("Send   ").x + ImGui::GetStyle().FramePadding.x * 2.0f;

        float inputWidth =
            ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x;
        inputWidth = std::max(0.0f, inputWidth);

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
            InputResizeCallback, &state.userQuery);
        if (!state.userQuery.newInputBufString.empty()) {
            state.userQuery.newInputBufString.clear();
        }

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
            // ImGui may insert a trailing newline (e.g. ctrl+enter).
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

    renderTabChatSessionPanel(state, log);
    renderTabChatLeftPanel(state.left, log);
    outputArea();
    inputArea();
    statusLine();
}

void renderTabLog(TuiState& state, Log& log) {
    ImVec2 DisplaySize = ImGui::GetIO().DisplaySize;
    ImVec2 childPos(0, 1.0f);
    ImVec2 childSize(DisplaySize.x, DisplaySize.y - 2);
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

    bool activateInputField = false;
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Chat")) {
            activateInputField = true;
            state.activeTab = ActiveTab::chat;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Log")) {
            state.activeTab = ActiveTab::log;
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    switch (state.activeTab) {
    case ActiveTab::chat:
        renderTabChat(state, activateInputField, log);
        break;
    case ActiveTab::log:
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

void tuiInitQueryHistory(TuiState& state, const std::vector<std::string>& history) {
    state.userQuery.inputHistory = history;
    state.userQuery.historyPos = -1;
}

} // namespace llmfun::tui
