module app;

import logger = std.logger;
import std.algorithm;
import std.array : empty;
import std.format : format;
import std.stdio : writeln, writefln;
import std.sumtype : match;

import argparse : CLI, NamedArgument, PositionalArgument, ArgumentGroup,
    ansiStylingArgument, Command, Description, Required,
    Optional, Parse, SubCommand, Placeholder, Default, matchCmd, MutuallyExclusive;
import my.term_color;
import my.path;
import colorlog;

int main(string[] args) {
    UserConfig cli;
    if (!CLI!UserConfig.parseArgs(cli, args[1 .. $]))
        return 1;
    confLogger(cli.verbosity);
    logger.trace(cli);

    return matchCmd!((a) => appMain(cli, a))(cli.cmd);
}

struct UserConfig {
    SubCommand!(Default!AgentChatConfig, Rag, PrintToolMetricsConfig) cmd;

    @(NamedArgument("v", "verbose").Description(format!"Log verbosity level"))
    VerboseMode verbosity;

    @(NamedArgument("config", "c").Description("Configuration file to read"))
    void config_(string v) {
        config = Path(v);
    }

    Path config;

    @(Command("agent"))
    struct AgentChatConfig {
        @(NamedArgument("workarea", "w")
                .Description("Agent only allowed to read/write files in workarea"))
        void workarea_(string v) {
            workArea = Path(v);
        }

        Path workArea;

        @(NamedArgument("setup").Description("Create the directory structure 'llmfun'/..."))
        bool setupDirs;
    }

    @(Command("rag"))
    struct Rag {
        @MutuallyExclusive() {
            @(NamedArgument().Description("Add files"))
            bool add;
            @(NamedArgument().Description("Remove files"))
            bool rm;
            @(NamedArgument().Description("List all sources"))
            bool list;
        }

        @(NamedArgument("path").Description("Recursively add all text files"))
        string path;

        @(NamedArgument("db").Description("RAG database"))
        string rag;

        @(NamedArgument("include", "i")
                .Description(
                    "Include pattern for RAG files (can be repeated). Overrides config file."))
        string[] ragInclude;

        @(NamedArgument("exclude", "e")
                .Description(
                    "Exclude pattern for RAG files (can be repeated). Overrides config file."))
        string[] ragExclude;

        @(NamedArgument("setup").Description("Create the directory structure 'llmfun'/..."))
        bool setupDirs;
    }

    @(Command("tool_metrics"))
    struct PrintToolMetricsConfig {
        @(NamedArgument("data").Required().Description("Metric data file to read"))
        void data_(string v) {
            data = Path(v);
        }

        Path data;

        @(NamedArgument("number", "n").Description("Number of tools to print"))
        int number;
    }
}

LlmConfigT userToLlmConfig(LlmConfigT, ConfigT)(LlmConfigT llm, ConfigT conf) {
    static foreach (llmMemberName; __traits(allMembers, LlmConfigT)) {
        static foreach (confMemberName; __traits(allMembers, ConfigT)) {
            {
                static if (llmMemberName == confMemberName) {
                    alias Type = typeof(__traits(getMember, llm, llmMemberName));
                    static if (is(Type : Path)) {
                        if (!__traits(getMember, conf, confMemberName).empty) {
                            __traits(getMember, llm, llmMemberName) = __traits(getMember,
                                    conf, confMemberName).Path;
                        }
                    } else static if (is(Type : string)) {
                        if (!__traits(getMember, conf, confMemberName).empty) {
                            __traits(getMember, llm, llmMemberName) = __traits(getMember,
                                    conf, confMemberName);
                        }
                    } else {
                        static assert(0,
                                "unknown conversion of field " ~ llmMemberName ~ " type " ~ typeof(member)
                                    .stringof);
                    }
                }
            }
        }
    }
    logger.trace(llm);
    return llm;
}

