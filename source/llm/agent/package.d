/// Core Agent orchestration: `Agent` drives the model request/tool-call loop and `StreamResponse` parses streaming model responses.
module llm.agent;

import core.thread : Thread;
import core.time : dur;
import logger = std.logger;
import std.algorithm;
import std.array;
import std.conv : to, text;
import std.datetime : Clock, SysTime, Duration, dur;
import std.exception : collectException;
import std.file : readText, exists, read, rename;
import std.format : format, formattedWrite;
import std.json : JSONValue, parseJSON, JSONType, JSONOptions;
import std.path : stripExtension, baseName;
import std.range : empty;
import std.regex : Regex, regex;
import std.sumtype : SumType, match;
import std.typecons : Nullable, nullable;

import my.filter : ReFilter;
import my.path;

import llm.agent.nudges : NudgeTexts, loadNudgeTexts, nudgeFor,
    resolvedNudgeFiles, substituteNudgeVars;
import llm.chat;
import llm.config;
import llm.metric.feedback : FeedbackEngine;
import llm.metric.monitor : MetricMonitor, ToolCallEvent, brokerEvent;
import llm.query : LlmRequester;
import llm.rag.rag : RAG;
import llm.skill : SkillManager, makeSkillManager;
import llm.summary_agent;
import llm.tool_call : FunctionCall, Context;
import llm.tool_call.pipeline : PipelineControlContext;
import llm.tool_call.broker : BrokerState, alwaysOnTools;
import llm.utility : getValue;

import llm.environment.config : EnvironmentBackend;

public import llm.types : IBasicAgent, IAgent, ProcessResult, IStreamCallback,
    ServerStat, StreamToolCall;
public import llm.agent.context : AgentContext, VisionImage;

class Agent : IBasicAgent {
    string name;
    Chat chat;
    MetricMonitor monitor;

    // The model-facing tools array: pool + selectTools with empty activation,
    // plus the composed listToolTags description. Reassigned from pure
    // selectTools output ONLY at change points
    JSONValue[] tools;

    package {
        NudgeConfig defaultNudges_;
        NudgeConfig nudges_;
        NudgeTexts nudgeTexts_;
        Path[] promptDir_;
        // Per-kind strike counters (they map onto NudgeKind)
        int keepReasoningStrikes;
        int continueStrikes;

        AgentContext toolCtx;

        // Armed by syncContextFromChat after a session load/change: the context
        // size is then a pessimistic approximation (chars / ApproxTokenSize) and
        // the "almost full" reading it yields is very likely wrong. The first
        // automatic compression after such a load is blocked (see compress) so
        // the first message never compresses off the stale estimate. Consumed on
        // first use. Package-visible for the first-message regression test.
        bool firstMessageAfterLoad;
    }

    private {
        LlmRequester rq;
        string modelName_;
        RAG rag;
        SummaryAgent summary;
        FeedbackEngine feedbackEngine;
        IStreamCallback streamCallback;
        bool taskDone_;
        string taskDoneMessage_;
        long contextSize_;
        LlmConfig conf;

        bool compressNudgeSent;

        package SysTime lastToolCallWarning;
        int toolCallWarnCounter = -1;
        ReFilter toolFilter;

        string[] neverHideTools_;
        bool waitingForVisionResponse;
        ServerStat prevStat;

        // Invoked with the freshly resolved (global + per-model) config on
        // every model switch (resetModel). Mutations of the passed config are
        // the broker's own use - they never write back into LlmConfig and
        // cannot leak into later resolutions (each switch re-resolves from
        // llmConf) or other agents.
        void delegate(ref ToolBrokerConfig) brokerAdjustHook;
    }

    this(string name, LlmConfig llmConf, MetricMonitor monitor, RAG rag = null) {
        this(name, llmConf, makeSkillManager(llmConf), monitor, rag, ReFilter.init);
    }

    this(string name, LlmConfig llmConf, MetricMonitor monitor, RAG rag, ReFilter filter) {
        this(name, llmConf, makeSkillManager(llmConf), monitor, rag, filter);
    }

    this(string name, LlmConfig llmConf, SkillManager mgr, MetricMonitor monitor,
            RAG rag, ReFilter filter) {
        import llm.tool_call : getFunctions;
        import llm.tool_call.broker : BrokerState, filterRegFunctions, hiddenNeverHideTools;

        this.name = name;
        this.conf = llmConf;
        this.monitor = monitor;
        this.rag = rag;
        this.toolFilter = filter;
        this.toolCtx = new AgentContext(llmConf, rag, monitor);
        toolCtx.setSkillManager(mgr);
        toolCtx.setTaskDoneHandler(&this.taskDone);
        toolCtx.agentName = name;

        // Startup validation: warnings only, never fatal. All three
        // ctors funnel into this one, so every entry point is covered.
        foreach (w; validateToolBrokerConfig(llmConf))
            logger.warningf("%s", w);
        auto reg = getFunctions();
        toolCtx.pool = filterRegFunctions(reg, filter, llmConf.toolBroker.neverHideTools);
        foreach (n; hiddenNeverHideTools(reg, filter, llmConf.toolBroker.neverHideTools))
            logger.warningf("neverHide tool '%s' excluded by toolFilter; fix the config", n);
        toolCtx.broker = BrokerState.init;
        neverHideTools_ = llmConf.toolBroker.neverHideTools;

        // Resolved broker config for the ACTIVE model
        toolCtx.brokerConf = resolveToolBrokerConfig(llmConf, llmConf.activeCodeModel);

        if (brokerAdjustHook !is null)
            brokerAdjustHook(toolCtx.brokerConf);
        toolCtx.brokerEnabled = toolCtx.brokerConf.enabled;

        rebuildModelTools();

        defaultNudges_ = llmConf.nudges;
        promptDir_ = llmConf.promptDir;
        resetModel(llmConf.activeCodeModel);

        this.summary = SummaryAgent(llmConf.summaryModel);
        this.summary.setSystemPrompt(llmConf.getPrompt(skillManager: null,
                promptName: llmConf.summaryModel.prompt, addSkills: false));
    }

    override string id() {
        return name;
    }

    string modelName() const {
        return modelName_;
    }

    long modelContextSize() const {
        return contextSize_;
    }

    /// Last known statistic about the conversation
    ServerStat stat() @safe pure nothrow const @nogc {
        return prevStat;
    }

