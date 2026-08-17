#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "imtui/imtui.h"

#include "imgui_markdown.h"

namespace llmfun::tui {
inline ImVec4 lighten(ImVec4 color, float amount) {
    return ImVec4(std::min(color.x + amount, 1.0f), std::min(color.y + amount, 1.0f),
                  std::min(color.z + amount, 1.0f), color.w);
}

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

// Color configuration for each chat message type.
// 3 background groups (user, system/muted, finalAnswer) + 4 foreground colors for muted group.
struct ChatMessageStyle {
    // --- Background colors (3 groups) ---

    // User & Vision: Sky Blue background
    ImVec4 userBg = ImVec4(0.45f, 0.70f, 0.90f, 1.00f);
    ImVec4 userBgHover;
    ImVec4 userBgActive;

    // Assistant, ToolCall, ToolResponse, System: Dark Slate Gray background
    ImVec4 systemBg = ImVec4(0.35f, 0.37f, 0.40f, 1.00f);
    ImVec4 systemBgHover;
    ImVec4 systemBgActive;

    // FinalAnswer: Bright Green background
    ImVec4 finalAnswerBg = ImVec4(0.30f, 0.95f, 0.30f, 1.00f);
    ImVec4 finalAnswerBgHover;
    ImVec4 finalAnswerBgActive;

    // --- Foreground/text colors (4 types in muted group) ---

    // Assistant: Soft Green
    ImVec4 assistantFg = ImVec4(0.55f, 0.85f, 0.55f, 1.00f);

    // ToolCall: Warm Orange
    ImVec4 toolCallFg = ImVec4(0.95f, 0.70f, 0.30f, 1.00f);

    // ToolResponse: Dark Orange
    ImVec4 toolResponseFg = ImVec4(0.95f, 0.59f, 0.40f, 1.00f);

    // System: Light Slate Gray
    ImVec4 systemFg = ImVec4(0.70f, 0.75f, 0.80f, 1.00f);

    // Dark text for User and FinalAnswer (black text on bright backgrounds)
    ImVec4 darkText{0.0f, 0.0f, 0.0f, 1.0f};

    ChatMessageStyle() {
        userBgHover = lighten(userBg, 0.10f);
        userBgActive = lighten(userBg, 0.15f);

        systemBgHover = lighten(systemBg, 0.10f);
        systemBgActive = lighten(systemBg, 0.15f);

        finalAnswerBgHover = lighten(finalAnswerBg, 0.10f);
        finalAnswerBgActive = lighten(finalAnswerBg, 0.15f);
    }
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

struct ChatTabLeftPanel {
    ImVec4 thinkingNodeBg = ImVec4(0.2f, 0.2f, 0.2f, 1.0f);
    ImVec4 activeButton = ImVec4(0.4f, 0.4f, 0.45f, 1.0f);
    int panelW = 0;
    static constexpr int PanelWActivated = 30;
    bool panelOpen{true};

    std::vector<AgentStream> agents;
    static constexpr size_t MaxAgents = 16;

    int activeAgent{-1};

    bool autoScroll = true;
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

// One row of the session sidebar snapshot (full replace, A1).
struct SessionEntry {
    std::string id;
    std::string title;
    std::string preview;
    size_t messageCount{0};
    bool isActive{false};
};

struct ChatTabSessionPanel {
    ImVec4 activeButton = ImVec4(0.4f, 0.4f, 0.45f, 1.0f);
    ImVec4 previewColor = ImVec4(0.55f, 0.55f, 0.58f, 1.0f);  // dimmed preview line (A14)
    ImVec4 pendingButton = ImVec4(0.60f, 0.52f, 0.25f, 1.0f); // queued-switch row color (R18)
    ImVec4 matchColor = ImVec4(1.0f, 0.85f, 0.45f, 1.0f);     // filter match highlight (R25)
    int panelW = 0; // 0 = unset; init to PanelWActivated on first render
    static constexpr int PanelWActivated = 30;
    bool panelOpen{true}; // auto-open at startup

    std::vector<SessionEntry> sessions; // full snapshot (A1)
    std::string activeId;               // active session id from the snapshot
    std::deque<SessionAction> actions;  // UI -> D queue (A2/A7)

