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
    IStepEventEmitter, IRunDoneEmitter, IRunStartEmitter, ProcessResult;
import llm.tui;
import llmfun_tui;
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
    void runtimeStatus(UiRuntimeStatus status);
    void logFile(bool useFile);
    void streamRuntimeStatus(UiRuntimeStatus status);
    void pipelineStreamChatMessage(string agentId, string content, string thinking, string role,
            double tokensPerSecond, string currentTool, long contextUsed,
            long contextMax, long batch);
    void pipelineStreamDone(string agentId);
    void pipelineAgentStart(string agentId, string task);
    void pipelineClear();
    void pipelineToolCall(string agentId, string name, string arguments, string id, long batch);
    void pipelineToolResult(string agentId, string name, int ok, string text, long batch);
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

    override void runtimeStatus(UiRuntimeStatus status) {
        ch.uiMsg(status);
    }

    override void logFile(bool useFile) {
        ch.uiMsg(UiLogFile(useFile));
    }

    override void streamRuntimeStatus(UiRuntimeStatus status) {
        ch.uiMsg(status);
    }

    override void pipelineStreamChatMessage(string agentId, string content, string thinking, string role,
            double tokensPerSecond, string currentTool, long contextUsed,
            long contextMax, long batch) {
        ch.uiMsg(UiPipelineStreamChatMessage(agentId, content, thinking, role,
                tokensPerSecond, currentTool, contextUsed, contextMax, batch));
    }

    override void pipelineStreamDone(string agentId) {
        ch.uiMsg(UiPipelineStreamDone(agentId));
    }

    override void pipelineAgentStart(string agentId, string task) {
        ch.uiMsg(UiPipelineAgentStart(agentId, task));
    }

    override void pipelineToolCall(string agentId, string name,
            string arguments, string id, long batch) {
        ch.uiMsg(UiPipelineToolCall(agentId, name, arguments, id, batch));
    }

    override void pipelineToolResult(string agentId, string name, int ok, string text, long batch) {
        ch.uiMsg(UiPipelineToolResult(agentId, name, ok, text, batch));
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

    override void runtimeStatus(UiRuntimeStatus status) {
    }

    override void logFile(bool useFile) {
    }

    override void streamRuntimeStatus(UiRuntimeStatus status) {
    }

    override void pipelineStreamChatMessage(string agentId, string content, string thinking, string role,
            double tokensPerSecond, string currentTool, long contextUsed,
            long contextMax, long batch) {
    }

    override void pipelineStreamDone(string agentId) {
    }

    override void pipelineAgentStart(string agentId, string task) {
    }

    override void pipelineToolCall(string agentId, string name,
            string arguments, string id, long batch) {
    }

    override void pipelineToolResult(string agentId, string name, int ok, string text, long batch) {
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
        foreach (line; formatter.onSetTurns(turns, blocks))
            writeln(line);
    }

    override void turnUpsert(UiTurnData turn) {
        foreach (line; formatter.onTurnUpsert(turn))
            writeln(line);
    }

    override void blockUpsert(UiBlockData block) {
        foreach (line; formatter.onBlockUpsert(block))
            writeln(line);
    }

    override void turnRemove(long turnId) {
        formatter.onTurnRemove(turnId);
    }

    override void threadClear() {
        formatter.onThreadClear();
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
        long batch;
    }

    LaneUpdate[] laneUpdates;

    /// pipelineToolCall calls in call order: field-by-field copies, same
    /// rationale as LaneUpdate.
    struct ToolCallEvent {
        string agentId;
        string name;
        string arguments;
        string id;
        long batch;
    }

    ToolCallEvent[] toolCalls;

    /// pipelineToolResult calls in call order.
    struct ToolResultEvent {
        string agentId;
        string name;
        int ok;
        string text;
        long batch;
    }

    ToolResultEvent[] toolResults;

    /// pipelineAgentStart calls in call order
    struct AgentStartEvent {
        string agentId;
        string task;
    }

    AgentStartEvent[] agentStarts;

    int countOf(string kind) const {
        int n;
        foreach (c; calls)
            if (c.kind == kind)
                n++;
        return n;
    }

    /// The last Notice block body or "" when no notice was sent.
    string lastNoticeBody() {
        foreach_reverse (b; blockUpserts)
            if (b.kind == TuiBlockKind_Notice)
                return b.body;
        return null;
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

    override void runtimeStatus(UiRuntimeStatus status) {
        record("runtimeStatus", status.detail);
    }

    override void logFile(bool useFile) {
        record("logFile", useFile ? "on" : "off");
    }

    override void streamRuntimeStatus(UiRuntimeStatus status) {
        record("streamRuntimeStatus", status.detail);
    }

    override void pipelineStreamChatMessage(string agentId, string content, string thinking, string role,
            double tokensPerSecond, string currentTool, long contextUsed,
            long contextMax, long batch) {
        laneUpdates ~= LaneUpdate(agentId, content, role, tokensPerSecond,
                currentTool, contextUsed, contextMax, batch);
        record("pipelineStreamChatMessage", agentId ~ ": " ~ content);
    }

    override void pipelineStreamDone(string agentId) {
        record("pipelineStreamDone", agentId);
    }

    override void pipelineAgentStart(string agentId, string task) {
        agentStarts ~= AgentStartEvent(agentId, task);
        record("pipelineAgentStart", agentId ~ ": " ~ task);
    }

    override void pipelineToolCall(string agentId, string name,
            string arguments, string id, long batch) {
        toolCalls ~= ToolCallEvent(agentId, name, arguments, id, batch);
        record("pipelineToolCall", agentId ~ ": " ~ name);
    }

    override void pipelineToolResult(string agentId, string name, int ok, string text, long batch) {
        toolResults ~= ToolResultEvent(agentId, name, ok, text, batch);
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

    void runtimeStatus(UiRuntimeStatus status) {
        sink.runtimeStatus(status);
    }

    void logFile(bool useFile) {
        sink.logFile(useFile);
    }

    void streamRuntimeStatus(UiRuntimeStatus status) {
        sink.streamRuntimeStatus(status);
    }

    void pipelineStreamChatMessage(string agentId, string content, string thinking, string role,
            double tokensPerSecond, string currentTool, long contextUsed,
            long contextMax, long batch) {
        sink.pipelineStreamChatMessage(agentId, content, thinking, role,
                tokensPerSecond, currentTool, contextUsed, contextMax, batch);
    }

    void pipelineStreamDone(string agentId) {
        sink.pipelineStreamDone(agentId);
    }

    void pipelineAgentStart(string agentId, string task) {
        sink.pipelineAgentStart(agentId, task);
    }

    void pipelineToolCall(string agentId, string name, string arguments, string id, long batch) {
        sink.pipelineToolCall(agentId, name, arguments, id, batch);
    }

    void pipelineToolResult(string agentId, string name, int ok, string text, long batch) {
        sink.pipelineToolResult(agentId, name, ok, text, batch);
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
private void emitToolCallsFromJson(UiMessenger uiMsg, string agentId,
        const JSONValue toolCalls, long batch) {
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
                "arguments")), jsonStringAt(entry, "id"), batch);
    }
}

