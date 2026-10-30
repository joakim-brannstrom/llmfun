/// TUI core: the TuiState aggregate and the public tui* API.
/// Holds the shared UI state structs and the entry points the C API drives;
/// each state owns its attached backend (v4; see tui_backend.h).
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "imtui/imtui.h"

#include "model/status_chips.h"
#include "model/ui_state.h"
#include "store/thread_store.h"
#include "tui_backend.h"
#include "tui_settings.h"
#include "ui/tokens.h"
#include "views/composer.h"
#include "views/help.h"
#include "views/inspector.h"
#include "views/palette.h"
#include "views/rawlog.h"
#include "views/settings.h"
#include "views/topbar.h"

// Forward declaration (keeps the rich_md headers out of this header): the
// markdown context of a TuiState, created/destroyed by the tuiInit/tuiDestroy
// State lifecycle (tui_api.cpp, implemented in tui.cpp).
namespace RichMd {
struct Context;
}

namespace llmfun::tui {

// Internal C++ enum mirroring the C TuiChatMessageType.
enum class ChatMessageType : uint8_t {
    User = 0,
    Assistant = 1,
    ToolCall = 2,
    ToolResponse = 3,
    Vision = 4,
    System = 5,
    FinalAnswer = 6,
    Count = 7 // Sentinel — not a valid type
};

// Role colours for the message headers: exactly one foreground per role.
//
// A role header is a rule plus a label in this colour on the neutral thread
// surface, and nothing else: no per-role row fill, no hover/active variant and
// no derived black-on-bright text colour. Those fills turned the thread into a
// stack of bright bars whose labels could not be read.
//
// These six values are the v4 store's per-type colours. The restyled thread
// (tasks 25/26) draws ui::color(roleYou/roleAgent/roleTool/roleSystem) with a
// state glyph instead, so nothing on screen reads them; test_theme_contrast
// still measures the six, which is the only thing keeping the struct here.
struct ChatMessageStyle {
    // User & Vision: Sky Blue
    ImVec4 userFg = ImVec4(0.45f, 0.70f, 0.90f, 1.00f);

    // Assistant: Soft Green
    ImVec4 assistantFg = ImVec4(0.55f, 0.85f, 0.55f, 1.00f);

    // ToolCall: Warm Orange
    ImVec4 toolCallFg = ImVec4(0.95f, 0.70f, 0.30f, 1.00f);

    // ToolResponse: Dark Orange
    ImVec4 toolResponseFg = ImVec4(0.95f, 0.59f, 0.40f, 1.00f);

    // System: Light Slate Gray
    ImVec4 systemFg = ImVec4(0.70f, 0.75f, 0.80f, 1.00f);

    // FinalAnswer: Bright Green
    ImVec4 finalAnswerFg = ImVec4(0.30f, 0.95f, 0.30f, 1.00f);
};

struct ChatMessage {
    std::string summary;
    std::string text;
    std::string thinking;
    ChatMessageType type = ChatMessageType::Assistant;
    size_t id{0}; // Unique, monotonic increasing ID number
};

struct LogMessage {
    std::string summary;
    std::string text;
};

/// The archive's handle (`tuiSetLogging` -> `llmfun_ui_log.txt`), as a small owner.
///
/// One handle for the whole run, so a record that arrives between two frames pays no `fopen`:
/// the state owns it and its destructor closes it, so a state that enabled logging and went out
/// of scope cannot leak the descriptor. It is *copyable and starts empty* when copied: a
/// `TuiState` copy is a fresh state (the tests copy the aggregate), and two states sharing one
/// `FILE*` would close it twice.
struct LogArchive {
    FILE* file = nullptr;
    /// The archive could not be opened *or* a write to it failed (a read-only working
    /// directory, a full disk): no further attempt is made until the next
    /// `tuiSetLogging(true)`, so a session that cannot archive costs one attempt per request,
    /// not one per record.
    bool unavailable = false;

    LogArchive() = default;
    LogArchive(const LogArchive&) {}
    LogArchive& operator=(const LogArchive&) { return *this; }
    ~LogArchive() {
        if (file != nullptr)
            std::fclose(file);
    }
};

/// One log record with owned strings: the C ABI's `TuiLogRecord` copied at the call
/// boundary (D may free its buffers the moment `tuiAddLogRecord` returns - C-02).
///
/// The *value type* moved to `model/rawlog_data.h` (task 33) so `views/rawlog.h` can name it
/// without reaching `tui.h`: the Raw Log draws these records and a view may not include the
/// aggregate (D-ADAPTER). The alias keeps the one store's element type spelled as it was, so
/// `deque` in this header, `tuiAddLogRecord()` below and the archive all still say `LogRecord`.
///
/// This is the *raw* record and the ring's element type; it keeps the developer fields (module,
/// function, file, line) because the Raw Log shows them. Activity's element type is
/// `model::LogRecord`, which has none of those fields by construction -
/// `model::projectForActivity()` is the one place that turns one into the other. The two names
/// are deliberate: same record, two audiences, and the smaller one cannot leak the bigger one's
/// fields.
using LogRecord = model::RawLogRecord;

struct UserQueryState {
    // Dynamic input buffer
    std::string inputBuf;
    std::string draftBuf;
    std::string newInputBufString;

