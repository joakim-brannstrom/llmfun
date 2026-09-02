#include "tui_api.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

/* Helper: build a String from a null-terminated C string literal. */
String makeStr(const char* s) { return String{s, std::strlen(s)}; }
String makeStr(std::string s) { return String{s.data(), s.size()}; };

int main(int argc, char** argv) {
    // Headless smoke mode: --frames N (or --smoke = --frames 30) renders
    // exactly N frames then exits 0, so CI can exercise the session panel
    // and the action queue without a human. Without the argument the loop
    // below runs until the user quits.
    int maxFrames = -1;
    if (argc >= 3 && std::strcmp(argv[1], "--frames") == 0) {
        char* end = nullptr;
        long n = std::strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || n < 0) {
            std::fprintf(stderr, "error: --frames requires a non-negative integer\n");
            return 2;
        }
        maxFrames = static_cast<int>(n);
    } else if (argc == 2 && std::strcmp(argv[1], "--smoke") == 0) {
        maxFrames = 30;
    } else if (argc > 1) {
        std::fprintf(stderr, "usage: %s [--frames N | --smoke]\n", argv[0]);
        return 2;
    }

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

    // Standalone max-width cap via LLMFUN_TUI_MAX_WIDTH (env var only, no
    // CLI flag). Debugging aid: the user-facing config is the D YAML
    // tui.maxWidth. Tolerant parse: unset, empty, non-numeric, or negative
    // values are ignored (0 = unlimited); positive sub-40 caps are raised
    // to 40 by the C API (the TUI's minimum render width).
    if (const char* mwEnv = std::getenv("LLMFUN_TUI_MAX_WIDTH")) {
        char* end = nullptr;
        long mw = std::strtol(mwEnv, &end, 10);
        if (end != mwEnv && *end == '\0' && mw >= 0) {
            // Mirror validateConfig's upper bound so the int narrowing below
            // is well-defined (the standalone path bypasses D validation).
            if (mw > 10000)
                mw = 10000;
            tuiSetMaxWidth(state, static_cast<int>(mw));
        }
    }

    // Headless smoke seed: session sidebar snapshot so the panel renders
    // rows and the action poll can be exercised. Inbound strings are copied
    // by tuiSetSessionList, so the literals may go out of scope.
    if (maxFrames >= 0) {
        SessionItem sessions[] = {
            {makeStr("20260815-100000-aaaa"), makeStr("Alpha session"),
             makeStr("first user preview"), 3, 1},
            {makeStr("20260815-100000-bbbb"), makeStr("Beta session"),
             makeStr("second user preview"), 5, 0},
            {makeStr("20260815-100000-cccc"), makeStr("Gamma session"), makeStr(""), 0, 0},
        };
        tuiSetSessionList(state, sessions, sizeof(sessions) / sizeof(sessions[0]));
    }

    for (int i = 0; i < 300; ++i) {
        std::string summary{"hello"};
        std::string text{u8"**hello**\nthis is *some* much\n# Heading\nlonger text '😜' 'ö' "
                         u8"\n\n'⚠️'\n✅\n\n***\n\n"};
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

    // not using markdown for long line, which should mean it uses automatic line break
    tuiAddChatMessage(
        state,
        ChatMessageParam{
            makeStr("a long line"),
            makeStr(
                "# Heading1\nsmurf\n[a link](to somewhere)\n\n## Heading2\nsmurf\n\n"
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat\n"),
            makeStr(
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                "long cat, long cat\n"),
            TuiChatMessageType_Assistant});

    // using markdown for long line, which should mean it should not automatically line break
    tuiAddChatMessage(
        state,
        ChatMessageParam{
            makeStr("a long line"),
            makeStr(
                "# Heading1\nsmurf\n[a link](to somewhere)\n\n## Heading2\nsmurf\n\n"
                "```\nlong cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat\n```\n"),
            makeStr(
                "```\nlong cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat, long cat, long cat, long cat, long cat, long cat, long "
                "cat, long cat, long cat\n```\n"),
            TuiChatMessageType_Assistant});

    auto addPipelineAgents = [&state]() {
        for (int ii = 0; ii < 40; ++ii) {
            const auto i = ii % 16;
            std::string agentId{"agent "};
            agentId.append(std::to_string(i));

            std::string content{"hello"};
            std::string reasoning{u8"**hello**\nthis is *some* much\n# Heading\nlonger text '😜' "
                                  u8"'ö' \n'⚠️'\n✅\n \n***\n\n"};
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

    // Frame-count exit for --frames N; the body is identical to the
    // interactive loop.
    int frame = 0;
    while (true) {
        if (maxFrames >= 0 && frame >= maxFrames)
            break;

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
        ++frame;
    }

    if (maxFrames >= 0) {
        // Headless smoke: exercise the session action poll. Nothing was
        // clicked in the dry-run, so the queue must be empty and the pop
        // must return the None sentinel with NULL strings (String_Free is
        // a no-op on them). This proves tuiIsSessionActionReady /
        // tuiGetSessionAction work end-to-end in the executable.
        if (tuiIsSessionActionReady(state) != 0) {
            std::fprintf(stderr, "error: expected empty session action queue\n");
            tuiDestroyState(state);
            tuiShutdown(screen);
            return 1;
        }
        SessionAction action = tuiGetSessionAction(state);
        String_Free(action.sessionId);
        String_Free(action.title);
        if (action.type != TuiSessionAction_None) {
            std::fprintf(stderr, "error: expected TuiSessionAction_None sentinel\n");
            tuiDestroyState(state);
            tuiShutdown(screen);
            return 1;
        }
        std::printf("smoke ok: rendered %d frames, session action queue empty (None sentinel)\n",
                    frame);
    }

    tuiDestroyState(state);
    tuiShutdown(screen);
    return 0;
}