/// The per-step emitter: walk one completed step's chat in order and send
/// each tool call and tool result as its own structured message, beside the streamed
/// content instead of spliced into it. Tool messages and tool responses carry the
/// traffic; finish reasons are never consulted. `batch` is the index of the
/// response the step belongs to: the step always follows that response's
/// stream-done, so the emitter's traffic groups under the batch whose stream
/// the drill-down just saw. This is the pipeline's UI seam: the pipeline knows
/// only IStepEventEmitter, and this function supplies the UI knowledge.
void emitStepToolEvents(UiMessenger uiMsg, string agentId, ProcessResult result, long batch) {
    foreach (ref msg; result.chat) {
        msg.match!((ToolMessage tool) {
            emitToolCallsFromJson(uiMsg, agentId, tool.toolCalls, batch);
        }, (ToolResponse resp) {
            uiMsg.pipelineToolResult(agentId, resp.toolName, resp.success()
                ? 1 : 0, truncatedForWire(resp.content), batch);
        }, (Message _) {}, (VisionMessage _) {});
    }
}

/// Forwards streaming pipeline output to the TUI (per-agent messages).
class PipelineStreamMessageUpdater : IStreamCallback, IStepEventEmitter,
    IRunStartEmitter, IRunDoneEmitter {
        UiMessenger uiMsg;
        long contextSize;
        string modelName;
        string agentId;

        /// 0-based index of the batch whose stream is live: one LLM response of
        /// the lane's run, counted on stream-done and reset by run-done -
        /// the run is the boundary on both ends, so every run counts from 0 again
        /// . The payloads carry it so the C++ derives no step numbering of its own.
        long batchIndex;

        this(UiMessenger messenger, long contextSize, string modelName)
        in (messenger !is null, "UiMessenger must not be null") {
            this.uiMsg = messenger;
            this.contextSize = contextSize;
            this.modelName = modelName;
        }

        override void messageUpdate(StreamMessage msg, StreamToolCall[] tools, ServerStat stat) {
            string content = msg.content;

            uiMsg.pipelineStreamChatMessage(agentId: agentId, content: content, thinking: msg.reasoning, role: msg
                    .role, tokensPerSecond: stat.predictedPerSecond, currentTool: tools.empty
                    ? null : tools[$ - 1].toolName,
                    contextUsed: stat.context, contextMax: contextSize, batch: batchIndex);
        }

        override void streamMessageDone() {
            ++batchIndex;
        }

        override void runDone() {
            uiMsg.pipelineStreamDone(agentId);
            batchIndex = 0;
        }

        override void runStart(string task) {
            uiMsg.pipelineAgentStart(agentId, task);
        }

        override void stepEvent(ProcessResult result) {
            // The step follows the batch's stream-done (the agent's loop calls step
            // after process returns), so the finished batch is the one below the
            // counter - the batch whose stream the drill-down just saw.
            emitStepToolEvents(uiMsg, agentId, result, batchIndex - 1);
        }

        override void setId(string id) {
            agentId = id;
        }

        override IStreamCallback clone() {
            return new PipelineStreamMessageUpdater(uiMsg, contextSize, modelName);
        }
    }

