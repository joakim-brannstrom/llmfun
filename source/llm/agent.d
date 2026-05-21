module llm.agent;

import core.thread : Thread;
import core.time : dur;
import logger = std.logger;
import std.algorithm;
import std.array;
import std.conv : to;
import std.datetime : Clock, SysTime, Duration;
import std.exception : collectException;
import std.file : readText, exists, read;
import std.format : format;
import std.json : JSONValue, parseJSON;
import std.range;
import std.regex : Regex, regex;
import std.sumtype : SumType, match;
import std.typecons : Nullable, nullable;
import my.path;
import my.filter : ReFilter;

import llm.chat;
import llm.config;
import llm.metric.calculator : MetricsCalculator;
import llm.metric.feedback : FeedbackEngine;
import llm.metric.monitor : MetricMonitor, ToolCallEvent;
import llm.query;
import llm.rag.rag : RAG;
import llm.summary_agent;
import llm.tool_call : FunctionCall, Context;
import llm.tool_call.io : FileContext, RAGContext, VisionContext, ApiVisionContext;
import llm.tool_call.memory : MemoryContext;
import llm.tool_call.metrics : MetricsContext;
import llm.tool_call.pipeline : PipelineControlContext;
import llm.tool_call.sandbox : SandboxContext;
import llm.tool_call.think : ThinkingContext;
import llm.utility : getValue;
import llm.workarea;

public import llm.types : IBasicAgent, IAgent, ProcessResult;

class Agent : IBasicAgent {
    string name;
    Chat chat;
    MetricMonitor monitor;
    long contextSize;
    long contextUsed;

    private {
        LlmRequester rq;
        AgentContext toolCtx;
        RAG rag;
        SummaryAgent summary;
        MetricsCalculator calculator;
        FeedbackEngine feedbackEngine;
        bool taskDone_;
        int lastToolCallWarning;
        immutable WarnEveryNthToolCall = 5;
        ReFilter toolFilter;
    }

    this(string name, LlmConfig llmConf, MetricMonitor monitor, RAG rag = null) {
        this(name, llmConf, monitor, rag, ReFilter.init);
    }

    this(string name, LlmConfig llmConf, MetricMonitor monitor, RAG rag, ReFilter filter) {
        import llm.tool_call : descAllFunctions, filterToolDescriptions;
        import llm.utility : SystemPromptInit;

        this.name = name;
        this.monitor = monitor;
        this.rag = rag;
        this.toolFilter = filter;
        this.toolCtx = new AgentContext(this, llmConf);
        auto tools = filterToolDescriptions(descAllFunctions(), toolFilter);
        this.rq = LlmRequester(llmConf.codeModel.toRequestConfig, tools.nullable);
        this.contextSize = llmConf.codeModel.contextSize;

        this.summary = SummaryAgent(llmConf.summaryModel);
        this.summary.setSystemPrompt(SystemPromptInit(
                llmConf.promptToPath(llmConf.summaryModel.prompt)).toString);

        auto slot = LlmSlotRequester(llmConf.codeModel.server.toSlotUrl,
                llmConf.codeModel.server.apiKey.empty
                ? getEnvApiKey() : llmConf.codeModel.server.apiKey);
        this.contextSize = slot.request(llmConf.codeModel.contextSize);
    }

    ~this() @safe {
    }

    override string id() {
        return name;
    }

    void setSystemPrompt(string x) {
        chat.setSystemPrompt(x);
    }

    void setPipelineContext(PipelineControlContext ctx) @trusted {
        toolCtx.pipelineCtx = ctx;
    }

    void addUserQuery(string query) nothrow {
        if (auto image = toolCtx.drainVisionImage()) {
            chat.add(VisionMessage(query, image));
        } else {
            chat.add(Message(Role.user, query));
        }
    }

    void addKeepReasoning() @safe nothrow {
        chat.add(Message(Role.user, "Please continue"));
    }

    void addContinue() @safe nothrow {
        chat.add(Message(Role.user,
                "You stopped without calling 'taskDone'. Please continue your work, or call 'taskDone' if you're finished."));
    }

