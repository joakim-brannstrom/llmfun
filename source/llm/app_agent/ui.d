/// UI plumbing for the agent subcommand: UiMessenger (TUI message bridge),
/// status-text formatting, and the stream updaters that forward streaming
/// model output to the TUI.
module llm.app_agent.ui;

import std.array : empty;
import std.conv : text;
import std.format : format;
import std.stdio : writeln;

import llm.types : ServerStat, StreamMessage, StreamToolCall, IStreamCallback;
import llm.tui; // Ui* message types (UiChatMessage, UiPipelineClear, ...)
import llmfun_tui; // TuiChatMessageType (C binding)
import my.path : Path;
import my.actor.channel : Channel;
import my.actor.mailbox : TypedAddress;

/// One-way sink for agent->TUI messages. Abstracts where TUI traffic goes so
/// the agent can target a live TUI actor (channel), the console (one-shot),
/// or a recording buffer (tests) without the call sites changing.
interface TuiSink {
    bool isActive();
    void ready();
    void busy();
    void terminate();
    void chatMessage(string msg, TuiChatMessageType type);
    void chatThinkMessage(string msg, string thinking, TuiChatMessageType type);
    void statusText(string status);
    void finalAnswer(string msg);
    void clearChat();
    void logFile(bool useFile);
    void setIniFile(string path);
    void streamStatusText(string status);
    void streamChatMessage(string msg, string thinking);
    void streamChatDone();
    void pipelineStreamChatMessage(string agentId, string content,
            string thinking, string role, string status);
    void pipelineStreamDone(string agentId);
    void pipelineClear();
    void sessionList(const(UiSessionItem)[] items);
    void initHistory(immutable(string)[] history);
}

/// Production sink: each call is one checked message on the TUI actor's
/// Channel (the app spawns the actor and hands its address in here).
class TuiChannelSink : TuiSink {
    private Channel!TUICommands ch;

    this(TypedAddress!TextUserInterfaceActor addr) {
        // Send-only channel: self is null (every TUICommands method is void).
        ch = Channel!TUICommands(addr, null);
    }

    override bool isActive() {
        return true;
    }

    override void ready() {
        ch.uiAgentReady();
    }

    override void busy() {
        ch.uiAgentBusy();
    }

    override void terminate() {
        ch.uiTerminate();
    }

    override void chatMessage(string msg, TuiChatMessageType type) {
        ch.uiMsg(UiChatMessage(msg, type));
    }

    override void chatThinkMessage(string msg, string thinking, TuiChatMessageType type) {
        ch.uiMsg(UiChatThinkMessage(msg, thinking, type));
    }

    override void statusText(string status) {
        ch.uiMsg(UiStatusText(status));
    }

    override void finalAnswer(string msg) {
        ch.uiMsg(UiFinalAnswer(msg));
    }

    override void clearChat() {
        ch.uiClearChat();
    }

    override void logFile(bool useFile) {
        ch.uiMsg(UiLogFile(useFile));
    }

    override void setIniFile(string path) {
        ch.uiMsg(UiSetIniFile(Path(path)));
    }

    override void streamStatusText(string status) {
        ch.uiMsg(UiStatusText(status));
    }

    override void streamChatMessage(string msg, string thinking) {
        ch.uiMsg(UiStreamChatMessage(msg, thinking));
    }

    override void streamChatDone() {
        ch.uiStreamChatDone();
    }

    override void pipelineStreamChatMessage(string agentId, string content,
            string thinking, string role, string status) {
        ch.uiMsg(UiPipelineStreamChatMessage(agentId, content, thinking, role, status));
    }

    override void pipelineStreamDone(string agentId) {
        ch.uiMsg(UiPipelineStreamDone(agentId));
    }

    override void pipelineClear() {
        ch.uiPipelineClear();
    }

    override void sessionList(const(UiSessionItem)[] items) {
        ch.uiMsg(UiSessionList(items));
    }

    override void initHistory(immutable(string)[] history) {
        ch.uiMsg(UiInitHistory(history));
    }
}

/// Blocked (one-shot) sink: exact legacy writeln semantics. Streaming and
/// status messages are dropped (one-shot mode only emits final output).
class TuiBlockedSink : TuiSink {
    override bool isActive() {
        return false;
    }

    override void ready() {
    }

    override void busy() {
    }

    override void terminate() {
    }

    override void chatMessage(string msg, TuiChatMessageType type) {
        writeln(msg);
    }

    override void chatThinkMessage(string msg, string thinking, TuiChatMessageType type) {
        if (!thinking.empty)
            writeln("Thinking: ", thinking);
        writeln(msg);
    }

    override void statusText(string status) {
    }

    override void finalAnswer(string msg) {
        writeln(msg);
    }

    override void clearChat() {
    }

    override void logFile(bool useFile) {
    }

    override void setIniFile(string path) {
    }

    override void streamStatusText(string status) {
    }

    override void streamChatMessage(string msg, string thinking) {
    }

    override void streamChatDone() {
    }

    override void pipelineStreamChatMessage(string agentId, string content,
            string thinking, string role, string status) {
    }

    override void pipelineStreamDone(string agentId) {
    }

    override void pipelineClear() {
    }

    override void sessionList(const(UiSessionItem)[] items) {
    }

    override void initHistory(immutable(string)[] history) {
    }
}