package bool blockDisplayable(UiBlockData b) {
    if (b.kind != TuiBlockKind_Tool)
        return true;
    // Pending/Running exist only on the live path. The console prints a
    // call's line only once its result arrived (Ok/Failed) or the turn
    // ended (Cancelled), so an un-merged live call renders nothing yet.
    if (b.status == TuiBlockStatus_Pending || b.status == TuiBlockStatus_Running)
        return false;
    return (b.flags & TuiBlockFlag_Hidden) == 0;
}

// Tool blocks are collapsible, open by default, selectable.
enum int kToolBlockFlags = TuiBlockFlag_Collapsible | TuiBlockFlag_DefaultOpen
    | TuiBlockFlag_Selectable;

/// The config's displayability, resolved once per turn: `isHidden` is the
/// config's dual-axis predicate (`LlmConfig.isHiddenTool`), null when the
/// config hides nothing. `of` folds it into the block's wire flags - a hidden
/// tool's block carries TuiBlockFlag_Hidden, and the receiver (C++ store,
/// console) decides displayability from the flag alone, never the name.
struct ToolFlags {
    bool delegate(in string) isHidden;

    int of(string name) const {
        return kToolBlockFlags | (isHidden !is null && isHidden(name) ? TuiBlockFlag_Hidden : 0);
    }
}

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

// Build a wire-shaped block from D values. The body crosses uncut: folding
// bounds what renders, so an unbounded body costs copies, not display. The
// id/batchId/title are exact - truncating the id would break the store's
// upsert-merge; meta arrives pre-summarized, tool bodies pre-capped.
package UiBlockData makeBlockData(long turnId, string id, string batchId, int kind,
        int status, int flags, string title, string meta, string body, long durationMs) {
    return UiBlockData(turnId, id, batchId, title, meta, body, durationMs, 0,
            0, kind, status, flags);
}

@("makeBlockData crosses an oversized body verbatim, uncut")
unittest {
    // The reported bug: a long answer ended in "..." at the wire. Folding
    // bounds the render, so the body must arrive whole - 1000 was the cap.
    string long_body;
    foreach (_; 0 .. 1500)
        long_body ~= "x";
    long_body ~= "\nfinal line";
    const r = makeBlockData(7, "answer", "7#0", TuiBlockKind_Answer,
            TuiBlockStatus_None, 0, null, null, long_body, 0);
    assert(r.body == long_body, "body must cross uncut");
    assert(r.body[$ - "final line".length .. $] == "final line", "a byte cut would mangle the tail");
}

// The call's arguments summary for a streaming call whose arguments may still
// be partial JSON.
package string summarizedCallBody(string arguments) {
    string argsSummary;
    try {
        argsSummary = summarizeToolCallArguments(parseJSON(arguments), 1000);
    } catch (Exception e) {
        // Partial/streaming arguments: summarize the raw text instead.
        argsSummary = truncatedForWire(arguments);
    }
    return argsSummary;
}

@("summarizedCallBody carries the arguments only, one pair per line")
unittest {
    // Complete JSON arguments: the summarizer's pairs verbatim (sorted by
    // length, the ",\n" joiner stays) - no name, no parentheses.
    assert(summarizedCallBody(`{"limit":5,"query":"d lang"}`) == "limit=5,\nquery=\"d lang\"");

    // Still-partial streaming arguments: the raw text summarized, never a throw.
    assert(!summarizedCallBody(`{"query":"d`).empty);

    // Empty arguments: an empty body (the header is the whole card then).
    assert(summarizedCallBody(`{}`).empty);
}

