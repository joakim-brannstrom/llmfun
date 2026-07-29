#include "tui_api.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

/* Helper: build a String from a null-terminated C string literal. */
String makeStr(const char* s) { return String{s, std::strlen(s)}; }
String makeStr(std::string s) { return String{s.data(), s.size()}; };

int main() {
    TuiScreen* screen = nullptr;

    screen = tuiInit();
    if (!screen) {
        std::fprintf(stderr, "Failed to initialize TUI. Check terminal compatibility.\n");
        String err = tuiLastError();
        if (err.data && err.len > 0) {
            std::fprintf(stderr, "Error: %.*s\n", (int)err.len, err.data);
            String_Free(err);
        }
        return 1;
    }

    TuiState* state = tuiCreateState();
    if (!state) {
        std::fprintf(stderr, "Failed to create TUI state.\n");
        tuiShutdown(screen);
        return 1;
    }

    tuiSetLogging(state, true);
    tuiSetStatusText(state, makeStr("Context: 0/0 tokens | Model: none | Ready"));
    tuiSetIniFilename(state, makeStr("imgui2.ini"));

    for (int i = 0; i < 300; ++i) {
        std::string summary{"hello"};
        std::string text{
            u8"**hello**\nthis is *some* much\n# Heading\nlonger text '😜' 'ö' \n\n***\n\n"};
        text.append(std::to_string(i));

        TuiChatMessageType type = TuiChatMessageType_Assistant;
        if (i % 15 == 0) {
            type = TuiChatMessageType_FinalAnswer;
        } else if (i % 10 == 0) {
            type = TuiChatMessageType_User;
        }
        tuiAddChatMessage(state, ChatMessageParam{makeStr(summary.c_str()), makeStr(text.c_str()),
                                                  makeStr("thinking..."), type});

        // if (i == 299) {
        // tuiAddChatMessage(state, ChatMessageParam{makeStr(summary.c_str()),
        // makeStr(text.c_str()), makeStr("thinking..."), TuiChatMessageType_FinalAnswer});
        // }
    }

    tuiAddChatMessage(
        state,
        ChatMessageParam{
            makeStr("a code block"),
            makeStr("# Heading1\nsmurf\n[a link](to somewhere)\n\n## Heading2\nsmurf\n\n### "
                    "Heading 3\nsmurf\n\nSome content "
                    "before.\n**bold**\n*italic*\n\n```json\n{\"key\": \"value\", \"nested\": [1, "
                    "2, 3]}\n{\"another\": true}\n```\n\nText after code block.\n"),
            makeStr("Some thinking before.\n\n```json\n{\"key\": \"value\", \"nested\": [1, "
                    "2, 3]}\n{\"another\": true}\n```\n\nText after code block.\n"),
            TuiChatMessageType_Assistant});

    auto addPipelineAgents = [&state]() {
        for (int ii = 0; ii < 40; ++ii) {
            const auto i = ii % 16;
            std::string agentId{"agent "};
            agentId.append(std::to_string(i));

            std::string content{"hello"};
            std::string reasoning{
                u8"**hello**\nthis is *some* much\n# Heading\nlonger text '😜' 'ö' \n\n***\n\n"};
            reasoning.append(std::to_string(i));

            std::string role{"assistant"};
            std::string finishReason = (ii % 3 == 0) ? "work" : "done";

            tuiPipelineAgentUpdate(state, makeStr(agentId),
                                   PipelineChatMessage{makeStr(content), makeStr(reasoning),
                                                       makeStr(role), makeStr(finishReason)});
        }

        for (int i = 7; i < 9; ++i) {
            std::string agentId{"agent "};
            agentId.append(std::to_string(i));
            tuiPipelineAgentDone(state, makeStr(agentId));
        }
    };

    auto pipelineShow = true;
    auto pipelineShowAt = std::chrono::system_clock::now() + std::chrono::seconds{1};

    while (true) {
        tuiBackendNewFrame();

        if (tuiRender(state) == 0)
            break;

        if (pipelineShow && std::chrono::system_clock::now() > pipelineShowAt) {
            pipelineShow = false;
            addPipelineAgents();
        }

        if (tuiIsSubmitReady(state) != 0) {
            String query_ = tuiGetSubmitQuery(state);
            std::string query;
            if (query_.data && query_.len > 0) {
                query = std::string{query_.data, query_.len};
            }
            String_Free(query_);

            if (!query.empty()) {
                if (query.rfind("/pipeline show", 0) == 0) {
                    addPipelineAgents();
                } else if (query.rfind("/pipeline clear", 0) == 0) {
                    tuiPipelineClear(state);
                } else {
                    std::string text = query;
                    std::string summary = text.size() > 30 ? text.substr(0, 30) : text;
                    tuiAddChatMessage(
                        state, ChatMessageParam{String{summary.c_str(), summary.size()},
                                                String{text.c_str(), text.size()},
                                                makeStr("thinking..."), TuiChatMessageType_User});
                    std::string log = std::to_string(text.size());
                    tuiAddLogMessage(state, String{summary.c_str(), summary.size()},
                                     String{log.c_str(), log.size()});
                }
            }
            tuiResetSubmit(state);
        }

        tuiBackendRender(screen);
    }

    tuiDestroyState(state);
    tuiShutdown(screen);
    return 0;
}