    /// Sets the broker adjust hook. The hook is invoked on every model
    /// switch (resetModel) with the freshly resolved (global + per-model)
    void setBrokerAdjustHook(void delegate(ref ToolBrokerConfig) hook) @safe {
        brokerAdjustHook = hook;
    }

    /// Rebuilds the model-facing tools array from the current pool and the
    /// effective (hook-adjusted, per-model) broker config
    private void rebuildModelTools() @trusted {
        import llm.tool_call : descAllFunctions, filterToolDescriptions, getFunctions;
        import llm.tool_call.broker : selectTools;
        import llm.tool_call.discovery : composeDiscoveryDesc;

        if (toolCtx.brokerConf.enabled) {
            tools = selectTools(toolCtx.pool, toolCtx.broker.activated,
                    toolCtx.brokerConf.neverHideTools,
                    alwaysOnTools(toolCtx.brokerConf, toolCtx.pool));
            composeDiscoveryDescription(tools, toolCtx.brokerConf, toolCtx.broker);
            toolCtx.rebuildTools = &rebuildModelTools;
        } else {
            // Kill switch: the pre-broker tools array - registry intersect
            // toolFilter (no neverHide union-back), so tools are a subset of
            // the pool, the only possible difference being a neverHide tool
            // excluded by toolFilter (in the pool only; warned at
            // construction)
            tools = filterToolDescriptions(getFunctions(), toolFilter).array;
            toolCtx.rebuildTools = null;
        }
    }

    /// Reset the agent's model to a new configuration. Does NOT modify chat history or SummaryAgent.
    void resetModel(CodeModelConfig modelConfig) {
        import llm.endpoint : getContextSize;

        if (modelConfig.modelName.empty) {
            throw new Exception("Cannot reset to empty model config");
        }

        auto oldModel = modelName_;

        this.rq = LlmRequester(modelConfig.toRequestConfig);

        this.contextSize_ = modelConfig.getContextSize;

        this.modelName_ = modelConfig.modelName;

        logger.tracef("Agent model reset: %s -> %s, context: %s", oldModel,
                modelConfig.modelName, this.contextSize_);
        nudges_ = modelConfig.nudges.get(defaultNudges_);
        nudgeTexts_ = loadNudgeTexts(promptDir_, nudges_);
        toolCtx.broker = BrokerState.init;
        toolCtx.brokerConf = resolveToolBrokerConfig(conf, modelConfig);
        if (brokerAdjustHook !is null)
            brokerAdjustHook(toolCtx.brokerConf);
        toolCtx.brokerEnabled = toolCtx.brokerConf.enabled;
        rebuildModelTools();
    }

    /// MCP connect change point: an MCP server has just registered its tools
    /// into the global registry (llm.mcp_server.registration), so the pool is
    /// recomputed from the live registry.
    void onMcpServerConnected() {
        import llm.tool_call : descAllFunctions, filterToolDescriptions, getFunctions;
        import llm.tool_call.broker : filterRegFunctions;

        toolCtx.pool = filterRegFunctions(getFunctions(), toolFilter, neverHideTools_);
        if (toolCtx.rebuildTools !is null)
            toolCtx.rebuildTools();
        else
            tools = filterToolDescriptions(getFunctions(), toolFilter).array;
    }

    Message[] getUserQueries() @safe nothrow {
        return chat.getUserQueries;
    }

    void setSystemPrompt(string x) {
        chat.setSystemPrompt(x);
    }

    override void setStreamUpdate(IStreamCallback callback) {
        this.streamCallback = callback;
    }

    void setPipelineContext(PipelineControlContext ctx) @trusted {
        toolCtx.setPipelineContext(ctx);
    }

    /// Adds a user query. Chat owns the turn policy: a user query always opens a new turn inside Chat.add - call sites cannot violate it.
    void addUserQuery(string query) nothrow {
        chat.addUserQuery(query);
    }

    // Adds a harness control message that continues the current turn
    void addContinueMessage(string msg) @safe nothrow {
        chat.add(Message(Role.user, userQuery: false, content: msg, thinking: null));
    }

    /// Emits the strike-appropriate nudge for `kind`
    package bool addEscalatedNudge(NudgeKind kind) @safe {
        final switch (kind) with (NudgeKind) {
        case keepReasoning:
            return escalateNudge(nudges_.keepReasoning, kind, keepReasoningStrikes);
        case recovery:
            return escalateNudge(nudges_.recovery, kind, continueStrikes);
        }
    }

    private static string nudgeSourceFile(in EscalationConfig esc, NudgeKind kind,
            bool hard, in long stepNo) @safe pure {
        const names = resolvedNudgeFiles(esc, kind, hard);
        return names[min(stepNo, cast(long) names.length) - 1];
    }

    private bool escalateNudge(const(EscalationConfig) esc, NudgeKind kind, ref int strike) @safe {
        if (!esc.enabled)
            return true;

        strike++;
        immutable inSoftPhase = strike <= esc.softStrikes;
        if (!inSoftPhase && esc.hardStrikes > 0 && strike - esc.softStrikes > esc.hardStrikes) {
            logger.warningf("agent %s: %s nudge ladder exhausted at strike %s - failing the turn (agentStuckInLoop)",
                    modelName_, kind, strike);
            return false; // ladder exhausted: the caller fails the turn
        }

        auto ladder = inSoftPhase ? nudgeTexts_.soft[kind] : nudgeTexts_.hard[kind];
        assert(!ladder.empty, "escalateNudge: empty ladder for an enabled kind");
        immutable phaseNo = inSoftPhase ? strike : strike - esc.softStrikes;
        logger.tracef("agent %s: nudge %s strike %s (soft=%s) from %s", modelName_, kind,
                strike, inSoftPhase, nudgeSourceFile(esc, kind, !inSoftPhase, phaseNo));
        chat.add(Message(Role.user, userQuery: false, thinking: null, content: nudgeFor(ladder,
                phaseNo)));
        return true;
    }

    /// Emits the strike-appropriate keep-reasoning nudge
    void addKeepReasoning() @safe {
        addEscalatedNudge(NudgeKind.keepReasoning);
    }

    /// Emits the strike-appropriate continue/recovery nudge
    void addContinue() @safe {
        addEscalatedNudge(NudgeKind.recovery);
    }

