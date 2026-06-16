import logger = std.logger;
import std.range;
import std.algorithm;
import std.array : array, empty;
import std.conv : to;
import std.format : format;
import std.json : JSONValue, JSONOptions, parseJSON;
import std.logger;
import std.stdio : writeln, writefln, write;
import std.sumtype : SumType, match;
import std.utf : decode, UseReplacementDchar, byUTF;

import argparse : CLI, NamedArgument, PositionalArgument, ArgumentGroup,
    ansiStylingArgument, Command, Description, Required,
    Optional, Parse, SubCommand, Placeholder, Default, matchCmd, MutuallyExclusive;
import requests;
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
    SubCommand!(Default!PrintOk, ChatTestConfig, SummaryTestConfig, FuncCallPrint,
            TestSlotApiConfig, PrintToolMetricsConfig, TestPipelineConfig, TestRagSqliteConfig) cmd;

    @(NamedArgument("v", "verbose").Description(format!"Log verbosity level"))
    VerboseMode verbosity;

    @(Command("print_ok"))
    struct PrintOk {
    }

    @(Command("chat_test"))
    struct ChatTestConfig {
        @(NamedArgument("config", "c").Description("Configuration file to read"))
        void config_(string v) {
            config = Path(v);
        }

        Path config = "config/remote.json";
    }

    @(Command("summary_test"))
    struct SummaryTestConfig {
        @(NamedArgument("config", "c").Description("Configuration file to read"))
        void config_(string v) {
            config = Path(v);
        }

        Path config = "config/remote.json";

        @(NamedArgument("history").Required().Description("History file to summarize"))
        string history;
    }

    @(Command("test_slot_api"))
    struct TestSlotApiConfig {
    }

    @(Command("test_rag_sqlite"))
    struct TestRagSqliteConfig {
        @(NamedArgument("config").Required().Description("configuration file to read"))
        string config;
    }

    @(Command("print_tools"))
    struct FuncCallPrint {
    }

    @(Command("print_tool_metrics"))
    struct PrintToolMetricsConfig {
        @(NamedArgument("data").Required().Description("Metric data file to read"))
        void data_(string v) {
            data = Path(v);
        }

        Path data;

        @(NamedArgument("number", "n").Description("Number of tools to print"))
        int number;
    }

    @(Command("test_pipeline"))
    struct TestPipelineConfig {
        @(NamedArgument("config").Required().Description("configuration file to read"))
        string config;
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

int appMain(UserConfig uconf, UserConfig.PrintOk conf) {
    return 0;
}

int appMain(UserConfig uconf, UserConfig.ChatTestConfig conf) {
    import std.file : readText;
    import llm.config;
    import llm.chat;
    import llm.query;

    auto llmConf = readConfig(conf.config, false, true).userToLlmConfig(conf);
    llmConf.codeModel.server.httpVerbosity = 2;
    Chat chat;
    chat.add(Message(Role.system, readText(llmConf.codeModel.prompt)));
    chat.add(Message(Role.user, "who are you"));

    auto requester = LlmRequester(llmConf.codeModel.toRequestConfig);
    auto resp = requester.request(chat);
    logger.info(resp);
    resp.match!((JSONValue j) {
        logger.info(j);
        foreach (a; j["choices"].array.retro.take(1)) {
            chat.add(Message(Role.assistant, a["message"]["content"].str));
        }
    }, (LlamaRequestError e) { logger.warning(e); });

    chat.add(Message(Role.user, "what is your age"));
    resp = requester.request(chat);
    logger.info(resp);

    return 0;
}

int appMain(UserConfig uconf, UserConfig.SummaryTestConfig conf) {
    import std.file : readText;
    import llm.chat;
    import llm.config;
    import llm.query;
    import llm.summary_agent;

    auto llmConf = readConfig(conf.config, false, true).userToLlmConfig(conf);
    llmConf.summaryModel.server.httpVerbosity = 2;

    Chat chat;
    chat.load(readText(conf.history).parseJSON);

    auto summary = SummaryAgent(llmConf.summaryModel);
    auto res = summary.compress(chat);
    writeln(res);
    writeln(chat);

    return 0;
}

int appMain(UserConfig uconf, UserConfig.TestSlotApiConfig conf) {
    import llm.agent;
    import llm.config;
    import llm.chat;
    import llm.query;

    auto llmConf = readConfig(Path("config/remote.json"), false, true).userToLlmConfig(conf);
    auto slot = LlmSlotRequester(llmConf.codeModel.server.toSlotUrl,
            llmConf.codeModel.server.apiKey.empty ? getEnvApiKey() : llmConf
                .codeModel.server.apiKey);
    auto res = slot.request();
    res.match!((JSONValue j) { writeln(j.toPrettyString); }, (LlamaRequestError e) {
        writeln("error: ", e);
    });

    return 0;
}

int appMain(UserConfig uconf, UserConfig.FuncCallPrint conf) {
    import llm.tool_call;
    import llm.tool_call.io;
    import llm.tool_call.sandbox;
    import llm.tool_call.memory;
    import llm.tool_call.think;
    import llm.config;

    auto llmConf = readConfig(Path(), false, true).userToLlmConfig(conf);

    writeln(getFunctions);

    class DummyCtx : Context {
    }

    static class DummyContext : SandboxContext, FileContext, MemoryContext, ThinkingContext {
        override bool isPathInsideWorkArea(AbsolutePath p) {
            return true;
        }

        override AbsolutePath workArea() {
            return AbsolutePath(".");
        }

        override string getContainerCmd() {
            return "docker";
        }

        override string[] getMemoryFileTopics() {
            import std.file : dirEntries, SpanMode;
            import std.path : stripExtension, baseName;

            try {
                return dirEntries("scratch/memory", SpanMode.shallow).map!(
                        a => a.name.baseName.stripExtension).array;
            } catch (Exception e) {
                logger.warning("unable to read file area for memory topics: ", e.msg);
            }
            return null;
        }

        override Path getMemoryFile(string topic) {
            return Path("scratch/memory") ~ (topic ~ ".md");
        }

        override Path getThinkingTemplatesDir() {
            return Path("scratch/thinking");
        }

        override ToolLimits getToolLimits() {
            return ToolLimits();
        }

        override void taskDone() {
    }

    auto ctx = new DummyContext;

    // writeln(getMemoryTopics(ctx));
    writeln(listThinkingTemplates(ctx));
    // writeln(getThinkingTemplate(ctx, "system_design"));

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

int appMain(UserConfig uconf, UserConfig.TestPipelineConfig conf) {
    import std.file : readText;
    import std.stdio : writeln;
    import llm.config;
    import llm.agent;
    import llm.pipeline;
    import llm.metric.monitor : MetricMonitor;

    auto llmConf = readConfig(conf.config.Path, false, true).userToLlmConfig(conf);
    auto monitor = new MetricMonitor(llmConf.scratchArea ~ "monitor_pipeline_test.jsonl");

    auto writer = new Agent("writer", llmConf, monitor);
    writer.setSystemPrompt("You are a technical writer. Write a short, clear summary on the given topic. " ~ "Focus on accuracy and readability." ~ "You **MUST** call the tool pipelineOutput with the result and **THEN** call taskDone. Never call taskDone before pipelineOutput.");

    auto reviewer = new Agent("reviewer", llmConf, monitor);
    reviewer.setSystemPrompt("You are a harsh reviewer. Evaluate the following draft.\n\n" ~ "If the draft is good, output: APPROVED: <the draft text>\n" ~ "If the draft has issues, output: REJECT: <specific feedback for rewriting>\n" ~ "You **MUST** call the tool pipelineOutput with the result of the review and **THEN** call taskDone. Never call taskDone before pipelineOutput.");

    auto finalizer = new Agent("finalizer", llmConf, monitor);
    finalizer.setSystemPrompt("You are a final editor. Take the approved text and format it nicely " ~ "with a title and clean structure. Output the final version only." ~ "You **MUST** call the tool pipelineOutput with the result of the final edit and **THEN** call taskDone. Never call taskDone before pipelineOutput.");

    // --- Transition condition: only forward if reviewer approved ---
    //
    // The condition delegate receives:
    //   output  — the output from the source node (reviewer)
    //   fromId  — source node ID ("reviewer")
    //   toId    — target node ID ("finalizer")
    int fallback;
    bool reviewerApproved(string out_, Node from, Node to) {
        logger.trace("fallback: ", fallback);
        return out_.startsWith("APPROVED:") || fallback++ > 10;
    };

    auto pipeline = pipelineBuilder.addNode("writer", writer).addNode("reviewer", reviewer)
        .addNode("finalizer", finalizer).addEdge("writer", "reviewer").addEdge("reviewer", "finalizer",
            &reviewerApproved).addEdge("reviewer", "writer", null, 3u)
        .startNode("writer").stopNode("finalizer").build();
    writeln(pipeline);

    // string query = "Explain how BFS graph traversal works";
    string query = "count from 1 to 5. Only output 1,2,3,4,5 and nothing else";
    auto result = pipeline.run(query);

    writeln("Pipeline completed: allSuccess=", result.allSuccess);
    writeln("Execution order: ", result.executionOrder);
    writeln("Total duration: ", result.totalDurationMs, "ms");
    writeln("Final output:\n", result.finalOutput);

    // Print per-agent results
    foreach (ar; result.agentResults) {
        writeln("  Agent: ", ar.agentName, " | success: ", ar.success, " | duration: ",
                ar.durationMs, "ms", " | output length: ", ar.output.length);
    }

    return 0;
}

int appMain(UserConfig uconf, UserConfig.TestRagSqliteConfig conf) {
    import llm.rag.database;
    import llm.config;
    import llm.rag.embedder : createEmbedder;
    import llm.rag.rag;
    import d2sqlite3 : ResultRange;

    auto llmConf = readConfig(conf.config.Path, false, true).userToLlmConfig(conf);
    auto embedder = createEmbedder(llmConf.embedConfig);

    // auto db = openDatabase("smurf.sqlite3".Path, 768);
    auto rag = new RAG(embedder, [
        RagDatabaseConfig("smurf.sqlite3".Path, ""),
        RagDatabaseConfig("llmfun/data/rag.sqlite3".Path, "")
    ], llmConf.embedConfig.match!((RemoteEmbedConfig a) => a.dimensions,
            (LocalEmbedConfig a) => a.dimensions));
    // logger.warning(llmConf);
    scope (exit)
        rag.destroy;

    auto query = "planner";

    auto result = rag.queryBestMatch(query, 10);
    logger.info("best match: ", result);

    // result = rag.querySemantic(query, 10);
    // logger.info("semantic: ", result);

    // result = rag.queryTextSearch(query, 10);
    // logger.info("text: ", result);

    bool process(ResultRange result) {
        foreach (a; result.enumerate) {
            writefln("%s: %s", a.index, a.value);
        }
        return true;
    }
    // rag.db.fts5Rebuild;
    rag.dbs[0].run("SELECT count(*) FROM TextChunkTbl", &process);

    // rag.run("SELECT vec_version()", &process);

    // auto src = Source(Origin(Path("smurf.txt")), 4242.SourceChecksum);
    // logger.info("Add source: ", src);
    // auto srcId = rag.addSource(src);
    //
    // logger.info("Add embeddings");
    // float[] embed = [0.1, 0.2, 0.3, 0.4];
    // rag.addEmbedding(srcId, Embedding(Offset(42, 84), "here we are", embed));
    //
    logger.info("Result");
    // rag.run("SELECT rowid,rank, snippet(FtsChunksTbl, '<<', '>>') AS snippet_text FROM FtsChunksTbl WHERE text MATCH 'dlang' LIMIT 5",
    //         &process);
    rag.db.run("SELECT * from SourceTbl", &process);
    // db.run("SELECT * from EmbeddingsTbl", &process);
    {
        // auto stmt = rag.prepare("SELECT * from EmbeddingsTbl");
    }

    // logger.info("Add same source, should ignore it");
    // rag.addSource(src);
    // rag.run("SELECT * from SourceTbl", &process);
    // rag.run("SELECT * from EmbeddingsTbl", &process);
    //
    // logger.info("Search");
    // auto searchResult = rag.getBestMatch(Search(embed), 3);
    // logger.info(searchResult);
    //
    // logger.info("should now be deleted");
    // logger.info(rag.getSource(src.origin));
    // rag.removeSource(src.origin);
    // rag.run("SELECT * from EmbeddingsTbl", &process);
    // rag.run("SELECT * from EmbeddingsTbl_rowids", &process);

    logger.info("Sources");
    foreach (a; rag.getSources)
        logger.info(a);

    return 0;
}
