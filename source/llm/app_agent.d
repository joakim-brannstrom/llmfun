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
import llm.types : ServerStat, StreamMessage;
import llmfun_tui;

import my.path : Path;

struct AgentApp {
    private {
        LlmConfig llmConf;
        RAG rag;
        Path agentHistory;
        MetricMonitor monitor;
        Agent agent_;
        bool oneShotQuery;
        Tid uiTid;
        ServerStat lastServerStat;
        bool debugMode;
        UserConfig.AgentChatConfig conf_;
        UiMessenger uiMsg;

        enum AgentStatus {
            active,
            terminate
        }
    }

    @disable this(this);

    this(UserConfig.AgentChatConfig conf) {
        conf_ = conf;
    }

    void dispose() {
        if (uiTid != Tid.init) {
            try {
                send(uiTid, UiTerminate.init);
            } catch (Exception) {
                // UI thread may have already terminated
            }
            uiTid = Tid.init;
        }
        if (rag) {
            rag.destroy;
            rag = null;
        }
        if (agent_) {
            agent_.saveHistory(agentHistory);
            agent_ = null;
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
        if (oneShotQuery) {
            writeln(msg);
        } else {
            send(uiTid, UiChatMessage(msg, type));
        }
    }

    private void sendChatThinkMessage(Args...)(string msg, string thinking,
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

    private void progressCallback(size_t currentChunk, size_t totalChunks, string status) {
        if (!oneShotQuery) {
            send(uiTid, UiChatMessage(format!"[assistant]: Compressing... %s/%s : %s"(currentChunk,
                    totalChunks, status), TuiChatMessageType_Assistant));
        }
    }

    private void setStatusText(bool readyState) {
        send(uiTid, UiStatusText(formatStatusText(readyState,
                agent_.contextSize, lastServerStat, llmConf.activeModelName())));
    }

    private void doCompress(bool force) {
        if (!agent_.needCompression && !force)
            return;
        const ctxUsed = agent_.contextUsed;
        uiMsg.busy;
        auto res = agent_.compress(force: force, callback: &this.progressCallback);
        if (!oneShotQuery) {
            send(uiTid, UiChatMessage(compressionResultToString(res.compressed, res.originalLength,
                    res.newLength, res.keptXCount, res.keptXTokens, ctxUsed, res.newContextSize),
                    TuiChatMessageType_Assistant));
        } else {
            writeln(compressionResultToString(res.compressed, res.originalLength,
                    res.newLength, res.keptXCount, res.keptXTokens, ctxUsed, res.newContextSize));
        }
        uiMsg.ready;
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
                if (!oneShotQuery) {
                    send(uiTid, UiFinalAnswer(a.getFinalAnswer()));
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
        agent_.saveHistory(agentHistory);
        logger.trace(result.status != ProcessResult.Status.ok, result);
        lastServerStat = result.stat;
    }

    private AgentStatus runAgent(string query) {
        if (query.among("/quit", "/q", "/exit")) {
            return AgentStatus.terminate;
        } else if (query == "/compact") {
            this.doCompress(true);
            return AgentStatus.active;
        } else if (query == "/new") {
            agent_.clearHistory;
            send(uiTid, UiClearChat.init);
            return AgentStatus.active;
        } else if (query == "/help") {
            auto helpText = this.printHelp(conf_);
            if (helpText !is null) {
                this.sendChatMessage(helpText, TuiChatMessageType_User);
            }
            return AgentStatus.active;
        } else if (query == "/debug") {
            debugMode = !debugMode;
            send(uiTid, UiLogFile(debugMode));
            logger.globalLogLevel = debugMode ? logger.LogLevel.trace : logger.LogLevel.info;
            this.sendChatMessage("Debug output: %s",
                    TuiChatMessageType_Assistant, debugMode ? "ON" : "OFF");
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
                this.sendChatMessage(m, TuiChatMessageType_Assistant);
            } else {
                const oldModel = llmConf.activeCodeModel.name;
                bool switched;
                size_t idx = ifThrown(arg.to!long, -1);
                if (idx >= 0) {
                    switched = llmConf.selectModelByIndex(idx);
                    if (!switched) {
                        this.sendChatMessage("error: Invalid model index '%s'. Valid indices: 0-%s.",
                                TuiChatMessageType_Assistant, arg, llmConf.codeModels.length - 1);
                    }
                } else {
                    auto result = llmConf.selectModelByName(arg);
                    switched = result.empty;
                    if (!switched)
                        this.sendChatMessage("failed to switch model: %s",
                                TuiChatMessageType_Assistant, result);
                }
                if (switched) {
                    agent_.resetModel(llmConf.activeCodeModel());
                    agent_.setStreamUpdate(new StreamMessageUpdater(uiTid,
                            agent_.contextSize, llmConf.activeModelName));
                    this.sendChatMessage("switched to model: %s\nAgent model reset: %s -> %s, context: %s",
                            TuiChatMessageType_Assistant,
                            llmConf.activeModelName(), oldModel,
                            agent_.modelName, agent_.modelContextSize);
                }
            }
            return AgentStatus.active;
        } else if (query.startsWith("/plan ")) {
            auto q = query["/plan ".length .. $];
            this.sendChatMessage("[assistant]: Running plan pipeline: %s",
                    TuiChatMessageType_Assistant, q);
            auto result = runPlanPipeline(q, llmConf, rag, monitor, () {
                return isInterruptTriggered;
            }, llmConf.toolFilter.to());
            this.sendChatMessage(prettyPrint(result), TuiChatMessageType_Assistant);
            return AgentStatus.active;
        } else if (query.startsWith("/code ")) {
            auto q = query["/code ".length .. $];
            this.sendChatMessage("[assistant]: Running coder pipeline: %s",
                    TuiChatMessageType_Assistant, q);
            auto result = runCoderPipeline(q, llmConf, rag, monitor, () {
                return isInterruptTriggered;
            }, llmConf.toolFilter.to());
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

        agent_.addUserQuery(query);
        this.doCompress(false);
        auto result = agent_.runToCompletion(&this.processResult,
                compressCallback: &this.progressCallback, interrupt: () {
            return isStopAgentTriggered;
        });
        return AgentStatus.active;
    }

    private void updateRagMemory() {
        import llm.vfs : FlatVfs;
        import std.path : baseName, stripExtension, dirName;
        import llm.rag.rag : add, Document, Origin, Offset, Topic;
        import my.set;
        import my.path : AbsolutePath;

        // do not slowdown startup if the user only have an in-memory because
        // then they are indexed every time the user start
        if (rag is null || rag.isPrimaryInMemory)
            return;

        auto vfs = FlatVfs(llmConf.memoryArea);
        Set!string topics;
        foreach (name; vfs.getAllFiles) {
            try {
                vfs.read(name.baseName).match!((string content) {
                    auto res = rag.add(Document(Origin(cast(Path) name),
                        content, Offset.init), llmConf.ragConfig);
                    logger.infof(res.length != 0, "Add memory '%s' to RAG", name);
                    topics.add(name);
                }, (_) {});
            } catch (Exception e) {
                logger.trace(e.msg);
            }
        }
        logger.trace(topics);

        foreach (src; rag.db.getSources) {
            src.origin.match!((Path a) {
                if (a !in topics && vfs.isRoot(a.dirName.AbsolutePath)) {
                    logger.tracef("Removing memory '%s'", a);
                    rag.db.removeSource(src.origin);
                }
            }, (_) {});
        }
    }

    int run(UserConfig uconf) {
        makeDefaultFileStructure();
        if (conf_.setupDirs)
            makeLocalSetupFileStructure(LlmConfig.init);

        llmConf = readConfig(uconf.config, !conf_.prompt.empty, uconf.noCwdConfig).userToLlmConfig(
                conf_);
        rag = createRag(llmConf);
        agentHistory = llmConf.scratchArea;
        monitor = new MetricMonitor(llmConf.scratchArea ~ "monitor.jsonl");
        agent_ = new Agent("main", llmConf, monitor, rag, llmConf.toolFilter.to());
        agent_.loadHistory(agentHistory);
        agent_.setSystemPrompt(llmConf.getPrompt(llmConf.agentPrompt));
        lastServerStat.context = agent_.chat.approxContextSize;
        scope (exit)
            this.dispose(); // Ensures cleanup on any exception after setup

        oneShotQuery = !conf_.prompt.empty;

        if (oneShotQuery) {
            uiMsg = UiMessenger(uiTid: Tid.init, blocked: true);
            this.runAgent(conf_.prompt);
            return 0;
        }

        // only update memory for non-oneshot because it is assumed that oneshot need max speed/low latency
        updateRagMemory();

        uiTid = spawn(&spawnUserInterface, thisTid);
        uiMsg = UiMessenger(uiTid: uiTid, blocked: false);
        send(uiTid, UiInitHistory(agent_.getUserQueries.map!(a => a.content).array.idup));
        send(uiTid, UiSetIniFile(llmConf.scratchArea ~ "imgui.ini"));
        agent_.setStreamUpdate(new StreamMessageUpdater(uiTid,
                agent_.contextSize, llmConf.activeModelName));

        foreach (m; agent_.chat.getMessages()) {
            this.processChatMessage(m, printUser: true);
        }
        auto helpText = this.printHelp(conf_);
        if (helpText !is null) {
            this.sendChatMessage(helpText, TuiChatMessageType_User);
        }

        if (llmConf.beginConsolidation) {
            logger.infof("Memory consolidation pending at session #%s", llmConf.sessionCount + 1);
            runMemoryConsolidation(llmConf, rag, monitor, (string msg,
                    TuiChatMessageType t) => this.sendChatMessage(msg, t));
        }

        bool running = true;
        do {
            this.setStatusText(true);
            receive((UiUserQuery a) {
                auto query = a.query.strip;
                if (!query.empty) {
                    this.sendChatMessage(query, TuiChatMessageType_User);
                    send(uiTid, UiAgentBusy.init);
                    clearStopAgent();
                    this.setStatusText(false);
                    final switch (this.runAgent(query)) {
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
}

struct UiMessenger {
    Tid uiTid;
    bool blocked;

    bool isActive() {
        return !blocked;
    }

    void setActive(bool onOff) {
        blocked = !onOff;
    }

    void ready() {
        if (blocked)
            return;
        send(uiTid, UiAgentReady.init);
    }

    void busy() {
        if (blocked)
            return;
        send(uiTid, UiAgentBusy.init);
    }
}

string formatStatusText(bool readyState, long contextSize, ServerStat stat, string model) {
    return format!"Context: %s/%s tokens | %.1f tok/s | Model: '%s' | %s"(stat.context,
            contextSize, stat.predictedPerSecond, model, readyState ? "Ready" : "Busy");
}

class StreamMessageUpdater : IStreamCallback {
    Tid uiTid;
    long contextSize;
    string modelName;

    this(Tid ui, long contextSize, string modelName) {
        this.uiTid = ui;
        this.contextSize = contextSize;
        this.modelName = modelName;
    }

    override void messageUpdate(StreamMessage msg, ServerStat stat) {
        send(uiTid, UiStatusText(formatStatusText(false, contextSize, stat, modelName)));
        send(uiTid, UiStreamChatMessage(msg: msg.content, thinking: msg.reasoning));
    }

    override void streamMessageDone() {
        send(uiTid, UiStreamChatDone.init);
    }
}

int appMain(UserConfig uconf, UserConfig.AgentChatConfig conf) {
    auto app = AgentApp(conf);
    return app.run(uconf);
}