/// Test sink: records every call so tests can assert what the agent sent to
/// the TUI without a live UI thread.
class TuiRecordingSink : TuiSink {
    struct Call {
        string kind; // method name, e.g. "chatMessage"
        string payload; // best-effort summary of the arguments
    }

    Call[] calls;

    /// chatMessage payloads in call order (most recent last).
    string[] chatMessages;
    /// sessionList snapshots in call order; items are copied at record time
    /// so later store mutations cannot leak into the recording.
    UiSessionItem[][] sessionLists;

    /// Number of recorded calls of `kind` (0 if none).
    int countOf(string kind) const {
        int n;
        foreach (c; calls)
            if (c.kind == kind)
                n++;
        return n;
    }

    /// Most recently recorded chatMessage payload ("" if none).
    string lastChatMessage() {
        return chatMessages.length == 0 ? "" : chatMessages[chatMessages.length - 1];
    }

    /// Most recently recorded sessionList snapshot (empty if none).
    UiSessionItem[] lastSessionList() {
        return sessionLists.length == 0 ? null : sessionLists[sessionLists.length - 1];
    }

    private void record(string kind, string payload = "") {
        calls ~= Call(kind, payload);
    }

    override bool isActive() {
        return true;
    }

    override void ready() {
        record("ready");
    }

    override void busy() {
        record("busy");
    }

    override void terminate() {
        record("terminate");
    }

    override void chatMessage(string msg, TuiChatMessageType type) {
        chatMessages ~= msg;
        record("chatMessage", msg);
    }

    override void chatThinkMessage(string msg, string thinking, TuiChatMessageType type) {
        record("chatThinkMessage", msg ~ "\n[thinking] " ~ thinking);
    }

    override void statusText(string status) {
        record("statusText", status);
    }

    override void finalAnswer(string msg) {
        record("finalAnswer", msg);
    }

    override void clearChat() {
        record("clearChat");
    }

    override void logFile(bool useFile) {
        record("logFile", useFile ? "on" : "off");
    }

    override void setIniFile(string path) {
        record("setIniFile", path);
    }

    override void streamStatusText(string status) {
        record("streamStatusText", status);
    }

    override void streamChatMessage(string msg, string thinking) {
        record("streamChatMessage", msg);
    }

    override void streamChatDone() {
        record("streamChatDone");
    }

    override void pipelineStreamChatMessage(string agentId, string content,
            string thinking, string role, string status) {
        record("pipelineStreamChatMessage", agentId ~ ": " ~ content);
    }

    override void pipelineStreamDone(string agentId) {
        record("pipelineStreamDone", agentId);
    }

    override void pipelineClear() {
        record("pipelineClear");
    }

    override void sessionList(const(UiSessionItem)[] items) {
        auto copy = new UiSessionItem[items.length];
        foreach (i, ref const it; items)
            copy[i] = it;
        sessionLists ~= copy;
        record("sessionList", "n=" ~ items.length.text);
    }

    override void initHistory(immutable(string)[] history) {
        record("initHistory", "n=" ~ history.length.text);
    }
}

/// Message bridge between the agent and the TUI. A thin delegate over a
/// TuiSink: the public API is unchanged, but the destination of the traffic is
/// now chosen by the injected sink (channel for the actor, console for
/// one-shot, recording for tests).
class UiMessenger {
    TuiSink sink;

    /// Production/test: wrap an existing sink (e.g. TuiChannelSink,
    /// TuiBlockedSink, TuiRecordingSink).
    this(TuiSink s) {
        sink = s;
    }

    bool isActive() {
        return sink.isActive();
    }

    // No-op: a sink's mode is fixed at construction; kept for API parity
    // with the legacy UiMessenger.
    void setActive(bool onOff) {
    }

    void ready() {
        sink.ready();
    }

    void busy() {
        sink.busy();
    }

    void terminate() {
        sink.terminate();
    }

    void chatMessage(string msg, TuiChatMessageType type) {
        sink.chatMessage(msg, type);
    }

    void chatThinkMessage(string msg, string thinking, TuiChatMessageType type) {
        sink.chatThinkMessage(msg, thinking, type);
    }

    void statusText(string status) {
        sink.statusText(status);
    }

    void finalAnswer(string msg) {
        sink.finalAnswer(msg);
    }

    void clearChat() {
        sink.clearChat();
    }

    void logFile(bool useFile) {
        sink.logFile(useFile);
    }

    void setIniFile(string path) {
        sink.setIniFile(path);
    }

    void streamStatusText(string status) {
        sink.streamStatusText(status);
    }

    void streamChatMessage(string msg, string thinking) {
        sink.streamChatMessage(msg, thinking);
    }

    void streamChatDone() {
        sink.streamChatDone();
    }

    void pipelineStreamChatMessage(string agentId, string content,
            string thinking, string role, string status) {
        sink.pipelineStreamChatMessage(agentId, content, thinking, role, status);
    }

    void pipelineStreamDone(string agentId) {
        sink.pipelineStreamDone(agentId);
    }

    void pipelineClear() {
        sink.pipelineClear();
    }

    void sessionList(const(UiSessionItem)[] items) {
        sink.sessionList(items);
    }

    void initHistory(immutable(string)[] history) {
        sink.initHistory(history);
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
