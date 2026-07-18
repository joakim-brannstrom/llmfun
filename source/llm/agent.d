module llm.agent;

import core.thread : Thread;
import core.time : dur;
import logger = std.logger;
import std.algorithm;
import std.array;
import std.conv : to;
import std.datetime : Clock, SysTime, Duration;
import std.exception : collectException;
import std.file : readText, exists, read, rename;
import std.format : format, formattedWrite;
import std.json : JSONValue, parseJSON, JSONType;
import std.path : stripExtension, baseName;
import std.range : empty;
import std.regex : Regex, regex;
import std.sumtype : SumType, match;
import std.typecons : Nullable, nullable;

import my.filter : ReFilter;
import my.optional;
import my.path;

import llm.chat;
import llm.config;
import llm.metric.calculator : MetricsCalculator;
import llm.metric.feedback : FeedbackEngine;
import llm.metric.monitor : MetricMonitor, ToolCallEvent;
import llm.query;
import llm.rag.rag : RAG;
import llm.summary_agent;
import llm.tool_call : FunctionCall, Context;
import llm.tool_call.io : FileContext, VisionContext;
import llm.tool_call.memory : MemoryContext;
import llm.tool_call.metrics : MetricsContext;
import llm.tool_call.pipeline : PipelineControlContext;
import llm.tool_call.rag : RAGContext;
import llm.tool_call.sandbox : SandboxContext;
import llm.tool_call.think : ThinkingContext;
import llm.utility : getValue;
import llm.workarea;

public import llm.types : IBasicAgent, IAgent, ProcessResult, IStreamCallback, ServerStat;

class Agent : IBasicAgent {
    string name;
    Chat chat;
    MetricMonitor monitor;
    long contextSize;
    long contextUsed;

    private {
        LlmRequester rq;
        string modelName_;
        AgentContext toolCtx;
        RAG rag;
        SummaryAgent summary;
        MetricsCalculator calculator;
        FeedbackEngine feedbackEngine;
        IStreamCallback streamCallback;
        bool taskDone_;
        string taskDoneMessage_;
        int lastToolCallWarning;
        static immutable WarnEveryNthToolCall = 5;
        static immutable string[] ExcludedTools = ["taskDone"];
        ReFilter toolFilter;
        bool waitingForVisionResponse;
        ServerStat prevStat;
    }

    this(string name, LlmConfig llmConf, MetricMonitor monitor, RAG rag = null) {
        this(name, llmConf, monitor, rag, ReFilter.init);
    }

    this(string name, LlmConfig llmConf, MetricMonitor monitor, RAG rag, ReFilter filter) {
        import llm.tool_call : descAllFunctions, filterToolDescriptions;

        this.name = name;
        this.monitor = monitor;
        this.rag = rag;
        this.toolFilter = filter;
        this.toolCtx = new AgentContext(this, llmConf);

        resetModel(llmConf.activeCodeModel);

        this.summary = SummaryAgent(llmConf.summaryModel);
        this.summary.setSystemPrompt(llmConf.getPrompt(llmConf.summaryModel.prompt));
    }

    ~this() @safe {
    }

    override string id() {
        return name;
    }

    string modelName() const {
        return modelName_;
    }

    long modelContextSize() const {
        return contextSize;
    }

    /// Reset the agent's model to a new configuration.
    /// Does NOT modify chat history or SummaryAgent.
    void resetModel(CodeModelConfig modelConfig) {
        import llm.tool_call : descAllFunctions, filterToolDescriptions;

        if (modelConfig.name == "") {
            throw new Exception("Cannot reset to empty model config");
        }

        auto oldModel = modelName_;

        auto tools = filterToolDescriptions(descAllFunctions(), toolFilter);
        this.rq = LlmRequester(modelConfig.toRequestConfig, tools.nullable);

        auto slot = LlmSlotRequester(modelConfig.toRequestConfig);
        this.contextSize = slot.request(modelConfig.contextSize);

        this.modelName_ = modelConfig.name;

        logger.tracef("Agent model reset: %s -> %s, context: %s", oldModel,
                modelConfig.name, this.contextSize);
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
        toolCtx.pipelineCtx = ctx;
    }

    void addUserQuery(string query) nothrow {
        chat.add(Message(Role.user, userQuery: true, content: query, thinking: null));
    }

    void addKeepReasoning() @safe nothrow {
        chat.add(Message(Role.user, userQuery: false, content: "Please continue", thinking: null));
    }