    ProcessResult process() @trusted nothrow {
        ProcessResult rval;

        try {
            auto res = rq.request(chat);
            res.match!((JSONValue j) {
                if (!checkResponse(j)) {
                    logger.warning("Bad response: ", j.toPrettyString);
                    return;
                }
                rval.status = parseResponse(j);
                if (auto a = "timing" in j)
                    rval.timing = *a;
                if (auto a = "usage" in j) {
                    rval.usage = *a;
                    contextUsed = (*a)["total_tokens"].integer;
                }
            }, (LlamaRequestError e) {
                if (e.code == 400 && (e.response.canFind("exceed_context_size_error")
                    || e.response.canFind("exceeds the available context size"))) {
                    // Try to extract token counts from the error response
                    string detail = e.response;
                    try {
                        auto json = parseJSON(e.response);
                        if (auto err = "error" in json) {
                            long promptTokens = 0, ctxSize = 0;
                            if (auto n = "n_prompt_tokens" in *err)
                                promptTokens = n.integer;
                            if (auto n = "n_ctx" in *err)
                                ctxSize = n.integer;
                            if (promptTokens || ctxSize) {
                                detail = format!(
                                    "Context overflow: %s tokens used, %s tokens available")(promptTokens,
                                    ctxSize);
                                rval.status = ProcessResult.Status.needCompression;
                            }
                        }
                    } catch (Exception a) {
                        logger.warning("Context overflow detected: ", a.msg);
                    }
                    logger.warning("Context overflow detected: ", detail);
                    rval.status = ProcessResult.Status.needCompression;
                } else {
                    logger.trace(e);
                }
            });
            rval.chat = chat.lastResponses;
            chat.resetResponseIndex;

            if (!rval.chat.empty) {
                rval.chat[$ - 1].match!((Message a) {}, (ToolMessage a) {
                    rval.hasToolCall = true;
                }, (ToolResponse a) { rval.hasToolCall = true; }, (VisionMessage a) {
                });
            }
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
            rval.status = ProcessResult.Status.unknownFailure;
        }

        return rval;
    }

    bool needCompression(double threshold = 0.9) {
        return contextUsed > contextSize * threshold;
    }

    SummaryAgent.CompressResult compress(double threshold = 0.9, bool force = false,
            SummaryAgent.ProgressCallback callback = null) {
        if (contextUsed < contextSize * threshold && !force)
            return typeof(return)(compressed: true);
        long oldContextSize = contextUsed;
        auto result = summary.compress(chat, callback);
        contextUsed = result.newContextSize;
        if (force) {
            logger.infof("Forced compression: context %ld -> %ld tokens (saved %ld)",
                    oldContextSize, contextUsed, oldContextSize - contextUsed);
        }
        return result;
    }

