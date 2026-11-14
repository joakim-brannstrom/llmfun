/// UI plumbing for the agent subcommand: UiMessenger (TUI message bridge),
/// status-text formatting, and the stream updaters that forward streaming
/// model output to the TUI.
module llm.app_agent.ui;

import std.algorithm : filter;
import std.array : empty;
import std.conv : text;
import std.json : JSONType, JSONValue, parseJSON;
import std.stdio : writeln;
import std.sumtype : match;

import llm.types : ServerStat, StreamMessage, StreamToolCall, IStreamCallback,
    IStepEventEmitter, ProcessResult;
import llm.tui; // Ui* message types (UiChatMessage, UiPipelineClear, ...)
import llmfun_tui; // TuiChatMessageType (C binding)
import my.path : Path;
import my.actor.channel : Channel;
import my.actor.mailbox : TypedAddress;
import llm.chat : MessageTime, NoProcessingTime, Message, ToolMessage,
    ToolResponse, VisionMessage;
import llm.utility : summarizeToolCallArguments;

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
    void pipelineToolCall(string agentId, string name, string arguments, string id);
    void pipelineToolResult(string agentId, string name, int ok, string text);
    void sessionList(UiSessionItem[] items);
    void initHistory(string[] history);
    void setTurns(UiTurnData[] turns, UiBlockData[] blocks);
    void turnUpsert(UiTurnData turn);
    void blockUpsert(UiBlockData block);
    void turnRemove(long turnId);
    void threadClear();
    void dropChipReport(UiDropReport report);
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

    override void pipelineToolCall(string agentId, string name, string arguments, string id) {
        ch.uiMsg(UiPipelineToolCall(agentId, name, arguments, id));
    }

    override void pipelineToolResult(string agentId, string name, int ok, string text) {
        ch.uiMsg(UiPipelineToolResult(agentId, name, ok, text));
    }

    override void pipelineClear() {
        ch.uiPipelineClear();
    }

    override void sessionList(UiSessionItem[] items) {
        ch.uiMsg(UiSessionList(items));
    }

    override void initHistory(string[] history) {
        ch.uiMsg(UiInitHistory(history));
    }

    override void dropChipReport(UiDropReport report) {
        ch.uiMsg(report);
    }

    override void setTurns(UiTurnData[] turns, UiBlockData[] blocks) {
        ch.uiMsg(UiSetTurns(turns, blocks));
    }

    override void turnUpsert(UiTurnData turn) {
        ch.uiMsg(UiTurnUpsert(turn));
    }

    override void blockUpsert(UiBlockData block) {
        ch.uiMsg(UiBlockUpsert(block));
    }

    override void turnRemove(long turnId) {
        ch.uiMsg(UiTurnRemove(turnId));
    }

    override void threadClear() {
        ch.uiThreadClear();
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

    override void pipelineToolCall(string agentId, string name, string arguments, string id) {
    }

    override void pipelineToolResult(string agentId, string name, int ok, string text) {
    }

    override void pipelineClear() {
    }

    override void sessionList(UiSessionItem[] items) {
    }

    override void initHistory(string[] history) {
    }

    override void dropChipReport(UiDropReport report) {
    }

    private ConsoleFormatter formatter = new ConsoleFormatter;

    override void setTurns(UiTurnData[] turns, UiBlockData[] blocks) {
    }

    override void turnUpsert(UiTurnData turn) {
    }

    override void blockUpsert(UiBlockData block) {
    }

    override void turnRemove(long turnId) {
    }

    override void threadClear() {
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

    /// pipelineToolCall calls in call order: field-by-field copies, same
    /// rationale as LaneUpdate.
    struct ToolCallEvent {
        string agentId;
        string name;
        string arguments;
        string id;
    }

    ToolCallEvent[] toolCalls;

    /// pipelineToolResult calls in call order.
    struct ToolResultEvent {
        string agentId;
        string name;
        int ok;
        string text;
    }

    ToolResultEvent[] toolResults;

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

    override void pipelineToolCall(string agentId, string name, string arguments, string id) {
        toolCalls ~= ToolCallEvent(agentId, name, arguments, id);
        record("pipelineToolCall", agentId ~ ": " ~ name);
    }

    override void pipelineToolResult(string agentId, string name, int ok, string text) {
        toolResults ~= ToolResultEvent(agentId, name, ok, text);
        record("pipelineToolResult", agentId ~ ": " ~ name);
    }

    override void pipelineClear() {
        record("pipelineClear");
    }

    override void sessionList(UiSessionItem[] items) {
        auto copy = new UiSessionItem[items.length];
        foreach (i, ref const it; items)
            copy[i] = it;
        sessionLists ~= copy;
        record("sessionList", "n=" ~ items.length.text);
    }

    override void initHistory(string[] history) {
        record("initHistory", "n=" ~ history.length.text);
    }

    override void dropChipReport(UiDropReport report) {
        record("dropChipReport",
                report.chipId.text ~ " state " ~ report.state.text ~ " " ~ report.display);
    }

    struct TurnSnapshot {
        UiTurnData[] turns;
        UiBlockData[] blocks;
    }

    TurnSnapshot[] setTurnsSnapshots;
    /// turnUpsert payloads in call order (copied at record time).
    UiTurnData[] turnUpserts;
    /// blockUpsert payloads in call order (copied at record time).
    UiBlockData[] blockUpserts;
    /// turnRemove ids in call order.
    long[] turnRemovals;

    override void setTurns(UiTurnData[] turns, UiBlockData[] blocks) {
        setTurnsSnapshots ~= TurnSnapshot(turns.dup, blocks.dup);
        record("setTurns", "turns=" ~ turns.length.text ~ " blocks=" ~ blocks.length.text);
    }

    override void turnUpsert(UiTurnData turn) {
        turnUpserts ~= turn;
        record("turnUpsert", turn.id.text);
    }

    override void blockUpsert(UiBlockData block) {
        blockUpserts ~= block;
        record("blockUpsert", block.id);
    }

    override void turnRemove(long turnId) {
        turnRemovals ~= turnId;
        record("turnRemove", turnId.text);
    }

    override void threadClear() {
        record("threadClear");
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

    void pipelineToolCall(string agentId, string name, string arguments, string id) {
        sink.pipelineToolCall(agentId, name, arguments, id);
    }

    void pipelineToolResult(string agentId, string name, int ok, string text) {
        sink.pipelineToolResult(agentId, name, ok, text);
    }

    void pipelineClear() {
        sink.pipelineClear();
    }

    void sessionList(UiSessionItem[] items) {
        sink.sessionList(items);
    }

    void initHistory(string[] history) {
        sink.initHistory(history);
    }

    void dropChipReport(UiDropReport report) {
        sink.dropChipReport(report);
    }

    void setTurns(UiTurnData[] turns, UiBlockData[] blocks) {
        sink.setTurns(turns, blocks);
    }

    void turnUpsert(UiTurnData turn) {
        sink.turnUpsert(turn);
    }

    void blockUpsert(UiBlockData block) {
        sink.blockUpsert(block);
    }

    void turnRemove(long turnId) {
        sink.turnRemove(turnId);
    }

    void threadClear() {
        sink.threadClear();
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

/// Cut a tool payload to the wire cap: D is the producer, so the C side
/// never sees an unbounded string. A cut ends with `...`, and backs off to a
/// code-point boundary first: a byte-level cut can split a UTF-8 sequence, and
/// the garbage glyph ImGui then renders at the cut is worse than a byte or two
/// less of payload.
private string truncatedForWire(string payload) {
    if (payload.length <= 1000)
        return payload;
    size_t end = 1000;
    while (end > 0 && (payload[end] & 0xC0) == 0x80)
        --end;
    return payload[0 .. end] ~ "...";
}

// TODO: use getValue instead
/// Read `obj[key]` if it is a JSON string, null otherwise
package string jsonStringAt(const JSONValue obj, string key) {
    if (obj.type != JSONType.object)
        return null;
    auto v = key in obj;
    if (v is null || v.type != JSONType.string)
        return null;
    return v.str;
}

/// Send every tool call in a ToolMessage's `toolCalls` JSON as its own structured
/// message. Entries without a usable `function.name` are skipped.
private void emitToolCallsFromJson(UiMessenger uiMsg, string agentId, const JSONValue toolCalls) {
    if (toolCalls.type != JSONType.array)
        return;
    foreach (entry; toolCalls.array) {
        if (entry.type != JSONType.object)
            continue;
        auto fn = "function" in entry;
        if (fn is null || fn.type != JSONType.object)
            continue;
        auto name = jsonStringAt(*fn, "name");
        if (name.empty)
            continue;
        uiMsg.pipelineToolCall(agentId, name, truncatedForWire(jsonStringAt(*fn,
                "arguments")), jsonStringAt(entry, "id"));
    }
}

/// The per-step emitter: walk one completed step's chat in order and send
/// each tool call and tool result as its own structured message, beside the streamed
/// content instead of spliced into it. Tool messages and tool responses carry the
/// traffic; finish reasons are never consulted. This is the pipeline's UI seam: the
/// pipeline knows only IStepEventEmitter, and this function supplies the UI knowledge.
void emitStepToolEvents(UiMessenger uiMsg, string agentId, ProcessResult result) {
    foreach (ref msg; result.chat) {
        msg.match!((ToolMessage tool) {
            emitToolCallsFromJson(uiMsg, agentId, tool.toolCalls);
        }, (ToolResponse resp) {
            uiMsg.pipelineToolResult(agentId, resp.toolName, resp.success()
                ? 1 : 0, truncatedForWire(resp.content));
        }, (Message _) {}, (VisionMessage _) {});
    }
}

/// Forwards streaming pipeline output to the TUI (per-agent messages).
class PipelineStreamMessageUpdater : IStreamCallback, IStepEventEmitter {
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

        uiMsg.pipelineStreamChatMessage(agentId: agentId, content: content, thinking: msg.reasoning, role: msg.role,
                tokensPerSecond: stat.predictedPerSecond,
                currentTool: tools.empty ? null : tools[$ - 1].toolName,
                contextUsed: stat.context, contextMax: contextSize);
    }

    override void streamMessageDone() {
        uiMsg.pipelineStreamDone(agentId);
    }

    override void stepEvent(ProcessResult result) {
        emitStepToolEvents(uiMsg, agentId, result);
    }

    override void setId(string id) {
        agentId = id;
    }

    override IStreamCallback clone() {
        return new PipelineStreamMessageUpdater(uiMsg, contextSize, modelName);
    }
}

// TODO: this is really bad, hidden dependency between the tools and rendering.
// Refactor so this is not needed. If the tool name change, more hidden tools
// are added etc then this needs to be updated.
package immutable string[] kHiddenToolNames = ["pipeline_output"];

package bool blockDisplayable(UiBlockData b) {
    if (b.kind != TuiBlockKind_Tool)
        return true;
    // Pending/Running exist only on the live path. The console prints a
    // call's line only once its result arrived (Ok/Failed) or the turn
    // ended (Cancelled), so an un-merged live call renders nothing yet.
    if (b.status == TuiBlockStatus_Pending || b.status == TuiBlockStatus_Running)
        return false;
    foreach (hidden; kHiddenToolNames)
        if (b.title == hidden)
            return false;
    return true;
}

// Tool blocks are collapsible, open by default, selectable.
enum int kToolBlockFlags = TuiBlockFlag_Collapsible | TuiBlockFlag_DefaultOpen
    | TuiBlockFlag_Selectable;

// Stable block ids / batch ids. Batch ids are "<turnId>#<batchIndex>" (0-based
// per-response index within the turn, recomputed deterministically on replay);
// query/vision/notice blocks are not response batches and use "<turnId>#q".
package string responseBatchId(long turnId, int batchIndex) {
    return text(turnId) ~ "#" ~ text(batchIndex);
}

package string queryBatchId(long turnId) {
    return text(turnId) ~ "#q";
}

package string contentBlockId(int batchIndex) {
    return "content#" ~ text(batchIndex);
}

package string reasoningBlockId(int batchIndex) {
    return "reasoning#" ~ text(batchIndex);
}

// Build a wire-shaped block from D values. The body is cut to the wire cap
// (truncatedForWire): D is the producer, the C side never sees an unbounded
// string. The id/batchId/title/meta are exact - truncating the id would break
// the store's upsert-merge; meta arrives pre-summarized.
package UiBlockData makeBlockData(long turnId, string id, string batchId, int kind,
        int status, int flags, string title, string meta, string body, long durationMs) {
    return UiBlockData(turnId, id, batchId, title, meta,
            truncatedForWire(body), durationMs, 0, 0, kind, status, flags);
}

// The call's summarized "name(args)" line for a streaming call whose arguments
// may still be partial JSON (the same shape summarizeToolCalls produces per
// call, so the Pending block and the step walk's merged block agree).
package string summarizedCallBody(string name, string arguments) {
    string argsSummary;
    try {
        argsSummary = summarizeToolCallArguments(parseJSON(arguments), 1000);
    } catch (Exception e) {
        // Partial/streaming arguments: summarize the raw text instead.
        argsSummary = truncatedForWire(arguments);
    }
    return argsSummary.empty ? name ~ "()" : name ~ "(" ~ argsSummary ~ ")";
}

/// Shared per-turn state for the typed live emissions (task 3): the agent's
/// emitters (llm.app_agent) create one per live turn and hand it to the typed
/// stream updater, which mutates it per tick. One instance per live turn.
package final class TypedEmitContext {
    /// The turn the emissions attach to (chat's currentTurnId at turn start).
    long turnId;
    /// The turn's display position (D-assigned, D5): one slot per live turn.
    int ordinal;
    /// 0-based per-response index within the turn (the batch counter).
    int batchIndex;
    /// Vision block ids: unique across the live turn (two vision entries in
    /// one turn must not upsert-merge into one block).
    int visionSeq;
    /// Informational count of the LLM messages the turn covers so far (the
    /// C-side stepCount over the blocks is the rendered number).
    int messageCount;
    /// The role the streamed content blocks carry.
    string role = "assistant";

    /// One in-flight tool call of the live batch: what the Pending upsert
    /// carried, kept so stream-done can flip the whole batch to Running and a
    /// stop can flip it to Cancelled without rebuilding the blocks.
    struct LiveCall {
        long turnId;
        string id; // the call id (the tool block's id)
        string batchId;
        string title; // tool name
        string body; // the call's summarized name-args line
    }

    LiveCall[] liveCalls;

    string batchId() const {
        return text(turnId) ~ "#" ~ text(batchIndex);
    }

    void recordLiveCall(string callId, string title, string body) {
        foreach (ref c; liveCalls)
            if (c.id == callId)
                return; // re-upsert of a known call: the record stays as-is
        liveCalls ~= LiveCall(turnId, callId, batchId, title, body);
    }

    void clearLiveCall(string callId) {
        foreach (i, ref c; liveCalls)
            if (c.id == callId) {
                liveCalls = liveCalls[0 .. i] ~ liveCalls[i + 1 .. $];
                return;
            }
    }
}

/// Typed sibling of StreamMessageUpdater (task 3): upserts the current batch's
/// content/reasoning blocks and the known tool calls as Pending per tick - no
/// `--- Tool ---` splicing into the content, that dies at the cutover - and
/// flips the batch's Pending calls to Running on stream-done (the statuses ride
/// existing events; design appendix "States"). The v4 senders own the run path
/// until task 5; until then this class is exercised by the recording-sink tests.
package final class TypedStreamMessageUpdater : IStreamCallback {
    UiMessenger uiMsg;
    long contextSize;
    string modelName;
    TypedEmitContext ctx;

    this(UiMessenger messenger, long contextSize, string modelName, TypedEmitContext ctx)
    in (messenger !is null, "UiMessenger must not be null")
    in (ctx !is null, "TypedEmitContext must not be null") {
        this.uiMsg = messenger;
        this.contextSize = contextSize;
        this.modelName = modelName;
        this.ctx = ctx;
    }

    override void messageUpdate(StreamMessage msg, StreamToolCall[] tools, ServerStat stat) {
        uiMsg.streamRuntimeStatus(UiRuntimeStatus(phase: !tools.empty ? TuiPhase.tooling
                : msg.content.empty ? TuiPhase.thinking : TuiPhase.writing, detail: tools.empty
                ? null : tools[$ - 1].toolName,
                contextUsed: stat.context, contextMax: contextSize, compactAt: (contextSize * 9) / 10,
                tokensPerSecond: stat.predictedPerSecond, model: modelName));

        // The batch's content-so-far block: same id/batchId every tick, so the
        // store merges in place and the last tick before the step walk wins.
        uiMsg.blockUpsert(makeBlockData(ctx.turnId, contentBlockId(ctx.batchIndex),
                ctx.batchId, TuiBlockKind_Prose, TuiBlockStatus_None, 0, null,
                ctx.role, msg.content, 0));
        if (!msg.reasoning.empty)
            uiMsg.blockUpsert(makeBlockData(ctx.turnId, reasoningBlockId(ctx.batchIndex), ctx.batchId,
                    TuiBlockKind_Reasoning, TuiBlockStatus_None,
                    TuiBlockFlag_Collapsible, null, null, msg.reasoning, 0));

        foreach (tool; tools.filter!(a => !a.id.empty)) {
            const body_ = summarizedCallBody(tool.toolName, tool.arguments);
            uiMsg.blockUpsert(makeBlockData(ctx.turnId, tool.id, ctx.batchId, TuiBlockKind_Tool,
                    TuiBlockStatus_Pending, kToolBlockFlags, tool.toolName, null, body_, 0));
            ctx.recordLiveCall(tool.id, tool.toolName, body_);
        }
    }

    override void streamMessageDone() {
        // The batch's Pending -> Running flip rides the stream-done: re-upsert every call the ticks upserted, status Running.
        foreach (call; ctx.liveCalls) {
            uiMsg.blockUpsert(makeBlockData(call.turnId, call.id, call.batchId, TuiBlockKind_Tool,
                    TuiBlockStatus_Running, kToolBlockFlags, call.title, null, call.body, 0));
        }
    }

    override void setId(string id) {
    }

    override IStreamCallback clone() {
        return new TypedStreamMessageUpdater(uiMsg, contextSize, modelName, ctx);
    }
}

/// The one-shot console rebuilt over typed events (task 4): a small accumulator
/// with one method per typed event, each returning the lines that event
/// produces. Pure - no stdout, no clock - so tests can compare its output to a
/// golden. The parity test drives it directly; the TuiBlockedSink's typed
/// overrides stay no-ops until task 5 wires them.
///
/// Rendering rules (byte-parity with today's v4 console output, task 4):
/// query block -> raw body on a live turn (the v4 echo has no prefix),
/// "user: " + body on replay; reasoning -> held ("Thinking: " + body) until
/// the batch's content line or the batch end; content -> "role: body";
/// tool blocks -> buffered per batch, flushed as one "tool call: " line
/// (visible calls joined by "\n") plus one "tool result <mark>: <name> <meta>"
/// line per Ok/Failed call; answer -> flush the batch's thinking/call lines,
/// then the body, then the result lines (today's console prints the answer
/// inside the tool-call message, before the results walk); notice -> the
/// body verbatim; hidden blocks (blockDisplayable) produce no lines. A new
/// batch flushes the previous one; a turn end (Done/Interrupted upsert)
/// flushes whatever is pending.
package final class ConsoleFormatter {
    private string pendingThinking;
    private string pendingContent;
    private string pendingContentMeta;
    private string currentBatchKey;
    private UiBlockData[] pendingTools; // insertion order, upsert-merged by id
    private bool[long] seenTurns;
    private bool echoArmed;
    private bool replayMode;

    string[] onSetTurns(UiTurnData[] turns, UiBlockData[] blocks) {
        resetPending();
        seenTurns = null;
        replayMode = true;
        string[] lines;
        foreach (b; blocks)
            lines ~= handleBlock(b);
        replayMode = false;
        return lines;
    }

    string[] onTurnUpsert(UiTurnData t) {
        string[] lines;
        if (t.state == TuiTurnState_Running && (t.id in seenTurns) is null) {
            seenTurns[t.id] = true;
            echoArmed = true; // the next query block echoes the raw text
            return lines;
        }
        // Done / Interrupted / a re-upsert: the turn's displayable tail is
        // complete, so flush whatever batch is pending.
        return flushPending();
    }

    string[] onBlockUpsert(UiBlockData b) {
        return handleBlock(b);
    }

    string[] onTurnRemove(long turnId) {
        // Console output is append-only: a removal just drops the pending
        // batch state so nothing stale is flushed later.
        resetPending();
        return null;
    }

    string[] onThreadClear() {
        resetPending();
        seenTurns = null;
        return null;
    }

    private string[] handleBlock(UiBlockData b) {
        if (!blockDisplayable(b))
            return null;
        string[] lines;
        if (b.kind == TuiBlockKind_Notice) {
            lines ~= b.body;
            return lines;
        }
        if (b.kind == TuiBlockKind_Answer) {
            // Today's console prints, in order: the batch's thinking, the
            // tool-call line, the final answer, then the result lines (the
            // v4 walk sends the answer inside the tool-call message, before
            // the results walk). The answer block therefore flushes in two
            // halves around itself.
            lines ~= flushHead();
            lines ~= b.body;
            lines ~= flushResults();
            return lines;
        }
        if (b.kind == TuiBlockKind_Prose && b.meta == "user") {
            // The query: live echo is the raw text; replay prefixes the role.
            if (replayMode)
                lines ~= "user: " ~ b.body;
            else if (echoArmed) {
                lines ~= b.body;
                echoArmed = false;
            }
            return lines;
        }
        // Response-batch blocks: a new batch flushes the previous one.
        if (b.batchId != currentBatchKey) {
            lines ~= flushPending();
            currentBatchKey = b.batchId;
        }
        if (b.kind == TuiBlockKind_Reasoning)
            pendingThinking = "Thinking: " ~ b.body;
        else if (b.kind == TuiBlockKind_Prose) {
            pendingContent = b.body;
            pendingContentMeta = b.meta;
        } else
            upsertTool(b);
        return lines;
    }

    private string[] flushPending() {
        return flushHead() ~ flushResults();
    }

    /// The batch's thinking and content lines, then the tool-call line; does
    /// not touch the result lines (the answer block flushes them after
    /// itself, see handleBlock).
    private string[] flushHead() {
        import std.array : join;

        string[] lines;
        if (!pendingThinking.empty) {
            lines ~= pendingThinking;
            pendingThinking = null;
        }
        if (!pendingContent.empty)
            lines ~= pendingContentMeta ~ ": " ~ pendingContent;
        pendingContent = null;
        pendingContentMeta = null;
        bool anyVisible;
        foreach (t; pendingTools)
            if (blockDisplayable(t))
                anyVisible = true;
        if (anyVisible) {
            string[] bodies;
            foreach (t; pendingTools)
                if (blockDisplayable(t))
                    bodies ~= t.body;
            lines ~= "tool call: " ~ bodies.join("\n");
        }
        return lines;
    }

    /// The batch's tool-result lines (visible calls with Ok/Failed status),
    /// then the pending state is cleared.
    private string[] flushResults() {
        string[] lines;
        foreach (t; pendingTools)
            if (t.status == TuiBlockStatus_Ok || t.status == TuiBlockStatus_Failed)
                lines ~= "tool result " ~ (t.status == TuiBlockStatus_Ok
                        ? "✅" : "❌") ~ ": " ~ t.title ~ " " ~ t.meta;
        pendingTools = null;
        currentBatchKey = null;
        return lines;
    }

    private void upsertTool(UiBlockData b) {
        foreach (i, ref t; pendingTools)
            if (t.id == b.id) {
                pendingTools[i] = b;
                return;
            }
        pendingTools ~= b;
    }

    private void resetPending() {
        pendingThinking = null;
        pendingContent = null;
        pendingContentMeta = null;
        pendingTools = null;
        currentBatchKey = null;
        echoArmed = false;
    }
}
