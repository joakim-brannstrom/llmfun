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
import llm.skill : SkillManager, buildAlwaysApplyBlock, makeSkillManager;
import llm.tui;
import llm.utility;
import llm.types : ServerStat, StreamMessage, StreamToolCall;
import llmfun_tui;

import my.path : Path, AbsolutePath;

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
        SkillManager skillManager_;

        enum AgentStatus {
            active,
            terminate
        }
    }

    @disable this(this);

    this(UserConfig.AgentChatConfig conf) {
        this.conf_ = conf;
        this.uiMsg = new UiMessenger(Tid.init, true);
    }

    void dispose() {
        if (uiTid != Tid.init) {
            try {
                uiMsg.terminate();
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
        s ~= "   /skills            List available skills";
        return s.join("\n");
    }

    private string formatSkillsList() {
        if (skillManager_ is null) {
            return "No skill manager initialized.";
        }

        auto skills = skillManager_.getManifest();
        if (skills.empty) {
            return "No skills are currently loaded.";
        }

        auto alwaysApplyCount = skills.filter!(skill => skill.alwaysApply).array.length;
        string[] lines;
        lines ~= "Available skills:";
        lines ~= "";

        foreach (skill; skills) {
            auto tag = skill.alwaysApply ? " [always-apply]" : "";
            auto desc = skill.description.length > 80
                ? skill.description[0 .. 77] ~ "..." : skill.description;
            lines ~= format("  %-25s %s", skill.name ~ tag, desc);
        }

        lines ~= "";
        lines ~= format("%s skills available, %s always-apply", skills.length, alwaysApplyCount);
        return lines.join("\n");
    }

    private void sendChatMessage(Args...)(string msg, TuiChatMessageType type, Args args) {
        static if (args.length > 0) {
            msg = format(msg, args);
        }
        uiMsg.chatMessage(msg, type);
    }

    private void sendChatThinkMessage(Args...)(string msg, string thinking,
            TuiChatMessageType type, Args args) {
        static if (args.length > 0) {
            msg = format(msg, args);
        }
        uiMsg.chatThinkMessage(msg, thinking, type);
    }

    private void progressCallback(size_t currentChunk, size_t totalChunks, string status) {
        uiMsg.chatMessage(format!"[assistant]: Compressing... %s/%s : %s"(currentChunk,
                totalChunks, status), TuiChatMessageType_Assistant);
    }

    private void setStatusText(bool readyState) {
        uiMsg.statusText(formatStatusText(readyState, agent_.contextSize,
                lastServerStat, llmConf.activeModelName()));
    }

    private void doCompress(bool force) {
        if (!agent_.needCompression && !force)
            return;
        const ctxUsed = agent_.contextUsed;
        uiMsg.busy;
        auto res = agent_.compress(force: force, callback: &this.progressCallback);
        uiMsg.chatMessage(compressionResultToString(res.compressed, res.originalLength,
                res.newLength, res.keptXCount, res.keptXTokens, ctxUsed, res.newContextSize),
                TuiChatMessageType_Assistant);
        uiMsg.ready;
    }

    private void processChatMessage(Chat.MessageT m, bool printUser) {
        m.match!((Message a) {
            if (!a.role.among(Role.user, Role.system) || (printUser && a.role != Role.system)) {
                auto msgType = a.role == Role.user ? TuiChatMessageType_User
                    : TuiChatMessageType_Assistant;
                this.sendChatThinkMessage("%s: %s", a.thinking, msgType, a.role, a.content);
            } else {
                logger.tracef("[%s]: %s", a.role, a.content);
            }
        }, (ToolMessage a) {
            auto calls = summarizeToolCalls(a.toolCalls, 500);
            this.sendChatThinkMessage("tool calls %s: %(%-s\n%)", a.thinking,
                TuiChatMessageType_ToolCall, calls.length, calls);
            if (a.isFinalAnswer()) {
                uiMsg.finalAnswer(a.getFinalAnswer());
            }
        }, (ToolResponse a) {
            if (!isHiddenToolResponse(a.toolName)) {
                this.sendChatMessage("tool result %-s: %s", TuiChatMessageType_ToolResponse,
                    a.toolName, summarizeToolResponse(a, 500));
            }
        }, (VisionMessage a) {
            this.sendChatMessage("user: %s (with image)", TuiChatMessageType_User, a.content);
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
            uiMsg.clearChat();
            lastServerStat.context = 0;
            uiMsg.pipelineClear;
            return AgentStatus.active;
        } else if (query == "/help") {
            auto helpText = this.printHelp(conf_);
            if (helpText !is null) {
                this.sendChatMessage(helpText, TuiChatMessageType_User);
            }
            return AgentStatus.active;
        } else if (query == "/debug") {
            debugMode = !debugMode;
            uiMsg.logFile(debugMode);
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
                    agent_.setStreamUpdate(makeStreamCallback);
                    this.sendChatMessage("switched to model: %s\nAgent model reset: %s -> %s, context: %s",
                            TuiChatMessageType_Assistant,
                            llmConf.activeModelName(), oldModel,
                            agent_.modelName, agent_.modelContextSize);
                }
            }
            return AgentStatus.active;
        } else if (query.startsWith("/plan ")) {
            uiMsg.pipelineClear;
            auto q = query["/plan ".length .. $];
            sendChatMessage("[assistant]: Running plan pipeline: %s",
                    TuiChatMessageType_Assistant, q);
            auto result = runPlanPipeline(q, llmConf, rag, monitor, () {
                return isStopAgentTriggered;
            }, llmConf.toolFilter.to(), makePipelineStreamCallback);
            sendChatMessage(prettyPrint(result), TuiChatMessageType_Assistant);
            return AgentStatus.active;
        } else if (query.startsWith("/code ")) {
            uiMsg.pipelineClear;
            auto q = query["/code ".length .. $];
            sendChatMessage("[assistant]: Running coder pipeline: %s",
                    TuiChatMessageType_Assistant, q);
            auto result = runCoderPipeline(q, llmConf, rag, monitor, () {
                return isStopAgentTriggered;
            }, llmConf.toolFilter.to(), makePipelineStreamCallback);
            if (result.wasInterrupted) {
                this.sendChatMessage("[assistant]: Pipeline interrupted by user.",
                        TuiChatMessageType_Assistant);
                return AgentStatus.active;
            }
            this.sendChatMessage(format!"[assistant]: %s"(prettyPrint(result)),
                    TuiChatMessageType_Assistant);
            return AgentStatus.active;
        } else if (query == "/skills") {
            this.sendChatMessage(this.formatSkillsList(), TuiChatMessageType_Assistant);
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

    private IStreamCallback makeStreamCallback() {
        return new StreamMessageUpdater(uiMsg, agent_.contextSize, llmConf.activeModelName);
    }

    private IStreamCallback makePipelineStreamCallback() {
        return new PipelineStreamMessageUpdater(uiMsg, agent_.contextSize,
                llmConf.activeModelName);
    }

    private void updateRagMemory() {
        import llm.vfs : FlatVfs;
        import std.path : baseName, stripExtension, dirName;
        import llm.rag.rag : add, Document, Origin, Offset, Topic;
        import my.set;
        import my.path : AbsolutePath;

        // do not slowdown startup if the user only have an in-memory because
        // then they are indexed every time the user start
        if (rag is null || rag.isPrimaryInMemory || llmConf.noMemory)
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
        if (rag is null)
            return 1;

        // Skill discovery
        skillManager_ = makeSkillManager(llmConf);

        agentHistory = llmConf.scratchArea;
        monitor = new MetricMonitor(llmConf.scratchArea ~ "monitor.jsonl");
        agent_ = new Agent("main", llmConf, skillManager_, monitor, rag, llmConf.toolFilter.to());
        agent_.loadHistory(agentHistory);
        agent_.setSystemPrompt(llmConf.getPrompt(skillManager: skillManager_,
                promptName: llmConf.agentPrompt, addSkills: true));

        lastServerStat.context = agent_.chat.approxContextSize;
        scope (exit)
            this.dispose(); // Ensures cleanup on any exception after setup

        // oneShotQuery: true  = CLI prompt mode (no UI thread, UiMessenger blocked)
        //                 false = full UI mode (UI thread spawned, UiMessenger active)
        oneShotQuery = !conf_.prompt.empty;

        if (oneShotQuery) {
            uiMsg = new UiMessenger(Tid.init, true);
            this.runAgent(conf_.prompt);
            return 0;
        }

        // only update memory for non-oneshot because it is assumed that oneshot need max speed/low latency
        updateRagMemory();

        uiTid = spawn(&spawnUserInterface, thisTid);
        uiMsg = new UiMessenger(uiTid, false);
        uiMsg.setIniFile(llmConf.scratchArea ~ "imgui.ini");
        send(uiTid, UiInitHistory(agent_.getUserQueries.map!(a => a.content).array.idup));
        send(uiTid, UiSetIniFile(llmConf.scratchArea ~ "imgui.ini"));
        agent_.setStreamUpdate(makeStreamCallback);

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
                    uiMsg.busy();
                    clearStopAgent();
                    this.setStatusText(false);
                    final switch (this.runAgent(query)) {
                    case AgentStatus.active:
                        break;
                    case AgentStatus.terminate:
                        uiMsg.terminate();
                        break;
                    }
                    uiMsg.ready();
                }
            }, (UiTerminated _) { running = false; });
        }
        while (running);

        return 0;
    }
}

