/// Handles the 'agent' subcommand: interactive chat loop with TUI integration.
module llm.app_agent;

import logger = std.logger;
import std.algorithm;
import std.array : empty, array;
import std.concurrency;
import std.conv : to;
import std.exception : ifThrown;
import std.file : exists, readText;
import std.format : format;
import std.stdio : writeln, writefln, readln, writef, stdout;
import std.string : strip, startsWith, join, toStringz, split;
import std.sumtype : match;

import llm.app_config : UserConfig, userToLlmConfig, createRag;
import llm.agent;
import llm.chat;
import llm.coder;
import llm.config;
import llm.metric.monitor : MetricMonitor;
import llm.pipeline : prettyPrint;
import llm.plan;
import llm.query;
import llm.tui;
import llm.utility;
import llmfun_tui;

int appMain(UserConfig uconf, UserConfig.AgentChatConfig conf) {
    // TODO: If help text ever needs externalization (config file, i18n),
    //       the function signature should accept a content parameter.
    string printHelp() {
        import std.process : environment;

        if (environment.get("LLMFUN_NO_SPLASH") || !conf.prompt.empty)
            return null;

        string[] s;
        s ~= "llmfun agent mode - type a query and press Tab to start.";
        s ~= " Use /commands for special actions:";
        s ~= "";
        s ~= "   (bare query)       Send a message to the agent";
        s ~= "   /help              Show this help message";
        s ~= "   /quit, /q, /exit   Exit the agent";
        s ~= "   /stop              Stop processing the currently active query";
        s ~= "   /compact           Force compress the chat history";
        s ~= "   /new               Clear history and start a new conversation";
        s ~= "   /model             List available models";
        s ~= "   /model <index>     Select model by index";
        s ~= "   /model <name>      Select model by exact name (case-insensitive)";
        s ~= "   /plan <query>      Run the plan pipeline";
        s ~= "   /code <query>      Run the coder pipeline";
        s ~= "   /debug             Toggle verbose debug output";
        return s.join("\n");
    }

    if (conf.setupDirs)
        makeFileStructure(LlmConfig.init);
    auto llmConf = readConfig(uconf.config, !conf.prompt.empty, uconf.noCwdConfig)
        .userToLlmConfig(conf);
    auto rag = createRag(llmConf);
    scope (exit) {
        rag.destroy;
    }

    immutable agentHistory = llmConf.scratchArea;
    auto monitor = new MetricMonitor(llmConf.scratchArea ~ "monitor.jsonl");
    auto agent = new Agent("main", llmConf, monitor, rag, llmConf.toolFilter.to());
    scope (exit)
        agent.saveHistory(agentHistory);
    const systemPrompt = SystemPromptInit(llmConf.promptToPath(llmConf.agentPrompt)).toString;
    agent.setSystemPrompt(systemPrompt);
    agent.loadHistory(agentHistory);

    const bool oneShotQuery = !conf.prompt.empty;
    Tid uiTid;

    void sendChatMessage(Args...)(string msg, TuiChatMessageType type, Args args) {
        static if (args.length > 0) {
            msg = format(msg, args);
        }
        if (oneShotQuery) {
            writeln(msg);
        } else {
            send(uiTid, UiChatMessage(msg, type));
        }
    }

    void sendChatThinkMessage(Args...)(string msg, string thinking,
            TuiChatMessageType type, Args args) {
        static if (args.length > 0) {
            msg = format(msg, args);
        }
        if (oneShotQuery) {
            writeln(msg);
        } else {
            send(uiTid, UiChatThinkMessage(msg, thinking, type));
        }
    }

    void progressCallback(size_t currentChunk, size_t totalChunks, string status) {
        send(uiTid, UiChatMessage(format!"[assistant]: Compressing... %s/%s : %s"(currentChunk,
                totalChunks, status), TuiChatMessageType_Assistant));
    };

    void doCompress(ref Agent agent, bool force) {
        if (!agent.needCompression && !force)
            return;
        const ctxUsed = agent.contextUsed;
        send(uiTid, UiAgentBusy.init);
        auto res = agent.compress(force: force, callback: &progressCallback);
        send(uiTid, UiChatMessage(compressionResultToString(res.compressed, res.originalLength,
                res.newLength, res.keptXCount, res.keptXTokens, ctxUsed, res.newContextSize),
                TuiChatMessageType_Assistant));
        send(uiTid, UiAgentReady.init);
    }

    void processChatMessage(Chat.MessageT m, bool printUser) {
        m.match!((Message a) {
            if (!a.role.among(Role.user, Role.system) || (printUser && a.role != Role.system)) {
                auto msgType = a.role == Role.user ? TuiChatMessageType_User
                    : TuiChatMessageType_Assistant;
                sendChatThinkMessage("[%s]: %s", a.thinking, msgType, a.role, a.content);
            } else {
                logger.tracef("[%s]: %s", a.role, a.content);
            }
        }, (ToolMessage a) {
            if (!a.hasTool("taskDone")) {
                auto calls = summarizeToolCalls(a.toolCalls, 500);
                sendChatMessage("[tool calls %s]: %(%-s\n%)",
                    TuiChatMessageType_ToolCall, calls.length, calls);
            }
            if (a.isFinalAnswer()) {
                send(uiTid, UiFinalAnswer(a.getFinalAnswer()));
            }
        }, (ToolResponse a) {
            if (!isHiddenToolResponse(a.toolName)) {
                sendChatMessage("[tool result %-s]: %s", TuiChatMessageType_ToolResponse,
                    a.toolName, summarizeToolResponse(a, 500));
            }
        }, (VisionMessage a) {
            sendChatMessage("[user]: %s (with image)", TuiChatMessageType_User, a.content);
        });
    }

    double lastTokensPerSecond = 0.0;
    void processResult(ProcessResult result) {
        foreach (m; result.chat) {
            processChatMessage(m, printUser: false);
        }
        agent.saveHistory(agentHistory);
        logger.trace(result.status != ProcessResult.Status.ok, result);

        try {
            if (auto t = "predicted_per_second" in result.timing)
                lastTokensPerSecond = t.floating;
        } catch (Exception e) {
            logger.trace("Failed to extract predicted_per_second: ", e.msg);
        }
    }

    void setStatusText(bool readyState) {
        auto status = format!"Context: %s/%s tokens | %.1f tok/s | Model: '%s' | %s"(
                agent.contextUsed, agent.contextSize,
                lastTokensPerSecond, llmConf.activeModelName(), readyState ? "Ready" : "Busy");
        send(uiTid, UiStatusText(status));
    }

    enum AgentStatus {
        active,
        terminate,
    }

    bool debugMode = false;

    AgentStatus runAgent(string query) {
        if (query.among("/quit", "/q", "/exit")) {
            return AgentStatus.terminate;
        } else if (query == "/compact") {
            doCompress(agent, force: true);
            return AgentStatus.active;
        } else if (query == "/new") {
            agent.clearHistory;
            send(uiTid, UiClearChat.init);
            return AgentStatus.active;
        } else if (query == "/help") {
            sendChatMessage(printHelp(), TuiChatMessageType_User);
            return AgentStatus.active;
        } else if (query == "/debug") {
            debugMode = !debugMode;
            send(uiTid, UiLogFile(debugMode));
            logger.globalLogLevel = debugMode ? logger.LogLevel.trace : logger.LogLevel.info;
            sendChatMessage("Debug output: %s", TuiChatMessageType_Assistant,
                    debugMode ? "ON" : "OFF");
            return AgentStatus.active;
        } else if (query == "/model" || query.startsWith("/model ")) {
            auto arg = query == "/model" ? "" : query["/model ".length .. $].strip();
            if (arg.empty) {
                auto m = "Available models:";
                foreach (i, model; llmConf.codeModels) {
                    auto activeMarker = (i == cast(size_t) llmConf.activeCodeModelIndex) ? " [active]"
                        : "";
                    m ~= format("  %s  %s%s\n", i, model.name, activeMarker);
                }
                m ~= "Use /model <index> or /model <name> to switch.";
                sendChatMessage(m, TuiChatMessageType_Assistant);
            } else {
                const oldModel = llmConf.activeCodeModel.name;
                // Try to switch model
                bool switched;
                size_t idx = ifThrown(arg.to!long, -1);
                if (idx >= 0) {
                    switched = llmConf.selectModelByIndex(idx);
                    if (!switched) {
                        sendChatMessage("error: Invalid model index '%s'. Valid indices: 0-%s.",
                                TuiChatMessageType_Assistant, arg, llmConf.codeModels.length - 1);
                    }
                } else {
                    auto result = llmConf.selectModelByName(arg);
                    switched = result.empty;
                    if (result.empty)
                        sendChatMessage("failed to switch model: %s",
                                TuiChatMessageType_Assistant, result);
                }
                if (switched) {
                    agent.resetModel(llmConf.activeCodeModel());
                    sendChatMessage("switched to model: %s\nAgent model reset: %s -> %s, context: %s",
                            TuiChatMessageType_Assistant,
                            llmConf.activeModelName(), oldModel,
                            agent.modelName, agent.modelContextSize);
                }
            }
            return AgentStatus.active;
        } else if (query.startsWith("/plan ")) {
            auto q = query["/plan ".length .. $];
            sendChatMessage("[assistant]: Running plan pipeline: %s",
                    TuiChatMessageType_Assistant, q);
            auto result = runPlanPipeline(q, llmConf, rag, monitor, () {
                return isInterruptTriggered;
            }, llmConf.toolFilter.to());
            sendChatMessage(prettyPrint(result), TuiChatMessageType_Assistant);
            return AgentStatus.active;
        } else if (query.startsWith("/code ")) {
            auto q = query["/code ".length .. $];
            sendChatMessage("[assistant]: Running coder pipeline: %s",
                    TuiChatMessageType_Assistant, q);
            auto result = runCoderPipeline(q, llmConf, rag, monitor, () {
                return isInterruptTriggered;
            }, llmConf.toolFilter.to());
            if (result.wasInterrupted) {
                sendChatMessage("[assistant]: Pipeline interrupted by user.",
                        TuiChatMessageType_Assistant);
                return AgentStatus.active;
            }
            sendChatMessage(format!"[assistant]: %s"(prettyPrint(result)),
                    TuiChatMessageType_Assistant);
            return AgentStatus.active;
        } else if (query.empty) {
            return AgentStatus.active;
        }

        agent.addUserQuery(query);
        doCompress(agent, force: false);
        auto result = agent.runToCompletion(&processResult, compressCallback: &progressCallback,
                interrupt: () { return isStopAgentTriggered; });
        return AgentStatus.active;
    }

    if (oneShotQuery) {
        runAgent(conf.prompt);
        return 0;
    }

    uiTid = spawn(&spawnUserInterface, thisTid);
    send(uiTid, UiSetIniFile(llmConf.scratchArea ~ "imgui.ini"));

    foreach (m; agent.chat.getMessages()) {
        processChatMessage(m, printUser: true);
    }
    sendChatMessage(printHelp(), TuiChatMessageType_User);

    if (llmConf.beginConsolidation) {
        import llm.memory;

        logger.infof("Memory consolidation pending at session #%s", llmConf.sessionCount + 1);
        runMemoryConsolidation(llmConf, rag, monitor, (string msg,
                TuiChatMessageType t) => sendChatMessage(msg, t));
    }

    bool running = true;
    do {
        setStatusText(true);
        receive((UiUserQuery a) {
            auto query = a.query.strip;
            if (!query.empty) {
                sendChatMessage(query, TuiChatMessageType_User);
                send(uiTid, UiAgentBusy.init);
                clearStopAgent();
                setStatusText(false);
                final switch (runAgent(query)) {
                case AgentStatus.active:
                    break;
                case AgentStatus.terminate:
                    send(uiTid, UiTerminate.init);
                    break;
                }
                send(uiTid, UiAgentReady.init);
            }
        }, (UiTerminated _) { running = false; });
    }
    while (running);

    return 0;
}