    char renameBuf[128] = {};            // rename input; init on row change or
                                         // toggle-open, never per frame
    bool renameActive{false};            // rename input visible (L8)
    std::string renameRowId;             // row renameBuf was initialized for (L3)
    bool renameFocus{false};             // focus the rename input on the next frame
    int renameSeq{0};                    // bumped on each toggle-open; keeps the
                                         // InputText state fresh per open
    std::string pendingDeleteId;         // two-step delete state (A5)
    std::string pendingSelectId;         // queued switch while busy (A12/R18); single
                                         // slot, last click wins; set by the busy
                                         // row-click branch, flushed as an ordinary
                                         // Select on the first ready frame, cleared
                                         // by tuiSetSessionList when stale (M3)
    std::array<char, 64> filterBuf = {}; // filter query (A19); whitespace = no
                                         // filter; zero-init because TuiState's
                                         // user-provided ctor skips value-init
    int filterSeq{0};                    // bumped on every programmatic clear
                                         // (A23); suffixes the InputText id to
                                         // force a fresh state (C10)
    bool filterNonEmptyLastFrame{false}; // rendered buffer was a real query at
                                         // the end of the last frame; lets A23
                                         // tell that 1.81's cancel_edit revert
                                         // already emptied the buffer on the
                                         // Esc frame (the revert runs in
                                         // NewFrame, before this code)
};

struct ChatTab {
    ImVec4 nestedAssistNodeBg = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    ImVec4 thinkingNodeBg = ImVec4(0.2f, 0.2f, 0.2f, 1.0f);

    ChatMessageStyle style;
    std::set<size_t> outputLineOpen;
    std::deque<ChatMessage> outputLines;
    std::size_t outputLineNextId{1};
    static constexpr size_t MaxChatMessages = 1000;

    std::vector<RenderGroup> renderGroups;
    size_t renderGroupFirstId{0};
    size_t renderGroupLastId{0};

    static constexpr size_t StreamChatMessageId = 0;
    bool hasStreamMsg;
    ChatMessage streamMsg;
};

enum class ActiveTab { chat, log };

struct MarkdownStyle {
    ImVec4 heading[4] = {ImVec4(1.0f, 0.9f, 0.2f, 1.0f), ImVec4(0.2f, 1.0f, 1.0f, 1.0f),
                         ImVec4(0.5f, 1.0f, 0.2f, 1.0f), ImVec4(0.8f, 0.8f, 0.8f, 1.0f)};
    std::string headingPrefix[4] = {"# ", "## ", "### ", "> "};

    ImVec4 strong = ImVec4(1.0f, 1.0f, 0.8f, 1.0f);
    ImVec4 italic = ImVec4(0.3f, 0.9f, 0.9f, 1.0f);
    ImVec4 inlineCode = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
};

struct TuiState {
    bool isLogActive{false};
    std::string iniFilename;

    bool readyStatus{true};
    std::chrono::system_clock::time_point startProcesssingTime;

    ChatTab chat;
    ChatTabLeftPanel left;
    ChatTabSessionPanel sessionPanel;

    std::deque<LogMessage> logMessages;
    static constexpr size_t MaxLogMessages = 1000;

    MarkdownStyle mdStyle;
    ImGui::MarkdownConfig mdConfig;

    bool autoScroll = true;

    UserQueryState userQuery;

    std::string statusText;

    ActiveTab activeTab = ActiveTab::chat;

    TuiState() { startProcesssingTime = std::chrono::system_clock::now(); }
};

/// Resolved width of the left panel slot (A6/H1): the pipeline panel wins
/// whenever it has agents (open or collapsed); otherwise the session panel
/// renders when open and keeps its 8-wide "Open" strip when closed (the
/// output area never covers the panel).
int leftPanelWidth(const TuiState& s);

void initMarkdownConfig(TuiState& state);

/// Initialize terminal and create TScreen.
/// Returns true on success, false on failure.
bool tuiInit(ImTui::TScreen** screen);

/// Cleanup and restore terminal state.
void tuiShutdown(ImTui::TScreen* screen);

/// Backend frame wrappers — encapsulate imtui backend details.
/// Call tuiNewFrame() at the start of each frame and tuiRenderFrame() at the end.
void tuiNewFrame();

void tuiRenderFrame(ImTui::TScreen* screen);

/// Render one frame. Returns false to exit.
bool tuiRender(TuiState& state);

/// Set the logging to on/off. Must be done before tuiRender is called.
void tuiSetLogging(TuiState& state, bool onOff);

/// Set the filename that imgui save window settings to.
/// By default it is in the "$cwd/imgui.ini".
/// Call this **after** `ImGui::CreateContext()` but **before** your main loop starts calling
/// `ImGui::NewFrame()`.
void tuiSetIniFilename(TuiState& state, const std::string& filename);

void tuiInitQueryHistory(TuiState& state, const std::vector<std::string>& history);

/// Add a log message with FIFO eviction if bound exceeded.
void tuiAddLogMessage(TuiState& state, const LogMessage& msg);

/// Add an output line with FIFO eviction if bound exceeded.
void tuiAddOutputLine(TuiState& state, const ChatMessage& msg);

/// Clear all output lines.
void tuiClearOutput(TuiState& state);

void tuiUpdateStreamChatMessage(TuiState& state, const ChatMessage& msg);

void tuiStreamChatMessageClear(TuiState& state);

/// Set the status line text.
void tuiSetStatusText(TuiState& state, const std::string& text);

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

} // namespace llmfun::tui
