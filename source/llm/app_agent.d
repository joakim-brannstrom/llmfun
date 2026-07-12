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

import llm.agent;
import llm.app_config : UserConfig, userToLlmConfig, createRag;
import llm.chat;
import llm.coder;
import llm.config;
import llm.memory;
import llm.metric.monitor : MetricMonitor;
import llm.pipeline : prettyPrint;
import llm.plan;
import llm.query;
import llm.rag.rag : RAG;
import llm.tui;
import llm.utility;
import llmfun_tui;

import my.path : Path;

struct AgentApp {
    private {
        LlmConfig _llmConf;
        RAG _rag;
        Path _agentHistory;
        MetricMonitor _monitor;
        Agent _agent;
        string _systemPrompt;
        bool _oneShotQuery;
        Tid _uiTid;
        double _lastTokensPerSecond;
        bool _debugMode;
        UserConfig.AgentChatConfig _conf;

        enum AgentStatus {
            active,
            terminate
        }
    }

    @disable this(this);

    void dispose() {
        if (_uiTid != Tid.init) {
            try {
                send(_uiTid, UiTerminate.init);
            } catch (Exception) {
                // UI thread may have already terminated
            }
            _uiTid = Tid.init;
        }
        if (_rag) {
            _rag.destroy;
            _rag = null;
        }
        if (_agent) {
            _agent.saveHistory(_agentHistory);
            _agent = null;
        }
    }