    void addContinue() @safe nothrow {
        chat.add(Message(Role.user, userQuery: false, content: "You stopped without calling 'taskDone'. Please continue your work, or call 'taskDone' if you're finished.",
                thinking: null));
    }

    ProcessResult process(bool delegate() interrupt) @trusted nothrow {
        import std.functional : toDelegate;

        ProcessResult rval;

        try {
            auto sp = StreamResponse(prevStat);
            auto stream = (const(char)[] chunk) { /*logger.trace(chunk);*/ sp.parse(chunk);
                if (streamCallback !is null) {
                    streamCallback.messageUpdate(sp.message, sp.stat);
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
            prevStat = sp.stat;

            if (sp.hasError) {
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
                }
            } else {
                rval.stat = sp.stat;
                rval.status = parseResponse(sp);
            }

            rval.chat = chat.lastResponses;
            chat.resetResponseIndex;

            if (!rval.chat.empty) {
                rval.hasToolCall = rval.chat[$ - 1].match!((ToolMessage _) => true,
                        (ToolResponse _) => true, (_) => false);
            }
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
            rval.status = ProcessResult.Status.unknownFailure;
        }

        return rval;
    }

    bool needCompression(double threshold = 0.9) {
        return prevStat.context > contextSize * threshold;
    }

    SummaryAgent.CompressResult compress(double threshold = 0.9, bool force = false,
            SummaryAgent.ProgressCallback callback = null) {
        if (prevStat.context < contextSize * threshold && !force)
            return typeof(return)(compressed: true);
        long oldContextSize = prevStat.context;
        auto result = summary.compress(chat, callback, ExcludedTools.dup);
        prevStat.context = result.newContextSize;
        if (force) {
            logger.infof("Forced compression: context %s -> %s tokens (saved %s)",
                    oldContextSize, prevStat.context, oldContextSize - prevStat.context);
        }
        return result;
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
                } else if (!result.hasToolCall) {
                    addContinue;
                    keepRunning = true;
                }
                break;
            case needCompression:
                this.compress(force: true, callback: compressCallback);
                keepRunning = true;
                break;
            case unknownFailure:
                keepRunning = false;
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
        prevStat.context = chat.approxContextSize;
    }

