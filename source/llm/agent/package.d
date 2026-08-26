/// Core Agent orchestration: `Agent` drives the model request/tool-call loop and
/// `StreamResponse` parses streaming model responses.
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

import llm.chat;
import llm.config;
import llm.metric.feedback : FeedbackEngine;
import llm.metric.monitor : MetricMonitor, ToolCallEvent;
import llm.query : LlmRequester;
import llm.rag.rag : RAG;
import llm.skill : SkillManager, makeSkillManager;
import llm.summary_agent;
import llm.tool_call : FunctionCall, Context;
import llm.tool_call.pipeline : PipelineControlContext;
import llm.utility : getValue;

import llm.environment.config : EnvironmentBackend;

public import llm.types : IBasicAgent, IAgent, ProcessResult, IStreamCallback,
    ServerStat, StreamToolCall;
public import llm.agent.context : AgentContext, VisionImage;

class Agent : IBasicAgent {
    string name;
    Chat chat;
    MetricMonitor monitor;

    private {
        LlmRequester rq;
        string modelName_;
        AgentContext toolCtx;
        RAG rag;
        SummaryAgent summary;
        FeedbackEngine feedbackEngine;
        IStreamCallback streamCallback;
        bool taskDone_;
        string taskDoneMessage_;
        long contextSize_;

        SysTime lastToolCallWarning;
        static immutable ToolCallWarnInterval = 15.dur!"minutes";
        int toolCallWarnCounter = -1;
        static immutable MinToolCallInterval = 50;
        static immutable MaxStrikes = 3;
        int keepReasoningStrikes;
        int continueStrikes;

        ReFilter toolFilter;
        bool waitingForVisionResponse;
        ServerStat prevStat;
    }

    this(string name, LlmConfig llmConf, MetricMonitor monitor, RAG rag = null) {
        this(name, llmConf, makeSkillManager(llmConf), monitor, rag, ReFilter.init);
    }

    this(string name, LlmConfig llmConf, MetricMonitor monitor, RAG rag, ReFilter filter) {
        this(name, llmConf, makeSkillManager(llmConf), monitor, rag, filter);
    }

    this(string name, LlmConfig llmConf, SkillManager mgr, MetricMonitor monitor,
            RAG rag, ReFilter filter) {
        import llm.tool_call : descAllFunctions, filterToolDescriptions;

        this.name = name;
        this.monitor = monitor;
        this.rag = rag;
        this.toolFilter = filter;
        this.toolCtx = new AgentContext(llmConf, rag, monitor);
        toolCtx.setSkillManager(mgr);
        toolCtx.setTaskDoneHandler(&this.taskDone);

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

    /// Reset the agent's model to a new configuration.
    /// Does NOT modify chat history or SummaryAgent.
    void resetModel(CodeModelConfig modelConfig) {
        import llm.tool_call : descAllFunctions, filterToolDescriptions;
        import llm.endpoint : getContextSize;

        if (modelConfig.modelName.empty) {
            throw new Exception("Cannot reset to empty model config");
        }

        auto oldModel = modelName_;

        auto tools = filterToolDescriptions(descAllFunctions(), toolFilter);
        this.rq = LlmRequester(modelConfig.toRequestConfig, tools.nullable);

        this.contextSize_ = modelConfig.getContextSize;

        this.modelName_ = modelConfig.modelName;

        logger.tracef("Agent model reset: %s -> %s, context: %s", oldModel,
                modelConfig.modelName, this.contextSize_);
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

    /// Adds a user query. Chat owns the turn policy (A3): a user query always
    /// opens a new turn inside Chat.add — call sites cannot violate it.
    void addUserQuery(string query) nothrow {
        chat.addUserQuery(query);
    }

    /// Adds a harness control message that continues the current turn
    /// (A3/H1): a user-role nudge with userQuery:false — never opens a turn
    /// and appears in neither the dialogue nor the trace projection. Used by
    /// the pipeline retry loop instead of addUserQuery, which would fragment
    /// one logical turn into N turns and leak harness text into the Facts
    /// projection.
    void addContinueMessage(string msg) @safe nothrow {
        chat.add(Message(Role.user, userQuery: false, content: msg, thinking: null));
    }

    void addKeepReasoning() @safe nothrow {
        keepReasoningStrikes++;

        string msg;
        if (keepReasoningStrikes < MaxStrikes) {
            // STRIKE 1 or 2: Gentle diagnostic (but explicitly forbids plain‑text diagnosis)
            msg = q"(**⚠️ SYSTEM NUDGE:** You stopped generating without calling a tool.

You have two possible states (choose ONLY ONE):
1. BLOCKED – You asked a question or need user input.
2. COMPLETE – You have fully solved the user's request.

EXECUTION RULES:
- If BLOCKED → Call taskDone immediately with your question.
- If COMPLETE → Call taskDone immediately with your final answer.

IMPORTANT: Do NOT describe your state in plain text. Your very next output MUST be a tool call (taskDone) or your next reasoning step/tool call. If you need to continue working, just output the next tool call right now.)";
        } else {
            // STRIKE 3+: Strict JSON force (breaks the loop)
            msg = q"(**⚠️ SYSTEM OVERRIDE - FINAL WARNING:** You have repeatedly stopped without calling a tool.

Your state is BLOCKED. Do not re‑diagnose.

YOUR ENTIRE NEXT RESPONSE MUST BE EXACTLY THIS JSON (output ONLY this, no extra text):
{"name": "taskDone", "arguments": {"answer": "<Put your exact question here>"}}

ABSOLUTELY NO OTHER TEXT. Do not explain, apologise, or write anything outside this JSON. Output ONLY the tool call.)";
        }

        chat.add(Message(role: Role.user, userQuery: false, content: msg, thinking: null));
    }