    // Submission flag
    bool submitReady = false;
    bool isSubmitted = true;
    std::string submitQuery;

    // Input history
    std::vector<std::string> inputHistory;
    int historyPos = -1;
    static constexpr size_t MAX_HISTORY = 500;

    /// The field held ImGui's edit focus at the previous frame boundary. A press
    /// that Escape-releases a focused text field has already cleared the active
    /// id inside NewFrame by the time the composer's block runs, so without this
    /// latch the press that should act on the composer would look like nobody's
    /// (the same reason `ChatTabSessionPanel::renameHadFocus` exists). Read by
    /// the D-ESC chain (`tui.cpp`), written once per frame by the field.
    bool focusedLastFrame = false;
};

enum class GroupKind { UserQuery, FinalAnswer, AssistantWork };

struct RenderGroup {
    size_t start; // inclusive index into outputLines
    size_t end;   // exclusive index into outputLines
    GroupKind kind;
};

struct AgentStreamMessage {
    std::string content;
    std::string thinking;
    std::string role;
    std::string finishReason;
    std::string status;
};

struct AgentStream {
    std::string agentId;
    AgentStreamMessage stream;
    std::chrono::system_clock::time_point lastUpdate = std::chrono::system_clock::now();

    int64_t updateCnt{1};
    // each time updateCnt is incremented activity is set to true
    bool activity{false};

    std::vector<AgentStreamMessage> messages;
    static constexpr size_t MaxMessages = 10;

    void finished();
};

/// The pipeline's v4 agent store: `tuiPipelineAgentUpdate` fills it (with the LRU eviction the
/// lane mirror repeats) and `tuiPipelineClear` empties it. Nothing draws it any more - the
/// Inspector's Agents tab draws `model::UiState::agents`, the lane mirror built beside this one.
struct ChatTabLeftPanel {
    std::vector<AgentStream> agents;
    static constexpr size_t MaxAgents = 16;
};

// Internal C++ mirror of the C TuiSessionActionType (tui_api.h).
// Append-only ordering: existing values must never be renumbered or
// reused, so future actions can be added without breaking the D mapping.
enum class SessionActionType : int {
    None = 0,
    Select = 1,
    New = 2,
    Rename = 3,
    Delete = 4,
};

// One queued sidebar action (UI -> D). Mirrors the C SessionAction
// (tui_api.h); the C API layer converts between the two.
struct SessionAction {
    SessionActionType type = SessionActionType::None;
    std::string sessionId; // empty for New
    std::string title;     // new title for Rename, empty otherwise
};

// One row of the session sidebar snapshot (full replace).
//
// `updatedAtMs` and `flags` are the v5 additions (task 24) the workbench rail
// groups and marks rows with. Both are *appended* facts, so a snapshot that
// leaves them at their defaults is a legal one: 0 is "unknown" (the rail then
// draws one flat list instead of fabricating dates) and flags 0 is "no run".
// `updatedAtMs` is `SessionMeta.updatedAt` (unix seconds) * 1000 on the D side;
// `createdAt` is deliberately not sent - groups and relative time use the
// update time only (D-SESSION-TS).
struct SessionEntry {
    std::string id;
    std::string title;
    std::string preview;
    size_t messageCount{0};
    bool isActive{false};
    int64_t updatedAtMs{0};
    /// Bit 0 = a run is in flight for this session. `unread` and `pinned` are
    /// UI-local and never cross the ABI (FR-03, D-19).
    int flags{0};
};

/// The `SessionEntry::flags` bit that says a run is in flight (tui_api.h's
/// `TuiSessionFlag_Running`; the C API layer static_asserts the two agree).
inline constexpr int kSessionFlagRunning = 1;

/// The session list the rail draws, and the queue the UI->D hand-off goes through.
///
/// What is left of the v4 sidebar is its *data*: the snapshot, the active id and the action
/// queue. Its renderer, its rename/search/menu/panel fields and its five ImVec4 chrome colours
/// retired with it in task 41 - the rail keeps its own state (`model::RailState`), and no view
/// reads a colour from here.
struct ChatTabSessionPanel {
    std::vector<SessionEntry> sessions; // full snapshot
    std::string activeId;               // active session id from the snapshot
    std::deque<SessionAction> actions;  // UI -> D queue
};

struct ChatTab {
    ChatMessageStyle style;
    /// Ids of the messages whose body is shown. Keyed by `ChatMessage::id`, not
    /// by the deque index: ids are monotonic and unique, so an evicted message
    /// can never hand its expansion state to a neighbour. The default state
    /// (newest messages and final answers open) is applied once per message —
    /// see `expansionDefaultThroughId` — so a user toggle is never overwritten
    /// and a thread loaded from disk opens only its newest messages.
    std::set<size_t> outputLineOpen;
    /// Ids of the expanded assistant-work groups. Groups are not messages, so
    /// they keep their own set; the key is the group's first message id (the
    /// group identity today — task 25 replaces it with the adapter's group id).
    std::set<size_t> openWorkGroups;
    /// Highest message id whose default expansion state has been applied. Ids
    /// are monotonic, so one watermark is enough; it makes the default a
    /// one-shot decision exactly like the ImGui storage it replaces, without
    /// storing per-id state. Expansion is data (D-GROUPID), never ImGui
    /// per-id storage; reset with the thread (tuiClearOutput).
    size_t expansionDefaultThroughId{0};
    std::deque<ChatMessage> outputLines;
    std::size_t outputLineNextId{1};
    static constexpr size_t MaxChatMessages = 1000;

