#include "tui_api.h"
#include "tui.h"

#include "imtui/imtui-impl-ncurses.h"
#include "imtui/imtui-impl-text.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <unordered_set>
#include <utility>
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

TuiState* tuiCreateState(void) {
    ::llmfun::tui::TuiState* inner = nullptr;

    try {
        inner = new ::llmfun::tui::TuiState();
        TuiState* state = new TuiState{inner};
        ::llmfun::tui::initMarkdownConfig(*state->inner);
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

void tuiUpdateStreamChatMessage(TuiState* state, ChatMessageParam param) {
    if (!state || !state->inner)
        return;
    std::string summary_;
    std::string text_(param.text.data ? param.text.data : "", param.text.data ? param.text.len : 0);
    std::string thinking_(param.thinking.data ? param.thinking.data : "",
                          param.thinking.data ? param.thinking.len : 0);
    auto cppType = ::llmfun::tui::ChatMessageType::Assistant;
    ::llmfun::tui::tuiUpdateStreamChatMessage(
        *state->inner, ::llmfun::tui::ChatMessage{summary_, text_, thinking_, cppType,
                                                  state->inner->chat.StreamChatMessageId});
}

void tuiStreamChatMessageClear(TuiState* state) {
    if (!state || !state->inner)
        return;
    ::llmfun::tui::tuiStreamChatMessageClear(*state->inner);
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

void tuiInitQueryHistory(TuiState* state, const String* history, size_t count) {
    if (!state || !state->inner)
        return;
    std::vector<std::string> vec;
    if (history && count > 0) {
        const size_t maxEntries = state->inner->userQuery.MAX_HISTORY;
        const size_t n = count < maxEntries ? count : maxEntries;
        const size_t startAt = count < maxEntries ? 0 : (count - maxEntries);
        vec.reserve(n);
        for (size_t i = startAt; i < n; ++i) {
            const String* s = &history[i];
            if (s->data) {
                vec.emplace_back(s->data, s->len);
            } else {
                vec.emplace_back();
            }
            if (vec.size() > maxEntries) {
                vec.erase(vec.begin());
            }
        }
    }
    ::llmfun::tui::tuiInitQueryHistory(*state->inner, vec);
}

void tuiPipelineAgentUpdate(TuiState* state, String agentId, PipelineChatMessage msg) {
    if (!state || !state->inner)
        return;
    std::string id(agentId.data ? agentId.data : "", agentId.data ? agentId.len : 0);
    if (id.empty())
        return;
    std::string c(msg.content.data ? msg.content.data : "", msg.content.data ? msg.content.len : 0);
    std::string r(msg.reasoning.data ? msg.reasoning.data : "",
                  msg.reasoning.data ? msg.reasoning.len : 0);
    std::string role_(msg.role.data ? msg.role.data : "", msg.role.data ? msg.role.len : 0);
    std::string fr(msg.finishReason.data ? msg.finishReason.data : "",
                   msg.finishReason.data ? msg.finishReason.len : 0);
    std::string st(msg.status.data ? msg.status.data : "", msg.status.data ? msg.status.len : 0);

    auto& agents = state->inner->left.agents;
    for (auto& agent : agents) {
        if (agent.agentId == id) {
            agent.stream.content = c;
            agent.stream.thinking = r;
            agent.stream.role = role_;
            agent.stream.finishReason = fr;
            agent.stream.status = st;
            agent.lastUpdate = std::chrono::system_clock::now();
            agent.activity = true;
            return;
        }
    }

    // Not found - auto-register
    // If at capacity, evict the agent with the oldest lastUpdate
    if (agents.size() >= state->inner->left.MaxAgents) {
        auto oldestIt = std::min_element(
            agents.begin(), agents.end(),
            [](const ::llmfun::tui::AgentStream& a, const ::llmfun::tui::AgentStream& b) {
                return a.lastUpdate < b.lastUpdate;
            });
        agents.erase(oldestIt);
    }

    agents.emplace_back(::llmfun::tui::AgentStream{
        id,
        ::llmfun::tui::AgentStreamMessage{c, r, role_, fr},
        std::chrono::system_clock::now(),
        1,
        true,
        std::vector<::llmfun::tui::AgentStreamMessage>{},
    });
}

void tuiPipelineAgentDone(TuiState* state, String agentId) {
    if (!state || !state->inner)
        return;
    std::string id(agentId.data ? agentId.data : "", agentId.data ? agentId.len : 0);
    if (id.empty())
        return;

    auto& agents = state->inner->left.agents;
    for (auto& agent : agents) {
        if (agent.agentId == id) {
            agent.finished();
            // Note: lastUpdate is NOT refreshed — done agents are evicted
            // first (LRU policy) if capacity is exceeded.
            return;
        }
    }
}

void tuiPipelineClear(TuiState* state) {
    if (!state || !state->inner)
        return;
    state->inner->left.agents.clear();
}

/* Compile-time linkage between the C enum (tui_api.h) and the internal C++
 * mirror (tui.h): both lists are append-only and must stay in sync, or the
 * static_cast in tuiGetSessionAction would silently mis-map future values.
 */
static_assert(static_cast<int>(::llmfun::tui::SessionActionType::None) == TuiSessionAction_None,
              "C/C++ session action enum mismatch (None)");
static_assert(static_cast<int>(::llmfun::tui::SessionActionType::Select) == TuiSessionAction_Select,
              "C/C++ session action enum mismatch (Select)");
static_assert(static_cast<int>(::llmfun::tui::SessionActionType::New) == TuiSessionAction_New,
              "C/C++ session action enum mismatch (New)");
static_assert(static_cast<int>(::llmfun::tui::SessionActionType::Rename) == TuiSessionAction_Rename,
              "C/C++ session action enum mismatch (Rename)");
static_assert(static_cast<int>(::llmfun::tui::SessionActionType::Delete) == TuiSessionAction_Delete,
              "C/C++ session action enum mismatch (Delete)");

/* ---- Session sidebar ---- */

void tuiSetSessionList(TuiState* state, const SessionItem* items, size_t count) {
    if (!state || !state->inner)
        return;
    auto& panel = state->inner->sessionPanel;
    // The previous active id, for the stale-pending rule (M3): a slash
    // /switch typed while busy changes the active session, and the queued
    // click must not override the user's explicit switch.
    const std::string prevActiveId = panel.activeId;
    panel.sessions.clear();
    panel.activeId.clear();
    if (items && count > 0) {
        panel.sessions.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const SessionItem& it = items[i];
            ::llmfun::tui::SessionEntry e;
            e.id = it.id.data ? std::string(it.id.data, it.id.len) : std::string();
            e.title = it.title.data ? std::string(it.title.data, it.title.len) : std::string();
            e.preview =
                it.preview.data ? std::string(it.preview.data, it.preview.len) : std::string();
            e.messageCount = it.messageCount;
            e.isActive = it.isActive != 0;
            if (e.isActive)
                panel.activeId = e.id; // at most one active entry expected; last wins
            panel.sessions.push_back(std::move(e));
        }
    }
    // Two-step delete confirmation: drop the pending id when the target
    // session is no longer in the snapshot.
    if (!panel.pendingDeleteId.empty()) {
        const std::string& pending = panel.pendingDeleteId;
        auto found = std::find_if(
            panel.sessions.begin(), panel.sessions.end(),
            [&pending](const ::llmfun::tui::SessionEntry& s) { return s.id == pending; });
        if (found == panel.sessions.end())
            panel.pendingDeleteId.clear();
    }
    // Pending-switch slot (A12): drop the queued id when its target left
    // the snapshot (mirror of the pendingDeleteId rule above) or when the
    // active session changed since the previous snapshot (M3) - a slash
    // /switch typed while busy wins over the queued click.
    if (!panel.pendingSelectId.empty()) {
        const std::string& pending = panel.pendingSelectId;
        auto found = std::find_if(
            panel.sessions.begin(), panel.sessions.end(),
            [&pending](const ::llmfun::tui::SessionEntry& s) { return s.id == pending; });
        if (found == panel.sessions.end() || panel.activeId != prevActiveId)
            panel.pendingSelectId.clear();
    }
    // The rename box binds to the active row; when the active row is no
    // longer in the snapshot (deleted between refreshes, or the snapshot
    // has no active entry at all), close the box - an open box bound to a
    // missing row is dead-but-harmless state (Task 8 tracked fix).
    if (panel.renameActive) {
        auto found = std::find_if(
            panel.sessions.begin(), panel.sessions.end(),
            [&panel](const ::llmfun::tui::SessionEntry& s) { return s.id == panel.activeId; });
        if (found == panel.sessions.end()) {
            panel.renameActive = false;
            panel.renameFocus = false;
        }
    }
}

int tuiIsSessionActionReady(TuiState* state) {
    if (!state || !state->inner)
        return 0;
    return state->inner->sessionPanel.actions.empty() ? 0 : 1;
}

SessionAction tuiGetSessionAction(TuiState* state) {
    if (!state || !state->inner)
        return {TuiSessionAction_None, {nullptr, 0}, {nullptr, 0}};
    auto& actions = state->inner->sessionPanel.actions;
    if (actions.empty())
        return {TuiSessionAction_None, {nullptr, 0}, {nullptr, 0}};
    ::llmfun::tui::SessionAction a = std::move(actions.front());
    actions.pop_front();
    SessionAction out{};
    out.type = static_cast<TuiSessionActionType>(a.type);
    // On OOM, String_NewBuf returns {NULL, 0}, indistinguishable from an
    // empty field; mirrors the API's existing failure model (only
    // tuiCreateState reports allocation failure).
    if (!a.sessionId.empty())
        out.sessionId = String_NewBuf(a.sessionId.data(), a.sessionId.size());
    if (!a.title.empty())
        out.title = String_NewBuf(a.title.data(), a.title.size());
    return out;
}
#ifdef __cplusplus
}
#endif
