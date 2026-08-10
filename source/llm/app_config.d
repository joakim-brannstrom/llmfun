/// CLI configuration structs and config conversion utilities.
module llm.app_config;

import logger = std.logger;
import std.array : empty;
import std.sumtype : match;

import argparse : CLI, NamedArgument, PositionalArgument, Command, Description,
    Required, Optional, Parse, SubCommand, Placeholder, Default, MutuallyExclusive;
import my.path;
import colorlog : VerboseMode;

import llm.config : RagDatabaseConfig, LlmConfig, EmbedConfig, RemoteEmbedConfig;
import llm.rag.rag : RAG;

struct UserConfig {
    SubCommand!(Default!AgentChatConfig, Rag, PrintToolMetricsConfig, Mcp) cmd;

    @(NamedArgument("v", "verbose").Description("Log verbosity level"))
    VerboseMode verbosity;

    @(NamedArgument("config", "c").Description("Configuration file to read"))
    void config_(string v) {
        config = Path(v);
    }

    Path config;

    @(NamedArgument("no-cwd-config")
            .Description("Do not read .llmfun.json from current directory (security)"))
    bool noCwdConfig;

    @(NamedArgument("trusted-config")
            .Description("Allow loading .llmfun.json from CWD when workarea equals CWD"))
    bool trustedConfig;

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

        @(NamedArgument("no-memory").Description("Deactivate the persistent read/write memory"))
        bool noMemory;
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
                .Optional.Description("Paths for the arguments --add/--rm/--list/--sync"))
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

    @(Command("mcp"))
    struct Mcp {
        @MutuallyExclusive() {
            @(NamedArgument("stdio").Description("Use stdio transport"))
            bool stdio;

            @(NamedArgument("list-tools").Description("List available tools and exit (diagnostic)"))
            bool listTools;
        }

        @(NamedArgument("host").Description("Host for HTTP transport (future)"))
        string host = "127.0.0.1";

        @(NamedArgument("port").Description("Port for HTTP transport (future)"))
        int port = 8787;
    }
}

LlmConfigT userToLlmConfig(LlmConfigT, ConfigT)(LlmConfigT llm, ConfigT conf) {
    import logger = std.logger;

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
                    } else static if (is(Type : bool)) {
                        if (__traits(getMember, conf, confMemberName)) {
                            __traits(getMember, llm, llmMemberName) = __traits(getMember,
                                    conf, confMemberName);
                        }
                    } else {
                        static assert(0,
                                "unknown conversion of field " ~ llmMemberName
                                ~ " type " ~ Type.stringof);
                    }
                }
            }
        }
    }
    logger.trace(llm);
    return llm;
}

/// Create a RAG instance with fallback to default embedder on failure.
RAG createRag(LlmConfig conf) {
    import llm.common.embedder : createEmbedder;
    import llm.rag.rag;

    try {
        auto embed = createEmbedder(conf.embedConfig);
        if (embed is null) {
            logger.warningf("Unable to create an embedder from the configuration: %s",
                    conf.embedConfig);
        } else {
            return new RAG(embed, conf.ragPrimary, conf.getRagSecondary);
        }
    } catch (Exception e) {
        logger.warningf("Failed to create embedder with configured settings: %s", e.msg);
        return null;
    }

    return null;
}