    void addCompressionNudge() @safe nothrow {
        if (!nudges_.compression.enabled || compressNudgeSent)
            return;
        if (prevStat.context <= nudges_.compression.threshold * contextSize_)
            return;
        compressNudgeSent = true;

        try {
            auto percent = format("%.1f",
                    cast(double) prevStat.context / cast(double) contextSize_ * 100.0);
            auto msg = substituteNudgeVars(nudgeTexts_.compression, [
                "context_percent": percent
            ]);
            chat.add(Message(Role.user, userQuery: false, thinking: null, content: msg));
        } catch (Exception e) {
            try {
                logger.trace(e.msg);
            } catch (Exception e) {
            }
        }
    }

    ProcessResult process(bool delegate() interrupt) @trusted nothrow {
        import std.functional : toDelegate;
        import llm.query : HttpResult, HttpError, canRetry;

        ProcessResult rval;
        // Failure and interrupt paths report the previous stat (not a zero one): the discarded partial message did not grow the context, so the last known context size is still the best estimate.
        rval.stat = prevStat;

        ServerStat useOrApproxStatistic(ServerStat stat) {
            if (stat.startContext == prevStat.startContext) {
                // no timing/usage message received, so the context size must be estimated
                stat.startContext = chat.approxContextSize;
            } else if (stat.context == 0) {
                return prevStat;
            }
            return stat;
        }

        try {
            logger.tracef("agent %s: processing turn %s (%s messages)", name,
                    chat.currentTurnId(), chat.length);
            auto sp = StreamResponse(prevStat);
            auto stream = (const(char)[] chunk) { /*logger.trace(chunk);*/ sp.parse(chunk);
                if (streamCallback !is null) {
                    streamCallback.messageUpdate(sp.message, orderedToolCalls(sp), sp.stat);
                }
            };
            rq.setCallbacks(stream: stream.toDelegate, interrupt: interrupt);
            scope (exit)
                rq.setCallbacks(null, null);
            scope (exit) {
                if (streamCallback !is null) {
                    streamCallback.streamMessageDone();
                }
            };

            // Metrics: the tools array size + approximate
            // schema token cost (the chat.d ApproxTokenSize heuristic) per
            // request. The record path is guarded (recordBrokerEvent); the
            // token estimate itself cannot throw (plain tool cards).
            {
                import llm.common.config : ApproxTokenSize;

                long schemaTokens;
                foreach (t; tools)
                    schemaTokens += t.toString(JSONOptions.doNotEscapeSlashes).length;
                schemaTokens /= ApproxTokenSize;
                recordBrokerEvent(this, "tools_request", [
                    "toolsCount": JSONValue(cast(long) tools.length),
                    "schemaTokens": JSONValue(schemaTokens),
                ]);
            }
            const t0 = Clock.currTime;
            auto res = rq.request(chat, tools);
            auto llmCall = MessageTime(Clock.currTime - t0);

            bool hasHttpError;
            bool httpRetryOnError;
            res.match!((HttpResult _) {}, (HttpError e) {
                logger.trace(e);
                hasHttpError = true;
                httpRetryOnError = canRetry(e);
            });

            if (hasHttpError && httpRetryOnError) {
                // soft http failures that can be retried
                rval.status = ProcessResult.Status.retryLater;
            } else if (hasHttpError) {
                // hard failures
                rval.status = ProcessResult.Status.unknownFailure;
            } else if (sp.hasError) {
                if (sp.error.codeNr == 400 && sp.error.type == "exceed_context_size_error") {
                    // llama.cpp
                    logger.trace("Context overflow detected: ", sp.error);
                    rval.status = ProcessResult.Status.needCompression;
                } else if (sp.error.type == "invalid_request_error"
                        && sp.error.code == "context_length_exceeded") {
                    // openai
                    logger.trace("Context overflow detected: ", sp.error);
                    rval.status = ProcessResult.Status.needCompression;
                } else {
                    logger.trace("unhandled error: ", sp.error);
                    rval.status = ProcessResult.Status.unknownFailure;

                    // Recovery: the server rejected the request. The history may contain invalid UTF-8 (typically binary command output). Sanitize it in place (messages preserved).
                    if (chat.sanitizeHistory > 0) {
                        logger.warning("Sanitized chat history (invalid UTF-8)");
                    }
                }
            } else {
                rval.stat = useOrApproxStatistic(sp.stat);
                rval.status = parseResponse(sp, llmCall);
                rval.chat = chat.lastResponses;
                chat.resetResponseIndex;

                if (!rval.chat.empty) {
                    rval.hasToolCall = rval.chat[$ - 1].match!((ToolMessage _) => true,
                            (ToolResponse _) => true, (_) => false);
                }
                // Advance context bookkeeping only when the turn completed and its messages were committed to the chat. On interrupts (/stop) and request failures the partial message is thrown away, so the previous context size stays valid as-is. Re-estimating here (chars / ApproxTokenSize, a pessimistic value) would inflate prevStat and could trigger a spurious compression on the next query.
                prevStat = useOrApproxStatistic(sp.stat).newTurn;
            }
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
            rval.status = ProcessResult.Status.unknownFailure;
        }

        return rval;
    }

    bool needCompression(double threshold = 0.9) @safe pure nothrow const @nogc {
        return prevStat.context > contextSize_ * threshold;
    }