    std::vector<RenderGroup> renderGroups;
    size_t renderGroupFirstId{0};
    size_t renderGroupLastId{0};

    /// The streaming message's reserved id, shared with `model::kStreamMessageId`:
    /// one number names the stream group's block.
    static constexpr size_t StreamChatMessageId = model::kStreamMessageId;
    /// True while a stream bubble is in flight: the answer is arriving, so the thread
    /// draws it and the now bar's phase reads `writing` (task 28). Initialised here
    /// because the frame reads it on every render and a default-constructed `TuiState`
    /// would otherwise hand out an indeterminate value.
    bool hasStreamMsg{false};
    ChatMessage streamMsg;

    /// The newest stream snapshot the update throttle has not applied yet (task 38).
    ///
    /// D calls `tuiUpdateStreamChatMessage` on every stream tick - up to ~100 times a second,
    /// while the thread it feeds is drawn at 20-30 - and each snapshot is the *whole*
    /// accumulated message. A snapshot that arrives inside `model::kStreamUpdateIntervalMs` of
    /// the last one is therefore parked here instead of being stored: the next frame after the
    /// window applies it, so a burst costs one repaint and nothing is lost (the parked snapshot
    /// is the newest text, and a stop applies it immediately). `pendingStream` is false whenever
    /// no stream is live - a clear drops both the parked snapshot and the flag.
    ChatMessage pendingStreamMsg;
    bool pendingStream{false};
};

struct TuiState {
    bool isLogActive{false};
    std::string iniFilename;
    int maxWidth = 0; // 0 = unlimited (current behavior)

    /// The workbench's `ui.conf` was read for this state (D-UI-CONF): the frame layer's
    /// one-shot latch, because the read happens on the first frame - the first moment the state
    /// knows its profile name - and the debounced write runs on every frame after it.
    bool uiConfLoaded{false};

    /// The pins were pruned against a complete session snapshot (D-UI-CONF): the first non-empty
    /// snapshot is the moment the id set is known, and pruning earlier would drop the pins of
    /// sessions that are merely still loading. One-shot, like the load above.
    bool uiConfPinsPruned{false};

    /// How many log records this state has ever accepted (`tuiAddLogRecord` is the only
    /// writer; the two-string `tuiAddLogMessage` goes through it). A monotonic *counter*, not
    /// a store: the record ring is bounded, so "the ring got shorter" cannot tell a new record
    /// from an eviction, and the inspector's unread badge has to count arrivals - it reads
    /// `UiState::unreadActivity` since task 31, which the Activity projection increments from
    /// this same arrival.
    std::size_t logRecordSeq{0};

    /// v4 backend ownership: attached by the C API's tuiInit(), torn down by
    /// tuiDestroyState() before this state is freed; null = backend-less
    /// (legal and inert — headless harnesses never attach one).
    std::unique_ptr<Backend> backend;
    /// ImGui context created by tuiInit() for this state; null = none.
    /// tuiDestroyState() destroys exactly this context and never a context it
    /// did not create (headless harnesses keep their own).
    ImGuiContext* ownedContext = nullptr;

    /// GUI view settings (font scale, persisted window size); owned here so
    /// the Options window and the GUI backend share one authoritative copy.
    TuiSettings settings;
    /// Options window open (View -> Options); guards stacking duplicates.
    bool optionsOpen = false;
    /// Options window: content extent measured after the last text widget in
    /// the previous frame (window-local, title bar included). Text lines
    /// define the width, the full-width slider only stretches.
    ImVec2 optionsContentSize = ImVec2(0.0f, 0.0f);
    /// Text scale the Options window was last fitted for; a differing value
    /// triggers one re-fit (first open, then every font-scale change).
    float optionsFitScale = 0.0f;

    bool readyStatus{true};
    std::chrono::system_clock::time_point startProcesssingTime;

