#include "tui_api.h"
#include "tui.h"
#include "tui_backend.h"

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

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

/* ----------------------------------------------------------------------------
 * The Agents tab's mirror (task 30, design §5.21).
 *
 * `views/` may not name a v4 type (D-ADAPTER), so the two pipeline calls that carry state
 * copy what a lane needs into `UiState::agents` as it arrives: the tab then draws a value
 * list it owns instead of reading the v4 store through a seam that does not exist. No new ABI
 * symbol is involved - `tuiPipelineAgentUpdate/Done/Clear` are the surface D has always had;
 * task 39a widened the lane's own message struct, so the number and the tool name arrive as
 * fields instead of being parsed out of the stream's own text.
 *
 * The body is `model::applyAgentUpdate()`/`finishAgentLane()`: what a lane *is* is a model
 * answer, and it is testable without a C surface in the way (`./test_agents_tab`).
 * -------------------------------------------------------------------------- */

namespace {

/// One lane update, with the clock read once so the create path and the update path cannot
/// stamp a lane from two different instants. The count and the tool name are the fields D sent:
/// nothing here reads the stream's text (D-STATUS, task 39a).
void mirrorLaneUpdate(::llmfun::tui::TuiState& state, const std::string& id,
                      const std::string& role, const std::string& content,
                      const std::string& currentTool, std::int64_t contextUsed) {
    ::llmfun::tui::model::applyAgentUpdate(state.ui, id, role, content, currentTool, contextUsed,
                                           ::llmfun::tui::model::agentClockMs());
}

} // namespace

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

// TODO: global variables are bad.
/* v4 init guard: the one state whose backend is attached (at most one
   initialized state per process). Set by tuiInit, cleared by tuiDestroyState. */
static TuiState* attachedState = nullptr;

// TODO: llmfun may execute the TUI on different threads
/* Fallback note of the most recent tuiInit on the calling thread (empty when
   no fallback happened); read via tuiBackendNote(). */
static thread_local std::string backendNote;

int tuiInit(TuiState* state, TuiBackendMode mode) {
    backendNote.clear();

    if (!state || !state->inner) {
        setLastError("tuiInit: NULL state");
        return 1;
    }
    if (state->inner->backend) {
        setLastError("tuiInit: state is already initialized");
        return 1;
    }
    if (attachedState != nullptr) {
        setLastError("tuiInit: another state is already initialized (at most one per process)");
        return 1;
    }

    // Create the ImGui context for this state (destroyed by tuiDestroyState;
    // headless harnesses never call tuiInit and keep their own). setlocale
    // mirrors the pre-v4 init order.
    setlocale(LC_ALL, "");

    IMGUI_CHECKVERSION();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::GetIO().IniFilename =
        state->inner->iniFilename.empty() ? nullptr : state->inner->iniFilename.c_str();
    state->inner->ownedContext = context;

    std::unique_ptr<::llmfun::tui::Backend> backend;
    std::string error;
    bool initialized = false;

    if (mode == TuiBackendMode_Tui) {
        // Rich context + font warm-up BEFORE the text backend init, which
        // builds and locks the atlas (fonts added after lock would bake at
        // the wrong size).
        ::llmfun::tui::initMarkdownTextMode(*state->inner);
        backend = ::llmfun::tui::makeNcursesBackend();
        initialized = backend->init(error);
    } else if (mode == TuiBackendMode_Gui) {
        backend = ::llmfun::tui::makeGuiBackend(state->inner->iniFilename, state->inner->settings);
        initialized = backend->init(error);
        if (initialized)
            ::llmfun::tui::initMarkdownGuiMode(*state->inner);
    } else {
        // Auto (also the defensive default for unknown values): try the GUI
        // backend first. On failure, fall back to the terminal UI iff both
        // stdio ends are TTYs — a full-screen TUI drawn into a pipe is
        // useless, so there the GUI error is the result.
        // A failed GUI init() has already loaded the settings into the shared
        // record (loadTuiSettings runs before window creation), so a TTY
        // fallback inherits the loaded font scale. Harmless: text mode ignores
        // it, and a failed init never saves.
        backend = ::llmfun::tui::makeGuiBackend(state->inner->iniFilename, state->inner->settings);
        initialized = backend->init(error);
        if (initialized) {
            ::llmfun::tui::initMarkdownGuiMode(*state->inner);
        } else {
            if (isatty(0) && isatty(1)) {
                const std::string note =
                    "GUI unavailable: " + error + "; falling back to terminal UI";
                std::fprintf(stderr, "%s\n", note.c_str());
                ::llmfun::tui::initMarkdownTextMode(*state->inner);
                backend = ::llmfun::tui::makeNcursesBackend();
                std::string tuiError;
                if (backend->init(tuiError)) {
                    backendNote = note;
                    initialized = true;
                } else {
                    error = "GUI unavailable: " + error + "; terminal UI also failed: " + tuiError;
                }
            }
        }
    }

    if (!initialized) {
        // Clean unwind: the state stays backend-less and retryable.
        ::llmfun::tui::destroyMarkdown(*state->inner);
        ImGui::DestroyContext(context);
        state->inner->ownedContext = nullptr;
        setLastError(error.empty() ? "tuiInit: backend initialization failed" : error.c_str());
        return 1;
    }

    state->inner->backend = std::move(backend);
    attachedState = state;

    // Theme AFTER the backend inits: ImTui_ImplText_Init() resets several
    // style colors (notably Colors[ImGuiCol_NavHighlight] = (0,0,0,0), which
    // aliases ImGuiCol_NavCursor in ImGui 1.91.4+), so applying the theme
    // before the backend would let the backend undo the NavCursor override.
    // applyUiTheme() also picks the colour depth for the active profile
    // (ui/tokens.h): once per state, never per frame.
    ::llmfun::tui::applyUiTheme();
    // Pin the text-grid default font after the theme: the whole UI (not just
    // markdown) must stay at 1-cell metrics.
    if (::llmfun::tui::tuiIsTextGrid())
        ::llmfun::tui::finishMarkdownTextMode(*state->inner);

    return 0;
}

