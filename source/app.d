module app;

import logger = std.logger;
import std.algorithm;
import std.array : empty, array;
import std.conv : to;
import std.exception : ifThrown;
import std.file : exists;
import std.format : format;
import std.stdio : writeln, writefln;
import std.sumtype : match;
import std.string : strip, startsWith;

import argparse : CLI, NamedArgument, PositionalArgument, ArgumentGroup,
    ansiStylingArgument, Command, Description, Required,
    Optional, Parse, SubCommand, Placeholder, Default, matchCmd,
    MutuallyExclusive, PositionalArgument;
import my.term_color;
import my.path;
import colorlog;

import llm.config : RagDatabaseConfig;

int main(string[] args) {
    UserConfig cli;
    if (!CLI!UserConfig.parseArgs(cli, args[1 .. $]))
        return 1;
    confLogger(cli.verbosity);
    logger.trace(cli);

    import llm.utility : catchSignalSIGPIPE;

    catchSignalSIGPIPE;

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

    @(NamedArgument("no-cwd-config")
            .Description("Do not read .llmfun.json from current directory (security)"))
    bool noCwdConfig;

    @(Command("agent"))
    struct AgentChatConfig {
        @(NamedArgument("workarea", "w")
                .Description("Agent only allowed to read/write files in workarea"))
        void workarea_(string v) {
            workArea = Path(v);
        }

        Path workArea;

        @(NamedArgument("local-setup")
                .Description("Create the directory structure 'llmfun'/... in current directory"))
        bool setupDirs;

        @(NamedArgument("db").Description("Primary RAG database (read/write)"))
        string ragPrimary;

        @(NamedArgument("prompt", "p").Description("One shot prompt for the agent"))
        string prompt;
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
            @(NamedArgument().Description("Sync files with database"))
            bool sync;
        }

        @(NamedArgument("db").Description("Primary RAG database (read/write)"))
        string ragPrimary;

        @(NamedArgument("include", "i")
                .Description(
                    "Include pattern for RAG files (can be repeated). Overrides config file."))
        string[] ragInclude;

        @(NamedArgument("exclude", "e")
                .Description(
                    "Exclude pattern for RAG files (can be repeated). Overrides config file."))
        string[] ragExclude;

        @(NamedArgument("local-setup")
                .Description("Create the directory structure 'llmfun'/... in current directory"))
        bool setupDirs;

        @(NamedArgument("dry-run").Description("Preview changes without modifying database"))
        bool dryRun;

        @(PositionalArgument("PATHS")
                .Description("Paths for the arguments --add/--rm/--list/--sync"))
        string[] path;
    }

    @(Command("tool_metrics"))
    struct PrintToolMetricsConfig {
        @(NamedArgument("data").Description("Metric data file to read (.jsonl)"))
        void data_(string v) {
            data = Path(v);
        }

        Path data = "llmfun/data/scratch/monitor.jsonl";

        @(NamedArgument("number", "n").Description("Number of tools to print"))
        int number;

        @(NamedArgument("follow", "f").Description("Live monitor the tool calls"))
        bool follow;
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
                    } else static if (is(Type == RagDatabaseConfig)) {
                        alias ConfType = typeof(__traits(getMember, conf, confMemberName));
                        static if (is(ConfType == string)) {
                            auto cv = __traits(getMember, conf, confMemberName);
                            if (!cv.empty) {
                                __traits(getMember, llm, llmMemberName) = RagDatabaseConfig(cv.Path,
                                        "Primary database (read/write)");
                            }
                        } else {
                            static assert(0,
                                    "unknown conversion of field " ~ llmMemberName ~ " type " ~ typeof(member)
                                        .stringof);
                        }
                    } else static if (is(Type == Path[])) {
                        alias ConfType = typeof(__traits(getMember, conf, confMemberName));
                        static if (is(ConfType == string)) {
                            auto cv = __traits(getMember, conf, confMemberName);
                            if (!cv.empty) {
                                __traits(getMember, llm, llmMemberName) = [
                                    cv.Path
                                ];
                            }
                        } else static if (is(ConfType == string[])) {
                            auto cv = __traits(getMember, conf, confMemberName);
                            if (!cv.empty) {
                                Path[] result;
                                foreach (s; cv) {
                                    result ~= s.Path;
                                }
                                __traits(getMember, llm, llmMemberName) = result;
                            }
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

    bool debugMode = false;

    /// TODO: If help text ever needs externalization (config file, i18n),
    ///       the function signature should accept a content parameter.
    void printHelp() {
        import std.process : environment;

        if (environment.get("LLMFUN_NO_SPLASH") || !conf.prompt.empty)
            return;

        writeln("llmfun agent mode — type a query and press Enter to start.");
        writeln(" Use /commands for special actions:");
        writeln("");
        writeln("   (bare query)       Send a message to the agent");
        writeln("   /help              Show this help message");
        writeln("   /quit, /q, /exit   Exit the agent");
        writeln("   /compact           Force compress the chat history");
        writeln("   /new               Clear history and start a new conversation");
        writeln("   /model             List available models");
        writeln("   /model <index>     Select model by index");
        writeln("   /model <name>      Select model by exact name (case-insensitive)");
        writeln("   /plan <query>      Run the plan pipeline");
        writeln("   /code <query>      Run the coder pipeline");
        writeln("   /debug             Toggle verbose debug output");
    }

    if (conf.setupDirs)
        makeFileStructure(LlmConfig.init);
    auto llmConf = readConfig(uconf.config, !conf.prompt.empty, uconf.noCwdConfig)
        .userToLlmConfig(conf);
    auto rag = () {
        try {
            auto embed = createEmbedder(llmConf.embedConfig);
            return new RAG(embed, llmConf.getRagDatabases);
        } catch (Exception e) {
            logger.warning(e);
        }
        return new RAG(createEmbedder(EmbedConfig(RemoteEmbedConfig.init)), null);
    }();
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
                if (!a.role.among(Role.user, Role.system)) {
                    writefln("[%s]: %s", a.role, a.content);
                } else {
                    logger.tracef("[%s]: %s", a.role, a.content);
                }
            }, (ToolMessage a) {
                if (!isHiddenToolCall(a.toolCalls)) {
                    writefln("[%s %s/%s %s", a.role, agent.contextUsed,
                        agent.contextSize, summarizeToolCalls(a.role, a.toolCalls));
                }
            }, (ToolResponse a) {
                if (!isHiddenToolResponse(a.toolName)) {
                    writefln("[%s %s/%s tool:%-s]: %s", a.role, agent.contextUsed,
                        agent.contextSize, a.toolName, a.content.length < 100
                        ? a.content : a.content[0 .. 100]);
                }
            }, (VisionMessage a) {
                writefln("[user]: %s (with image)", a.content);
            });
        }
        agent.saveHistory(agentHistory);
        logger.trace(result.status != ProcessResult.Status.ok, result.status);
    }

    printHelp();

    configCatchCtrlC();
    bool running = conf.prompt.empty;
    string query = conf.prompt;
    auto linenoiseHistory = llmConf.scratchArea.exists
        ? llmConf.scratchArea ~ Path("cli_history.txt") : Path.init;
    configLinenoise(historyFile: linenoiseHistory, len: 50000); // TODO: make history size configurable
    do {
        if (running) {
            playNotification;
            query = multiLineConsole(prompt: format!"[%s/%s %s]$ "(agent.contextUsed,
                    agent.contextSize, llmConf.activeModelName()), historyFile: linenoiseHistory);
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
            } else if (query == "/help") {
                printHelp();
                continue;
            } else if (query == "/debug") {
                debugMode = !debugMode;
                logger.globalLogLevel = debugMode ? logger.LogLevel.trace : logger.LogLevel.info;
                logger.infof("Debug output: %s", debugMode ? "ON" : "OFF");
                continue;
            } else if (query == "/model" || query.startsWith("/model ")) {
                auto arg = query == "/model" ? "" : query["/model ".length .. $].strip();
                if (arg.empty) {
                    writeln("Available models:");
                    foreach (i, model; llmConf.codeModels) {
                        auto activeMarker = (i == cast(size_t) llmConf.activeCodeModelIndex) ? " [active]"
                            : "";
                        writefln("  %s  %s%s", i, model.name, activeMarker);
                    }
                    writeln();
                    writeln("Use /model <index> or /model <name> to switch.");
                } else {
                    const oldModel = llmConf.activeCodeModel.name;
                    // Try to switch model
                    bool switched;
                    size_t idx = ifThrown(arg.to!long, -1);
                    if (idx >= 0) {
                        switched = llmConf.selectModelByIndex(idx);
                        if (!switched) {
                            logger.errorf("Error: Invalid model index '%s'. Valid indices: 0-%s.",
                                    arg, llmConf.codeModels.length - 1);
                        }
                    } else {
                        auto result = llmConf.selectModelByName(arg);
                        switched = result.empty;
                        logger.warningf(!result.empty, "failed to switch model: ", result);
                    }
                    if (switched) {
                        agent.resetModel(llmConf.activeCodeModel());
                        logger.infof("switched to model: %s", llmConf.activeModelName());
                        logger.infof("Agent model reset: %s -> %s, context: %s",
                                oldModel, agent.modelName, agent.modelContextSize);
                    }
                }
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
    import std.file : readText, isFile, isDir, dirEntries, SpanMode;
    import std.path : extension, baseName;
    import std.array : appender;
    import miniorm : spinSql;

    if (conf.setupDirs) {
        makeFileStructure(LlmConfig.init, rag: true);
    }
    auto llmConf = readConfig(uconf.config, false, uconf.noCwdConfig).userToLlmConfig(conf);

    auto rag = () {
        try {
            auto embed = createEmbedder(llmConf.embedConfig);
            return new RAG(embed, llmConf.getRagDatabases);
        } catch (Exception e) {
            logger.warning(e);
        }
        return new RAG(createEmbedder(EmbedConfig(RemoteEmbedConfig.init)), null);
    }();
    scope (exit) {
        rag.destroy;
    }

    if (rag.isPrimaryInMemory) {
        logger.errorf("No primary database opened for read/write. Tried to open '%s'",
                llmConf.ragPrimary.path);
        return 1;
    }

    void addFile(Path p) {
        try {
            auto added = rag.add(Document(p.Origin, readText(p.toString)),
                    llmConf.ragConfig).chunks != 0;
            logger.info(added, "Add ", p);
        } catch (Exception e) {
            logger.warning(e.msg);
        }
    }

    long printNoneExistingPaths(T)(T paths) {
        long counter;
        foreach (p; paths.filter!(a => !exists(a))) {
            logger.warningf("Path '%s' does not exist", p);
            counter++;
        }
        return counter;
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

    auto ragFilter = buildRagFilter();

    Path[] collectFiles(Path root, ReFilter filter) {
        if (!exists(root)) {
            return null;
        }
        auto files = appender!(Path[])();
        if (isFile(root) && filter.match(root)) {
            files.put(root.Path);
        } else if (isDir(root)) {
            foreach (p; dirEntries(root, SpanMode.depth).filter!(a => a.isFile)
                    .filter!(a => filter.match(a.name))) {
                files.put(p.name.Path);
            }
        }
        return files[];
    }

    long addData() {
        if (conf.path.empty) {
            logger.warning("No --path provided. Nothing to add.");
            return 0;
        }

        const failed = printNoneExistingPaths(conf.path);

        foreach (p; conf.path.filter!(a => exists(a))) {
            auto files = collectFiles(p.Path, ragFilter);

            if (files.empty) {
                if (isFile(p)) {
                    logger.infof("File %s excluded by ragFilter", p);
                } else {
                    logger.infof("No files matched in %s", p);
                }
                continue;
            }

            logger.infof("Adding files from %s", p);
            foreach (f; files) {
                addFile(f);
            }
        }
        return failed;
    }

    long removeData() {
        if (conf.path.empty) {
            if (conf.ragInclude.empty && conf.ragExclude.empty
                    && llmConf.ragFilter.include.empty && llmConf.ragFilter.exclude.empty) {
                logger.warning("No --path provided and no --include/--exclude filters active (CLI or config). " ~ "Nothing to remove. Use --include <pattern> or --exclude <pattern> to select sources for removal, " ~ "or provide --path for a specific file/directory.");
                return 0;
            }
        }

        // path-based removal
        if (!conf.path.empty) {
            long entriesRemoved = 0;
            long entriesFailed = 0;
            foreach (p; conf.path) {
                auto path = p.Path;
                try {
                    if (path.isFile) {
                        logger.infof("Removing embeddings from file %s", p);
                        entriesRemoved += rag.removeSource(Origin(path));
                    } else if (path.isDir) {
                        logger.infof("Removing embeddings from files in %s", p);
                        foreach (entry; dirEntries(path, SpanMode.depth).filter!(a => a.isFile)
                                .filter!(a => ragFilter.match(a.name))) {
                            entriesRemoved += rag.removeSource(Origin(entry.Path));
                        }
                    } else {
                        if (p.startsWith("http://") || p.startsWith("https://")) {
                            logger.infof("Removing URL %s", p);
                            entriesRemoved += rag.removeSource(Origin(Url(p)));
                        } else {
                            logger.warningf("Path '%s' does not exist and is not a URL, skipping",
                                    p);
                            entriesFailed++;
                        }
                    }
                } catch (Exception e) {
                    entriesFailed++;
                    logger.warningf("Failed to remove '%s': %s", p, e.msg);
                }
            }
            logger.infof("Removed %s embeddings, %s failed", entriesRemoved, entriesFailed);
            return entriesFailed;
        }

        // Filter-based source iteration and matching
        struct RemoveCandidate {
            Origin origin;
            string matchStr;
        }

        long entriesRemoved = 0; // Scoped to filter-based branch
        long entriesFailed = 0;

        auto candidates = appender!(RemoveCandidate[])();
        long topicSkipped = 0;
        foreach (src; rag.db.getSources) {
            src.origin.match!((Topic a) { ++topicSkipped; return; }, (Path a) {
                if (ragFilter.match(a.toString))
                    candidates.put(RemoveCandidate(src.origin, a.toString));
            }, (Url a) {
                if (ragFilter.match(a.value))
                    candidates.put(RemoveCandidate(src.origin, a.value));
            });
        }
        if (topicSkipped > 0) {
            logger.infof("Skipped %s topic source(s) — topics have no file paths to filter",
                    topicSkipped);
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

        logger.infof("Removed %s embeddings from %s source(s), %s failed",
                entriesRemoved, candidateArray.length, entriesFailed);
        return entriesFailed;
    }

    void listSources() {
        logger.info("List all sources");
        foreach (dbSrc; rag.getSources) {
            logger.infof("Database '%s'", dbSrc.name);
            foreach (src; dbSrc.sources) {
                auto cs = src.checksum.get;
                src.origin.match!((Topic a) {
                    logger.infof("topic:'%s' (%s)", a.name, cs);
                }, (Path a) { logger.infof("path:'%s' (%s)", a, cs); }, (Url a) {
                    logger.infof("url:'%s' (%s)", a.value, cs);
                });
            }
        }
    }

    long syncData() {
        import my.set : Set;
        import std.path : buildNormalizedPath;

        if (conf.path.empty) {
            logger.warning("--path is required for sync");
            return 1;
        }

        const invalidPaths = printNoneExistingPaths(conf.path);

        // Build normalized paths for prefix matching
        Path[] normalizedPaths = conf.path
            .filter!(a => exists(a))
            .map!(a => a.buildNormalizedPath.Path)
            .array;
        if (normalizedPaths.empty) {
            logger.warning("No valid paths to sync");
            return 1;
        }

        // Collect files from all paths, deduplicate
        Set!string seenFiles;
        Path[] allFiles;
        foreach (np; normalizedPaths) {
            auto files = collectFiles(np, ragFilter);
            foreach (f; files) {
                if (!seenFiles.contains(f)) {
                    seenFiles.add(f);
                    allFiles ~= f.Path;
                }
            }
        }

        long added = 0;
        long skipped = 0;
        long failed = 0;

        Set!string syncedOrigins;

        // Phase 1: Scan and add
        logger.warningf(invalidPaths > 0, "Skipped %s invalid path(s)", invalidPaths);
        foreach (p; allFiles) {
            syncedOrigins.add(p);
            try {
                if (conf.dryRun) {
                    logger.infof("  [dry-run] Would add: %s", p);
                    added++;
                } else {
                    auto result = rag.add(Document(Origin(p),
                            readText(p.toString)), llmConf.ragConfig);
                    if (result.chunks > 0) {
                        logger.infof("  Added/updated: %s (%s chunks)", p, result.chunks);
                        added++;
                    } else {
                        logger.infof("  Skipped (unchanged): %s", p);
                        skipped++;
                    }
                }
            } catch (Exception e) {
                logger.warningf("Failed to process '%s': %s", p, e.msg);
                failed++;
            }
        }

        // Phase 2: Remove stale sources
        logger.info("Phase 2: Checking for deleted sources");
        long removed = 0;
        long removeFailed = 0;

        // Helper: check if normPath is under any managed path with boundary check
        bool isUnderManagedPath(string normPath) {
            foreach (np; normalizedPaths.map!(a => a.toString)
                    .filter!(a => normPath.startsWith(a))) {
                return true;
            }
            return false;
        }

        foreach (src; rag.getSources.map!(a => a.sources).joiner) {
            src.origin.match!((Topic a) { return; }, (Path a) {
                auto normPath = a.toString.buildNormalizedPath;
                if (isUnderManagedPath(normPath) && ragFilter.match(normPath)
                    && !syncedOrigins.contains(normPath)) {
                    try {
                        if (conf.dryRun) {
                            logger.infof("  [dry-run] Would remove: %s", a);
                        } else {
                            logger.infof("  Removing: %s", a);
                            rag.removeSource(Origin(a.Path));
                        }
                        removed++;
                    } catch (Exception e) {
                        logger.warningf("  Failed to remove '%s': %s", a, e.msg);
                        removeFailed++;
                    }
                }
            }, (Url a) { return; });
        }

        logger.infof("Sync complete: %s added/updated, %s skipped, %s removed, %s failed",
                added, skipped, removed, failed + removeFailed);
        if (!conf.dryRun && (added > 0 || removed > 0)) {
            spinSql!(() { rag.fts5Rebuild; });
        }
        return failed + removeFailed;
    }

    if (conf.add) {
        long failed = addData();
        spinSql!(() { rag.vacuum; rag.fts5Rebuild; });
        return failed != 0 ? 1 : 0;
    } else if (conf.rm) {
        long failed = removeData();
        spinSql!(() { rag.vacuum; rag.fts5Rebuild; });
        return failed != 0 ? 1 : 0;
    } else if (conf.sync) {
        return syncData() != 0 ? 1 : 0;
    } else if (conf.list) {
        listSources();
    }

    return 0;
}

int appMain(UserConfig uconf, UserConfig.PrintToolMetricsConfig conf) {
    import core.thread : Thread;
    import core.time : dur;
    import std.json : parseJSON;
    import llm.metric.monitor : MetricMonitor, ToolCallEvent;
    import llm.metric.calculator : MetricsCalculator;
    import std.algorithm : countUntil;
    import my.term_color;

    if (conf.follow) {
        ToolCallEvent lastEvent;
        while (true) {
            try {
                scope monitor = new MetricMonitor(conf.data);
                auto events = monitor.getRecentEvents(50);
                auto idx = events.countUntil!(a => a == lastEvent);
                events = idx < 0 ? events : events[idx + 1 .. $];
                foreach (e; events) {
                    auto args = e.arguments.object.byKeyValue.map!(a => format!"%s:%s"(a.key.color(Color.yellow),
                            a.value.toString)).joiner(", ");
                    writefln("%s - agent:%s time:%s tool:%s(%s) -> %s",
                            e.timestamp.to!string.color(e.success ? Color.green
                                : Color.red), e.agentName, e.responseTimeMs == 0
                            ? "0 ms" : e.responseTimeMs
                                .dur!"msecs"
                                .to!string, e.toolName, args, e.result);

                }
                if (!events.empty) {
                    lastEvent = events[$ - 1];
                }
            } catch (Exception e) {
                logger.trace(e.msg);
            }
            Thread.sleep(3.dur!"seconds");
        }
    } else {
        try {
            scope monitor = new MetricMonitor(conf.data);
            scope calculator = new MetricsCalculator();
            calculator.setEvents(monitor.getRecentEvents(10000));
            writeln(calculator.generateReport(conf.number));
        } catch (Exception e) {
            writeln("Error: ", e.msg);
            return 1;
        }
    }

    return 0;
}
