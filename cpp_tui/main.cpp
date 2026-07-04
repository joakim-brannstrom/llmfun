#include "tui_api.h"

#include <cstdio>
#include <cstring>
#include <string>

/* Helper: build a String from a null-terminated C string literal. */
static inline String makeStr(const char* s) { return String{s, std::strlen(s)}; }

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
        std::string text{"hello\nthis is some much longer text"};
        text.append(std::to_string(i));
        tuiAddChatMessage(state, makeStr(summary.c_str()), makeStr(text.c_str()));
    }

    while (true) {
        tuiBackendNewFrame();

        if (tuiRender(state) == 0)
            break;

        if (tuiIsSubmitReady(state) != 0) {
            String query = tuiGetSubmitQuery(state);
            if (query.data && query.len > 0) {
                std::string text(query.data, query.len);
                std::string summary = text.size() > 30 ? text.substr(0, 30) : text;
                tuiAddChatMessage(state, String{summary.c_str(), summary.size()},
                                  String{text.c_str(), text.size()});
                std::string log = std::to_string(text.size());
                tuiAddLogMessage(state, String{summary.c_str(), summary.size()},
                                 String{log.c_str(), log.size()});
            }
            String_Free(query);
            tuiResetSubmit(state);
        }

        tuiBackendRender(screen);
    }

    tuiDestroyState(state);
    tuiShutdown(screen);
    return 0;
}