    /// The model backend is unreachable: the thread shows the offline card
    /// (`model::ThreadView::Offline`) and the composer is disabled with a
    /// tooltip. A flag, not a string: the UI never parses an error sentence to
    /// find this out.
    ///
    /// Trigger (task 39, D-STATUS): `tuiSetRuntimeStatus` sets it when the run's phase is
    /// `offline`, and clears it when the status is cleared - the numbers are the only source
    /// that can say it, and no message text is ever matched. A harness sets it directly for the
    /// offline scenes (the headless smoke test does).
    bool offline{false};

    /// The unknown phase ordinal `tuiSetRuntimeStatus` last complained about, whether it has
    /// complained at all, and how many lines it has written. Per state, not per process: the
    /// dedup belongs to this UI's log, so a fresh state (a test, a re-init) starts clean.
    /// `-1` is a valid ordinal from a newer sender and cannot double as "nothing yet" - the bool
    /// is the absence marker. Frame-thread only, like the fields around it.
    std::int32_t lastUnknownPhase{-1};
    bool warnedUnknownPhase{false};
    /// One per line actually written; the contract test asserts this instead of capturing stderr.
    std::size_t unknownPhaseWarnings{0};

    ChatTab chat;
    ChatTabLeftPanel left;
    ChatTabSessionPanel sessionPanel;

    /// The raw log ring: the single bounded store for everything the C ABI's
    /// `tuiAddLogRecord` delivers (task 32, D-LOG). FIFO, 5000 records or 2 MB of string
    /// payload, whichever comes first - the Raw Log (task 33) renders it and the Activity
    /// tab draws a 500-row projection of it.
    std::deque<LogRecord> logRecords;
    static constexpr size_t MaxLogRecords = 5000;
    static constexpr size_t MaxLogRecordBytes = 2u * 1024u * 1024u;
    /// The string bytes the ring currently holds (the second bound's measurement).
    size_t logRecordBytes = 0;
    /// Records the ring had to drop because a bound was reached (never silently: the Raw
    /// Log draws `N records dropped` and an error drop raises one Activity error).
    size_t droppedRecords = 0;
    /// The archive file (`tuiSetLogging`), opened lazily by the first record a session
    /// archives and closed by `tuiSetLogging(false)` or by the state's destruction. One handle
    /// for the whole run: an archive write must not cost one `fopen` per record, and a failed
    /// open is not retried either.
    LogArchive logArchive;

    RichMd::Context* mdContext =
        nullptr; // rich_md context; owned by the tuiInit/tuiDestroyState lifecycle

    bool autoScroll = true;

    UserQueryState userQuery;

    /// The raw status sentence D sent (`formatStatusText`). Write-only inside
    /// C++: the view reads `statusChips`, not this string. `tuiSetStatusText` is
    /// its only writer and returns early on an unchanged value, which is what
    /// keeps the chip derivation off the per-render path.
    std::string statusText;

    /// The four status chips derived from `statusText` (task 6). Recomputed by
    /// tuiSetStatusText — once per *change*, never per frame: D re-sends the same
    /// sentence on every render and the setter drops the repeat, so a chip is a
    /// formatted number computed once per value change (§6.3, and task 28's "no
    /// per-frame formatting"). Seeded from the default sentence so a state that
    /// never receives one still renders `ctx 0/0 | — | none | Ready`.
    model::StatusChips statusChips = model::splitStatusText(model::kDefaultStatusSentence);

    /// Pending run-cancel request: set by the composer's Stop button (frame
    /// code, tuiRequestCancel), consumed once by the D frame loop
    /// (tuiTakeCancel -> the same stopAgent() the "/stop" command uses).
    /// Frame-thread only, no mutex (§3.8) — debugIsFrameThread(*this) proves it.
    bool cancelPending{false};

    /// UI-side latch for a cancel the user already asked for. Unlike
    /// cancelPending (one frame: the D loop consumes it right after the render
    /// that set it) this stays set until the run ends (`tuiReadyStatus(1)`), so
    /// the composer can keep showing `Stopping` and refuse a second request
    /// across frames — the run itself may take seconds to wind down.
    bool cancelRequested{false};

    /// Exit request: set by the palette's Quit row (task 10) and by the keymap's
    /// quit chord (task 11). tuiRender() returns false from the next
    /// frame on, which the D frame loop turns into its normal exit handshake -
    /// the same path a window close takes. Frame-thread only.
    bool quitRequested{false};

    /// `Ctrl+Q` while a run is active asks before it quits (D-KEYMAP): the first
    /// press sets this and the log says so, the second press quits. Cleared when
    /// the run ends (tuiSetReadyStatus(true)), so an arming from an old run can
    /// never turn the next single press into a quit. Frame-thread only.
    bool quitConfirmArmed{false};