    SummaryAgent.CompressResult compress(double threshold = 0.9, bool force = false,
            SummaryAgent.ProgressCallback callback = null) {
        import llm.common.config : ApproxTokenSize;
        import llm.tool_call.broker : applyPrune, scanInactiveTools;

        // The first automatic compression after a session load/change is
        // blocked. syncContextFromChat left prevStat.context as a pessimistic
        // approximation (chars / ApproxTokenSize); that "almost full" reading
        // is very likely wrong, so the first message after a load must not
        // compress off it. A forced compression (a genuine server overflow) or
        // an explicit agent compression request still proceeds.
        if (firstMessageAfterLoad && !force && !toolCtx.agentCompressionRequest) {
            firstMessageAfterLoad = false;
            return typeof(return)(compressed: true);
        }
        if (!needCompression(threshold) && !force && !toolCtx.agentCompressionRequest)
            return typeof(return)(compressed: true);
        scope (exit) {
            toolCtx.clearAgentCompressionRequest;
            compressNudgeSent = false;
        }

        // Stamp the owning session into any checkpoint this compression
        // fires. The agent-internal compress paths (the requestCompression
        // tool, the automatic threshold compression in runToCompletion) never
        // went through AgentApp.doCompress, the only place that used to set
        // the id, so their checkpoints carried an empty session id and the
        // dialogue indexer refused every one of them - no session database
        // was ever created. The tool context knows the owning session for
        // the whole process lifetime, and pooled agents carry it in their
        // own conf, so stamping here covers every compress path.
        summary.setCheckpointSessionId(toolCtx.currentSessionId());
        // Prune scan: the intact pre-compression chat is the
        // usage corpus. Runs after the early-return gate, before summary.compress
        // rewrites the chat; summary text and assistant prose never count as usage.
        string[] inactive = scanInactiveTools(toolCtx.broker, chat, toolCtx.pool);
        long oldContextSize = prevStat.context;
        auto result = summary.compress(chat, callback, null);
        prevStat.startContext = result.newContextSize;
        if (force) {
            logger.infof("Forced compression: context %s -> %s tokens (saved %s)",
                    oldContextSize, prevStat.context, oldContextSize - prevStat.context);
        }
        if (result.compressed && toolCtx.agentCompressionRequest
                && !toolCtx.agentCompressionMessageToSelf.empty) {
            auto msg = i"[SYSTEM MESSAGE - NOT USER INPUT]
Context compression has been performed. The following is the summary you wrote for yourself before compression. Use it as your memory of the previous conversation.

Summary:$(
                    toolCtx.agentCompressionMessageToSelf)".text;
            chat.add(Message(Role.user, userQuery: false, thinking: null, content: msg));
            prevStat.startContext += msg.length / ApproxTokenSize;
        }
        if (result.compressed) {
            auto msg = "[SYSTEM MESSAGE - NOT USER INPUT]
Continue your work from where you left off.";
            chat.add(Message(Role.user, userQuery: false, thinking: null, content: msg));
            prevStat.startContext += msg.length / ApproxTokenSize;
        }
        // Prune apply: only when the compression actually
        // rewrote history. The tools-array rebuild goes through the toolCtx.rebuildTools hook
        // (selectTools + the composed listToolTags description), so both change
        // points produce byte-identical arrays for the same broker state and
        // the discovery entry keeps the composed description. Null (kill
        // switch) means the tools array never changes.
        if (result.originalLength != result.newLength || result.purgedCount > 0) {
            applyPrune(toolCtx.broker, inactive);
            // Metrics: one event per applied prune; the scan
            // epoch counted the same list applyPrune just consumed.
            recordBrokerEvent(this, "broker_prune",
                    ["pruned": JSONValue(cast(long) inactive.length)]);
            if (toolCtx.rebuildTools !is null)
                toolCtx.rebuildTools();
        }
        return result;
    }

    // Register a listener for compression checkpoints. The seam is multicast:
    // interested subsystems (currently the dialogue indexer) subscribe
    // independently. A listener fires exactly once per compression that
    // actually evicts verbatim content, and a throwing listener never breaks
    // compression.
    void addCompressionCheckpointListener(SummaryAgent.CheckpointListener listener) {
        summary.addCheckpointListener(listener);
    }

    /// Public accessor for the tool context (needed by app_agent to inject the DialogueIndex after construction).
    AgentContext toolContext() {
        return toolCtx;
    }

    // Set the session id stamped into each checkpoint event. Call sites that
    // know the owning session (doCompress: activeSession.id) set it before
    // compressing; pool callbacks set their own or leave "". The dialogue
    // indexer refuses checkpoints with an empty or invalid sessionId.
    void setCompressionCheckpointSessionId(string sessionId) {
        summary.setCheckpointSessionId(sessionId);
    }

    /// Run the agent until completion (no more tool calls, no more thinking needed)
    override ProcessResult runToCompletion(void delegate(ProcessResult) step = null,
            SummaryAgent.ProgressCallback compressCallback = null, bool delegate() interrupt = null) @trusted {
        taskDone_ = false;
        taskDoneMessage_ = null;
        ProcessResult result;

        bool keepRunning;

        ProcessResult.Status lastStatus = ProcessResult.Status.unknownFailure;

        size_t consecutiveSameStatus;
        immutable MaxConsecutiveSameStatus = 3;
        size_t consecutiveNoToolCallOk;
        immutable MaxConsecutiveNoToolCallOk = 5;

        void resetStrikes() {
            keepReasoningStrikes = 0;
            continueStrikes = 0;
        }

        resetStrikes();

        do {
            result = this.process(interrupt);
            if (step)
                step(result);
            if (taskDone_ || (interrupt && interrupt()))
                break;
            keepRunning = result.hasToolCall;

            final switch (result.status) with (ProcessResult.Status) {
            case ok:
                if (!result.hasToolCall && waitingForVisionResponse) {
                    waitingForVisionResponse = false;
                    keepRunning = true;
                    resetStrikes;
                } else if (!result.hasToolCall) {
                    // No tool call: continue the turn via the configurable
                    // recovery ladder
                    if (!addEscalatedNudge(NudgeKind.recovery)) {
                        result.status = ProcessResult.Status.agentStuckInLoop;
                        keepRunning = false;
                        break;
                    }
                    keepRunning = true;
                } else {
                    // ok WITH tool calls
                    keepRunning = true;
                    resetStrikes;
                }
                break;
            case needCompression:
                this.compress(force: true, callback: compressCallback);
                keepRunning = true;
                resetStrikes;
                break;
            case unknownFailure:
                keepRunning = false;
                break;
            case retryLater:
                keepRunning = true;
                break;
            case networkFailure:
                keepRunning = false;
                break;
            case needMoreThinking:
                if (!addEscalatedNudge(NudgeKind.keepReasoning)) {
                    result.status = ProcessResult.Status.agentStuckInLoop;
                    keepRunning = false;
                    break;
                }
                keepRunning = true;
                break;
            case agentStuckInLoop:
                // unreachable: the only place this status is set is inside this
                // very switch (ladder exhaustion), which ends the turn in the
                // same pass (keepRunning = false).
                keepRunning = false;
            }

            // Safety check: detect stuck loops
            if (result.status == lastStatus && result.status != ProcessResult.Status.ok) {
                consecutiveSameStatus++;
                if (consecutiveSameStatus > MaxConsecutiveSameStatus) {
                    logger.warningf("Agent stuck in loop with status %s after %s iterations, breaking",
                            result.status, consecutiveSameStatus);
                    result.status = ProcessResult.Status.agentStuckInLoop;
                    keepRunning = false;
                }
            } else {
                lastStatus = result.status;
                consecutiveSameStatus = 1;
            }
            // Safety check: detect LLM failing to call tools (continue spam loop)
            if (result.status == ProcessResult.Status.ok && !result.hasToolCall) {
                consecutiveNoToolCallOk++;
                if (consecutiveNoToolCallOk > MaxConsecutiveNoToolCallOk) {
                    logger.warningf("Agent stuck in continue loop without tool calls after %s iterations, breaking",
                            consecutiveNoToolCallOk);
                    result.status = ProcessResult.Status.agentStuckInLoop;
                    keepRunning = false;
                }
            } else {
                consecutiveNoToolCallOk = 0;
            }

            if (needCompression || toolCtx.agentCompressionRequest) {
                // compress at the end because it could be filled with junk
                this.compress(callback: compressCallback);
            } else {
                addCompressionNudge();
            }
        }
        while (keepRunning);
        return result;
    }