    /// Save chat history to dir / name_history.json
    void saveHistory(Path dir) @trusted nothrow {
        import std.stdio : File;

        try {
            auto historyPath = dir ~ (this.name ~ "_history.json");
            string tempFile = historyPath.toString ~ ".tmp";
            File(tempFile, "w").write(chat.toSaveJson.toPrettyString);
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
            prevStat.context = chat.approxContextSize;
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

            if (!sp.message.isEmpty) {
                chat.add(Message(Role.assistant, userQuery: false, content: sp.message.content,
                        thinking: sp.message.reasoning));
            }
            if (!sp.toolCalls.empty) {
                handleToolCalls(sp.toolCalls);
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

    void handleToolCalls(ref StreamResponse.ToolCall[long] toolCalls) {
        import llm.tool_call : executeFunc;

        JSONValue[] rval;
        foreach (call; toolCalls.byValue) {
            // Check for warnings before processing each tool call
            try {
                if (monitor !is null && lastToolCallWarning >= WarnEveryNthToolCall) {
                    lastToolCallWarning = 0;
                    feedbackEngine.setEvents(monitor.getRecentEvents(100));
                    auto warnings = feedbackEngine.getWarnings();
                    foreach (warning; warnings) {
                        chat.add(Message(Role.system, userQuery: false,
                                content: "warning: " ~ warning, thinking: null));
                    }
                }
            } catch (Exception e) {
                logger.tracef("feedback check failed: %s", e.msg);
            }
            ++lastToolCallWarning;

            const startTime = Clock.currTime;
            bool success;
            string result;
            try {
                auto res = executeFunc(toolCtx, call.name, parseJSON(call.arguments), toolFilter);
                result = res.msg;
                success = res.success;
            } catch (Exception e) {
                result = e.msg.length > 200 ? e.msg[0 .. 200] : e.msg;
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
            chat.add(ToolMessage(JSONValue([call.toJson]), JSONValue.init, sd));
            chat.add(ToolResponse(content: result, toolCallId: call.id, toolName: call.name));
            if (auto image = toolCtx.drainVisionImage) {
                chat.add(VisionMessage(image.query, image.data));
                waitingForVisionResponse = true;
            }
        }
    }
}

struct VisionImage {
    string data; // base64 data URL
    string query;
    bool opCast(T)() if (is(T : bool)) {
        return !data.empty;
    }
}

class AgentContext : Context, FileContext, SandboxContext, RAGContext, MemoryContext,
    ThinkingContext, MetricsContext, PipelineControlContext, VisionContext {
        import llm.vfs : FlatVfs;

        private {
            LlmConfig conf;
            AbsolutePath workArea_;
            RAG rag;
            Agent agent;
            PipelineControlContext pipelineCtx;
            FlatVfs memoryVfs;
            FlatVfs thinkingVfs;

            SysTime nextMetricCalculation;

            VisionImage pendingVisionImage;
        }

        this(Agent agent, LlmConfig conf) {
            this.conf = conf;
            this.workArea_ = conf.workArea.AbsolutePath;
            this.rag = agent.rag;
            this.agent = agent;

            this.memoryVfs = FlatVfs(conf.memoryArea);
            this.thinkingVfs = FlatVfs(conf.thinkingTemplatesDir);
        }

        ~this() {
        }

        VisionImage drainVisionImage() nothrow {
            auto result = pendingVisionImage;
            pendingVisionImage = VisionImage.init;
            return result;
        }

        override bool addVisionImage(AbsolutePath path, string query) nothrow {
            try {
                import std.path : extension;
                import std.string : toLower;
                import std.base64 : Base64;
                import std.file : getSize;

                string mimeType;
                switch (path.toString.extension.toLower) {
                case ".jpg":
                case ".jpeg":
                    mimeType = "image/jpeg";
                    break;
                case ".png":
                    mimeType = "image/png";
                    break;
                case ".bmp":
                    mimeType = "image/bmp";
                    break;
                case ".gif":
                    mimeType = "image/gif";
                    break;
                default:
                    return false;
                }
                immutable maxImageSize = 20 * 1024 * 1024;
                const fileSize = getSize(path.toString);
                if (fileSize > maxImageSize) {
                    return false;
                }

                auto data = read(path);
                string encoded = Base64.encode(cast(const(ubyte)[]) data);
                string dataUrl = "data:" ~ mimeType ~ ";base64," ~ encoded;
                pendingVisionImage = VisionImage(dataUrl, query);
                return true;
            } catch (Exception e) {
                return false;
            }
        }

        override bool isPathInsideWorkArea(AbsolutePath p) {
            import std.algorithm : startsWith;

            logger.tracef("checking if %s is inside %s", p.toString, workArea_.toString);
            return p.toString.startsWith(workArea_.toString);
        }

        override AbsolutePath workArea() {
            return workArea_;
        }

        override ToolLimits getToolLimits() {
            return conf.toolLimits;
        }

        override string getContainerCmd() {
            return conf.containerCmd;
        }

        override RAG getRAG() {
            return rag;
        }

        override RagConfig getRagConfig() {
            return conf.ragConfig;
        }

        override string[] getMemoryFileTopics() {
            try {
                return memoryVfs.getAllFiles.map!(a => a.baseName.stripExtension).array;
            } catch (Exception e) {
                logger.warning("unable to read memory directories: ", e.msg);
            }
            return null;
        }

        override bool saveMemoryFile(string topic, string content) {
            import llm.rag.rag : add, Document, Origin, Offset;

            auto rval = memoryVfs.save(topic ~ ".md", content);
            if (rval) {
                memoryVfs.query(topic ~ ".md").match!((AbsolutePath a) {
                    rag.add(Document(Origin(a), content, Offset.init), conf.ragConfig);
                }, (_) {});
            }
            return rval;
        }

        override Optional!string readMemory(string topic) {
            return memoryVfs.read(topic ~ ".md");
        }

        override bool removeMemory(string topic) {
            import llm.rag.rag : Origin;

            memoryVfs.query(topic ~ ".md").match!((AbsolutePath a) {
                rag.removeSource(Origin(a));
            }, (_) {});

            return memoryVfs.remove(topic ~ ".md");
        }

        override string[] getThinkingTemplates() {
            try {
                return thinkingVfs.getAllFiles.map!(a => a.baseName.stripExtension).array;
            } catch (Exception e) {
                logger.warning("unable to read thinking directories: ", e.msg);
            }
            return null;
        }

        override Optional!string readThinkingTemplate(string name) {
            return thinkingVfs.read(name ~ ".md");
        }

        override ref MetricsCalculator getCalculator() {
            if (Clock.currTime > nextMetricCalculation) {
                agent.calculator.setEvents(agent.monitor.getRecentEvents(10000));
                nextMetricCalculation = Clock.currTime + 10.dur!"seconds";
            }
            return agent.calculator;
        }

        override ToolCallEvent[] getRecentEvents(long count) {
            if (agent.monitor !is null) {
                return agent.monitor.getRecentEvents(count);
            }
            return null;
        }

        override void taskDone(string answer) {
            agent.taskDone(answer);
        }

        override void setPipelineOutput(string output) {
            if (pipelineCtx) {
                pipelineCtx.setPipelineOutput(output);
            } else {
                logger.trace("Pipeline agent produced output but no receiving context set");
            }
        }
    }

    public:

struct StreamResponse {
    import std.range : isOutputRange;
    import std.datetime : Clock;
    import llm.types : ServerStat, StreamMessage;

    alias Message = StreamMessage;

    // TODO: maybe this should be in llm/chat.d instead?
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
            return format!"ToolCall(id:%s name:%s arguments:'%s')"(id, name, arguments);
        }

        string toPrettyString() {
            auto buf = appender!string;

            try {
                formattedWrite(buf, "%s(%s)", name, parseJSON(arguments));
            } catch (Exception e) {
                formattedWrite(buf, "%s(%s)", name, arguments);
            }

            formattedWrite(buf, "%s(%s)", name, arguments);
            return buf[];
        }
    }

    struct ErrorMessage {
        string type;
        string message;
        string code;
        // only llama-server set this
        long codeNr = -1;

        string toString() @safe const {
            return format!"ErrorMessage(type:%s message:%s code:%s codeNr:%s)"(type,
                    message, code, codeNr);
        }
    }

    bool isDone;
    bool hasError;
    Message message;
    ToolCall[long] toolCalls;
    ServerStat stat;
    ErrorMessage error;

    private {
        SysTime start;
        long tokens;
    }

    this(ServerStat prevStat) {
        stat = prevStat;
    }

    string toString() @safe const {
        import std.array : appender;

        auto buf = appender!string;
        toString(buf);
        return buf.data;
    }

    void toString(Writer)(ref Writer w) const if (isOutputRange!(Writer, char)) {
        formattedWrite(w, "StreamResponse(isDone:%s%s %s %s %s)", isDone, hasError
                ? format!"ErrorMessage(%s)"(error) : "", stat, message, toolCalls);
    }

    void parse(const(char)[] response) {
        import std.string : strip;

        if (start == SysTime.init) {
            start = Clock.currTime;
        }

        immutable prefix = "data: ";
        immutable llamaErrorPrefix = "error: ";

        response = response.strip;
        if (response.startsWith(llamaErrorPrefix)) {
            response = response[llamaErrorPrefix.length .. $];
            parseLlamaCppError(response);
            return;
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
                } else if ("content" in delta) {
                    parseMessage(delta);
                } else if (auto j = "reasoning_content" in delta) {
                    parseReasoning(*j);
                } else if (auto reason = "finish_reason" in choice) {
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

    void incrToken() @safe nothrow {
        tokens++;
        stat.context++;
    }

    void parseStat(ref JSONValue json) @safe nothrow {
        try {
            if (auto timings = "timings" in json) {
                stat.predictedPerSecond = getValue(*timings,
                        (v) => v["predicted_per_second"].floating, stat.predictedPerSecond);
                stat.promptPerSecond = getValue(*timings,
                        (v) => v["prompt_per_second"].floating, stat.promptPerSecond);
                stat.context = getValue(*timings, (v) => v["cache_n"].integer, stat.context);
            } else {
                const s = (Clock.currTime - start).total!"seconds";
                if (s > 0 && tokens > 0) {
                    stat.predictedPerSecond = cast(double) tokens / cast(double)(s);
                }
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
                if (auto a = index in toolCalls) {
                    (*a).arguments ~= jcall["function"]["arguments"].str;
                    incrToken;
                } else {
                    toolCalls[index] = ToolCall(id: jcall["id"].str, name: jcall["function"]["name"].str,
                            arguments: jcall["function"]["arguments"].str);
                }
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
                    message.content ~= content.str;
                    incrToken;
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
                message.reasoning ~= json.str;
                incrToken;
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
            error.code = json["error"]["code"].str;
        } catch (Exception e) {
            logger.tracef("invalid error structure '%s': %s", json, e.msg);
        }
    }
}
