/// UI plumbing for the agent subcommand: UiMessenger (TUI message bridge),
/// status-text formatting, and the stream updaters that forward streaming
/// model output to the TUI.
module llm.app_agent.ui;

import std.array : empty;
import std.conv : text;
import std.stdio : writeln;

import llm.types : ServerStat, StreamMessage, StreamToolCall, IStreamCallback;
import llm.tui; // Ui* message types (UiChatMessage, UiPipelineClear, ...)
import llmfun_tui; // TuiChatMessageType (C binding)
import my.path : Path;
import my.actor.channel : Channel;
import my.actor.mailbox : TypedAddress;
import llm.chat : MessageTime, NoProcessingTime;

/// One-way sink for agent->TUI messages. Abstracts where TUI traffic goes so
/// the agent can target a live TUI actor (channel), the console (one-shot),
/// or a recording buffer (tests) without the call sites changing.
interface TuiSink {
    bool isActive();
    void ready();
    void busy();
    void terminate();
    void chatMessage(string msg, TuiChatMessageType type, size_t nr, MessageTime time);
    void chatThinkMessage(string msg, string thinking, TuiChatMessageType type,
            size_t nr, MessageTime time);
    void runtimeStatus(UiRuntimeStatus status);
    void finalAnswer(string msg, size_t nr, MessageTime time);
    void clearChat();
    void logFile(bool useFile);
    void streamRuntimeStatus(UiRuntimeStatus status);
    void streamChatMessage(string msg, string thinking);
    void streamChatDone();
    void pipelineStreamChatMessage(string agentId, string content, string thinking, string role,
            double tokensPerSecond, string currentTool, long contextUsed, long contextMax);
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

    override void chatMessage(string msg, TuiChatMessageType type, size_t nr, MessageTime time) {
        ch.uiMsg(UiChatMessage(msg: msg, type: type, nr: nr, time: time));
    }

    override void chatThinkMessage(string msg, string thinking,
            TuiChatMessageType type, size_t nr, MessageTime time) {
        ch.uiMsg(UiChatThinkMessage(msg: msg, thinking: thinking, type: type, nr: nr, time: time));
    }

    override void runtimeStatus(UiRuntimeStatus status) {
        ch.uiMsg(status);
    }

    override void finalAnswer(string msg, size_t nr, MessageTime time) {
        ch.uiMsg(UiFinalAnswer(msg: msg, nr: nr, time: time));
    }

    override void clearChat() {
        ch.uiClearChat();
    }

    override void logFile(bool useFile) {
        ch.uiMsg(UiLogFile(useFile));
    }

    override void streamRuntimeStatus(UiRuntimeStatus status) {
        ch.uiMsg(status);
    }

    override void streamChatMessage(string msg, string thinking) {
        ch.uiMsg(UiStreamChatMessage(msg, thinking));
    }

    override void streamChatDone() {
        ch.uiStreamChatDone();
    }