TuiState* tuiCreateState(void) {
    ::llmfun::tui::TuiState* inner = nullptr;

    try {
        inner = new ::llmfun::tui::TuiState();
        TuiState* state = new TuiState{inner};
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
    if (state->inner) {
        // The close path's settings save (D-UI-CONF, task 29): a GUI window closed with its X
        // ends the D loop through `shouldClose()` and never sets `quitRequested`, so the quit
        // path's save does not run - this is the one place every exit goes through. It carries a
        // change and nothing else, and it is idempotent with the quit path's save.
        ::llmfun::tui::saveUiConfOnShutdown(*state->inner);
        // The archive handle (task 32): every record line is flushed as it is written, and a
        // quit closes the descriptor the record path opened lazily.
        ::llmfun::tui::closeLogArchive(*state->inner);
        if (state->inner->backend) {
            state->inner->backend->shutdown();
            state->inner->backend.reset();
        }
        // Destroy only the context tuiInit created FOR THIS state; a context
        // created by the caller (headless test harness) must survive.
        if (state->inner->ownedContext != nullptr) {
            ImGui::DestroyContext(state->inner->ownedContext);
            state->inner->ownedContext = nullptr;
        }
        if (attachedState == state)
            attachedState = nullptr;
        delete state->inner;
    }
    delete state;
}

void tuiBackendNewFrame(TuiState* state) {
    // None-safe (v4): NULL and backend-less states are legal and inert.
    if (!state || !state->inner || !state->inner->backend)
        return;
    state->inner->backend->newFrame();
}

void tuiBackendRender(TuiState* state) {
    if (!state || !state->inner || !state->inner->backend)
        return;
    state->inner->backend->renderFrame();
}

int tuiRender(TuiState* state) {
    if (!state || !state->inner)
        return 0;
    // None-safety: a backend-less state is inert — no ImGui access, keep
    // going (deliberate asymmetry with the NULL case above, which stays on
    // the v3 exit convention).
    if (!state->inner->backend)
        return 1;
    const bool continueRender = ::llmfun::tui::tuiRender(*state->inner);
    // A closed GUI window (backend exit request) ends the loop.
    return (continueRender && !state->inner->backend->shouldClose()) ? 1 : 0;
}

/* Compile-time linkage between the C enums (tui_api.h) and the internal C++
 * mirror (tui_backend.h): both lists are append-only and must stay in sync,
 * or tuiBackendActive would silently mis-report the attached backend.
 */
static_assert(static_cast<int>(::llmfun::tui::BackendKind::None) == TuiBackendKind_None,
              "C/C++ backend kind mismatch (None)");
static_assert(static_cast<int>(::llmfun::tui::BackendKind::Tui) == TuiBackendKind_Tui,
              "C/C++ backend kind mismatch (Tui)");
static_assert(static_cast<int>(::llmfun::tui::BackendKind::Gui) == TuiBackendKind_Gui,
              "C/C++ backend kind mismatch (Gui)");

// The Raw Log's own level constants (model/rawlog_data.h) are the ABI's numbers: the surface orders
// rows by severity and the floor's compare assumes the ABI's order, so a renumbering on either
// side is a build failure rather than a `trace` row drawn above an `error`.
static_assert(::llmfun::tui::model::kRawLogLevelTrace == TuiLogLevel_Trace &&
                  ::llmfun::tui::model::kRawLogLevelDebug == TuiLogLevel_Debug &&
                  ::llmfun::tui::model::kRawLogLevelInfo == TuiLogLevel_Info &&
                  ::llmfun::tui::model::kRawLogLevelWarn == TuiLogLevel_Warn &&
                  ::llmfun::tui::model::kRawLogLevelError == TuiLogLevel_Error,
              "model/rawlog_data.h level constants must mirror TuiLogLevel");

int tuiBackendActive(const TuiState* state) {
    if (!state || !state->inner || !state->inner->backend)
        return TuiBackendKind_None;
    return static_cast<int>(state->inner->backend->kind());
}

String tuiBackendNote(void) {
    if (backendNote.empty())
        return {nullptr, 0};
    return String_New(backendNote.c_str());
}

void tuiSetLogging(TuiState* state, int onOff) {
    if (!state || !state->inner)
        return;
    ::llmfun::tui::tuiSetLogging(*state->inner, onOff != 0);
}

void tuiSetMaxWidth(TuiState* state, int maxWidth) {
    if (!state || !state->inner)
        return;
    // Enforce the shared cap invariant at the C API boundary (0 = unlimited,
    // else [40, 10000]): a positive cap below the TUI's 40-column minimum
    // render width would leave it stuck on the "Terminal too small!" screen,
    // so raise sub-40 caps to the minimum.
    if (maxWidth > 0 && maxWidth < 40)
        maxWidth = 40;
    ::llmfun::tui::tuiSetMaxWidth(*state->inner, maxWidth);
}

void tuiSetIniFilename(TuiState* state, String filename) {
    if (!state || !state->inner)
        return;
    std::string filename_(filename.data ? filename.data : "", filename.data ? filename.len : 0);
    ::llmfun::tui::tuiSetIniFilename(*state->inner, filename_);
}

void tuiRequestCancel(TuiState* state) {
    if (!state || !state->inner)
        return;
    ::llmfun::tui::tuiRequestCancel(*state->inner);
}

int tuiTakeCancel(TuiState* state) {
    if (!state || !state->inner)
        return 0;
    return ::llmfun::tui::tuiTakeCancel(*state->inner) ? 1 : 0;
}

void tuiSetRawLogOpen(TuiState* state, int open) {
    if (!state || !state->inner)
        return;
    // One flag, the shell's own (`UiState::rawLogOpen`): the now bar's badge, the layout's
    // drawer and the `Esc` chain all read it, and the request is idempotent - `--debug-ui` may
    // set it on the frame after the user typed `/rawlog`, and the surface must simply be open.
    ::llmfun::tui::model::UiState& ui = state->inner->ui;
    if (open != 0) {
        if (!ui.rawLogOpen)
            ::llmfun::tui::views::toggleRawLog(*state->inner);
        return;
    }
    if (ui.rawLogOpen)
        ::llmfun::tui::views::closeRawLog(*state->inner);
}

int tuiIsRawLogOpen(const TuiState* state) {
    if (!state || !state->inner)
        return 0;
    return state->inner->ui.rawLogOpen ? 1 : 0;
}

void tuiAddLogMessage(TuiState* state, String summary, String text) {
    if (!state || !state->inner)
        return;
    std::string summary_(summary.data ? summary.data : "", summary.data ? summary.len : 0);
    std::string text_(text.data ? text.data : "", text.data ? text.len : 0);
    ::llmfun::tui::tuiAddLogMessage(*state->inner, ::llmfun::tui::LogMessage{summary_, text_});
}

/// The one record copy: the inbound POD becomes an owned record (every String is copied
/// during the call, so D may free its buffers as soon as the call returns - C-02).
///
/// A pure copy on purpose. The two *rules* - stamping a missing clock and folding a level or
/// action the UI has no rendering for - belong to the one place that feeds both stores
/// (`tuiAddLogRecord`), so the ring and the Activity projection cannot be handed two different
/// answers (task-32 review, Minor 4/5).
static ::llmfun::tui::LogRecord copyRecord(const TuiLogRecord& record) {
    const auto stringOf = [](String s) {
        return s.data ? std::string(s.data, s.len) : std::string();
    };
    ::llmfun::tui::LogRecord out;
    out.level = record.level;
    out.action = record.action;
    out.timestampMs = record.timestampMs;
    out.line = record.line;
    out.message = stringOf(record.message);
    out.detail = stringOf(record.detail);
    out.module = stringOf(record.module);
    out.function = stringOf(record.function);
    out.file = stringOf(record.file);
    out.actionPayload = stringOf(record.actionPayload);
    return out;
}

void tuiAddLogRecord(TuiState* state, const TuiLogRecord* record) {
    if (!state || !state->inner || !record)
        return;
    // One normalised record feeds both stores: the arrival stamp (0 = "the sender had none")
    // and the fallbacks for values the UI has no rendering for are applied here, once, so the
    // raw record and the Activity projection of it cannot disagree - an out-of-range level the
    // ring clamps to info must not stay unrenderable in the tab the same record is projected
    // into (task-32 review, Minor 5).
    TuiLogRecord inbound = *record;
    if (inbound.level < TuiLogLevel_Trace || inbound.level > TuiLogLevel_Error)
        inbound.level = TuiLogLevel_Info;
    if (inbound.action < TuiLogAction_None || inbound.action > TuiLogAction_Details)
        inbound.action = TuiLogAction_None;
    if (inbound.line < 0)
        inbound.line = 0; // a negative source line is "unknown", the value the archive prints as 0
    if (inbound.timestampMs <= 0)
        inbound.timestampMs = ::llmfun::tui::nowMs();
    // The raw ring (the Raw Log's store, task 33) ...
    ::llmfun::tui::tuiAddLogRecord(*state->inner, copyRecord(inbound));
    // ... and Activity, its user-facing projection: the same arrival, decided by the one
    // function that knows what Activity may show (D-ACTIVITY). `visible` is the last frame's
    // derivation, which is what makes a record arrive "read" while the tab is on screen.
    ::llmfun::tui::model::appendActivity(
        state->inner->ui, ::llmfun::tui::model::projectForActivity(inbound), inbound.level,
        ::llmfun::tui::isActivityVisible(*state->inner));
}

void tuiAddLogMessageEx(TuiState* state, TuiLogLevel level, String summary, String text) {
    if (!state || !state->inner)
        return;
    TuiLogRecord record{};
    record.level = level;
    record.action = TuiLogAction_None;
    record.timestampMs = 0; // the UI stamps the arrival
    record.message = summary;
    record.detail = text;
    tuiAddLogRecord(state, &record);
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

void tuiSetRuntimeStatus(TuiState* state, const TuiRuntimeStatus* status) {
    // Copied field by field across the boundary, so the layout is a wire contract: the D side
    // pins the same size (`llm/tui/package.d`), and `tui_api.h` states it ("reserved keeps
    // 8-byte alignment explicit").
    static_assert(sizeof(TuiRuntimeStatus) == 72,
                  "TuiRuntimeStatus layout changed: update the D mirror as well");
    if (!state || !state->inner)
        return;
    // Clearing is always honoured: a status set before a switch must still be droppable (the
    // same rule `tuiSetRawLogOpen`'s close follows).
    if (!status) {
        ::llmfun::tui::tuiSetRuntimeStatus(*state->inner, ::llmfun::tui::model::RuntimeStatus{});
        return;
    }
    ::llmfun::tui::model::RuntimeStatus inbound;
    inbound.phase = status->phase;
    if (inbound.phase < 0 || inbound.phase >= ::llmfun::tui::model::kPhaseCount) {
        // A phase this build does not know is idle, not a crash and not a neighbour: the sender may
        // be newer than the UI, and its other numbers are still readable. Warned when the unknown
        // ordinal changes, not once per update: a newer sender's 20 Hz stream would otherwise fill
        // the log for a whole run with the same line. The memory is this state's (a test or a
        // re-init starts clean); the counter is what makes the dedup observable.
        if (!state->inner->warnedUnknownPhase || status->phase != state->inner->lastUnknownPhase) {
            state->inner->warnedUnknownPhase = true;
            state->inner->lastUnknownPhase = status->phase;
            ++state->inner->unknownPhaseWarnings;
            std::fprintf(stderr, "llmfun: tuiSetRuntimeStatus: unknown phase %d, using idle\n",
                         static_cast<int>(status->phase));
        }
        inbound.phase = ::llmfun::tui::model::phaseOrdinal(::llmfun::tui::model::Phase::Idle);
    }
    // Every String is copied during the call, like every other inbound String in this API.
    inbound.detail.assign(status->detail.data ? status->detail.data : "",
                          status->detail.data ? status->detail.len : 0);
    inbound.contextUsed = status->contextUsed;
    inbound.contextMax = status->contextMax;
    inbound.compactAt = status->compactAt;
    inbound.tokensPerSecond = status->tokensPerSecond;
    inbound.elapsedMs = status->elapsedMs;
    inbound.pendingQuestions = status->pendingQuestions;
    inbound.structured = true;
    ::llmfun::tui::tuiSetRuntimeStatus(*state->inner, inbound);
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
    return state->inner->autoScroll ? 1 : 0;
}

void tuiSetAutoScroll(TuiState* state, int enabled) {
    if (!state || !state->inner)
        return;
    state->inner->autoScroll = enabled != 0;
}

void tuiReadyStatus(TuiState* state, int ready) {
    if (!state || !state->inner)
        return;
    ::llmfun::tui::tuiSetReadyStatus(*state->inner, ready == 1);
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
        // startAt..count-1 is exactly the last `n` entries. `n` is the number
        // to keep, not an end index: looping to `n` copied the wrong range
        // (0 entries when count >= 2 * maxEntries).
        for (size_t i = startAt; i < count; ++i) {
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
    // Copied field by field across the boundary, so the layout is a wire contract: `tui_api.h`
    // states it, `llm/tui/package.d` pins the same size in a `static assert`, and
    // `./test_tui_api_v4` (case 1f) pins the size and every offset on this side.
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
    std::string tool(msg.currentTool.data ? msg.currentTool.data : "",
                     msg.currentTool.data ? msg.currentTool.len : 0);
    // `msg.contextMax` is deliberately not read: it is the producer's zero-vs-none bit, and the
    // lane draws no number for either zero, so it has no consumer on this side.
    const std::int64_t ctxUsed = msg.contextUsed;

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
            mirrorLaneUpdate(*state->inner, id, role_, c, tool, ctxUsed);
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
    mirrorLaneUpdate(*state->inner, id, role_, c, tool, ctxUsed);
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
            ::llmfun::tui::model::finishAgentLane(state->inner->ui, id);
            return;
        }
    }
}

void tuiPipelineClear(TuiState* state) {
    if (!state || !state->inner)
        return;
    state->inner->left.agents.clear();
    ::llmfun::tui::model::clearAgentLanes(state->inner->ui);
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

/* The lane list's capacity is the pipeline panel's (task 30): `model/` may not include
 * `tui.h`, so the model carries the number and this is where the two are pinned together. A
 * change to `ChatTabLeftPanel::MaxAgents` that forgot the lanes would otherwise let the tab
 * list an agent the v4 store has already evicted.
 */
static_assert(::llmfun::tui::model::kAgentLaneMax == ::llmfun::tui::ChatTabLeftPanel::MaxAgents,
              "the Agents tab's lane capacity must match the pipeline's MaxAgents");

/* Same linkage for the v5 session-row flags (task 24): `SessionItem::flags` is a
 * bit field, not an enum, so the two spellings are pinned with a static_assert
 * on the one bit that exists rather than by an enum mirror.
 */
static_assert(::llmfun::tui::kSessionFlagRunning == TuiSessionFlag_Running,
              "C/C++ session row flag mismatch (Running)");

/* Session sidebar */

void tuiSetSessionList(TuiState* state, const SessionItem* items, size_t count) {
    if (!state || !state->inner)
        return;
    auto& panel = state->inner->sessionPanel;
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
            // v5 (task 24): the appended fields pass through unchanged - no
            // conversion, no clamping. A caller that never sets them sends 0/0,
            // which is "unknown time, no run" and renders as a flat rail.
            e.updatedAtMs = it.updatedAtMs;
            e.flags = it.flags;
            if (e.isActive)
                panel.activeId = e.id; // at most one active entry expected; last wins
            panel.sessions.push_back(std::move(e));
        }
    }
    // No stale-slot pruning here any more: the rail's answers are one-shot (`views::RailState`),
    // so a select or a rename the frame queued is already gone by the next snapshot - there is no
    // queued id in this struct to go stale (task 41).
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

int tuiHasMoreEvents() { return ::llmfun::tui::tuiHasMoreEvents() ? 1 : 0; }

#ifdef __cplusplus
}
#endif