    /// Get the text content of the last assistant message
    string lastAssistantText() @safe {
        string rval;
        foreach (i; 0 .. chat.length) {
            auto msg = chat.getMessages[chat.length - 1 - i];
            msg.match!((Message m) {
                if (m.role == Role.assistant) {
                    rval = m.content;
                }
            }, (ToolMessage m) {}, (ToolResponse m) {}, (VisionMessage m) {});
            if (!rval.empty)
                return rval;
        }
        return "";
    }

    /// Get the last N assistant messages as a MessageT array for pipeline handoff
    Chat.MessageT[] lastResponsesAsMessages(uint count = 1) @safe {
        Chat.MessageT[] result;
        foreach (i; 0 .. chat.length) {
            auto msg = chat.getMessages[chat.length - 1 - i];
            msg.match!((Message m) {
                if (m.role == Role.assistant && i < count) {
                    result ~= msg;
                }
            }, (ToolMessage m) {}, (ToolResponse m) {}, (VisionMessage m) {});
            if (result.length >= count)
                return result;
        }
        return result;
    }

    void clearHistory() @safe {
        chat.clear;
        syncContextFromChat();
    }

    /// Sync prevStat.context from current chat context size. Called after loading chat history so the agent knows the starting context.
    void syncContextFromChat() @safe {
        prevStat = ServerStat(startContext: chat.approxContextSize);
        // The just-synced context size is a pessimistic approximation: arm the
        // first-message block so the next automatic compression cannot trigger
        // off this stale estimate (see compress).
        firstMessageAfterLoad = true;
    }

