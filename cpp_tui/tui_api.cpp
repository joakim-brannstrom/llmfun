#include "tui_api.h"
#include "tui.h"

#include "imtui/imtui-impl-ncurses.h"
#include "imtui/imtui-impl-text.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>
#include <unordered_set>
#include <vector>

struct TuiScreen {
    ImTui::TScreen* screen;
};

struct TuiState {
    ::llmfun::tui::TuiState* inner;
};

// String ownership tracking

static std::unordered_set<const char*> ownedPointers;

static void cleanupOwnedStrings() {
    for (auto ptr : ownedPointers) {
        std::free(const_cast<char*>(ptr));
    }
    ownedPointers.clear();
}

static struct CleanupRegistrar {
    CleanupRegistrar() { std::atexit(cleanupOwnedStrings); }
} cleanupRegistrar;

//  Thread-local error handling

static thread_local char lastError[512];
static thread_local bool hasError = false;

static void setLastError(const char* msg) {
    if (msg) {
        std::strncpy(lastError, msg, sizeof(lastError) - 1);
        lastError[sizeof(lastError) - 1] = '\0';
    } else {
        lastError[0] = '\0';
    }
    hasError = true;
}

#ifdef __cplusplus
extern "C" {
#endif

String String_New(const char* cstr) {
    if (!cstr)
        return {nullptr, 0};
    return String_NewBuf(cstr, std::strlen(cstr));
}

String String_NewBuf(const char* data, size_t len) {
    if (!data)
        return {nullptr, 0};
    if (len == 0)
        return {"", 0}; /* valid empty string, distinguishable from error */
    /* +1 to guarantee null-termination for safe C-string interop */
    char* buf = static_cast<char*>(std::malloc(len + 1));
    if (!buf)
        return {nullptr, 0};
    std::memcpy(buf, data, len);
    buf[len] = '\0';
    ownedPointers.insert(buf);
    return {buf, len};
}

void String_Free(String s) {
    if (!s.data)
        return;
    auto it = ownedPointers.find(s.data);
    if (it != ownedPointers.end()) {
        ownedPointers.erase(it);
        std::free(const_cast<char*>(s.data));
    } else {
        /* Critical fix: detect double-free or wrong-allocator misuse */
        setLastError(
            "String_Free: pointer not found in owned set (double-free or wrong allocator)");
    }
}

String tuiLastError(void) {
    if (!hasError)
        return {nullptr, 0};
    hasError = false; /* consume */
    return String_New(lastError);
}

/* Backend initialization guard — prevents crashes from calling
   backend functions before tuiInit() or after tuiShutdown(). */
static bool backendInitialized{false};

TuiScreen* tuiInit(void) {
    ImTui::TScreen* raw = nullptr;
    if (::llmfun::tui::tuiInit(&raw)) {
        backendInitialized = true;
        return new TuiScreen{raw};
    }
    setLastError("Failed to initialize TUI terminal");
    return nullptr;
}

void tuiShutdown(TuiScreen* screen) {
    if (!screen) {
        backendInitialized = false;
        setLastError("tuiShutdown called with NULL screen");
        return;
    }
    ::llmfun::tui::tuiShutdown(screen->screen);
    screen->screen = nullptr; /* prevent accidental reuse */
    delete screen;
    backendInitialized = false;
}

void markdownFormatCallback(const ImGui::MarkdownFormatInfo& markdownFormatInfo_, bool start_) {
    ImGui::defaultMarkdownFormatCallback(markdownFormatInfo_, start_);
}

TuiState* tuiCreateState(void) {
    ::llmfun::tui::TuiState* inner = nullptr;

    auto initMdConfig = [&inner](ImGui::MarkdownConfig& mdConfig) {
        // mdConfig.linkCallback =         nullptr;
        // mdConfig.tooltipCallback =      nullptr;
        // mdConfig.imageCallback =        nullptr;
        // mdConfig.linkIcon =             "";
        // #ifdef IMGUI_HAS_TEXTURES // used to detect dynamic font capability
        //     mdConfig.headingFormats[0] =    { nullptr, true,  fontSize * 1.1f };
        //     mdConfig.headingFormats[1] =    { nullptr, true,  fontSize };
        //     mdConfig.headingFormats[2] =    { nullptr, false, fontSize };
        // #else
        //     mdConfig.headingFormats[0] =    { nullptr, true };
        //     mdConfig.headingFormats[1] =    { nullptr, true };
        //     mdConfig.headingFormats[2] =    { nullptr, false };
        // #endif
        // mdConfig.userData =             inner;
        // mdConfig.formatCallback =       nullptr;
    };

    try {
        inner = new ::llmfun::tui::TuiState();
        TuiState* state = new TuiState{inner};
        // initMdConfig(inner->mdConfig);
        return state;
    } catch (const std::bad_alloc&) {
        delete inner;
        setLastError("Failed to allocate TUI state (out of memory)");
        return nullptr;
    }
}

void tuiDestroyState(TuiState* state) {
    if (!state)
        return;
    delete state->inner;
    delete state;
}

// Backend frame. render, main-thread onl

void tuiBackendNewFrame(void) {
    if (!backendInitialized) {
        setLastError("Backend not initialized. Call tuiInit() first.");
        return;
    }
    ImTui_ImplNcurses_NewFrame();
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
}

void tuiBackendRender(TuiScreen* screen) {
    if (!backendInitialized) {
        setLastError("Backend not initialized. Call tuiInit() first.");
        return;
    }
    if (!screen)
        return;
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), screen->screen);
    ImTui_ImplNcurses_DrawScreen();
}