    /// True on a frame where a view-local handler already took the Escape press:
    /// the rail's rename box, the rail filter, or an open ImGui popup/menu. The
    /// frame-level Esc steps and the composer's own chain stand down while this
    /// is set, so one press can never close a local surface *and* cancel the run
    /// behind it (D-ESC's "one step per press"). Reset at the top of tuiRender;
    /// written by the shell, read by the composer and by dispatchKeys. Frame-
    /// thread only.
    bool escapeConsumed{false};

    /// Whether an ImGui popup/menu was open at the end of the previous frame.
    /// ImGui's NavUpdate closes popups inside NewFrame, before tuiRender runs, so
    /// "a popup was up when this Esc arrived" has to be read from the previous
    /// frame; the popup part of escapeConsumed is latched from this. Frame-
    /// thread only.
    bool popupOpenLastFrame{false};

    /// Thread that drives this state's frames: claimed by the first tuiRender
    /// (debugMarkFrameThread) and checked by the frame-thread-only accessors.
    /// Per state, so a second state driven from another thread (a test harness,
    /// a future second window) is not flagged by the first one's claim.
    std::thread::id frameThread{};
    bool frameThreadClaimed{false};

    /// The `Ctrl+K` palette overlay (views/palette.h): its row table is built
    /// from this state on open and on query change, and it is drawn as an
    /// overlay above the shell, never as part of its tiling (D-OVERLAY).
    views::PaletteState palette;

    /// The workbench's own state (model/ui_state.h): the shell preferences, the
    /// derived thread body, and the thread's paging / expansion / follow flags.
    /// One writer (the frame thread); `views/*` only reads it.
    model::UiState ui;

    /// The top bar's state (views/topbar.h): its inline rename box is the part that lives
    /// across frames, while the session title, the run state and the `RAW` badge are derived
    /// from this aggregate once per frame by the shell (the bar itself reads no aggregate).
    /// A committed rename and a consumed Escape are the bar's *answers*: the shell applies
    /// and clears them in the same frame (`views/shell.cpp`).
    views::TopBarState topBar;

    /// The composer's state (views/composer.h, task 27): the height it drew with (fed
    /// back to the layout so the thread reserves it), the field's keyboard latch, the
    /// history cursor and the paste question. The *draft* is not here - it is
    /// `UserQueryState::inputBuf`, the store the field edits - and the
    /// answers (`submit`, `cancelRequested`, `escapeConsumed`) are applied by the shell
    /// in the frame that produced them.
    views::ComposerState composer;

    /// The inspector panel's state (views/inspector.h, task 29): the frame derives which tab is
    /// showing and whether the region floats, and the surface answers with the tab it wants.
    /// The tab itself is a preference (`ShellPrefs::tab`) - this struct mirrors it for the one
    /// frame that draws it, so a click and a saved `InspectorTab=` cannot disagree.
    views::InspectorState inspector;

    /// The Raw Log surface's state (views/rawlog.h, task 33): its filter field, the one expanded
    /// record and the answers the frame applies. The *open* flag is not here - it is
    /// `model::UiState::rawLogOpen`, which the now bar's badge also reads and which is never
    /// persisted (D-RAWLOG); this struct mirrors it for the frame that draws the surface.
    views::RawLogState rawLog;

    /// The settings sheet's state (views/settings.h, task 34). Named `settingsSheet` and not
    /// `settings`: `TuiState::settings` is the `TuiSettings` of `imgui2.conf` (the font scale and
    /// the window size), and the sheet *edits* that struct through
    /// `renderSettings()` - one name for the surface and one for the value keeps the two apart.
    /// The open flag lives here rather than on `UiState`: it is a mode of this process, never a
    /// preference, and `ui.conf` has no key for it.
    views::SettingsState settingsSheet;

    /// The help page's state (views/help.h, task 42): its query, its derived rows and the scroll
    /// position. The open flag is here and not on `UiState` for the sheet's reason - it is a mode
    /// of this process, never a preference, and no file carries it.
    views::HelpState help;

    /// The v4 message store, seen through the adapter (store/thread_store.h,
    /// D-ADAPTER): the only v4-aware object the workbench frame touches. It owns
    /// the group ids - monotonic, never reused, reset by `tuiClearOutput()` - the
    /// expansion seam and the drawn-height cache.
    store::ThreadStoreAdapter threadAdapter{*this};

    /// True while an overlay owns the keyboard (D-OVERLAY).
    /// *frame-global* key handlers - the rail's Esc, the End-follow - must stand
    /// down while this holds, or one key press would act twice: once inside the
    /// overlay and once on the region behind it (D-ESC's "one step per press").
    /// Focus-gated handlers do not need it. Later overlays add their flag here.
    ///
    /// The palette, the settings sheet, the help page and the Raw Log's full-screen overlay are
    /// *modal* surfaces (task 42 adds the page): all of them take the keyboard, and each spends
    /// its own plain keys from the character queue. The inspector's panel is not one (task 29): an
    /// inspector sits beside the composer and the composer keeps typing. The Raw Log's *drawer*
    /// (the pixel profile) is part of the tiling, so it is not one either - the composer keeps
    /// typing there and the surface is driven by its controls. `Esc` still closes the grid's
    /// overlay, the sheet, the page and the floating panel - that is
    /// `views::escStepClosesOverlay()`, because "who owns the keyboard" and "what does Esc close"
    /// are two questions.
    bool overlayOwnsKeyboard() const {
        return help.open || palette.open || settingsSheet.open || rawLog.ownsKeyboard();
    }