class UiMessenger {
    Tid uiTid;
    bool blocked;

    this(Tid t, bool b = false) {
        uiTid = t;
        blocked = b;
    }

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

    void terminate() {
        if (blocked)
            return;
        if (uiTid == Tid.init)
            return; // safety: no UI thread to terminate
        send(uiTid, UiTerminate.init);
    }

    void chatMessage(string msg, TuiChatMessageType type) {
        if (blocked) {
            writeln(msg);
        } else {
            send(uiTid, UiChatMessage(msg, type));
        }
    }

    void chatThinkMessage(string msg, string thinking, TuiChatMessageType type) {
        if (blocked) {
            if (!thinking.empty) {
                writeln("Thinking: ", thinking);
            }
            writeln(msg);
        } else {
            send(uiTid, UiChatThinkMessage(msg, thinking, type));
        }
    }

    void statusText(string status) {
        if (blocked)
            return;
        send(uiTid, UiStatusText(status));
    }

    void finalAnswer(string msg) {
        if (blocked) {
            writeln(msg);
        } else {
            send(uiTid, UiFinalAnswer(msg));
        }
    }

    void clearChat() {
        if (blocked)
            return;
        send(uiTid, UiClearChat.init);
    }

    void logFile(bool useFile) {
        if (blocked)
            return;
        send(uiTid, UiLogFile(useFile));
    }