    void addContinue() @safe nothrow {
        continueStrikes++;

        string msg;

        if (continueStrikes < MaxStrikes) {
            msg = q"([SYSTEM RECOVERY — NOT USER INPUT]
You stopped without calling a tool.

This is a harness control message. Do not treat it as a user reply.
Do not assume any pending question was answered.

If you still have unfinished work or need another tool call, continue now.

Otherwise call taskDone:
- If you need user input: answer = the exact question you just asked.
- If you are finished: answer = your final answer.

Do not include anything else in taskDone.answer.)";
        } else {
            msg = q"([FINAL RECOVERY - NOT USER INPUT]
Call taskDone now.

answer = your final answer, or the exact question you just asked if you need user input.

Output only the taskDone tool call. No other text.)";
        }

        chat.add(Message(Role.user, userQuery: false, thinking: null, content: msg));
    }

    ProcessResult process(bool delegate() interrupt) @trusted nothrow {
        import std.functional : toDelegate;
        import llm.query : HttpResult, HttpError, canRetry;

        ProcessResult rval;

        ServerStat useOrApproxStatistic(ServerStat stat) {
            if (stat.startContext == prevStat.startContext) {
                // the model never finished with a timing/usage message so total context need to be estimated
                stat.startContext = chat.approxContextSize;
            }
            return stat;
        }

        try {
            logger.tracef("agent %s: processing turn %s (%s messages)", name,
                    chat.currentTurnId(), chat.length);
            auto sp = StreamResponse(prevStat);
            auto stream = (const(char)[] chunk) { /*logger.trace(chunk);*/ sp.parse(chunk);
                if (streamCallback !is null) {
                    streamCallback.messageUpdate(sp.message,
                            sp.toolCalls.byValue.map!(a => a.toStream).array, sp.stat);
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

            auto res = rq.request(chat);

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

                    // Recovery: the server rejected the request and the
                    // history may cotanin invalid UTF-8 (typically binary
                    // command output). Sanitize the history in place (messages
                    // preserved).
                    if (chat.sanitizeHistory > 0) {
                        logger.warning("Sanitized chat history (invalid UTF-8)");
                    }
                }
            } else {
                rval.stat = useOrApproxStatistic(sp.stat);
                rval.status = parseResponse(sp); // adds messages to chat
                rval.chat = chat.lastResponses;
                chat.resetResponseIndex;

                if (!rval.chat.empty) {
                    rval.hasToolCall = rval.chat[$ - 1].match!((ToolMessage _) => true,
                            (ToolResponse _) => true, (_) => false);
                }
            }

            prevStat = useOrApproxStatistic(sp.stat).newTurn;
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
            rval.status = ProcessResult.Status.unknownFailure;
        }

        return rval;
    }

    bool needCompression(double threshold = 0.9) {
        return prevStat.context > contextSize_ * threshold;
    }

    SummaryAgent.CompressResult compress(double threshold = 0.9, bool force = false,
            SummaryAgent.ProgressCallback callback = null) {
        if (prevStat.context < contextSize_ * threshold && !force)
            return typeof(return)(compressed: true);
        long oldContextSize = prevStat.context;
        auto result = summary.compress(chat, callback, null);
        prevStat.startContext = result.newContextSize;
        if (force) {
            logger.infof("Forced compression: context %s -> %s tokens (saved %s)",
                    oldContextSize, prevStat.context, oldContextSize - prevStat.context);
        }
        return result;
    }

    /// Register a listener for compression checkpoints (A6/G2). The seam is
    /// multicast: Phase 1 and Phase 2 subscribe independently. A listener
    /// fires exactly once per compression that actually evicts verbatim
    /// content, and a throwing listener never breaks compression.
    void addCompressionCheckpointListener(SummaryAgent.CheckpointListener listener) {
        summary.addCheckpointListener(listener);
    }

    /// Set the session id stamped into each checkpoint event (A6/G1). Call
    /// sites that know the owning session (doCompress: activeSession.id) set
    /// it before compressing; pool callbacks set their own or leave "".
    /// Phase 1 refuses to index events with an empty sessionId.
    void setCompressionCheckpointSessionId(string sessionId) {
        summary.setCheckpointSessionId(sessionId);
    }

    /// Run the agent until completion (no more tool calls, no more thinking needed)
    override ProcessResult runToCompletion(void delegate(ProcessResult) step = null,
            SummaryAgent.ProgressCallback compressCallback = null, bool delegate() interrupt = null) @trusted {
        // make sure there is room in the context before doing anything
        this.compress(callback: compressCallback);

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
                    addContinue;
                    keepRunning = true;
                } else {
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
                this.addKeepReasoning();
                keepRunning = true;
                break;
            }

            // Safety check: detect stuck loops
            if (result.status == lastStatus && result.status != ProcessResult.Status.ok) {
                consecutiveSameStatus++;
                if (consecutiveSameStatus > MaxConsecutiveSameStatus) {
                    logger.warningf("Agent stuck in loop with status %s after %s iterations, breaking",
                            result.status, consecutiveSameStatus);
                    result.status = ProcessResult.Status.unknownFailure;
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
                    keepRunning = false;
                }
            } else {
                consecutiveNoToolCallOk = 0;
            }

            // compress at the end because it could be filled with junk
            this.compress(callback: compressCallback);
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

    /// Sync prevStat.context from current chat context size.
    /// Called after loading chat history so the agent knows the starting context.
    void syncContextFromChat() @safe {
        prevStat = ServerStat(startContext: chat.approxContextSize);
    }

    /// Save chat history to dir / name_history.json
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

    /// Load chat history from dir / name_history.json
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
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
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

    ProcessResult.Status parseResponse(ref StreamResponse sp) @trusted nothrow {
        try {
            logger.trace(sp);

            if (!sp.toolCalls.empty) {
                handleToolCalls(sp.message.reasoning, sp.toolCalls);
            } else if (!sp.message.isEmpty) {
                chat.add(Message(Role.assistant, userQuery: false, content: sp.message.content,
                        thinking: sp.message.reasoning));
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

    void handleToolCalls(string thinking, ref StreamResponse.ToolCall[long] toolCalls) {
        import llm.tool_call : executeFunc;
        import llm.utility : sanitizeUtf8;

        foreach (call; toolCalls.byValue) {
            try {
                if (monitor !is null && (Clock.currTime > lastToolCallWarning
                        && toolCallWarnCounter > MinToolCallInterval || toolCallWarnCounter == -1)) {
                    toolCallWarnCounter = 0;
                    lastToolCallWarning = Clock.currTime + ToolCallWarnInterval;

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
                auto res = executeFunc(toolCtx, call.name, parseJSON(call.arguments), toolFilter);
                result = res.msg.sanitizeUtf8;
                success = res.success;
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
            if (call.name == "taskDone" && taskDone_ && !taskDoneMessage_.empty) {
                sd["taskDoneAnswer"] = JSONValue(taskDoneMessage_);
            }
            chat.add(ToolMessage(thinking, JSONValue([call.toJson]), JSONValue.init, sd));
            chat.add(ToolResponse(content: result, toolCallId: call.id,
                    toolName: call.name, success: success));
            if (auto image = toolCtx.drainVisionImage) {
                chat.add(VisionMessage(image.query, image.data));
                waitingForVisionResponse = true;
            }
        }
    }
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
            // "correct" itself when the LLM go between generating a lof of
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

// --- Task 6 tests: call-site integration (real Agent, no LLM calls) ---

version (unittest) {
    /// Builds a minimal LlmConfig for a real Agent with no network, no skills
    /// and no RAG. promptDir points at the temp dir holding the one prompt
    /// file getPrompt reads (SUMMARY.md); the empty server type resolves to
    /// EndpointType.unknown, so the requesters never dial out.
    private LlmConfig makeAgentTestConfig(string promptDir) {
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

    /// Test hygiene: remove the temp prompt dir; failures only log.
    private void cleanupAgentTestDir(string dir) {
        import std.exception : collectException;
        import std.file : rmdirRecurse;

        try {
            if (dir.exists)
                rmdirRecurse(dir);
        } catch (Exception e) {
            logger.tracef("agent turn-id test cleanup failed: %s", e.msg).collectException;
        }
    }
}

/// Integration: a real Agent (no LLM calls) delegates addUserQuery to the
/// Chat, which owns the turn policy — §4.5 matrix agent:157: the system
/// prompt stamps 0, the first query opens turn 1, harness nudges (incl.
/// addContinueMessage, the pipeline retry path) continue the current turn,
/// and the next query opens turn 2 (A3, H1, Task 6).
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;

    // stdTime (100ns resolution) keeps two runs in the same second from
    // colliding on the temp dir.
    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_turnid_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");

    auto llmConf = makeAgentTestConfig(tmpDir);
    auto agent = new Agent("integration", llmConf, null, null);

    agent.setSystemPrompt("sys");
    agent.addUserQuery("first question"); // opens turn 1
    agent.addContinue(); // nudge continues turn 1
    agent.addUserQuery("second question"); // opens turn 2
    agent.addKeepReasoning(); // nudge continues turn 2
    agent.addContinueMessage("You stopped without calling 'pipelineOutput'."); // retry nudge continues turn 2

    assert(agent.chat.nextTurnId() == 2);
    auto msgs = agent.chat.getMessages;
    assert(msgs.length == 6);
    assert(turnIdOf(msgs[0]) == 0, "system prompt belongs to no turn");
    assert(turnIdOf(msgs[1]) == 1);
    assert(turnIdOf(msgs[2]) == 1);
    assert(turnIdOf(msgs[3]) == 2);
    assert(turnIdOf(msgs[4]) == 2);
    assert(turnIdOf(msgs[5]) == 2, "retry nudge continues turn 2, never opens one");

    // H1: the retry nudge is harness traffic — it appears in neither
    // projection and did not fragment the turn sequence.
    auto dialogue = agent.chat.getDialogueHistory();
    assert(dialogue.length == 2, "only the two real user queries are dialogue");
    assert(turnIdOf(dialogue[0]) == 1 && turnIdOf(dialogue[1]) == 2);
    assert(agent.chat.getReasoningTrace().length == 0, "harness nudges are not trace");
}