    /// Run the agent until completion (no more tool calls, no more thinking needed)
    override ProcessResult runToCompletion(void delegate(ProcessResult) step = null,
            SummaryAgent.ProgressCallback compressCallback = null, bool delegate() interrupt = null) @trusted {
        bool isLastResponseQuestion() {
            import std.string : strip, endsWith;

            foreach (msg; chat.lastResponse) {
                return msg.match!((Message a) => a.content,
                        (ToolMessage a) => string.init, (ToolResponse a) => string.init,
                        (VisionMessage a) => a.content).strip.endsWith("?");
            }
            return false;
        };
        // make sure there is room in the context before doing anything
        this.compress(callback: compressCallback);

        taskDone_ = false;
        ProcessResult result;
        bool keepRunning;
        ProcessResult.Status lastStatus = ProcessResult.Status.unknownFailure;
        size_t consecutiveSameStatus;
        immutable MaxConsecutiveSameStatus = 3;
        do {
            result = this.process();
            if (step)
                step(result);
            if (taskDone_ || isLastResponseQuestion || (interrupt && interrupt()))
                break;
            keepRunning = result.hasToolCall;

            final switch (result.status) with (ProcessResult.Status) {
            case ok:
                if (!result.hasToolCall) {
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
        contextUsed = chat.approxContextSize;
    }

    /// Save chat history to dir / name_history.json
    void saveHistory(Path dir) @trusted nothrow {
        import std.stdio : File;

        try {
            auto historyPath = dir ~ (this.name ~ "_history.json");
            File(historyPath.toString, "w").write(chat.toJson.toPrettyString);
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
            contextUsed = chat.approxContextSize;
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
    }

private:

    void taskDone() @safe {
        this.taskDone_ = true;
    }

    ProcessResult.Status parseResponse(JSONValue resp) @trusted nothrow {
        try {
            logger.trace(resp.toPrettyString);
            foreach (choice; resp["choices"].array) {
                try {
                    auto msg = choice["message"];
                    string content = getValue(msg, (v) => msg["content"].str, "");
                    if (!content.empty)
                        chat.add(Message(Role.assistant, content));
                    if (auto calls = getValue(msg, (v) => v["tool_calls"].array, null)) {
                        try {
                            handleToolCalls(calls);
                        } catch (Exception e) {
                        }
                    }
                    if (auto reason = getValue(choice, (v) => v["finish_reason"].str, null)) {
                        if (reason == "length")
                            return ProcessResult.Status.needCompression;
                        if (reason == "stop" && content.empty)
                            return ProcessResult.Status.needMoreThinking;
                    }
                } catch (Exception e) {
                    logger.trace(e.msg);
                }
            }
            return ProcessResult.Status.ok;
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
        return ProcessResult.Status.unknownFailure;
    }

    void handleToolCalls(JSONValue[] calls) {
        import llm.tool_call : executeFunc;

        JSONValue[] rval;
        foreach (call; calls) {
            // Check for warnings before processing each tool call
            try {
                if (monitor !is null && lastToolCallWarning >= WarnEveryNthToolCall) {
                    lastToolCallWarning = 0;
                    feedbackEngine.setEvents(monitor.getRecentEvents(100));
                    auto warnings = feedbackEngine.getWarnings();
                    foreach (warning; warnings) {
                        chat.add(Message(Role.system, "warning: " ~ warning));
                    }
                }
            } catch (Exception e) {
                logger.tracef("feedback check failed: %s", e.msg);
            }
            ++lastToolCallWarning;

            const toolName = call["function"]["name"].str;
            const args = call["function"]["arguments"].str;
            immutable startTime = Clock.currTime;
            bool success;
            string result;
            try {
                auto res = executeFunc(toolCtx, toolName, parseJSON(args), toolFilter);
                result = res.msg;
                success = res.success;
            } catch (Exception e) {
                result = e.msg.length > 200 ? e.msg[0 .. 200] : e.msg;
            }
            immutable responseTimeMs = (Clock.currTime - startTime).total!"msecs";
            try {
                if (monitor !is null) {
                    monitor.record(this.name, toolName, args, result, success, responseTimeMs);
                }
            } catch (Exception e) {
                logger.tracef("monitor record failed: %s", e.msg);
            }
            chat.add(ToolMessage(JSONValue([call])));
            chat.add(ToolResponse(content: result, toolCallId: call["id"].str, toolName: toolName));
        }
    }
}

private bool checkResponse(JSONValue j) @trusted {
    import std.json : JSONType;

    immutable key = "choices";
    if (j.type != JSONType.object)
        return false;
    if (key !in j.object)
        return false;
    if (j[key].type != JSONType.array)
        return false;
    if (j[key].array.empty)
        return false;
    return true;
}

class AgentContext : Context, FileContext, SandboxContext, RAGContext, MemoryContext,
    ThinkingContext, MetricsContext, PipelineControlContext, ApiVisionContext {
        private {
            LlmConfig conf;
            AbsolutePath workArea_;
            RAG rag;
            Agent agent;
            PipelineControlContext pipelineCtx;

            SysTime nextMetricCalculation;

            // TODO: add a specific type for this instead of string.
            string pendingVisionImage; // base64 data URL
        }

        this(Agent agent, LlmConfig conf) {
            this.conf = conf;
            this.workArea_ = conf.workArea.AbsolutePath;
            this.rag = agent.rag;
            this.agent = agent;
        }

        ~this() {
        }

        override string drainVisionImage() nothrow {
            auto result = pendingVisionImage;
            pendingVisionImage = null;
            return result;
        }

        override bool addVisionImage(AbsolutePath path) nothrow {
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
                pendingVisionImage = dataUrl;
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

        override string getContainerCmd() {
            return conf.containerCmd;
        }

        override RAG getRAG() {
            return rag;
        }

        override string[] getMemoryFileTopics() {
            import std.file : dirEntries, SpanMode;
            import std.path : stripExtension, baseName;

            try {
                return dirEntries(conf.memoryArea, SpanMode.shallow).map!(
                        a => a.name.baseName.stripExtension).array;
            } catch (Exception e) {
                logger.warning("unable to read file area for memory topics: ", e.msg);
            }
            return null;
        }

        override Path getMemoryFile(string topic) {
            return conf.memoryArea ~ (topic ~ ".md");
        }

        override Path getThinkingTemplatesDir() {
            return conf.thinkingTemplatesDir;
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

        override void taskDone() {
            agent.taskDone;
        }

        override void setPipelineOutput(string output) {
            if (pipelineCtx) {
                pipelineCtx.setPipelineOutput(output);
            } else {
                logger.trace("Pipeline agent produced output but no receiving context set");
            }
        }
    }