int tuiRender(TuiState* state) {
    if (!state || !state->inner)
        return 0;
    return ::llmfun::tui::tuiRender(*state->inner) ? 1 : 0;
}

void tuiSetLogging(TuiState* state, int onOff) {
    if (!state || !state->inner)
        return;
    ::llmfun::tui::tuiSetLogging(*state->inner, onOff != 0);
}

void tuiSetIniFilename(TuiState* state, String filename) {
    if (!state || !state->inner)
        return;
    std::string filename_(filename.data ? filename.data : "", filename.data ? filename.len : 0);
    ::llmfun::tui::tuiSetIniFilename(*state->inner, filename_);
}

void tuiAddLogMessage(TuiState* state, String summary, String text) {
    if (!state || !state->inner)
        return;
    std::string summary_(summary.data ? summary.data : "", summary.data ? summary.len : 0);
    std::string text_(text.data ? text.data : "", text.data ? text.len : 0);
    ::llmfun::tui::tuiAddLogMessage(*state->inner, ::llmfun::tui::LogMessage{summary_, text_});
}

void tuiAddChatMessage(TuiState* state, ChatMessageParam param) {
    if (!state || !state->inner)
        return;
    if (param.type < 0 || param.type >= TuiChatMessageType_Count) {
        param.type = TuiChatMessageType_Assistant;
    }
    std::string summary_(param.summary.data ? param.summary.data : "",
                         param.summary.data ? param.summary.len : 0);
    std::string text_(param.text.data ? param.text.data : "", param.text.data ? param.text.len : 0);
    std::string thinking_(param.thinking.data ? param.thinking.data : "",
                          param.thinking.data ? param.thinking.len : 0);
    auto cppType = static_cast<::llmfun::tui::ChatMessageType>(param.type);
    ::llmfun::tui::tuiAddOutputLine(
        *state->inner, ::llmfun::tui::ChatMessage{summary_, text_, thinking_, cppType,
                                                  state->inner->chat.outputLineNextId++});
}

void tuiClearChatMessages(TuiState* state) {
    if (!state || !state->inner)
        return;
    ::llmfun::tui::tuiClearOutput(*state->inner);
}

void tuiSetStatusText(TuiState* state, String text) {
    if (!state || !state->inner)
        return;
    std::string s(text.data ? text.data : "", text.data ? text.len : 0);
    ::llmfun::tui::tuiSetStatusText(*state->inner, s);
}

String tuiGetInput(TuiState* state) {
    if (!state || !state->inner)
        return {nullptr, 0};
    std::string s = ::llmfun::tui::tuiGetInput(*state->inner);
    return String_NewBuf(s.data(), s.size());
}

void tuiClearInput(TuiState* state) {
    if (!state || !state->inner)
        return;
    ::llmfun::tui::tuiClearInput(*state->inner);
}

int tuiIsSubmitReady(TuiState* state) {
    if (!state || !state->inner)
        return 0;
    return ::llmfun::tui::tuiIsSubmitReady(*state->inner) ? 1 : 0;
}

void tuiResetSubmit(TuiState* state) {
    if (!state || !state->inner)
        return;
    ::llmfun::tui::tuiResetSubmit(*state->inner);
}

String tuiGetSubmitQuery(TuiState* state) {
    if (!state || !state->inner)
        return {nullptr, 0};
    std::string s = ::llmfun::tui::tuiGetSubmitQuery(*state->inner);
    return String_NewBuf(s.data(), s.size());
}

int tuiGetAutoScroll(TuiState* state) {
    if (!state || !state->inner)
        return 0;
#ifndef NDEBUG
    assert(backendInitialized &&
           "tuiGetAutoScroll must be called from main thread after tuiInit()");
#endif
    return state->inner->autoScroll ? 1 : 0;
}

void tuiSetAutoScroll(TuiState* state, int enabled) {
    if (!state || !state->inner)
        return;
#ifndef NDEBUG
    assert(backendInitialized &&
           "tuiSetAutoScroll must be called from main thread after tuiInit()");
#endif
    state->inner->autoScroll = enabled != 0;
}

void tuiReadyStatus(TuiState* state, int ready) {
    if (!state || !state->inner)
        return;
    state->inner->readyStatus = ready == 1;
}

#ifdef __cplusplus
}
#endif