int appMain(UserConfig uconf, UserConfig.AgentChatConfig conf) {
    import std.file : readText;
    import std.stdio : readln, writef, stdout;
    import std.string : strip;
    import llm.rag.embedder : createEmbedder;
    import llm.agent;
    import llm.chat;
    import llm.config;
    import llm.query;
    import llm.rag.rag;
    import llm.cli : configLinenoise, multiLineConsole;
    import llm.utility;
    import llm.metric.monitor : MetricMonitor;
    import llm.plan;
    import llm.coder;
    import llm.pipeline : prettyPrint;

    auto llmConf = readConfig(uconf.config).userToLlmConfig(conf);
    if (conf.setupDirs)
        makeFileStructure(llmConf);

    auto rag = () {
        try {
            auto embed = createEmbedder(llmConf.embedConfig);
            return new RAG(embed, llmConf.rag, llmConf.embedDimensions);
        } catch (Exception e) {
            logger.warning(e);
        }
        return new RAG(createEmbedder(EmbedConfig(RemoteEmbedConfig.init)),
                Path(":memory:"), llmConf.embedDimensions);
    }();
    scope (exit) {
        rag.destroy;
    }

    immutable agentHistory = llmConf.scratchArea;
    auto monitor = new MetricMonitor(llmConf.scratchArea ~ "monitor.jsonl");
    auto agent = new Agent("main", llmConf, monitor, rag, llmConf.toolFilter.to());
    scope (exit)
        agent.saveHistory(agentHistory);
    const systemPrompt = SystemPromptInit(llmConf.promptToPath(llmConf.codeModel.prompt)).toString;
    agent.setSystemPrompt(systemPrompt);
    agent.loadHistory(agentHistory);

    void progressCallback(size_t currentChunk, size_t totalChunks, string status) {
        displayProgress(currentChunk, totalChunks, status);
    };
    void doCompress(ref Agent agent, bool force) {
        if (!agent.needCompression && !force)
            return;
        logger.info("Compressing chat...");
        const ctxUsed = agent.contextUsed;
        auto res = agent.compress(force: force, callback: &progressCallback);
        displayCompressionResult(res.compressed, res.originalLength, res.newLength,
                res.keptXCount, res.keptXTokens, ctxUsed, res.newContextSize);
    }

    void processResult(ProcessResult result) {
        foreach (m; result.chat) {
            m.match!((Message a) {
                if (a.role != Role.user) {
                    writefln("[%s]: %s", a.role, a.content);
                }
            }, (ToolMessage a) {
                writefln("[%s %s/%s %s", a.role, agent.contextUsed,
                    agent.contextSize, summarizeToolCalls(a.role, a.toolCalls));
            }, (ToolResponse a) {
                writefln("[%s %s/%s tool:%-s]: %s", a.role, agent.contextUsed,
                    agent.contextSize, a.toolName, a.content.length < 100
                    ? a.content : a.content[0 .. 100]);
            }, (VisionMessage a) {
                writefln("[user]: %s (with image)", a.content);
            });
        }
        agent.saveHistory(agentHistory);
        logger.trace(result.status != ProcessResult.Status.ok, result.status);
    }

    configCatchCtrlC();
    bool running = true;
    configLinenoise();
    do {
        string query;
        if (running) {
            playNotification;
            query = multiLineConsole(format!"[%s/%s]$ "(agent.contextUsed, agent.contextSize));
            clearInterruptSignal();
            if (query.among("/quit", "/q", "/exit")) {
                break;
            } else if (query == "/compact") {
                doCompress(agent, force: true);
                continue;
            } else if (query == "/new") {
                agent.clearHistory;
                logger.info("context cleared");
                continue;
            } else if (query.startsWith("/plan ")) {
                auto q = query["/plan ".length .. $];
                logger.infof("Running plan pipeline: %s", q);
                auto result = runPlanPipeline(q, llmConf, rag, monitor, () {
                    return isInterruptTriggered;
                }, llmConf.toolFilter.to());
                writeln(prettyPrint(result));
                continue;
            } else if (query.startsWith("/code ")) {
                auto q = query["/code ".length .. $];
                logger.infof("Running coder pipeline: %s", q);
                auto result = runCoderPipeline(q, llmConf, rag, monitor, () {
                    return isInterruptTriggered;
                }, llmConf.toolFilter.to());
                if (result.wasInterrupted) {
                    writeln("\nPipeline interrupted by user.");
                    continue;
                }
                writeln(prettyPrint(result));
                continue;
            } else if (query.empty) {
                continue;
            }
        }
        agent.addUserQuery(query);
        doCompress(agent, force: false);
        auto result = agent.runToCompletion(&processResult, compressCallback: &progressCallback,
                interrupt: () { return isInterruptTriggered; });
    }
    while (running);

    return 0;
}