    void setIniFile(string path) {
        if (blocked)
            return;
        send(uiTid, UiSetIniFile(Path(path)));
    }

    // Streaming methods — silently skipped in blocked (one-shot) mode.
    // One-shot mode produces final output via writeln in chatMessage/finalAnswer;
    // incremental streaming feedback is not needed.
    void streamStatusText(string status) {
        statusText(status); // delegate to canonical method
    }

    void streamChatMessage(string msg, string thinking) {
        if (blocked)
            return;
        send(uiTid, UiStreamChatMessage(msg: msg, thinking: thinking));
    }

    void streamChatDone() {
        if (blocked)
            return;
        send(uiTid, UiStreamChatDone.init);
    }

    void pipelineStreamChatMessage(string agentId, string content,
            string thinking, string role, string status) {
        if (blocked)
            return;
        send(uiTid, UiPipelineStreamChatMessage(agentId: agentId, content: content,
                thinking: thinking, role: role, status: status));
    }

    void pipelineStreamDone(string agentId) {
        if (blocked)
            return;
        send(uiTid, UiPipelineStreamDone(agentId));
    }

    void pipelineClear() {
        if (blocked)
            return;
        send(uiTid, UiPipelineClear.init);
    }
}

string formatStatusText(bool readyState, long contextSize, ServerStat stat, string model) {
    return format!"Context: %s/%s tokens | %.1f tok/s | Model: '%s' | %s"(stat.context,
            contextSize, stat.predictedPerSecond, model, readyState ? "Ready" : "Busy");
}

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
        string content = msg.content;
        if (!tools.empty) {
            foreach (tool; tools) {
                content ~= "\n--- Tool ---\n";
                content ~= tool.toPrettyString(1000);
                content ~= "\n\n";
            }
        }

        uiMsg.streamStatusText(formatStatusText(false, contextSize, stat, modelName));
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

        string status = format!"Context %s/%s tokens | %.1f tok/s"(stat.context,
                contextSize, stat.predictedPerSecond);

        uiMsg.pipelineStreamChatMessage(agentId: agentId, content: content,
                thinking: msg.reasoning, role: msg.role, status: status);
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

int appMain(UserConfig uconf, UserConfig.AgentChatConfig conf) {
    import llm.subsystem : initLlmfunLocalModel, deinitLlmfunLocalModel;

    initLlmfunLocalModel();
    scope (exit)
        deinitLlmfunLocalModel();

    try {
        auto app = AgentApp(conf);
        return app.run(uconf);
    } catch (Exception e) {
        logger.warning(e.msg);
    }
    return 1;
}
