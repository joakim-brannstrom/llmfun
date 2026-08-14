/// UI plumbing for the agent subcommand: UiMessenger (TUI message bridge),
/// status-text formatting, and the stream updaters that forward streaming
/// model output to the TUI.
module llm.app_agent.ui;

import std.array : empty;
import std.concurrency : Tid, send;
import std.conv : text;
import std.format : format;
import std.stdio : writeln;

import llm.types : ServerStat, StreamMessage, StreamToolCall, IStreamCallback;
import llm.tui; // Ui* message types (UiChatMessage, UiPipelineClear, ...)
import llmfun_tui; // TuiChatMessageType (C binding)
import my.path : Path;

/// Message bridge between the agent thread and the TUI thread.
/// In blocked (one-shot) mode all messages go to stdout via writeln.
class UiMessenger {
    Tid uiTid;
    bool blocked;

    this(Tid t, bool b = false) {
        uiTid = t;
        blocked = b;
    }

    bool isActive() {
        return !blocked;
    }

    void setActive(bool onOff) {
        blocked = !onOff;
    }

    void ready() {
        if (blocked)
            return;
        send(uiTid, UiAgentReady.init);
    }

    void busy() {
        if (blocked)
            return;
        send(uiTid, UiAgentBusy.init);
    }

    void terminate() {
        if (blocked)
            return;
        if (uiTid == Tid.init)
            return; // safety: no UI thread to terminate
        send(uiTid, UiTerminate.init);
    }

    void chatMessage(string msg, TuiChatMessageType type) {
        if (blocked) {
            writeln(msg);
        } else {
            send(uiTid, UiChatMessage(msg, type));
        }
    }

    void chatThinkMessage(string msg, string thinking, TuiChatMessageType type) {
        if (blocked) {
            if (!thinking.empty) {
                writeln("Thinking: ", thinking);
            }
            writeln(msg);
        } else {
            send(uiTid, UiChatThinkMessage(msg, thinking, type));
        }
    }

    void statusText(string status) {
        if (blocked)
            return;
        send(uiTid, UiStatusText(status));
    }

    void finalAnswer(string msg) {
        if (blocked) {
            writeln(msg);
        } else {
            send(uiTid, UiFinalAnswer(msg));
        }
    }

    void clearChat() {
        if (blocked)
            return;
        send(uiTid, UiClearChat.init);
    }

    void logFile(bool useFile) {
        if (blocked)
            return;
        send(uiTid, UiLogFile(useFile));
    }

    void setIniFile(string path) {
        if (blocked)
            return;
        send(uiTid, UiSetIniFile(Path(path)));
    }

    // Streaming methods — silently skipped in blocked (one-shot) mode.
    // One-shot mode produces final output via writeln in chatMessage/finalAnswer;
    // incremental streaming feedback is not needed.
    void streamStatusText(string status) {
        statusText(status); // delegate to canonical method
    }

    void streamChatMessage(string msg, string thinking) {
        if (blocked)
            return;
        send(uiTid, UiStreamChatMessage(msg: msg, thinking: thinking));
    }

    void streamChatDone() {
        if (blocked)
            return;
        send(uiTid, UiStreamChatDone.init);
    }

    void pipelineStreamChatMessage(string agentId, string content,
            string thinking, string role, string status) {
        if (blocked)
            return;
        send(uiTid, UiPipelineStreamChatMessage(agentId: agentId, content: content,
                thinking: thinking, role: role, status: status));
    }

    void pipelineStreamDone(string agentId) {
        if (blocked)
            return;
        send(uiTid, UiPipelineStreamDone(agentId));
    }

    void pipelineClear() {
        if (blocked)
            return;
        send(uiTid, UiPipelineClear.init);
    }
}

/// Format the status bar text: context usage, tokens/s, active model, ready state.
string formatStatusText(bool readyState, long contextSize, ServerStat stat, string model) {
    return i"Context $(stat.context)/$(contextSize) tokens | $(
            format!"%.1f"(stat.predictedPerSecond)) tok/s | Model: '$(model)' | $(
            readyState ? "Ready" : "Busy")".text;
}

/// Forwards streaming model output to the TUI (single-agent queries).
class StreamMessageUpdater : IStreamCallback {
    UiMessenger uiMsg;
    long contextSize;
    string modelName;

    this(UiMessenger messenger, long contextSize, string modelName)
    in (messenger !is null, "UiMessenger must not be null") {
        this.uiMsg = messenger;
        this.contextSize = contextSize;
        this.modelName = modelName;
    }

    override void messageUpdate(StreamMessage msg, StreamToolCall[] tools, ServerStat stat) {
        string content = msg.content;
        if (!tools.empty) {
            foreach (tool; tools) {
                content ~= "\n--- Tool ---\n";
                content ~= tool.toPrettyString(1000);
                content ~= "\n\n";
            }
        }

        uiMsg.streamStatusText(formatStatusText(false, contextSize, stat, modelName));
        uiMsg.streamChatMessage(content, msg.reasoning);
    }

    override void streamMessageDone() {
        uiMsg.streamChatDone();
    }

    override void setId(string id) {
    }

    override IStreamCallback clone() {
        return new StreamMessageUpdater(uiMsg, contextSize, modelName);
    }
}

/// Forwards streaming pipeline output to the TUI (per-agent messages).
class PipelineStreamMessageUpdater : IStreamCallback {
    UiMessenger uiMsg;
    long contextSize;
    string modelName;
    string agentId;

    this(UiMessenger messenger, long contextSize, string modelName)
    in (messenger !is null, "UiMessenger must not be null") {
        this.uiMsg = messenger;
        this.contextSize = contextSize;
        this.modelName = modelName;
    }

    override void messageUpdate(StreamMessage msg, StreamToolCall[] tools, ServerStat stat) {
        string content = msg.content;
        if (!tools.empty) {
            foreach (tool; tools) {
                content ~= "\n--- Tool ---\n";
                content ~= tool.toPrettyString(1000);
                content ~= "\n";
            }
        }

        string status = i"Context $(stat.context)/$(contextSize) tokens | $(
                format!"%.1f"(stat.predictedPerSecond)) tok/s".text;

        uiMsg.pipelineStreamChatMessage(agentId: agentId, content: content,
                thinking: msg.reasoning, role: msg.role, status: status);
    }

    override void streamMessageDone() {
        uiMsg.pipelineStreamDone(agentId);
    }

    override void setId(string id) {
        agentId = id;
    }

    override IStreamCallback clone() {
        return new PipelineStreamMessageUpdater(uiMsg, contextSize, modelName);
    }
}
