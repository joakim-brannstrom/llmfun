#pragma once

#include <algorithm>
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
// Default values match the colorblind-safe palette from initializeDefaultColors().
struct ChatMessageStyle {
    // User message colors (Sky Blue)
    ImVec4 userColor = ImVec4(0.45f, 0.70f, 0.90f, 1.00f);
    ImVec4 userColorHover;
    ImVec4 userColorActive;
    // Assistant message colors (Soft Green)
    ImVec4 assistantColor = ImVec4(0.55f, 0.85f, 0.55f, 1.00f);
    ImVec4 assistantColorHover;
    ImVec4 assistantColorActive;
    // ToolCall message colors (Warm Orange)
    ImVec4 toolCallColor = ImVec4(0.95f, 0.70f, 0.30f, 1.00f);
    ImVec4 toolCallColorHover;
    ImVec4 toolCallColorActive;
    // ToolResponse message colors (Dark Warm Orange)
    ImVec4 toolResponseColor = ImVec4(0.95f, 0.59f, 0.40f, 1.00f);
    ImVec4 toolResponseColorHover;
    ImVec4 toolResponseColorActive;

    // Vision message colors (Purple — distinct from existing types)
    ImVec4 visionColor = ImVec4(0.70f, 0.50f, 0.90f, 1.00f);
    ImVec4 visionColorHover;
    ImVec4 visionColorActive;

    // System message colors (Slate Gray — subdued, non-user-facing)
    ImVec4 systemColor = ImVec4(0.60f, 0.65f, 0.70f, 1.00f);
    ImVec4 systemColorHover;
    ImVec4 systemColorActive;

    // FinalAnswer message colors (Bright Green — distinct from Assistant)
    ImVec4 finalAnswerColor = ImVec4(0.30f, 0.95f, 0.30f, 1.00f);
    ImVec4 finalAnswerColorHover;
    ImVec4 finalAnswerColorActive;

    ChatMessageStyle() {
        userColorHover = lighten(userColor, 0.10f);
        userColorActive = lighten(userColor, 0.15f);

        assistantColorHover = lighten(assistantColor, 0.10f);
        assistantColorActive = lighten(assistantColor, 0.15f);

        toolCallColorHover = lighten(toolCallColor, 0.10f);
        toolCallColorActive = lighten(toolCallColor, 0.15f);

        toolResponseColorHover = lighten(toolResponseColor, 0.10f);
        toolResponseColorActive = lighten(toolResponseColor, 0.15f);

        visionColorHover = lighten(visionColor, 0.10f);
        visionColorActive = lighten(visionColor, 0.15f);

        systemColorHover = lighten(systemColor, 0.10f);
        systemColorActive = lighten(systemColor, 0.15f);

        finalAnswerColorHover = lighten(finalAnswerColor, 0.10f);
        finalAnswerColorActive = lighten(finalAnswerColor, 0.15f);
    }
};

struct ChatMessage {
    std::string summary;
    std::string text;
    std::string thinking;
    ChatMessageType type = ChatMessageType::Assistant;
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

// Note: This struct is non-copyable and non-movable due to std::mutex.
// Always pass by reference (TuiState&) to avoid accidental copies.
struct TuiState {
    bool isLogActive{false};
    std::string iniFilename;

    bool readyStatus{true};
    std::uint32_t busyIndicatorState{0};
    std::chrono::system_clock::time_point nextIndicatorIncr;

    std::set<size_t> outputLineOpen;
    std::deque<ChatMessage> outputLines;
    static constexpr size_t MaxChatMessages = 1000;

    std::deque<LogMessage> logMessages;
    static constexpr size_t MaxLogMessages = 1000;

    ImGui::MarkdownConfig mdConfig;

    // Auto-scroll flag
    bool autoScroll = true;

    UserQueryState userQuery;

    // Status line text
    std::string statusText;
    // Color configuration for chat message headers
    ChatMessageStyle chatStyle;
};

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

/// Add a log message with FIFO eviction if bound exceeded.
void tuiAddLogMessage(TuiState& state, const LogMessage& msg);

/// Add an output line with FIFO eviction if bound exceeded.
void tuiAddOutputLine(TuiState& state, const ChatMessage& msg);

/// Clear all output lines.
void tuiClearOutput(TuiState& state);

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