    // TODO: If help text ever needs externalization (config file, i18n),
    //       the function signature should accept a content parameter.
    private string printHelp(UserConfig.AgentChatConfig conf) {
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

    private void sendChatMessage(Args...)(string msg, TuiChatMessageType type, Args args) {
        static if (args.length > 0) {
            msg = format(msg, args);
        }
        if (_oneShotQuery) {
            writeln(msg);
        } else {
            send(_uiTid, UiChatMessage(msg, type));
        }
    }

    private void sendChatThinkMessage(Args...)(string msg, string thinking,
            TuiChatMessageType type, Args args) {
        static if (args.length > 0) {
            msg = format(msg, args);
        }
        if (_oneShotQuery) {
            writeln(msg);
        } else {
            send(_uiTid, UiChatThinkMessage(msg, thinking, type));
        }
    }

    private void progressCallback(size_t currentChunk, size_t totalChunks, string status) {
        if (!_oneShotQuery) {
            send(_uiTid, UiChatMessage(format!"[assistant]: Compressing... %s/%s : %s"(currentChunk,
                    totalChunks, status), TuiChatMessageType_Assistant));
        }
    }

    private void setStatusText(bool readyState) {
        auto status = format!"Context: %s/%s tokens | %.1f tok/s | Model: '%s' | %s"(
                _agent.contextUsed, _agent.contextSize,
                _lastTokensPerSecond, _llmConf.activeModelName(), readyState ? "Ready" : "Busy");
        send(_uiTid, UiStatusText(status));
    }

    private void doCompress(bool force) {
        if (!_agent.needCompression && !force)
            return;
        const ctxUsed = _agent.contextUsed;
        if (!_oneShotQuery)
            send(_uiTid, UiAgentBusy.init);
        auto res = _agent.compress(force: force, callback: &this.progressCallback);
        if (!_oneShotQuery) {
            send(_uiTid, UiChatMessage(compressionResultToString(res.compressed, res.originalLength,
                    res.newLength, res.keptXCount, res.keptXTokens, ctxUsed, res.newContextSize),
                    TuiChatMessageType_Assistant));
            send(_uiTid, UiAgentReady.init);
        } else {
            writeln(compressionResultToString(res.compressed, res.originalLength,
                    res.newLength, res.keptXCount, res.keptXTokens, ctxUsed, res.newContextSize));
        }
    }

    private void processChatMessage(Chat.MessageT m, bool printUser) {
        m.match!((Message a) {
            if (!a.role.among(Role.user, Role.system) || (printUser && a.role != Role.system)) {
                auto msgType = a.role == Role.user ? TuiChatMessageType_User
                    : TuiChatMessageType_Assistant;
                this.sendChatThinkMessage("[%s]: %s", a.thinking, msgType, a.role, a.content);
            } else {
                logger.tracef("[%s]: %s", a.role, a.content);
            }
        }, (ToolMessage a) {
            if (!a.hasTool("taskDone")) {
                auto calls = summarizeToolCalls(a.toolCalls, 500);
                this.sendChatMessage("[tool calls %s]: %(%-s\n%)",
                    TuiChatMessageType_ToolCall, calls.length, calls);
            }
            if (a.isFinalAnswer()) {
                if (!_oneShotQuery) {
                    send(_uiTid, UiFinalAnswer(a.getFinalAnswer()));
                }
            }
        }, (ToolResponse a) {
            if (!isHiddenToolResponse(a.toolName)) {
                this.sendChatMessage("[tool result %-s]: %s", TuiChatMessageType_ToolResponse,
                    a.toolName, summarizeToolResponse(a, 500));
            }
        }, (VisionMessage a) {
            this.sendChatMessage("[user]: %s (with image)", TuiChatMessageType_User, a.content);
        });
    }

    private void processResult(ProcessResult result) {
        foreach (m; result.chat) {
            this.processChatMessage(m, printUser: false);
        }
        _agent.saveHistory(_agentHistory);
        logger.trace(result.status != ProcessResult.Status.ok, result);

        try {
            if (auto t = "predicted_per_second" in result.timing)
                _lastTokensPerSecond = t.floating;
        } catch (Exception e) {
            logger.trace("Failed to extract predicted_per_second: ", e.msg);
        }
    }

    private AgentStatus runAgent(string query) {
        if (query.among("/quit", "/q", "/exit")) {
            return AgentStatus.terminate;
        } else if (query == "/compact") {
            this.doCompress(true);
            return AgentStatus.active;
        } else if (query == "/new") {
            _agent.clearHistory;
            send(_uiTid, UiClearChat.init);
            return AgentStatus.active;
        } else if (query == "/help") {
            auto helpText = this.printHelp(_conf);
            if (helpText !is null) {
                this.sendChatMessage(helpText, TuiChatMessageType_User);
            }
            return AgentStatus.active;
        } else if (query == "/debug") {
            _debugMode = !_debugMode;
            send(_uiTid, UiLogFile(_debugMode));
            logger.globalLogLevel = _debugMode ? logger.LogLevel.trace : logger.LogLevel.info;
            this.sendChatMessage("Debug output: %s",
                    TuiChatMessageType_Assistant, _debugMode ? "ON" : "OFF");
            return AgentStatus.active;
        } else if (query == "/model" || query.startsWith("/model ")) {
            auto arg = query == "/model" ? "" : query["/model ".length .. $].strip();
            if (arg.empty) {
                auto m = "Available models:";
                foreach (i, model; _llmConf.codeModels) {
                    auto activeMarker = (i == cast(size_t) _llmConf.activeCodeModelIndex) ? " [active]"
                        : "";
                    m ~= format("  %s  %s%s\n", i, model.name, activeMarker);
                }
                m ~= "Use /model <index> or /model <name> to switch.";
                this.sendChatMessage(m, TuiChatMessageType_Assistant);
            } else {
                const oldModel = _llmConf.activeCodeModel.name;
                bool switched;
                size_t idx = ifThrown(arg.to!long, -1);
                if (idx >= 0) {
                    switched = _llmConf.selectModelByIndex(idx);
                    if (!switched) {
                        this.sendChatMessage("error: Invalid model index '%s'. Valid indices: 0-%s.",
                                TuiChatMessageType_Assistant, arg, _llmConf.codeModels.length - 1);
                    }
                } else {
                    auto result = _llmConf.selectModelByName(arg);
                    switched = result.empty;
                    if (!switched)
                        this.sendChatMessage("failed to switch model: %s",
                                TuiChatMessageType_Assistant, result);
                }
                if (switched) {
                    _agent.resetModel(_llmConf.activeCodeModel());
                    this.sendChatMessage("switched to model: %s\nAgent model reset: %s -> %s, context: %s",
                            TuiChatMessageType_Assistant,
                            _llmConf.activeModelName(), oldModel,
                            _agent.modelName, _agent.modelContextSize);
                }
            }
            return AgentStatus.active;
        } else if (query.startsWith("/plan ")) {
            auto q = query["/plan ".length .. $];
            this.sendChatMessage("[assistant]: Running plan pipeline: %s",
                    TuiChatMessageType_Assistant, q);
            auto result = runPlanPipeline(q, _llmConf, _rag, _monitor, () {
                return isInterruptTriggered;
            }, _llmConf.toolFilter.to());
            this.sendChatMessage(prettyPrint(result), TuiChatMessageType_Assistant);
            return AgentStatus.active;
        } else if (query.startsWith("/code ")) {
            auto q = query["/code ".length .. $];
            this.sendChatMessage("[assistant]: Running coder pipeline: %s",
                    TuiChatMessageType_Assistant, q);
            auto result = runCoderPipeline(q, _llmConf, _rag, _monitor, () {
                return isInterruptTriggered;
            }, _llmConf.toolFilter.to());
            if (result.wasInterrupted) {
                this.sendChatMessage("[assistant]: Pipeline interrupted by user.",
                        TuiChatMessageType_Assistant);
                return AgentStatus.active;
            }
            this.sendChatMessage(format!"[assistant]: %s"(prettyPrint(result)),
                    TuiChatMessageType_Assistant);
            return AgentStatus.active;
        } else if (query.empty) {
            return AgentStatus.active;
        } else if (query.startsWith("/")) {
            this.sendChatMessage("[system] Unknown command: '%s'. Type /help for available commands.",
                    TuiChatMessageType_Assistant, query);
            return AgentStatus.active;
        }

        _agent.addUserQuery(query);
        this.doCompress(false);
        auto result = _agent.runToCompletion(&this.processResult,
                compressCallback: &this.progressCallback, interrupt: () {
            return isStopAgentTriggered;
        });
        return AgentStatus.active;
    }

    int run(UserConfig uconf, UserConfig.AgentChatConfig conf) {
        if (conf.setupDirs)
            makeFileStructure(LlmConfig.init);

        _llmConf = readConfig(uconf.config, !conf.prompt.empty, uconf.noCwdConfig).userToLlmConfig(
                conf);
        _rag = createRag(_llmConf);
        _agentHistory = _llmConf.scratchArea;
        _monitor = new MetricMonitor(_llmConf.scratchArea ~ "monitor.jsonl");
        _agent = new Agent("main", _llmConf, _monitor, _rag, _llmConf.toolFilter.to());
        _agent.loadHistory(_agentHistory);
        _systemPrompt = SystemPromptInit(_llmConf.promptToPath(_llmConf.agentPrompt)).toString;
        _agent.setSystemPrompt(_systemPrompt);
        _conf = conf;
        scope (exit)
            this.dispose(); // Ensures cleanup on any exception after setup

        _oneShotQuery = !conf.prompt.empty;

        if (_oneShotQuery) {
            this.runAgent(conf.prompt);
            return 0;
        }

        _uiTid = spawn(&spawnUserInterface, thisTid);
        send(_uiTid, UiSetIniFile(_llmConf.scratchArea ~ "imgui.ini"));

        foreach (m; _agent.chat.getMessages()) {
            this.processChatMessage(m, printUser: true);
        }
        auto helpText = this.printHelp(conf);
        if (helpText !is null) {
            this.sendChatMessage(helpText, TuiChatMessageType_User);
        }

        if (_llmConf.beginConsolidation) {
            logger.infof("Memory consolidation pending at session #%s", _llmConf.sessionCount + 1);
            runMemoryConsolidation(_llmConf, _rag, _monitor, (string msg,
                    TuiChatMessageType t) => this.sendChatMessage(msg, t));
        }

        bool running = true;
        do {
            this.setStatusText(true);
            receive((UiUserQuery a) {
                auto query = a.query.strip;
                if (!query.empty) {
                    this.sendChatMessage(query, TuiChatMessageType_User);
                    send(_uiTid, UiAgentBusy.init);
                    clearStopAgent();
                    this.setStatusText(false);
                    final switch (this.runAgent(query)) {
                    case AgentStatus.active:
                        break;
                    case AgentStatus.terminate:
                        send(_uiTid, UiTerminate.init);
                        break;
                    }
                    send(_uiTid, UiAgentReady.init);
                }
            }, (UiTerminated _) { running = false; });
        }
        while (running);

        return 0;
    }
}

int appMain(UserConfig uconf, UserConfig.AgentChatConfig conf) {
    AgentApp app;
    return app.run(uconf, conf);
}
