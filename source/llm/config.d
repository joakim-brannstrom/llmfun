module llm.config;

import logger = std.logger;
import std.algorithm : filter, map;
import std.array : array, empty;
import std.file : readText, exists, mkdirRecurse;
import std.format : format;
import std.json : JSONValue, JSONType, parseJSON;
import std.sumtype : SumType;
import std.sumtype : match;

import my.path;

import llm.query : RequestConfig;

immutable ProgramName = "llmfun";

struct LlmConfig {
    Path dataDir = ProgramName ~ "/data";

    // LLM save a memory to this file which is used between runs.
    Path memoryArea = ProgramName ~ "/data/memory";

    Path scratchArea = ProgramName ~ "/data/scratch";

    Path thinkingTemplatesDir = ProgramName ~ "/config/thinking";

    Path promptDir = ProgramName ~ "/config/prompt";

    Path[] rag = [ProgramName ~ "/data/rag.sqlite3"];

    void resolvePaths() {
        import my.resource;
        import my.optional;

        // prio local dirs but VERY important which files are allowed to override
        // because there could be malicious configs in the cwd path.
        auto prioDataCwdDirs = AbsolutePath(ProgramName ~ "/data") ~ dataSearch(ProgramName);
        auto prioConfCwdDirs = AbsolutePath(ProgramName ~ "/config") ~ configSearch(ProgramName);

        memoryArea = prioDataCwdDirs.resolve("memory".Path)
            .orElse(ResourceFile(memoryArea.AbsolutePath)).get.Path;

        if (rag.length >= 1) {
            rag[0] = prioDataCwdDirs.resolve("rag.sqlite3".Path)
                .orElse(ResourceFile(rag[0].AbsolutePath)).get.Path;
        }

        scratchArea = prioDataCwdDirs.resolve("scratch".Path)
            .orElse(ResourceFile(scratchArea.AbsolutePath)).get.Path;

        thinkingTemplatesDir = prioConfCwdDirs.resolve("thinking".Path)
            .orElse(ResourceFile(thinkingTemplatesDir.AbsolutePath)).get.Path;

        promptDir = prioConfCwdDirs.resolve("prompt".Path)
            .orElse(ResourceFile(thinkingTemplatesDir.AbsolutePath)).get.Path;
    }

    // Directory where the LLM can work with assets, create files etc.
    Path workArea = ProgramName ~ "/workarea";

    string containerCmd = "podman";

    ToolFilter toolFilter;
    RagFilter ragFilter;

    CodeModelConfig codeModel;
    SummaryModelConfig summaryModel;

    EmbedConfig embedConfig;
    long embedDimensions() {
        return embedConfig.match!((LocalEmbedConfig a) => a.dimensions,
                (RemoteEmbedConfig a) => a.dimensions);
    }
}

Path promptToPath(LlmConfig conf, string prompt) {
    return conf.promptDir ~ prompt;
}

LlmConfig makeLlmConfig() {
    LlmConfig conf;
    conf.resolvePaths;
    return conf;
}

void makeFileStructure(LlmConfig conf, bool rag = false) {
    import std.file : mkdirRecurse;

    foreach (path; ([conf.scratchArea, conf.workArea] ~ (rag ? [conf.dataDir] : null)).filter!(
            a => !a.exists)) {
        try {
            logger.info("Creating directory ", path);
            mkdirRecurse(path);
        } catch (Exception e) {
            logger.warning(e);
        }
    }
}

struct ToolFilter {
    import my.filter : ReFilter;

    string[] include;
    string[] exclude;

    ReFilter to() @safe {
        return ReFilter(include, exclude);
    }
}

struct RagFilter {
    import my.filter : ReFilter;

    string[] include = [".*\\.txt", ".*\\.md"];
    string[] exclude;

    ReFilter to() @safe {
        return ReFilter(include, exclude);
    }
}

struct ServerConfig {
    string url;
    string promptUrl;
    string chatUrl;
    string slotUrl;
    string embedUrl;
    long timeoutSeconds;
    long httpVerbosity;
    bool verifySslCert = true;
    bool keepAlive = true;
    long maxRetries = 3; // maximum number of retries for transient failures
    long backoffMs = 500; // initial backoff in milliseconds (exponential)

    /// API key for Bearer token authentication (e.g. OpenAI API key).
    /// If empty, the OPENAI_API_KEY environment variable is checked as fallback.
    /// Leave empty for servers that do not require authentication (e.g. local llama.cpp).
    string apiKey;

    string toChatUrl() {
        return format!"%s/%s"(url, chatUrl);
    }

    string toPromptUrl() {
        return format!"%s/%s"(url, promptUrl);
    }

    string toSlotUrl() {
        return format!"%s/%s"(url, slotUrl);
    }

    string toEmbedUrl() {
        return format!"%s/%s"(url, embedUrl);
    }
}

struct CodeModelConfig {
    ServerConfig server;
    string name;
    string prompt = "AGENT.md";
    double temp;
    long contextSize;
    long maxTokens;
    long reasoningBudget;
    bool preserveThinking;
}

struct SummaryModelConfig {
    ServerConfig server;
    string name;
    string prompt = "SUMMARY.md";
    double temp;
    long contextSize;
    long contextChunkSize = 32768;
    long reasoningBudget;
    bool preserveThinking;
    long maxTokens;
}

/// Configuration for a local embedding backend (llama.cpp).
struct LocalEmbedConfig {
    Path modelPath;
    long context;
    long nBatch = 512;
    long dimensions = 768;
}