    override void pipelineStreamChatMessage(string agentId, string content, string thinking, string role,
            double tokensPerSecond, string currentTool, long contextUsed, long contextMax) {
        ch.uiMsg(UiPipelineStreamChatMessage(agentId, content, thinking, role,
                tokensPerSecond, currentTool, contextUsed, contextMax));
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

/// Blocked (one-shot) sink. Streaming and status messages are dropped (one-shot mode only emits final output).
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

    override void chatMessage(string msg, TuiChatMessageType type, size_t nr, MessageTime time) {
        writeln(msg);
    }

    override void chatThinkMessage(string msg, string thinking,
            TuiChatMessageType type, size_t nr, MessageTime time) {
        if (!thinking.empty)
            writeln("Thinking: ", thinking);
        writeln(msg);
    }

    override void runtimeStatus(UiRuntimeStatus status) {
    }

    override void finalAnswer(string msg, size_t nr, MessageTime time) {
        writeln(msg);
    }

    override void clearChat() {
    }

    override void logFile(bool useFile) {
    }

    override void streamRuntimeStatus(UiRuntimeStatus status) {
    }

    override void streamChatMessage(string msg, string thinking) {
    }

    override void streamChatDone() {
    }

    override void pipelineStreamChatMessage(string agentId, string content, string thinking, string role,
            double tokensPerSecond, string currentTool, long contextUsed, long contextMax) {
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
        string kind;
        string payload; // best-effort summary of the arguments
    }

    Call[] calls;

    struct Stamp {
        size_t nr;
        MessageTime time;
    }

    /// chatMessage payloads in call order (most recent last).
    string[] chatMessages;
    /// (nr, time) for every persistent send, in call order; includes chatThinkMessage/finalAnswer (chatMessages does not).
    Stamp[] stamps;
    /// sessionList snapshots in call order; items are copied at record time so later store mutations cannot leak into the recording.
    UiSessionItem[][] sessionLists;

    /// pipelineStreamChatMessage calls in call order. The arguments are copied field by field
    /// rather than summarised into `Call.payload`: a test asserts the numbers and the tool name
    /// the lane row shows, so it must not depend on a formatted string.
    struct LaneUpdate {
        string agentId;
        string content;
        string role;
        double tokensPerSecond;
        string currentTool;
        long contextUsed;
        long contextMax;
    }

    LaneUpdate[] laneUpdates;

    int countOf(string kind) const {
        int n;
        foreach (c; calls)
            if (c.kind == kind)
                n++;
        return n;
    }

    string lastChatMessage() {
        return chatMessages.length == 0 ? "" : chatMessages[chatMessages.length - 1];
    }

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

    override void chatMessage(string msg, TuiChatMessageType type, size_t nr, MessageTime time) {
        chatMessages ~= msg;
        stamps ~= Stamp(nr, time);
        record("chatMessage", msg);
    }

    override void chatThinkMessage(string msg, string thinking,
            TuiChatMessageType type, size_t nr, MessageTime time) {
        stamps ~= Stamp(nr, time);
        record("chatThinkMessage", msg ~ "\n[thinking] " ~ thinking);
    }

    override void runtimeStatus(UiRuntimeStatus status) {
        record("runtimeStatus", status.detail);
    }

    override void finalAnswer(string msg, size_t nr, MessageTime time) {
        stamps ~= Stamp(nr, time);
        record("finalAnswer", msg);
    }

    override void clearChat() {
        record("clearChat");
    }

    override void logFile(bool useFile) {
        record("logFile", useFile ? "on" : "off");
    }

    override void streamRuntimeStatus(UiRuntimeStatus status) {
        record("streamRuntimeStatus", status.detail);
    }

    override void streamChatMessage(string msg, string thinking) {
        record("streamChatMessage", msg);
    }

    override void streamChatDone() {
        record("streamChatDone");
    }

    override void pipelineStreamChatMessage(string agentId, string content, string thinking, string role,
            double tokensPerSecond, string currentTool, long contextUsed, long contextMax) {
        laneUpdates ~= LaneUpdate(agentId, content, role, tokensPerSecond,
                currentTool, contextUsed, contextMax);
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

/// Message bridge between the agent and the TUI: a thin wrapper over a
/// TuiSink. The destination of the traffic is chosen by the injected sink
/// (channel for the actor, console for one-shot, recording for tests).
class UiMessenger {
    TuiSink sink;

    this(TuiSink s) {
        sink = s;
    }

    bool isActive() {
        return sink.isActive();
    }

    // No-op: a sink's mode is fixed at construction; kept for call-site compatibility.
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
        sink.chatMessage(msg, type, 0, MessageTime(NoProcessingTime()));
    }

    void chatMessage(string msg, TuiChatMessageType type, size_t nr, MessageTime time) {
        sink.chatMessage(msg, type, nr, time);
    }

    void chatThinkMessage(string msg, string thinking, TuiChatMessageType type) {
        sink.chatThinkMessage(msg, thinking, type, 0, MessageTime(NoProcessingTime()));
    }

    void chatThinkMessage(string msg, string thinking, TuiChatMessageType type,
            size_t nr, MessageTime time) {
        sink.chatThinkMessage(msg, thinking, type, nr, time);
    }

    void runtimeStatus(UiRuntimeStatus status) {
        sink.runtimeStatus(status);
    }

    void finalAnswer(string msg) {
        sink.finalAnswer(msg, 0, MessageTime(NoProcessingTime()));
    }

    void finalAnswer(string msg, size_t nr, MessageTime time) {
        sink.finalAnswer(msg, nr, time);
    }

    void clearChat() {
        sink.clearChat();
    }

    void logFile(bool useFile) {
        sink.logFile(useFile);
    }

    void streamRuntimeStatus(UiRuntimeStatus status) {
        sink.streamRuntimeStatus(status);
    }

    void streamChatMessage(string msg, string thinking) {
        sink.streamChatMessage(msg, thinking);
    }

    void streamChatDone() {
        sink.streamChatDone();
    }

    void pipelineStreamChatMessage(string agentId, string content, string thinking, string role,
            double tokensPerSecond, string currentTool, long contextUsed, long contextMax) {
        sink.pipelineStreamChatMessage(agentId, content, thinking, role,
                tokensPerSecond, currentTool, contextUsed, contextMax);
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
        uiMsg.streamRuntimeStatus(UiRuntimeStatus(phase: !tools.empty ? TuiPhase.tooling
                : msg.content.empty ? TuiPhase.thinking : TuiPhase.writing, detail: tools.empty
                ? null : tools[$ - 1].toolName,
                contextUsed: stat.context, contextMax: contextSize, compactAt: (contextSize * 9) / 10,
                tokensPerSecond: stat.predictedPerSecond, model: modelName));

        string content = msg.content;
        if (!tools.empty) {
            foreach (tool; tools) {
                content ~= "\n--- Tool ---\n";
                content ~= tool.toPrettyString(1000);
                content ~= "\n\n";
            }
        }

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

        uiMsg.pipelineStreamChatMessage(agentId: agentId, content: content, thinking: msg.reasoning, role: msg.role,
                tokensPerSecond: stat.predictedPerSecond,
                currentTool: tools.empty ? null : tools[$ - 1].toolName,
                contextUsed: stat.context, contextMax: contextSize);
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