    TuiState() { startProcesssingTime = std::chrono::system_clock::now(); }
};

/// True while a run is active and no stop has been requested yet: the `Esc`
/// chain's "cancel the run" step applies exactly then, so the press after it
/// moves on to the input (D-ESC's one step per press). Read by the frame-level
/// keymap dispatcher (which performs that step) and by the composer's own chain
/// (which must not re-run it).
inline bool cancelStepOpen(const TuiState& state) {
    return !state.readyStatus && !state.cancelRequested;
}

/// The one-line prompt the quit confirmation shows while it is armed: on the
/// status row the active tab draws (the composer's row on the Chat tab, the
/// reserved bottom margin on the Log tab) and in the UI log. One string, three
/// readers - the chord it names is the one `plan/key_gate.md` measured.
constexpr const char* kQuitConfirmHint = "run active - press Ctrl+Q again to quit";

/// The same question in the shortest form that still names the key that answers it.
///
/// A narrow row cannot hold the full sentence, and cutting its *tail* would cut exactly
/// the instruction ("press Ctrl+Q again") - the one part a user cannot guess. So the row
/// that has to save room says this instead: 20 cells, still a key and still a question.
/// The framing ("run active -") is what is given up, because the row's left side already
/// says the run is active and what it is doing.
constexpr const char* kQuitConfirmShort = "Ctrl+Q again to quit";

/// Ask to quit through D-KEYMAP's "confirm while running" policy: with no run
/// active (or with the confirmation already armed) the request goes through and
/// this returns true; while a run is active the first request only arms
/// `TuiState::quitConfirmArmed` and returns false. Both the quit chord and the
/// palette's Quit row come through here, so the policy belongs to quitting and
/// not to one key. Frame-thread only.
bool tuiRequestQuitConfirmed(TuiState& state);

/// rich_md markdown lifecycle (the RichMd::Context* in TuiState). The context
/// and its fonts are created in tuiInit() for the active render profile and
/// destroyed in tuiDestroyState(); headless states never create one (their
/// renders take the plain-text fallback).
///
/// The lifecycle only; the render path is `ui/markdown.h`'s (the wrapper), which
/// the create/destroy functions hand the context to (`ui::setMarkdownContext`) so
/// that `RenderRaw`, the block ids, the token theme and the selection policy have
/// exactly one home.
///
/// Text (grid) profile: the fonts must be added and warmed up BEFORE the text
/// backend init, because ImTui_ImplText_Init() then builds and locks the
/// atlas — a later GetFontBaked() would resolve the closest existing bake
/// (16px -> 1/16 scale) instead of the exact 1-cell bake. So
/// initMarkdownTextMode() runs first, and finishMarkdownTextMode() pins the
/// default font by name once the backend is up.
/// GUI profile: fonts are lazy, so the context is created after the backend.
void initMarkdownTextMode(
    TuiState& state); // before the text backend init (fonts before ImTui_ImplText_Init)
                      //
void finishMarkdownTextMode(
    TuiState& state); // after the text backend init (pins io.FontDefault by name)
                      //
void initMarkdownGuiMode(TuiState& state); // after a successful GUI backend init
                                           //
/// Destroys `state`'s rich context, if it has one. The wrapper's context slot is
/// process-wide while a state is not, so it is cleared only when it holds *this*
/// state's context - destroying a headless state (no context of its own) must not
/// take the pointer away from the state that owns one.
void destroyMarkdown(TuiState& state); // before backend shutdown / ImGui::DestroyContext

/// Apply the Ink/Graphite theme (ui/tokens.h) for the ACTIVE render profile: the
/// pixel profile uses the design's exact colours, the text grid the depth the
/// terminal advertises (ui::detectColorDepth, design §4.15).
///
/// The C API's tuiInit() calls this after the backend initialized - a backend
/// init resets several style colours - and headless harnesses call it right
/// after their own ImGui::CreateContext() plus backend init. Once per state, not
/// per frame: the frame loop never applies a theme, and
/// ui::themeApplyCount() is what the tests assert that on.
///
/// This wrapper exists because the *profile* decision is app knowledge: `ui/`
/// owns the palette and must not read `tuiIsTextGrid()`. It also applies the
/// text grid's grab metric ("one cell is a dot, three read as a thumb") - a cell
/// measurement, not a colour, so it stays out of `ui::applyTheme`; the GUI keeps
/// ImGui's 12 px thumb, which a 3 px grab would replace on a 1280-wide window.
/// Task 13's metrics module takes the metric over.
void applyUiTheme();

/// True while the text (ncurses/imtui) backend is live: the UI renders on the
/// 1-cell terminal grid. Set by ImTui_ImplText_Init() and reset by
/// ImTui_ImplText_Shutdown(); the GUI path never sets it. The pixel profile
/// is selected when this is false — see the gates in `tui.cpp` and the ledger in
/// `tools/gate_ratchet.txt`.
///
/// A view does not call this (D-METRICS): geometry asks `ui::metricsFor()`,
/// which takes the profile as data, and a colour asks `ui::color()`, which the
/// theme already resolved for the active depth. The callers that remain are the
/// ledger in `tools/gate_ratchet.txt`, and `./test_metrics` fails when a new one
/// appears or a listed one grows.
bool tuiIsTextGrid();

/// Render one frame. Returns false to exit.
bool tuiRender(TuiState& state);

/// Set the logging to on/off. Must be done before tuiRender is called.
///
/// On: opens the archive lazily at the first line and clears a previous failed-open/write
/// latch (a new request is a new attempt). Off: closes the handle, so nothing lands in a file
/// the user switched off.
void tuiSetLogging(TuiState& state, bool onOff);

/// Set the maximum rendered width in terminal columns. 0 = unlimited.
/// Re-evaluated every frame, so the value applies from the next
/// tuiRender regardless of when it is set.
/// Text backend only: effective width = min(terminal width, maxWidth).
/// The GUI ignores the cap (pixel profile) — its DisplaySize is in
/// pixels, so a column value cannot be applied meaningfully; the window
/// size is the cap.
void tuiSetMaxWidth(TuiState& state, int maxWidth);

/// Set the ready flag (run finished / run started). A transition to ready also
/// drops the composer's Stopping latch (TuiState::cancelRequested), so the action
/// can send again and request a new cancel.
void tuiSetReadyStatus(TuiState& state, bool ready);

/// Ask the frame loop to stop the running agent: sets the one-slot
/// TuiState::cancelPending flag and the UI-side TuiState::cancelRequested latch
/// (idempotent — a second request after the first is a no-op while the run is
/// still winding down). Called by the composer's Stop button; the pending flag is
/// consumed by the D frame loop via tuiTakeCancel(). Frame-thread only.
void tuiRequestCancel(TuiState& state);

/// Consume the pending cancel request: true exactly once per request, false
/// afterwards or when nothing is pending. TuiState::cancelRequested stays set
/// until the run ends, so the UI can keep showing `Stopping`.
bool tuiTakeCancel(TuiState& state);

/// Ask the frame loop to exit: sets TuiState::quitRequested, and tuiRender()
/// returns false from the next frame on (the D loop's `tuiRender(...) == 0`
/// branch is the normal exit handshake). Called by the palette's Quit row now;
/// task 11's keymap uses the same slot. Frame-thread only.
void tuiRequestQuit(TuiState& state);

/// True before the first frame and on every frame after it on the thread that
/// drives this state's tuiRender(); false on any other thread. The cancel flags
/// are frame-thread state without a mutex, so their accessors assert this in
/// debug builds (NDEBUG builds keep the query available for tests and
/// diagnostics — the unit tests exercise it in Release, where the assertion is
/// compiled out).
bool debugIsFrameThread(const TuiState& state);

/// Claim the calling thread as this state's frame thread if none has been
/// claimed yet (tuiRender() calls this on every frame; tests call it directly to
/// exercise debugIsFrameThread() without a backend).
void debugMarkFrameThread(TuiState& state);

/// Release this state's frame-thread claim (tests only: lets a harness re-claim
/// the loop from another thread without tripping the debug assertion).
void tuiResetFrameThread(TuiState& state);

/// Set the file imgui persists its settings to (io.IniFilename); the GUI
/// backend stores the window size in settingsPath(filename) (imgui2.ini -> imgui2.conf).
/// Call this before the backend attach creates the ImGui context (`tuiInit`
/// in the v4 C API flow), which captures the name; a later call has no
/// effect. Empty disables persistence.
void tuiSetIniFilename(TuiState& state, const std::string& filename);

/// The close path's `ui.conf` save (D-UI-CONF, task 29), called by `tuiDestroyState()`: a GUI
/// window closed with its X ends the D loop through `shouldClose()` and never sets
/// `quitRequested`, so the quit path's save does not run for it. Workbench-only, and a no-op
/// without a profile or without a change; the quit path may have saved the same bytes already.
void saveUiConfOnShutdown(TuiState& state);

void tuiInitQueryHistory(TuiState& state, const std::vector<std::string>& history);

/// The wall clock, in unix milliseconds - the epoch every timestamp in the UI uses (the
/// agents' lanes, the rail's relative times and a log record's arrival stamp).
std::int64_t nowMs();

/// Add a log message with FIFO eviction if bound exceeded.
void tuiAddLogMessage(TuiState& state, const LogMessage& msg);

/// Append one raw record to the log ring (task 32, D-LOG): FIFO eviction at
/// `MaxLogRecords` / `MaxLogRecordBytes`, the `droppedRecords` counter, the arrival counter
/// (`logRecordSeq`), and - when the archive is on
/// - its historical `timestamp - level: message [function:line]` line in `llmfun_ui_log.txt`.
///
/// The caller owns the strings (this is the owned copy, not the C ABI's borrowed POD): the
/// C wrapper copies the inbound record first, so D may free its buffers as soon as the call
/// returns. Frame thread only, like every other mutating entry point.
void tuiAddLogRecord(TuiState& state, const LogRecord& record);

/// The historical archive line for a record:
/// `2026-10-27T20:57:14.123 - info: message [func:line]`.
///
/// The format is the one D used to produce (`"%s - %s: %s [%s:%d]"`), now formatted in C++
/// from the record's fields - that is what keeps the archive byte-compatible with the
/// logs users already have. The `[func:line]` suffix is written
/// only when the record carries the metadata (every record D sends does; the two-string
/// compatibility wrapper does not).
std::string logArchiveLine(const LogRecord& record);

/// Is the Activity tab on screen (the last frame's derivation)? The one answer both append
/// paths use for "this record arrived read or unread" (D-UNREAD).
bool isActivityVisible(const TuiState& state);

/// Close the archive handle (idempotent): called by `tuiSetLogging(false)`, by the state's
/// destroy path, and when the archive fails - a latched stream is not a retryable stream - so a
/// quit never leaves buffered record lines behind.
void closeLogArchive(TuiState& state);

/// Add an output line with FIFO eviction if bound exceeded.
void tuiAddOutputLine(TuiState& state, const ChatMessage& msg);

/// Clear all output lines.
void tuiClearOutput(TuiState& state);

/// Offer one stream snapshot (design FR-08, task 38): the store's write path and its update
/// throttle in one call.
///
/// D calls this on every stream tick - up to ~100 times a second, while the thread it feeds is
/// drawn at 20-30 - and each snapshot is the *whole* accumulated message. Three outcomes, one
/// store:
///
///   * byte-identical to the text already in the store (or parked) - counted in
///     `StreamState::unchanged` and nothing else happens: no copy, no revision, no repaint.
///     D's loop re-sends the accumulated message on every tick, including ticks that changed
///     nothing, so this is the common case and the cheapest one;
///   * the window has elapsed (`model::kStreamUpdateIntervalMs`, ~30 fps) or this is the
///     stream's first snapshot - stored now, `StreamState::applied` advances;
///   * inside the window - parked in `ChatTab::pendingStreamMsg`, counted in
///     `StreamState::coalesced`; the next frame after the window stores it, so a burst costs
///     one repaint instead of one per tick and the last text is never lost (a stop applies a
///     parked snapshot immediately, before the clock freezes).
void tuiUpdateStreamChatMessage(TuiState& state, const ChatMessage& msg);

void tuiStreamChatMessageClear(TuiState& state);

/// Set the status line text.
void tuiSetStatusText(TuiState& state, const std::string& text);

/// Apply the run's structured status (task 39's `tuiSetRuntimeStatus`), or clear it.
///
/// One entry point for both directions, because `model::RuntimeStatus::structured` is the
/// flag the frame layer already reads: `structured == true` stores the numbers and formats
/// the four chips from them (once per update, never per frame); `structured == false` hands
/// the row back to the status sentence and drops the one fact only the numbers can carry
/// (`TuiState::offline`). A clear cannot then forget half of the state.
void tuiSetRuntimeStatus(TuiState& state, const model::RuntimeStatus& status);

/// Get the current input buffer content.
std::string tuiGetInput(const TuiState& state);

/// Clear the input buffer.
/// @deprecated Input is now cleared internally by tuiRender() on submission.
void tuiClearInput(TuiState& state);

/// Check if input is ready to be submitted.
bool tuiIsSubmitReady(const TuiState& state);

/// Reset the submission flag.
void tuiResetSubmit(TuiState& state);

/// Get the last submitted query (set by tuiRender on Enter press).
/// Returns the captured query text.
/// Distinction: tuiGetInput() returns the current editable buffer;
/// tuiGetSubmitQuery() returns the last submitted query (read-only snapshot).
std::string tuiGetSubmitQuery(const TuiState& state);

/// Reports whether the frame we just rendered consumed at least one input
/// event (mouse, key, ...). If so, more input is likely to arrive: render
/// the next frame immediately instead of waiting for the refresh interval.
bool tuiHasMoreEvents();

} // namespace llmfun::tui
