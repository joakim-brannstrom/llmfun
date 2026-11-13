#include "tui_api.h"
#include "fnv1a.h"
#include "tui.h"
#include "tui_backend.h"

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdint>
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

/* The Agents tab's mirror.
 *
 * The two pipeline calls that carry state copy what a lane needs into `UiState::agents`
 * as it arrives: the tab draws a value list it owns instead of reading the store through
 * a seam that does not exist, and the number and the tool name arrive as fields on
 * `PipelineChatMessage` instead of being parsed out of the stream's own text.
 *
 * The body is `model::applyAgentUpdate()`/`finishAgentLane()`: what a lane *is* is a model
 * answer, and it is testable without a C surface in the way (`./test_agents_tab`).
 * -------------------------------------------------------------------------- */

namespace {

/// One lane update, with the clock read once so the create path and the update path cannot
/// stamp a lane from two different instants. The count, the rate and the tool name are the
/// fields D sent: nothing here reads the stream's text or recomputes a number.
void mirrorLaneUpdate(::llmfun::tui::TuiState& state, const std::string& id,
                      const std::string& role, const std::string& content,
                      const std::string& currentTool, std::int64_t contextUsed,
                      double tokensPerSecond) {
    ::llmfun::tui::model::applyAgentUpdate(state.ui, id, role, content, currentTool, contextUsed,
                                           tokensPerSecond, ::llmfun::tui::model::agentClockMs());
}

/// Find-or-insert the agent's store entry, evicting by `lastUpdate` at capacity —
/// the same policy the streamed path applies. A tool event may arrive before the
/// first streamed tick, so this path must not create a lane: lanes are born only
/// in `mirrorLaneUpdate`, and a store without a lane is a state the drill-down
/// handles by showing the store alone.
::llmfun::tui::AgentStream& agentStore(::llmfun::tui::TuiState& state, const std::string& id) {
    auto& agents = state.left.agents;
    for (auto& agent : agents) {
        if (agent.agentId == id)
            return agent;
    }
    if (agents.size() >= state.left.MaxAgents) {
        const auto oldest = std::min_element(
            agents.begin(), agents.end(),
            [](const ::llmfun::tui::AgentStream& a, const ::llmfun::tui::AgentStream& b) {
                return a.lastUpdate < b.lastUpdate;
            });
        if (state.ui.focusedAgent == oldest->agentId)
            state.ui.focusedAgent.clear();
        state.agentThreadAdapter.dropChat(&oldest->chat);
        agents.erase(oldest);
    }
    agents.emplace_back(::llmfun::tui::AgentStream{id, std::chrono::system_clock::now(), {}});
    return agents.back();
}

/// The tool block's flags: the same set D's tool upserts carry (collapsible,
/// open by default, selectable).
inline constexpr int kToolBlockFlags =
    TuiBlockFlag_Collapsible | TuiBlockFlag_DefaultOpen | TuiBlockFlag_Selectable;

/// Open the agent's live typed turn if none is open: the turn id
/// folds the agent id with the per-agent run counter. Answers the turn id.
std::int64_t openTypedTurn(::llmfun::tui::AgentStream& agent) {
    if (agent.typedTurnActive)
        return agent.typedTurnId;
    agent.typedTurnSeq++;
    ::llmfun::tui::Fnv1a64 h;
    h.foldBytes(agent.agentId);
    h.fold(agent.typedTurnSeq);
    agent.typedTurnId = static_cast<std::int64_t>(h.value());
    agent.typedTurnActive = true;
    agent.typedMessageCount = 0;
    agent.typedHasCall = false;
    ::llmfun::tui::store::TypedTurn turn;
    turn.id = agent.typedTurnId;
    turn.state = TuiTurnState_Running;
    turn.messageCount = 0;
    agent.chat.typedTurns.turnUpsert(turn);
    return agent.typedTurnId;
}

/// One block upsert into the agent's typed store, with the wire vocabulary the
/// store speaks (the same fields the main tab's `tuiBlockUpsert` copies).
void upsertTypedBlock(::llmfun::tui::AgentStream& agent, std::int64_t turnId, std::string id,
                      std::string batchId, int kind, int status, int flags, std::string title,
                      std::string meta, std::string body) {
    ::llmfun::tui::store::TypedBlock b;
    b.turnId = turnId;
    b.id = std::move(id);
    b.batchId = std::move(batchId);
    b.title = std::move(title);
    b.meta = std::move(meta);
    b.body = std::move(body);
    b.kind = kind;
    b.status = status;
    b.flags = flags;
    agent.chat.typedTurns.blockUpsert(std::move(b));
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

    ::llmfun::tui::model::loadUiConf(state->inner->ui.prefs, state->inner->iniFilename);

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
        backend = ::llmfun::tui::makeGuiBackend(state->inner->ui.prefs);
        initialized = backend->init(error);
        if (initialized)
            ::llmfun::tui::initMarkdownGuiMode(*state->inner);
    } else {
        // Auto (also the defensive default for unknown values): try the GUI
        // backend first. On failure, fall back to the terminal UI iff both
        // stdio ends are TTYs — a full-screen TUI drawn into a pipe is
        // useless, so there the GUI error is the result.
        // The settings were loaded above, before any backend ran, so a TTY
        // fallback inherits the loaded font scale.
        backend = ::llmfun::tui::makeGuiBackend(state->inner->ui.prefs);
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

    // GUI only: the window's drop callback appends to the state's chip row.
    // Guard on the attached kind, not the requested mode: Auto can fall back
    // to the ncurses backend, which has no window to receive drops.
    if (state->inner->backend->kind() == ::llmfun::tui::BackendKind::Gui)
        ::llmfun::tui::setDropRowTarget(&state->inner->ui.drop);

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
        // The status row is never empty: `UiState` seeds itself with the model
        // placeholder ("none" - the name D itself reports when no model is configured)
        // and the chips formatted from it, so a state built anywhere draws the same
        // first frame: `ctx — | — | none | Ready`.
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
        bool windowResized = false;
        if (state->inner->backend) {
            state->inner->backend->shutdown();
            windowResized = state->inner->backend->windowResizedSinceOpen();
            state->inner->backend.reset();
        }
        ::llmfun::tui::saveUiConfOnShutdown(*state->inner, windowResized);
        ::llmfun::tui::closeLogArchive(*state->inner);
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

void tuiRenderBlocking(TuiState* state, int blocking) {
    if (!state || !state->inner || !state->inner->backend)
        return;
    ::llmfun::tui::tuiRenderBlocking(*state->inner, blocking != 0);
}

int tuiQuitRequested(const TuiState* state) {
    if (!state || !state->inner)
        return 0;
    const auto& inner = *state->inner;
    return (inner.quitRequested || (inner.backend && inner.backend->shouldClose())) ? 1 : 0;
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

void tuiSetIdlePark(TuiState* state, int parkMs) {
    if (!state || !state->inner)
        return;
    ::llmfun::tui::tuiSetIdlePark(*state->inner, parkMs);
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

int tuiRawLogFloor(const TuiState* state) {
    if (!state || !state->inner)
        return 0;
    return state->inner->ui.prefs.rawLog;
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
    out.seq = record.seq;
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
    // The raw ring ...
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

void tuiSetRuntimeStatus(TuiState* state, const TuiRuntimeStatus* status) {
    if (!state || !state->inner)
        return;
    if (!status) {
        ::llmfun::tui::tuiSetRuntimeStatus(*state->inner, ::llmfun::tui::model::RuntimeStatus{});
        return;
    }
    ::llmfun::tui::model::RuntimeStatus inbound;
    inbound.phase = status->phase;
    if (inbound.phase < 0 || inbound.phase >= ::llmfun::tui::model::kPhaseCount) {
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
    inbound.model.assign(status->model.data ? status->model.data : "",
                         status->model.data ? status->model.len : 0);
    inbound.contextUsed = status->contextUsed;
    inbound.contextMax = status->contextMax;
    inbound.compactAt = status->compactAt;
    inbound.tokensPerSecond = status->tokensPerSecond;
    inbound.elapsedMs = status->elapsedMs;
    inbound.pendingQuestions = status->pendingQuestions;
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

void tuiPipelineAgentStart(TuiState* state, String agentId, String task) {
    if (!state || !state->inner)
        return;
    std::string id(agentId.data ? agentId.data : "", agentId.data ? agentId.len : 0);
    if (id.empty())
        return;
    std::string t(task.data ? task.data : "", task.data ? task.len : 0);
    auto& agent = agentStore(*state->inner, id);
    // The run's query row, the exact block the main thread's turn start emits
    // (`app_agent/package.d`'s turn open): one prose block on the query batch
    // ("<turnId>#q", role "user"), so the drill-down's turn anatomy matches the
    // main thread's - query -> work batches -> answer. Opening here owns the
    // run's turn: `openTypedTurn` opens only when none is active, so every tick
    // this run sends joins the already-open turn (one turn per run) and
    // the query rides its own batch, a turn-level row the fold never owns.
    const std::int64_t turnId = openTypedTurn(agent);
    upsertTypedBlock(agent, turnId, "query", std::to_string(turnId) + "#q", TuiBlockKind_Prose,
                     TuiBlockStatus_None, 0, "", "user", t);
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
    std::string tool(msg.currentTool.data ? msg.currentTool.data : "",
                     msg.currentTool.data ? msg.currentTool.len : 0);
    // `msg.contextMax` is deliberately not read: it is the producer's zero-vs-none bit, and the
    // lane draws no number for either zero, so it has no consumer on this side.
    const std::int64_t ctxUsed = msg.contextUsed;
    // `msg.finishReason` is part of the wire contract but has no consumer on this side:
    // the per-agent stream path reads no finish reason.

    auto& agent = agentStore(*state->inner, id);
    agent.lastUpdate = std::chrono::system_clock::now();

    // One batch per step, one reasoning/content pair per batch - the shape the
    // main thread's typed walk emits (`app_agent/package.d`'s per-batch walk).
    // The payload's `batch` is D's per-response counter, so the ids are
    // batch-scoped and a step's ticks still merge in place: the newest tick of
    // a batch wins, per batch, and the next batch starts a fresh pair.
    const std::int64_t turnId = openTypedTurn(agent);
    const std::string batchId = std::to_string(turnId) + "#" + std::to_string(msg.batch);
    upsertTypedBlock(agent, turnId, "content#" + std::to_string(msg.batch), batchId,
                     TuiBlockKind_Prose, TuiBlockStatus_None, 0, "", role_, c);
    if (!r.empty())
        upsertTypedBlock(agent, turnId, "reasoning#" + std::to_string(msg.batch), batchId,
                         TuiBlockKind_Reasoning, TuiBlockStatus_None, TuiBlockFlag_Collapsible, "",
                         "", r);

    mirrorLaneUpdate(*state->inner, id, role_, c, tool, ctxUsed, msg.tokensPerSecond);
}

void tuiPipelineAgentToolCall(TuiState* state, String agentId, TuiToolCallParam call) {
    if (!state || !state->inner)
        return;
    std::string id(agentId.data ? agentId.data : "", agentId.data ? agentId.len : 0);
    if (id.empty())
        return;
    std::string name(call.name.data ? call.name.data : "", call.name.data ? call.name.len : 0);
    std::string args(call.arguments.data ? call.arguments.data : "",
                     call.arguments.data ? call.arguments.len : 0);
    std::string callId(call.id.data ? call.id.data : "", call.id.data ? call.id.len : 0);

    auto& agent = agentStore(*state->inner, id);
    const std::int64_t turnId = openTypedTurn(agent);
    const std::string batchId = std::to_string(turnId) + "#" + std::to_string(call.batch);
    // The call's block, Pending (the merged step's walk upserts Ok/Failed over
    // it when the result lands). The in-flight slot remembers what a result
    // merges into, because the result payload carries no call id.
    upsertTypedBlock(agent, turnId, callId, batchId, TuiBlockKind_Tool, TuiBlockStatus_Pending,
                     kToolBlockFlags, name, "", args);
    agent.typedHasCall = true;
    agent.typedCallId = std::move(callId);
    agent.typedCallBatchId = batchId;
    agent.typedCallName = std::move(name);
    agent.typedCallBody = std::move(args);
    agent.typedMessageCount++;
}

void tuiPipelineAgentToolResult(TuiState* state, String agentId, TuiToolResultParam result) {
    if (!state || !state->inner)
        return;
    std::string id(agentId.data ? agentId.data : "", agentId.data ? agentId.len : 0);
    if (id.empty())
        return;
    std::string name(result.name.data ? result.name.data : "",
                     result.name.data ? result.name.len : 0);
    std::string text(result.text.data ? result.text.data : "",
                     result.text.data ? result.text.len : 0);
    const int status = result.ok != 0 ? TuiBlockStatus_Ok : TuiBlockStatus_Failed;

    auto& agent = agentStore(*state->inner, id);
    const std::int64_t turnId = openTypedTurn(agent);
    const std::string batchId = std::to_string(turnId) + "#" + std::to_string(result.batch);
    if (agent.typedHasCall && agent.typedCallName == name) {
        // The merged step: the call's block gains its outcome, the result
        // summary rides `meta` and the call's arguments stay the body - the
        // same shape D's merged walk emits for a call+result pair.
        upsertTypedBlock(agent, turnId, agent.typedCallId, agent.typedCallBatchId,
                         TuiBlockKind_Tool, status, kToolBlockFlags, name, text,
                         agent.typedCallBody);
        agent.typedHasCall = false;
        agent.typedMessageCount++;
        return;
    }
    // No in-flight call to merge into: a result-only block under its own id,
    // recorded never dropped.
    agent.typedResultSeq++;
    upsertTypedBlock(agent, turnId, "result#" + std::to_string(agent.typedResultSeq), batchId,
                     TuiBlockKind_Tool, status, kToolBlockFlags, name, text, "");
    agent.typedMessageCount++;
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
            if (agent.typedTurnActive) {
                // The run's close: a call that never got its result shows
                // Running, never a fabricated Ok (the stream-done flip,
                // applied here because the payload has no earlier boundary);
                // then the turn closes Done with its informational count.
                if (agent.typedHasCall) {
                    upsertTypedBlock(agent, agent.typedTurnId, agent.typedCallId,
                                     agent.typedCallBatchId, TuiBlockKind_Tool,
                                     TuiBlockStatus_Running, kToolBlockFlags, agent.typedCallName,
                                     "", agent.typedCallBody);
                    agent.typedHasCall = false;
                }
                ::llmfun::tui::store::TypedTurn turn;
                turn.id = agent.typedTurnId;
                turn.state = TuiTurnState_Done;
                turn.messageCount = agent.typedMessageCount;
                agent.chat.typedTurns.turnUpsert(turn);
                agent.chat.typedTurns.evictForCapacity(turn.id);
                agent.typedTurnActive = false;
                agent.typedTurnId = 0;
            }
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
    state->inner->agentThreadAdapter = {};
    state->inner->ui.focusedAgent.clear();
    ::llmfun::tui::model::clearAgentLanes(state->inner->ui);
}

/* --- Thread turns/blocks (the C ABI over the typed store) ------------------
 * The typed store's C entry points. Every one lands on the main tab's typed
 * store (`chat.typedTurns`): the store merges in place, and the adapter's
 * rebuild guard picks the change up on the next draw of the typed path. Null
 * state / null inner / null payload pointers are the silent no-op every entry
 * point here is (the legacy ones above set the same precedent).
 */

void tuiSetTurns(TuiState* state, const TuiTurn* turns, size_t turnCount, const TuiBlock* blocks,
                 size_t blockCount) {
    if (!state || !state->inner)
        return;
    const auto str = [](String s) { return std::string(s.data ? s.data : "", s.data ? s.len : 0); };
    std::vector<::llmfun::tui::store::TypedTurn> typedTurns;
    typedTurns.reserve(turnCount);
    for (size_t k = 0; turns && k < turnCount; ++k) {
        ::llmfun::tui::store::TypedTurn t;
        t.id = turns[k].id;
        t.state = turns[k].state;
        t.messageCount = turns[k].messageCount;
        typedTurns.push_back(t);
    }
    std::vector<::llmfun::tui::store::TypedBlock> typedBlocks;
    typedBlocks.reserve(blockCount);
    for (size_t k = 0; blocks && k < blockCount; ++k) {
        const TuiBlock& b = blocks[k];
        ::llmfun::tui::store::TypedBlock nb;
        nb.turnId = b.turnId;
        nb.id = str(b.id);
        nb.batchId = str(b.batchId);
        nb.title = str(b.title);
        nb.meta = str(b.meta);
        nb.body = str(b.body);
        nb.kind = b.kind;
        nb.status = b.status;
        nb.flags = b.flags;
        nb.durationMs = b.durationMs;
        nb.tokensIn = b.tokensIn;
        nb.tokensOut = b.tokensOut;
        for (uint32_t p = 0; b.resultKeys && b.resultValues && p < b.resultCount; ++p)
            nb.resultPairs.emplace_back(str(b.resultKeys[p]), str(b.resultValues[p]));
        nb.resultRaw = str(b.resultRaw);
        nb.resultMore = b.resultMore;
        typedBlocks.push_back(std::move(nb));
    }
    state->inner->chat.typedTurns.setTurns(std::move(typedTurns), std::move(typedBlocks));
    // A session switch re-emits the whole snapshot: no pin survives it -
    // the new session's turns start unpinned, their groups on the mode.
    state->inner->chat.rowPins.clear();
    state->inner->chat.groupPins.clear();
}

void tuiTurnUpsert(TuiState* state, const TuiTurn* turn) {
    if (!state || !state->inner || !turn)
        return;
    ::llmfun::tui::store::TypedTurn t;
    t.id = turn->id;
    t.state = turn->state;
    t.messageCount = turn->messageCount;
    state->inner->chat.typedTurns.turnUpsert(t);
    state->inner->chat.typedTurns.evictForCapacity(turn->id);
    // The eviction took whole turns out of the store; their pins would be memory
    // kept for nothing - a pin for a turn nobody can draw. Prune by
    // turn id, never by row index: the row list churns while a response streams.
    auto& rowPins = state->inner->chat.rowPins;
    for (auto it = rowPins.begin(); it != rowPins.end();) {
        if (state->inner->chat.typedTurns.turnById(it->first) == nullptr)
            it = rowPins.erase(it);
        else
            ++it;
    }
    auto& groupPins = state->inner->chat.groupPins;
    for (auto it = groupPins.begin(); it != groupPins.end();) {
        if (state->inner->chat.typedTurns.turnById(it->first) == nullptr)
            it = groupPins.erase(it);
        else
            ++it;
    }
}

void tuiBlockUpsert(TuiState* state, const TuiBlock* block) {
    if (!state || !state->inner || !block)
        return;
    const auto str = [](String s) { return std::string(s.data ? s.data : "", s.data ? s.len : 0); };
    ::llmfun::tui::store::TypedBlock nb;
    nb.turnId = block->turnId;
    nb.id = str(block->id);
    nb.batchId = str(block->batchId);
    nb.title = str(block->title);
    nb.meta = str(block->meta);
    nb.body = str(block->body);
    nb.kind = block->kind;
    nb.status = block->status;
    nb.flags = block->flags;
    nb.durationMs = block->durationMs;
    nb.tokensIn = block->tokensIn;
    nb.tokensOut = block->tokensOut;
    for (uint32_t p = 0; block->resultKeys && block->resultValues && p < block->resultCount; ++p)
        nb.resultPairs.emplace_back(str(block->resultKeys[p]), str(block->resultValues[p]));
    nb.resultRaw = str(block->resultRaw);
    nb.resultMore = block->resultMore;
    state->inner->chat.typedTurns.blockUpsert(std::move(nb));
}

void tuiTurnRemove(TuiState* state, int64_t turnId) {
    if (!state || !state->inner)
        return;
    state->inner->chat.typedTurns.turnRemove(turnId);
    // The turn's pins die with the turn: a compression re-emit
    // (turnRemove + re-upsert) leaves no stale pin behind.
    state->inner->chat.rowPins.erase(turnId);
    state->inner->chat.groupPins.erase(turnId);
}

void tuiThreadClear(TuiState* state) {
    if (!state || !state->inner)
        return;
    state->inner->chat.typedTurns.threadClear();
    // A clear is a reset: the pins are session-local state and
    // the session's thread is gone.
    state->inner->chat.rowPins.clear();
    state->inner->chat.groupPins.clear();
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
static_assert(static_cast<int>(::llmfun::tui::SessionActionType::PinToggle) ==
                  TuiSessionAction_PinToggle,
              "C/C++ session action enum mismatch (PinToggle)");
static_assert(static_cast<int>(::llmfun::tui::SessionActionType::Clone) == TuiSessionAction_Clone,
              "C/C++ session action enum mismatch (Clone)");

/* The lane list's capacity is the pipeline panel's: the model carries the number and
 * this is where the two are pinned together. A change to `ChatTabLeftPanel::MaxAgents`
 * that forgot the lanes would otherwise let the tab list an agent the store has
 * already evicted.
 */
static_assert(::llmfun::tui::model::kAgentLaneMax == ::llmfun::tui::ChatTabLeftPanel::MaxAgents,
              "the Agents tab's lane capacity must match the pipeline's MaxAgents");

/* The session-row flags: `SessionItem::flags` is a
 * bit field, not an enum, so the two spellings are pinned with static_asserts
 * on the bits that exist rather than by an enum mirror.
 */
static_assert(::llmfun::tui::kSessionFlagRunning == TuiSessionFlag_Running,
              "C/C++ session row flag mismatch (Running)");
static_assert(::llmfun::tui::kSessionFlagPinned == TuiSessionFlag_Pinned,
              "C/C++ session row flag mismatch (Pinned)");

/* The chip statuses: the same append-only pair of lists as the session
 * actions, and the same reason the two must stay in sync (the static_cast
 * in tuiSetChipStatus would silently mis-map a renumbered value).
 */
static_assert(static_cast<int>(::llmfun::tui::model::DropChipState::Pending) ==
                  TuiChipStatus_Pending,
              "C/C++ chip status enum mismatch (Pending)");
static_assert(static_cast<int>(::llmfun::tui::model::DropChipState::Done) == TuiChipStatus_Done,
              "C/C++ chip status enum mismatch (Done)");
static_assert(static_cast<int>(::llmfun::tui::model::DropChipState::FailedRaw) ==
                  TuiChipStatus_FailedRaw,
              "C/C++ chip status enum mismatch (FailedRaw)");
static_assert(static_cast<int>(::llmfun::tui::model::DropChipState::Failed) == TuiChipStatus_Failed,
              "C/C++ chip status enum mismatch (Failed)");

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
            // The appended fields pass through unchanged - no
            // conversion, no clamping. A caller that never sets them sends 0/0,
            // which is "unknown time, no run" and renders as a flat rail.
            e.updatedAtMs = it.updatedAtMs;
            e.flags = it.flags;
            if (e.isActive)
                panel.activeId = e.id; // at most one active entry expected; last wins
            panel.sessions.push_back(std::move(e));
        }
    }
    // No stale-slot pruning here: the rail's answers are one-shot (`views::RailState`),
    // so a select or a rename the frame queued is already gone by the next snapshot - there is
    // no queued id in this struct to go stale.
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

TuiDroppedPath tuiGetDroppedPath(TuiState* state) {
    if (!state || !state->inner)
        return {0, {nullptr, 0}};
    std::int64_t chipId = 0;
    std::string path;
    if (!::llmfun::tui::tuiPopDroppedPath(*state->inner, chipId, path))
        return {0, {nullptr, 0}};
    return {chipId, String_NewBuf(path.data(), path.size())};
}

void tuiSetChipStatus(TuiState* state, int64_t chipId, int32_t status, String path,
                      String display) {
    if (!state || !state->inner)
        return;
    const std::string pathText(path.data ? path.data : "", path.data ? path.len : 0);
    const std::string displayText(display.data ? display.data : "", display.data ? display.len : 0);
    ::llmfun::tui::tuiSetChipStatus(*state->inner, chipId, status, pathText, displayText);
}

size_t tuiGetChipCount(TuiState* state) {
    if (!state || !state->inner)
        return 0;
    return ::llmfun::tui::tuiGetChipCount(*state->inner);
}

TuiChipParam tuiGetChipAt(TuiState* state, size_t index) {
    if (!state || !state->inner)
        return {0, TuiChipStatus_Pending, {nullptr, 0}};
    const ::llmfun::tui::model::DropChip* chip = ::llmfun::tui::tuiGetChipAt(*state->inner, index);
    if (chip == nullptr)
        return {0, TuiChipStatus_Pending, {nullptr, 0}};
    TuiChipParam out{};
    out.chipId = chip->id;
    out.status = static_cast<int32_t>(chip->state);
    if (!chip->workareaPath.empty())
        out.path = String_NewBuf(chip->workareaPath.data(), chip->workareaPath.size());
    return out;
}

void tuiClearChipRow(TuiState* state) {
    if (!state || !state->inner)
        return;
    ::llmfun::tui::tuiClearChipRow(*state->inner);
}

int64_t tuiGetChipCancel(TuiState* state) {
    if (!state || !state->inner)
        return 0;
    return ::llmfun::tui::tuiPopChipCancel(*state->inner);
}

#ifdef __cplusplus
}
#endif