int appMain(UserConfig uconf, UserConfig.Rag conf) {
    import llm.rag.rag;
    import llm.config;
    import my.filter : ReFilter;
    import llm.rag.embedder : createEmbedder;
    import std.file : readText, exists, isFile, isDir, dirEntries, SpanMode;
    import std.path : extension, baseName;
    import std.array : appender;

    auto llmConf = readConfig(uconf.config).userToLlmConfig(conf);
    if (conf.setupDirs) {
        makeFileStructure(llmConf, rag: true);
    }

    auto rag = () {
        try {
            auto embed = createEmbedder(llmConf.embedConfig);
            return new RAG(embed, llmConf.rag, llmConf.embedDimensions);
        } catch (Exception e) {
            logger.warning(e);
        }
        return new RAG(createEmbedder(EmbedConfig(RemoteEmbedConfig.init)),
                Path(":memory:"), llmConf.embedDimensions);
    }();
    scope (exit) {
        rag.destroy;
    }

    void addFile(Path p) {
        import llm.rag.database;

        logger.info("Add ", p);
        try {
            rag.add(Document(p.Origin, readText(p.toString)));
        } catch (Exception e) {
            logger.warning(e.msg);
        }
    }

    ReFilter buildRagFilter() {
        auto filter = llmConf.ragFilter;

        if (!conf.ragInclude.empty) {
            filter.include = conf.ragInclude;
        }
        if (!conf.ragExclude.empty) {
            filter.exclude = conf.ragExclude;
        }

        if (filter.include.empty) {
            logger.warning("ragFilter include is empty - all file types will be indexed");
        }

        try {
            return filter.to();
        } catch (Exception e) {
            logger.warningf("Invalid ragFilter regex pattern: %s - falling back to defaults",
                    e.msg);
            filter.include = [".*\\.txt", ".*\\.md"];
            filter.exclude = [];
            return filter.to();
        }
    }

    void addData() {
        import std.file : dirEntries, SpanMode, isFile;

        if (!conf.path.exists) {
            logger.warningf("Path %s do not exist", conf.path);
        }

        auto ragFilter = buildRagFilter();

        if (conf.path.isFile) {
            if (ragFilter.match(baseName(conf.path))) {
                addFile(conf.path.Path);
            } else {
                logger.infof("File %s excluded by ragFilter", conf.path);
            }
            return;
        }

        logger.info("Add files from ", conf.path);
        foreach (p; dirEntries(conf.path, SpanMode.depth).filter!(a => a.isFile)
                .filter!(a => ragFilter.match(a.name))) {
            addFile(p.name.Path);
        }
    }

    void removeData() {
        auto ragFilter = buildRagFilter();

        if (conf.path.empty) {
            if (conf.ragInclude.empty && conf.ragExclude.empty
                    && llmConf.ragFilter.include.empty && llmConf.ragFilter.exclude.empty) {
                logger.warning("No --path provided and no --include/--exclude filters active (CLI or config). " ~ "Nothing to remove. Use --include <pattern> or --exclude <pattern> to select sources for removal, " ~ "or provide --path for a specific file/directory.");
                return;
            }
        }

        // path-based removal
        if (!conf.path.empty) {
            long entriesRemoved = 0; // Scoped to this branch

            if (conf.path.isFile) {
                logger.info("Remove embeddings from file", conf.path);
                entriesRemoved = rag.removeSource(Origin(conf.path.Path));
            } else if (conf.path.isDir) {
                logger.info("Remove embeddings from files in ", conf.path);
                foreach (p; dirEntries(conf.path, SpanMode.depth).filter!(a => a.isFile)
                        .filter!(a => ragFilter.match(a.name))) {
                    entriesRemoved += rag.removeSource(Origin(p.name.Path));
                }
            } else { // assuming it is a URL
                logger.info("Removing URL ", conf.path);
                entriesRemoved = rag.removeSource(Origin(Url(conf.path)));
            }
            logger.infof("Removed %s embeddings", entriesRemoved);
            return;
        }

        // Filter-based source iteration and matching
        struct RemoveCandidate {
            Origin origin;
            string matchStr;
        }

        long entriesRemoved = 0; // Scoped to filter-based branch
        long entriesFailed = 0;
        long unknownSkipped = 0;

        auto candidates = appender!(RemoveCandidate[])();
        foreach (src; rag.getSources) {
            src.origin.match!((Unknown _) { unknownSkipped++; return; }, (Path a) {
                if (ragFilter.match(a.toString))
                    candidates.put(RemoveCandidate(src.origin, a.toString));
            }, (Url a) {
                if (ragFilter.match(a.value))
                    candidates.put(RemoveCandidate(src.origin, a.value));
            });
        }

        auto candidateArray = candidates.data;
        logger.infof("Found %s source(s) matching filter for removal", candidateArray.length);
        foreach (c; candidateArray) {
            logger.infof("  Will remove: '%s'", c.matchStr);
        }

        foreach (c; candidateArray) {
            try {
                entriesRemoved += rag.removeSource(c.origin);
            } catch (Exception e) {
                entriesFailed++;
                logger.warningf("Failed to remove '%s': %s", c.matchStr, e.msg);
            }
        }

        logger.infof("Removed %s embeddings from %s source(s), %s failed, %s unknown skipped",
                entriesRemoved, candidateArray.length, entriesFailed, unknownSkipped);
    }

    void listSources() {
        logger.info("List all sources");
        foreach (src; rag.getSources) {
            src.origin.match!((Unknown _) {
                logger.infof("unknown (%s)", src.checksum.get);
            }, (Path a) { logger.infof("path:'%s' (%s)", a, src.checksum.get); }, (Url a) {
                logger.infof("url:'%s' (%s)", a.value, src.checksum.get);
            });
        }
    }

    if (conf.add)
        addData();
    else if (conf.rm)
        removeData();
    else if (conf.list)
        listSources();

    return 0;
}

int appMain(UserConfig uconf, UserConfig.PrintToolMetricsConfig conf) {
    import llm.metric.monitor : MetricMonitor;
    import llm.metric.calculator : MetricsCalculator;

    try {
        auto monitor = new MetricMonitor(conf.data);
        auto calculator = new MetricsCalculator();
        calculator.setEvents(monitor.getRecentEvents(10000));
        writeln(calculator.generateReport(conf.number));
    } catch (Exception e) {
        writeln("Error: ", e.msg);
        return 1;
    }

    return 0;
}