/// Configuration for a remote embedding backend (HTTP API).
struct RemoteEmbedConfig {
    ServerConfig server;
    string name;
    long nBatch = 512;
    long dimensions = 768;
}

/// Union type for embedding backend configuration.
alias EmbedConfig = SumType!(RemoteEmbedConfig, LocalEmbedConfig);

RequestConfig toRequestConfig(ConfigT)(ConfigT conf) {
    // dfmt off
    return RequestConfig(
         chatUrl: conf.server.toChatUrl,
         promptUrl: conf.server.toPromptUrl,
         slotUrl: conf.server.toSlotUrl,
         timeoutS: cast(int) conf.server.timeoutSeconds,
         verifySslCert: conf.server.verifySslCert,
         keepAlive: conf.server.keepAlive,
         verbosity: cast(int) conf.server.httpVerbosity,
         apiKey: conf.server.apiKey.empty ? getEnvApiKey() : conf.server.apiKey,
         chat: RequestConfig.Chat(model: conf.name,
                                  max_tokens: conf.maxTokens,
                                  temperature: conf.temp,
                                  reasoning_budget: conf.reasoningBudget,
                                  preserve_thinking: conf.preserveThinking));
    // dfmt on
}

LlmConfig readConfig(Path path, bool silent = false) {
    import std.process : environment;

    auto conf = makeLlmConfig();

    if (path.empty) {
        path = environment.get("LLMFUN_DEFAULT_CONFIG", "").Path;
    }
    if (path.exists) {
        logger.infof(!silent, "Reading configuration from %s", path);
        return jsonToLlmConfig(conf, readText(path.toString).parseJSON);
    }
    logger.infof("No configuration at %s. Using default values", path);
    return conf;
}

private EmbedConfig jsonToEmbedConfig(JSONValue json) {
    import std.exception : enforce;

    if ("type" !in json) {
        throw new Exception("embedConfig JSON missing required field 'type'");
    }

    string type = json["type"].str;
    json.object.remove("type");
    if (type == "remote") {
        return EmbedConfig(jsonToConfig!(RemoteEmbedConfig)(RemoteEmbedConfig.init, json));
    }
    if (type == "local") {
        return EmbedConfig(jsonToConfig!(LocalEmbedConfig)(LocalEmbedConfig.init, json));
    }
    throw new Exception("embedConfig: unknown type '" ~ type ~ "', expected 'remote' or 'local'");
}

auto jsonToConfig(ConfigT)(ConfigT conf, JSONValue json) {
    import std.traits;

    logger.trace("read json config start: " ~ ConfigT.stringof);
    bool[string] used;

    static foreach (llmMemberName; __traits(allMembers, ConfigT)) {
        {
            alias member = __traits(getMember, conf, llmMemberName);
            static if (!isType!member) {
                if (llmMemberName in json) {
                    try {
                        logger.tracef("using config value for %s:%s - %s",
                                ConfigT.stringof, llmMemberName, json[llmMemberName]);

                        used[llmMemberName] = true;
                        alias Type = typeof(member);
                        static if (is(Type : Path)) {
                            __traits(getMember, conf, llmMemberName) = json[llmMemberName].str.Path;
                        } else static if (is(Type == Path[])) {
                            auto val = json[llmMemberName];
                            if (val.type == JSONType.STRING) {
                                __traits(getMember, conf, llmMemberName) = [
                                    val.str.Path
                                ];
                            } else {
                                __traits(getMember, conf, llmMemberName) = val.array.map!(a => a.str.Path)
                                    .array;
                            }
                        } else static if (is(Type : string)) {
                            __traits(getMember, conf, llmMemberName) = json[llmMemberName].str;
                        } else static if (is(Type : bool)) {
                            __traits(getMember, conf, llmMemberName) = json[llmMemberName].boolean;
                        } else static if (isFloatingPoint!Type) {
                            __traits(getMember, conf, llmMemberName) = json[llmMemberName].floating;
                        } else static if (isIntegral!Type) {
                            __traits(getMember, conf, llmMemberName) = cast(Type) json[llmMemberName]
                                .integer;
                        } else static if (is(Type : string[])) {
                            __traits(getMember, conf, llmMemberName) = json[llmMemberName].array.map!(a => a.str)
                                .array;
                        } else static if (is(Type : EmbedConfig)) {
                            __traits(getMember, conf, llmMemberName) = jsonToEmbedConfig(
                                    json[llmMemberName]);
                        } else static if (isAggregateType!Type) {
                            __traits(getMember, conf, llmMemberName) = jsonToConfig(__traits(getMember,
                                    conf, llmMemberName), *(llmMemberName in json));
                        }
                    } catch (Exception e) {
                        logger.warningf("unable to read '%s': %s", llmMemberName, e.msg);
                    }
                } else {
                    logger.tracef("using default value for %s:%s", ConfigT.stringof, llmMemberName);
                }
            }
        }
    }

    foreach (k; json.object.byKey.filter!(a => a !in used)) {
        logger.warningf("Unknown json configuration %s:%s", ConfigT.stringof, k);
    }

    logger.trace("read json config done: " ~ ConfigT.stringof);
    return conf;
}

alias jsonToLlmConfig = jsonToConfig!LlmConfig;

/// Returns the OpenAI API key from the OPENAI_API_KEY environment variable, or "" if not set.
string getEnvApiKey() {
    import std.process : environment;

    return environment.get("OPENAI_API_KEY", null);
}