/// Shared per-turn state for the typed live emissions: the agent's
/// emitters (llm.app_agent) create one per live turn and hand it to the typed
/// stream updater, which mutates it per tick. One instance per live turn.
package final class TypedEmitContext {
    /// The turn the emissions attach to (chat's currentTurnId at turn start).
    long turnId;
    /// 0-based per-response index within the turn (the batch counter).
    int batchIndex;
    /// Vision block ids: unique across the live turn (two vision entries in
    /// one turn must not upsert-merge into one block).
    int visionSeq;
    /// Informational count of the LLM messages the turn covers so far (the
    /// C-side stepCount over the blocks is the rendered number).
    int messageCount;
    /// The step walk already upserted `Done` with the Answer: runAgent's
    /// exit path must not close the turn a second time.
    bool closedByAnswer;
    /// The role the streamed content blocks carry.
    string role = "assistant";
    /// The config's displayability for this turn's tool blocks (see ToolFlags).
    ToolFlags toolFlags;

    /// One in-flight tool call of the live batch: what the Pending upsert
    /// carried, kept so stream-done can flip the whole batch to Running and a
    /// stop can flip it to Cancelled without rebuilding the blocks.
    struct LiveCall {
        long turnId;
        string id;
        string batchId;
        string title;
        string body;
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

/// Typed sibling of StreamMessageUpdater: upserts the current batch's
/// content/reasoning blocks and the known tool calls as Pending per tick and flips the batch's Pending
/// calls to Running on stream-done (the statuses ride existing events; design
/// appendix "States"). The recording-sink tests exercise it too.
///
/// The context is resolved per event, never captured: every turn start (and a
/// compression or session switch) installs a new TypedEmitContext, and the
/// agent keeps ONE callback for the whole process, so a captured context would
/// take every later turn's stream traffic into the turn and batch it was born
/// in.
package final class TypedStreamMessageUpdater : IStreamCallback {
    UiMessenger uiMsg;
    long contextSize;
    string modelName;
    private TypedEmitContext delegate() ctxProvider;

    this(UiMessenger messenger, long contextSize, string modelName,
            TypedEmitContext delegate() ctxProvider)
    in (messenger !is null, "UiMessenger must not be null")
    in (ctxProvider !is null, "the context provider must not be null") {
        this.uiMsg = messenger;
        this.contextSize = contextSize;
        this.modelName = modelName;
        this.ctxProvider = ctxProvider;
    }

    /// The context the next event writes into; null when no turn is live.
    TypedEmitContext ctx() {
        return ctxProvider();
    }

    override void messageUpdate(StreamMessage msg, StreamToolCall[] tools, ServerStat stat) {
        uiMsg.streamRuntimeStatus(UiRuntimeStatus(phase: !tools.empty ? TuiPhase.tooling
                : msg.content.empty ? TuiPhase.thinking : TuiPhase.writing, detail: tools.empty
                ? null : tools[$ - 1].toolName,
                contextUsed: stat.context, contextMax: contextSize, compactAt: (contextSize * 9) / 10,
                tokensPerSecond: stat.predictedPerSecond, model: modelName));

        auto c = ctx();
        if (c is null)
            return; // a tick that raced a session switch: no turn to write into
        if (!msg.reasoning.empty)
            uiMsg.blockUpsert(makeBlockData(c.turnId, reasoningBlockId(c.batchIndex), c.batchId,
                    TuiBlockKind_Reasoning, TuiBlockStatus_None,
                    TuiBlockFlag_Collapsible, null, null, msg.reasoning, 0));
        // The batch's content-so-far block: same id/batchId every tick, so the
        // store merges in place and the last tick before the step walk wins.
        uiMsg.blockUpsert(makeBlockData(c.turnId, contentBlockId(c.batchIndex),
                c.batchId, TuiBlockKind_Prose, TuiBlockStatus_None, 0, null,
                c.role, msg.content, 0));

        foreach (tool; tools.filter!(a => !a.id.empty)) {
            const body_ = summarizedCallBody(tool.arguments);
            uiMsg.blockUpsert(makeBlockData(c.turnId, tool.id, c.batchId,
                    TuiBlockKind_Tool, TuiBlockStatus_Pending,
                    c.toolFlags.of(tool.toolName), tool.toolName, null, body_, 0));
            c.recordLiveCall(tool.id, tool.toolName, body_);
        }
    }

    override void streamMessageDone() {
        auto c = ctx();
        if (c is null)
            return;
        // The batch's Pending -> Running flip rides the stream-done: re-upsert every call the ticks upserted, status Running.
        foreach (call; c.liveCalls) {
            uiMsg.blockUpsert(makeBlockData(call.turnId, call.id, call.batchId,
                    TuiBlockKind_Tool, TuiBlockStatus_Running,
                    c.toolFlags.of(call.title), call.title, null, call.body, 0));
        }
    }

    override void setId(string id) {
    }

    override IStreamCallback clone() {
        return new TypedStreamMessageUpdater(uiMsg, contextSize, modelName, ctxProvider);
    }
}

/// The one-shot console rebuilt over typed events: a small accumulator
/// with one method per typed event, each returning the lines that event
/// produces.
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
                    bodies ~= t.title ~ "(" ~ t.body ~ ")";
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
