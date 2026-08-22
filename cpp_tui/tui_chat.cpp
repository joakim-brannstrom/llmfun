/// Definitions for the chat/log render cluster (tui_chat.h).
#include "tui_chat.h"
#include "session_panel.h"
#include "tui.h"
#include "tui_common.h"
#include "tui_widgets.h"

#include "imgui/imgui_internal.h"
#include "imgui_markdown.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <string>
#include <vector>

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

static std::string makeUniqueId(const std::string& base, const std::string& suffix, int i) {
    auto s = base;
    s.append("##");
    s.append(suffix);
    s.append(std::to_string(i));
    return s;
}

static bool renderAsMarkdown(const ChatMessageType type) {
    if (type == ChatMessageType::ToolCall || type == ChatMessageType::ToolResponse)
        return false;
    return true;
}

static size_t countNewLines(const std::string& str) {
    return std::count(str.begin(), str.end(), '\n');
}

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

static void markdownFormatCallback(const ImGui::MarkdownFormatInfo& markdownFormatInfo_,
                                   bool start_) {
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

static void markdownLinkCallback(ImGui::MarkdownLinkCallbackData data_) {
    std::string url(data_.link, data_.linkLength);
    if (!data_.isImage) {
        ImGui::SetClipboardText(url.c_str());
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

static int InputResizeCallback(ImGuiInputTextCallbackData* data) {
    auto* udata = reinterpret_cast<UserQueryState*>(data->UserData);

    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        udata->inputBuf.resize(data->BufSize);
        data->Buf = udata->inputBuf.data();
    }

    return 0;
}

static void renderTabChatLeftPanel(ChatTabLeftPanel& panel, Log& log) {
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

static void renderTabChat(TuiState& state, bool focusInput_, Log& log) {
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

static void renderTabLog(TuiState& state, Log& log) {
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

} // namespace llmfun::tui