    void saveHistory(Path dir) @trusted nothrow {
        import std.stdio : File;

        try {
            auto historyPath = dir ~ (this.name ~ "_history.json");
            string tempFile = historyPath.toString ~ ".tmp";
            File(tempFile, "w").write(
                    chat.toSaveJson.toPrettyString(JSONOptions.doNotEscapeSlashes));
            rename(tempFile, historyPath.toString);
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
    }

    void loadHistory(Path dir) @trusted nothrow {
        try {
            auto historyPath = dir ~ (this.name ~ "_history.json");
            if (!historyPath.exists) {
                logger.trace("agent history do not exist at ", historyPath);
                return;
            }

            logger.trace("load agent history from ", historyPath);
            auto j = readText(historyPath.toString).parseJSON;
            chat.load(j);
            chat.resetResponseIndex;
            syncContextFromChat();
            seedBrokerFromChat();
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
    }

    /// Seed the tool broker from the loaded chat: re-activate each tagged tool
    /// the history proves was used
    void seedBrokerFromChat() @safe {
        import llm.tool_call.broker : activateTool, scanUsedTools;

        if (!toolCtx.brokerEnabled)
            return;

        size_t seeded;
        auto used = () @trusted { return scanUsedTools(chat); }();

        foreach (name; used) {
            auto matches = toolCtx.pool.filter!(f => f.name == name || f.nameSnakeCase == name);
            if (matches.empty)
                continue;
            auto f = matches.front;
            if (f.tags.empty || neverHideTools_.canFind!(n => n == name))
                continue;
            auto before = toolCtx.broker.activated.length;
            activateTool(toolCtx.broker, toolCtx.pool, name);
            if (toolCtx.broker.activated.length != before)
                seeded++;
        }
        if (seeded > 0) {
            recordBrokerEvent(this, "broker_seed", [
                "seeded": JSONValue(cast(long) seeded)
            ]);
            if (toolCtx.rebuildTools !is null)
                toolCtx.rebuildTools();
        }
    }

    string takeTaskDoneMessage() @safe {
        auto msg = taskDoneMessage_;
        taskDoneMessage_ = null;
        return msg;
    }

private:
    void taskDone(string answer) @safe {
        this.taskDone_ = true;
        this.taskDoneMessage_ = answer;
    }

    package ProcessResult.Status parseResponse(ref StreamResponse sp,
            MessageTime llmCall) @trusted nothrow {
        try {
            logger.trace(sp);

            if (!sp.toolCalls.empty) {
                handleToolCalls(sp.message.reasoning, sp.toolCalls, llmCall);
            } else if (!sp.message.isEmpty) {
                auto m = Message(Role.assistant, userQuery: false, content: sp.message.content,
                        thinking: sp.message.reasoning);
                m.setProcessTime(llmCall);
                chat.add(m);
            }
            if (!sp.message.finishReason.empty) {
                if (sp.message.finishReason == "length")
                    return ProcessResult.Status.needCompression;
                if (sp.message.finishReason == "stop" && sp.message.isEmpty)
                    return ProcessResult.Status.needMoreThinking;
                if (sp.message.finishReason == "tool_calls")
                    return ProcessResult.Status.ok;
                if (sp.message.finishReason == "stop")
                    return ProcessResult.Status.ok;
                logger.tracef("unknown finish reason: %s", sp.message.finishReason);
            }
            return ProcessResult.Status.ok;
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
        return ProcessResult.Status.unknownFailure;
    }

    package void handleToolCalls(string thinking, ref StreamResponse.ToolCall[long] toolCalls,
            MessageTime llmCall = NoProcessingTime.init) {
        import llm.tool_call : executeFunc;
        import llm.utility : sanitizeUtf8;

        foreach (call; toolCalls.byValue) {
            try {
                if (monitor !is null && nudges_.feedback.enabled && (Clock.currTime > lastToolCallWarning
                        && toolCallWarnCounter > nudges_.feedback.minToolCalls
                        || toolCallWarnCounter == -1)) {
                    toolCallWarnCounter = 0;
                    lastToolCallWarning = Clock.currTime + dur!"seconds"(
                            nudges_.feedback.intervalSecs);

                    feedbackEngine.setEvents(monitor.getRecentEvents(100));
                    auto warnings = feedbackEngine.getWarnings();
                    chat.add(Message(Role.user, userQuery: false, content: warnings, thinking: null));
                }
            } catch (Exception e) {
                logger.tracef("feedback check failed: %s", e.msg);
            }
            toolCallWarnCounter++;

            const startTime = Clock.currTime;
            bool success;
            string result;
            try {
                if (toolCtx.brokerEnabled
                        && toolCtx.pool.canFind!(f => f.nameSnakeCase == call.name)
                        && !tools.canFind!(t => t["function"]["name"].str == call.name)) {
                    result = ("error: tool '" ~ call.name ~ "' is not visible to this agent in the current session (visibility resets when chat history is loaded or the session changes); discover available tool groups with `list_tool_tags`, then call `list_tool_tags` with the group name to activate it.")
                        .sanitizeUtf8;
                    success = false;

                    recordBrokerEvent(this, "tool_refusal",
                            [
                                "tool": JSONValue(call.name),
                                "tier": JSONValue(3L)
                    ]);
                } else {
                    auto res = executeFunc(toolCtx, call.name,
                            parseJSON(call.arguments), toolFilter);
                    result = res.msg.sanitizeUtf8;
                    success = res.success;
                }
            } catch (Exception e) {
                logger.tracef("Broken tool call. Incoming json: %s", e.msg);
                continue;
            }

            immutable responseTimeMs = (Clock.currTime - startTime).total!"msecs";
            try {
                if (monitor !is null) {
                    monitor.record(this.name, call.name,
                            parseJSON(call.arguments), result, success, responseTimeMs);
                }
            } catch (Exception e) {
                logger.tracef("monitor record failed: %s", e.msg);
            }

            JSONValue sd = JSONValue.init;
            if (conf.isTaskDone(call.name) && taskDone_ && !taskDoneMessage_.empty) {
                sd["taskDoneAnswer"] = JSONValue(taskDoneMessage_);
            }
            auto tm = ToolMessage(thinking, JSONValue([call.toJson]), JSONValue.init, sd);
            tm.setProcessTime(llmCall);
            chat.add(tm);
            auto tr = ToolResponse(content: result, toolCallId: call.id,
                    toolName: call.name, success: success);
            tr.setProcessTime(MessageTime(responseTimeMs.dur!"msecs"));
            chat.add(tr);
            if (auto image = toolCtx.drainVisionImage) {
                chat.add(VisionMessage(image.query, image.data));
                waitingForVisionResponse = true;
            }
        }
    }
}

/// The stream's tool calls in protocol order (task 39a).
///
/// `StreamResponse.toolCalls` is an associative array keyed by the call's index in the
/// response, and an AA's iteration order is not defined. Without this, both the `--- Tool ---`
/// block order in the streamed text and "the newest tool" (`tools[$ - 1]`) would depend on the
/// AA's layout: two runs of the same response could disagree, and the lane row could name the
/// wrong tool. Sorting by key makes the last element the highest-index call, which is what the
/// UI reports as `currentTool`.
package StreamToolCall[] orderedToolCalls(ref StreamResponse sp) {
    return sp.toolCalls
        .byKeyValue
        .array
        .sort!((a, b) => a.key < b.key)
        .map!(a => a.value.toStream)
        .array;
}

struct StreamResponse {
    import std.range : isOutputRange;
    import std.datetime : Clock;
    import llm.types : ServerStat, StreamMessage, StreamToolCall;
    import llm.utility : RollingAvg;

    struct ToolCall {
        string id;
        string name;
        string arguments;

        JSONValue toJson() @safe {
            JSONValue j;
            j["name"] = name;
            j["arguments"] = arguments;

            JSONValue rval;
            rval["function"] = j;
            rval["id"] = id;
            rval["type"] = "function";

            return rval;
        }

        string toString() @safe const {
            return i"ToolCall(id:$(id) name:$(name) arguments:'$(arguments)')".text;
        }

        StreamToolCall toStream() @safe nothrow const {
            return StreamToolCall(toolName: name, arguments: arguments);
        }

        string toPrettyString() @safe nothrow const {
            auto buf = appender!(char[])();

            try {
                if (!arguments.empty && arguments[$ - 1] == '}') {
                    formattedWrite(buf, "%s(%s)", name, parseJSON(arguments));
                    return buf[].idup;
                }
            } catch (Exception e) {
            }

            try {
                formattedWrite(buf, "%s(%s)", name, arguments);
            } catch (Exception e) {
            }
            return buf[].idup;
        }
    }

    struct ErrorMessage {
        string type;
        string message;
        string code;
        // only llama-server set this
        long codeNr = -1;

        string toString() @safe const {
            return i"ErrorMessage(type:$(type) message:$(message) code:$(code) codeNr:$(codeNr))"
                .text;
        }
    }

    bool isDone;
    bool hasError;
    StreamMessage message;
    ToolCall[long] toolCalls;
    ServerStat stat;
    ErrorMessage error;

    private {
        SysTime start;
        RollingAvg ravg;
        long accumulatedChars;
    }

    this(ServerStat prevStat) {
        stat = prevStat;
        ravg = RollingAvg(window: 10.dur!"seconds");
        ravg.put(stat.tokenCount);
    }

    string toString() @safe const {
        import std.array : appender;

        auto buf = appender!string;
        toString(buf);
        return buf.data;
    }

    void toString(Writer)(ref Writer w) const if (isOutputRange!(Writer, char)) {
        formattedWrite(w, "StreamResponse(isDone:%s%s %s %s %s)", isDone,
                hasError ? i"ErrorMessage($(error))".text : "", stat, message, toolCalls);
    }

    void parse(const(char)[] response) {
        import std.string : strip;

        if (start == SysTime.init) {
            start = Clock.currTime;
        }

        immutable prefix = "data: ";
        immutable llamaErrorPrefix = "error: ";
        immutable openAiv1Prefix = "{\"error\":";

        response = response.strip;
        if (response.startsWith(llamaErrorPrefix)) {
            response = response[llamaErrorPrefix.length .. $];
            parseLlamaCppError(response);
            return;
        } else if (response.startsWith(openAiv1Prefix)) {
            try {
                auto j = parseJSON(response);
                parseOpenAiError(j);
                return;
            } catch (Exception e) {
                logger.tracef("error parse of OpenAI error mesage: '%s': %s", response, e.msg);
            }
        } else if (response.startsWith(prefix)) {
            response = response[prefix.length .. $];
        } else {
            return;
        }
        if (response == "[DONE]" || isDone || hasError) {
            isDone = true;
            return;
        }

        try {
            auto rootj = parseJSON(response);

            if (auto e = "error" in rootj) {
                parseOpenAiError(*e);
                return;
            }

            auto choices = getValue(rootj, (v) => v["choices"].array, null);
            foreach (choice; choices) {
                auto delta = choice["delta"];
                if (auto j = "tool_calls" in delta) {
                    parseToolCall(*j);
                }
                if ("content" in delta) {
                    parseMessage(delta);
                }
                if (auto j = "reasoning_content" in delta) {
                    parseReasoning(*j);
                } else if (auto j = "reasoning" in delta) {
                    parseReasoning(*j);
                }
                if (auto reason = "finish_reason" in choice) {
                    // data: {"choices":[{"finish_reason":"tool_calls","index":0,"delta":{}}],"created":1784060897,"id":"chatcmpl-HPewiQRRMdrJz3CIV6PcoZKgXpwGB4GL","model":"qwen3.6-27b-code","system_fingerprint":"b9998-c036959df","object":"chat.completion.chunk","timings":{"cache_n":6151,"prompt_n":4,"prompt_ms":196.08,"prompt_per_token_ms":49.02,"prompt_per_second":20.39983680130559,"predicted_n":244,"predicted_ms":8121.839,"predicted_per_token_ms":33.286225409836064,"predicted_per_second":30.042457133168977,"draft_n":165,"draft_n_accepted":147}} [llm.agent.Agent.process.__lambda_L154_C27:154]
                    message.finishReason = getValue(*reason, (v) => v.str, null);
                } else {
                    logger.trace("unknown JSON response from server: ", delta);
                }
            }
            parseStat(rootj);
        } catch (Exception e) {
            logger.trace(e.msg);
        }
    }

    void incrToken(long textLen) @safe nothrow {
        import llm.common.config : ApproxTokenSize;

        double ratio = stat.charTokenRatio < 1.0 ? ApproxTokenSize : stat.charTokenRatio;
        stat.tokenCount += cast(long)(textLen / ratio) + 1;
        accumulatedChars += textLen;
    }

    void parseStat(ref JSONValue json) @safe nothrow {
        void mergeTokenRatio(double newV) {
            // smooth out "jitter" in the ratio. If the ratio is just set to
            // the last value there will be a large "difference" that never
            // "correct" itself when the LLM go between generating a lot of
            // thinking and then go to generating code.
            if (stat.charTokenRatio < 1.0)
                stat.charTokenRatio = newV;
            else
                stat.charTokenRatio = stat.charTokenRatio * 0.8 + newV * 0.2;
        }

        try {
            if (auto timings = "timings" in json) {
                // llama.cpp and maybe others
                stat.predictedPerSecond = getValue(*timings,
                        (v) => v["predicted_per_second"].floating, stat.predictedPerSecond);
                stat.promptPerSecond = getValue(*timings,
                        (v) => v["prompt_per_second"].floating, stat.promptPerSecond);
                auto oldCtx = stat.startContext;
                stat.startContext = getValue(*timings, (v) => v["cache_n"].integer, stat.context);
                stat.tokenCount = 0;

                auto completedTokens = getValue(*timings,
                        (v) => v["predicted_n"].integer, stat.startContext - oldCtx);

                if (completedTokens > 0)
                    mergeTokenRatio(cast(double) accumulatedChars / (cast(double) completedTokens));
            } else if (auto usage = "usage" in json) {
                // deepseek and maybe others
                stat.startContext = getValue(*usage, (v) => v["total_tokens"].integer, stat.context);
                stat.tokenCount = 0;
                const s = (Clock.currTime - start).total!"seconds";

                const cTokens = getValue(*usage, (v) => v["completion_tokens"].integer, 0);
                if (s > 0 && cTokens > 0)
                    stat.predictedPerSecond = cast(double) cTokens / cast(double)(s);
                const pTokens = getValue(*usage, (v) => v["prompt_tokens"].integer, 0);
                if (s > 0 && pTokens > 0)
                    stat.promptPerSecond = cast(double) pTokens / cast(double)(s);

                if (cTokens > 0)
                    mergeTokenRatio(cast(double) accumulatedChars / (cast(double) cTokens));
            } else if (stat.tokenCount > 0) {
                ravg.put(stat.tokenCount);
                stat.predictedPerSecond = ravg.avg();
            }
        } catch (Exception e) {
            try {
                logger.tracef("unknown timings structure '%s': %s", json, e.msg);
            } catch (Exception e) {
            }
        }
    }

    void parseToolCall(ref JSONValue jtoolCalls) {
        // {"choices":[{"finish_reason":null,"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"."}}]}}],"created":1783954301,"id":"chatcmpl-24rJM2FM6BcQPngWA5ktQh5TNzsIwLkD","model":"qwen3.6-27b-code","system_fingerprint":"b9992-348ed0017","object":"chat.completion.chunk"}
        // {"choices":[{"finish_reason":"tool_calls","index":0,"delta":{}}],"created":1783954301,"id":"chatcmpl-24rJM2FM6BcQPngWA5ktQh5TNzsIwLkD","model":"qwen3.6-27b-code","system_fingerprint":"b9992-348ed0017","object":"chat.completion.chunk","timings":{"cache_n":6150,"prompt_n":4,"prompt_ms":194.782,"prompt_per_token_ms":48.6955,"prompt_per_second":20.53 5778460021973,"predicted_n":254,"predicted_ms":8251.368,"predicted_per_token_ms":32.485700787401576,"predicted_per_second":30.782774444189133,"draft_n":198,"draft_n_accepted":165}}
        foreach (jcall; getValue(jtoolCalls, (v) => v.array, null)) {
            try {
                long index = jcall["index"].integer;
                long txtLen;
                if (auto a = index in toolCalls) {
                    auto arg = jcall["function"]["arguments"].str;
                    (*a).arguments ~= arg;
                    txtLen = arg.length;
                } else {
                    auto name = jcall["function"]["name"].str;
                    auto arg = getValue(jcall, (v) => v["function"]["arguments"].str, null); // arguments is optional for a new tool call
                    toolCalls[index] = ToolCall(id: jcall["id"].str, name: name, arguments: arg);
                    txtLen = arg.length + name.length;
                }
                incrToken(txtLen);
            } catch (Exception e) {
                logger.tracef("invalid tool call structure '%s': %s", jcall, e.msg);
            }
        }
    }

    void parseMessage(ref JSONValue delta) {
        // {"choices":[{"finish_reason":null,"index":0,"delta":{"role":"assistant","content":null}}],"created":1784015079,"id":"chatcmpl-EOriRpn903HuGe6xeo72PiY4ntCQXKP8","model":"qwen3.6-27b-code","system_fingerprint":"b9998-c036959d f","object":"chat.completion.chunk"}
        try {
            if (auto role = "role" in delta) {
                message.role = role.str;
            }
            if (auto content = "content" in delta) {
                if (content.type == JSONType.string) {
                    auto s = content.str;
                    message.content ~= s;
                    incrToken(s.length);
                }
            }
        } catch (Exception e) {
            logger.tracef("invalid message structure '%s': %s", delta, e.msg);
        }
    }

    void parseReasoning(ref JSONValue json) {
        // data: {"choices":[{"finish_reason":null,"index":0,"delta":{"reasoning_content":"The"}}],"created":1784060211,"id":"chatcmpl-TdH6AzIJTEArEd3CrH7pmnsPrHOELHuo","model":"qwen3.6-27b-code","system_fingerprint":"b9998-c036959df","object":"chat.completion.chunk"}
        try {
            if (json.type != JSONType.null_) {
                // if it isn't null then it must be a string or something is wrong
                auto s = json.str;
                message.reasoning ~= s;
                incrToken(s.length);
            }
        } catch (Exception e) {
            logger.tracef("invalid reasoning structure '%s': %s", json, e.msg);
        }
    }

    void parseLlamaCppError(const(char)[] raw) {
        hasError = true;
        try {
            auto json = parseJSON(raw);
            error.type = json["type"].str;
            error.message = json["message"].str;
            error.codeNr = json["code"].integer;
        } catch (Exception e) {
            logger.tracef("invalid error structure '%s': %s", raw, e.msg);
        }
    }

    void parseOpenAiError(ref JSONValue json) {
        hasError = true;
        try {
            error.type = json["error"]["type"].str;
            error.message = json["error"]["message"].str;
            error.code = getValue(json, (v) => v["error"]["code"].str, null);
        } catch (Exception e) {
            logger.tracef("invalid error structure '%s': %s", json, e.msg);
        }
    }
}

/// Swaps the discovery meta-tool's description in the tools array for the
/// composed one: the UDA base text plus the resolved config's described
/// groups, ordered by activation frequency then name
/// (composeDiscoveryDesc). A no-op when the discovery tool is absent from
/// the array (a toolFilter can exclude it, in which case there is nothing
/// to compose). Undescribed groups are not advertised by design, so there
/// is no empty-description warning here.
private void composeDiscoveryDescription(ref JSONValue[] tools,
        ToolBrokerConfig resolved, BrokerState st) @safe {
    import std.algorithm : countUntil, find;
    import llm.tool_call : getFunctions;
    import llm.tool_call.discovery : composeDiscoveryDesc;

    // The discovery meta-tool's model-facing name comes from the registry
    // (snake axis); a toolFilter can exclude it, in which case there is
    // nothing to compose and the array is left unchanged.
    auto meta = getFunctions().find!(f => f.name == "listToolTags");
    if (meta.empty)
        return;
    auto idx = tools.countUntil!(e => e["function"]["name"].str == meta.front.nameSnakeCase);
    if (idx >= tools.length)
        return; // real guard: the entry is absent from the array

    tools[idx]["function"]["description"] = composeDiscoveryDesc(
            tools[idx]["function"]["description"].str, resolved, st);
}

/// Emits one broker event to the agent's monitor: the
/// envelope (kind + ts + agent) comes from brokerEvent, the sink is
/// agent.monitor. Metrics failures never break the caller: null monitor
/// skipped, exceptions traced only.
private void recordBrokerEvent(Agent agent, string kind, JSONValue[string] fields) @safe {
    if (agent.monitor is null)
        return;
    try {
        agent.monitor.recordEvent(brokerEvent(kind, agent.name, fields));
    } catch (Exception e) {
        logger.tracef("broker metric failed: %s", e.msg);
    }
}

version (unittest) {
    // Builds a minimal LlmConfig for a real Agent with no network, no skills
    // and no RAG. promptDir points at the temp dir holding the one prompt file
    // getPrompt reads (SUMMARY.md); the empty server type resolves to
    // EndpointType.unknown, so the requesters never dial out.
    package LlmConfig makeAgentTestConfig(string promptDir) {
        LlmConfig llmConf;
        llmConf.disableSkills = true;
        llmConf.promptDir = [promptDir.Path];
        llmConf.workArea = promptDir.Path;
        CodeModelConfig codeCfg;
        codeCfg.modelName = "test-code-model";
        codeCfg.contextSize = 8192;
        llmConf.codeModels ~= codeCfg;
        SummaryModelConfig summaryCfg;
        summaryCfg.modelName = "test-summary-model";
        summaryCfg.contextSize = 8192;
        summaryCfg.contextChunkSize = 8192;
        llmConf.summaryModel = summaryCfg;
        return llmConf;
    }

    /// Agent construction eagerly loads every enabled nudge template, so a
    /// test prompt dir needs them alongside SUMMARY.md - each fixture writes
    /// that file the same way. Byte-identical copies: emission is rewired to
    /// the loaded texts, and the guards pin the shipped files.
    public void writeDefaultNudgeFiles(string promptDir) {
        import std.file : write;
        import std.path : buildPath;
        import std.traits : EnumMembers;

        import llm.config : DefaultCompressionNudge, NudgeKind, defaultNudgeFiles;

        const srcDir = "llmfun/config/prompt";
        foreach (kind; EnumMembers!NudgeKind) {
            foreach (file; defaultNudgeFiles(kind, false) ~ defaultNudgeFiles(kind, true)) {
                write(buildPath(promptDir, file), readText(buildPath(srcDir, file)));
            }
        }

        write(buildPath(promptDir, DefaultCompressionNudge),
                readText(buildPath(srcDir, DefaultCompressionNudge)));
    }
}
