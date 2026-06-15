module llm.config;

import logger = std.logger;
import std.algorithm : filter, map;
import std.array : array, empty, appender;
import std.conv : to;
import std.file : readText, exists, mkdirRecurse;
import std.format : format;
import std.json : JSONValue, JSONType, parseJSON;
import std.sumtype : SumType;
import std.sumtype : match;
import std.string : toLower, startsWith;

import my.path;

import llm.query : RequestConfig;

immutable ProgramName = "llmfun";

struct RagDatabaseConfig {
    Path path;
    string description;
}

struct LlmConfig {
    Path dataDir = ProgramName ~ "/data";

    // LLM save a memory to this file which is used between runs.
    Path memoryArea = ProgramName ~ "/data/memory";

    Path scratchArea = ProgramName ~ "/data/scratch";

    Path thinkingTemplatesDir = ProgramName ~ "/config/thinking";

    Path promptDir = ProgramName ~ "/config/prompt";

    RagDatabaseConfig ragPrimary = RagDatabaseConfig((ProgramName ~ "/data/rag.sqlite3")
            .Path, "Read/write database containing temporary data");
    RagDatabaseConfig[][string] ragSecondary;

    void resolvePaths() {
        import my.resource;
        import my.optional;

        // prio local dirs but VERY important which files are allowed to override
        // because there could be malicious configs in the cwd path.
        auto prioDataCwdDirs = AbsolutePath(ProgramName ~ "/data") ~ dataSearch(ProgramName);
        auto prioConfCwdDirs = AbsolutePath(ProgramName ~ "/config") ~ configSearch(ProgramName);

        memoryArea = prioDataCwdDirs.resolve("memory".Path)
            .orElse(ResourceFile(memoryArea.AbsolutePath)).get.Path;

        // Only ragPrimary is resolved
        ragPrimary.path = prioDataCwdDirs.resolve("rag.sqlite3".Path)
            .orElse(ResourceFile(ragPrimary.path.AbsolutePath)).get.Path;

        scratchArea = prioDataCwdDirs.resolve("scratch".Path)
            .orElse(ResourceFile(scratchArea.AbsolutePath)).get.Path;

        thinkingTemplatesDir = prioConfCwdDirs.resolve("thinking".Path)
            .orElse(ResourceFile(thinkingTemplatesDir.AbsolutePath)).get.Path;

        promptDir = prioConfCwdDirs.resolve("prompt".Path)
            .orElse(ResourceFile(promptDir.AbsolutePath)).get.Path;
    }

    // Directory where the LLM can work with assets, create files etc.
    Path workArea = ProgramName ~ "/workarea";

    string containerCmd = "podman";

    ToolFilter toolFilter;
    RagFilter ragFilter;

    // Searched for in promptDir
    string agentPrompt = "AGENT.md";

    CodeModelConfig[] codeModels;
    long activeCodeModelIndex = 0;
    SummaryModelConfig summaryModel;

    EmbedConfig embedConfig;
    long embedDimensions() const @safe {
        return embedConfig.match!((LocalEmbedConfig a) => a.dimensions,
                (RemoteEmbedConfig a) => a.dimensions);
    }

    // --- Multi-model methods ---

    /// Return the currently active code model config (value copy, no mutex needed).
    CodeModelConfig activeCodeModel() const @safe {
        if (codeModels.length == 0)
            throw new Exception("No code models configured");
        if (activeCodeModelIndex < 0 || activeCodeModelIndex >= codeModels.length)
            throw new Exception(format!"Active code model index %s is out of bounds (count: %s)"(
                    activeCodeModelIndex, codeModels.length));
        return codeModels[activeCodeModelIndex];
    }

    /// Return the name of the active model.
    string activeModelName() @safe const {
        return activeCodeModel().name;
    }

    /// Select model by index. Returns true on success, false if index out of bounds.
    bool selectModelByIndex(long index) @safe {
        if (index >= codeModels.length) {
            logger.warningf("Invalid model index %s. Available models: 0-%s",
                    index, codeModels.length - 1);
            return false;
        }
        activeCodeModelIndex = index;
        saveState();
        return true;
    }

    /// Select model by name (case-insensitive partial match). Returns empty string on success, error message on failure.
    string selectModelByName(string name) @safe {
        import std.algorithm : count;

        if (name.empty) {
            return "Model name cannot be empty";
        }

        auto lowerName = name.toLower;
        size_t matchCount = 0;
        size_t matchIndex = size_t.max;

        foreach (i, model; codeModels) {
            if (model.name.toLower == lowerName) {
                matchCount++;
                matchIndex = i;
            }
        }

        if (matchCount == 0) {
            return format!"No model matches '%s'. Available models: %s"(name,
                    codeModels.map!(m => m.name));
        }
        if (matchCount > 1) {
            return format!"Ambiguous model name '%s'. Matches: %s"(name,
                    codeModels.filter!(m => m.name.toLower == lowerName)
                        .map!(m => m.name));
        }

        activeCodeModelIndex = matchIndex;
        saveState();
        return "";
    }

    /// List all configured model names with index and active indicator.
    string[] listModels() const @safe {
        auto app = appender!(string[])();
        foreach (i, model; codeModels) {
            app.put(format!"%s (index: %s)%s"(model.name, i,
                    (i == activeCodeModelIndex ? " [active]" : "")));
        }
        return app.data;
    }

    /// Load state from llmfun/data/state.json. Silently ignores errors.
    void loadState() @safe {
        Path stateFile = dataDir ~ "state.json";
        if (!stateFile.exists) {
            return;
        }

        try {
            auto json = stateFile.readText.parseJSON;
            if ("activeCodeModelIndex" in json) {
                auto idxVal = json["activeCodeModelIndex"].integer;
                if (idxVal < 0) {
                    logger.tracef("Invalid negative activeCodeModelIndex: %s", idxVal);
                } else {
                    auto idx = cast(size_t) idxVal;
                    if (idx < codeModels.length) {
                        activeCodeModelIndex = idx;
                    }
                }
            }
        } catch (Exception e) {
            logger.tracef("Failed to load state: %s", e.msg);
        }
    }

    /// Save state to llmfun/data/state.json. Only saves if directory exists.
    void saveState() const @safe {
        import std.stdio : File;

        if (!dataDir.exists) {
            return;
        }

        try {
            auto stateFile = dataDir ~ "state.json";
            JSONValue stateObj;
            stateObj["activeCodeModelIndex"] = activeCodeModelIndex;
            File(stateFile.toString, "w").writeln(stateObj.toString);
        } catch (Exception e) {
            logger.tracef("Failed to save state: %s", e.msg);
        }
    }

    RagDatabaseConfig[] getRagDatabases() @safe {
        import std.algorithm : joiner;

        return [ragPrimary] ~ ragSecondary.byValue.joiner.array;
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
    string promptUrl = "v1/completion";
    string chatUrl = "v1/chat/completions";
    string slotUrl = "slots";
    string embedUrl = "v1/embeddings";
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

LlmConfig readConfig(Path path, bool silent = false, bool noCwdConfig = false) {
    import std.process : environment;

    auto conf = makeLlmConfig();
    bool loadedAnyFile = false;

    // Layer 1: Base config from LLMFUN_DEFAULT_CONFIG
    auto basePath = environment.get("LLMFUN_DEFAULT_CONFIG", "").Path;
    if (basePath.exists) {
        logger.infof(!silent, "Reading base configuration from %s", basePath);
        try {
            conf = jsonToLlmConfig(conf, readText(basePath.toString).parseJSON);
            loadedAnyFile = true;
        } catch (Exception e) {
            logger.errorf(!silent, "Failed to parse base config %s: %s", basePath, e.msg);
        }
    } else {
        logger.infof(!silent,
                "No base configuration found (LLMFUN_DEFAULT_CONFIG not set or file missing)");
    }

    // Layer 2: Overlay config
    Path overlayPath;
    if (!path.empty) {
        overlayPath = path; // from -c/--config
    } else if (!noCwdConfig) {
        overlayPath = Path(".llmfun.json");
    } else {
        logger.infof(!silent, "Skipping project configuration (--no-cwd-config)");
    }

    if (!overlayPath.empty && overlayPath.exists) {
        logger.infof(!silent, "Reading project configuration from %s", overlayPath);
        try {
            conf = jsonToLlmConfig(conf, readText(overlayPath.toString).parseJSON);
            loadedAnyFile = true;
        } catch (Exception e) {
            logger.errorf(!silent, "Failed to parse project config %s: %s", overlayPath, e.msg);
        }
    } else if (!overlayPath.empty) {
        logger.infof(!silent, "No project configuration found at %s", overlayPath);
    }

    // Validation guard
    if (loadedAnyFile) {
        validateConfig(conf);
    }

    conf.loadState();
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

    void validateRagDatabase(JSONValue elem) {
        // Object format: {"path": "...", "description": "..."}
        if ("path" !in elem) {
            throw new Exception("rag entry missing required field 'path'");
        }
        auto pathVal = elem["path"];
        if (pathVal.type != JSONType.STRING) {
            throw new Exception("rag 'path' must be a string");
        }
        if ("description" in elem) {
            auto descVal = elem["description"];
            if (descVal.type != JSONType.STRING) {
                throw new Exception("rag 'description' must be a string");
            }
        }
    }

    logger.trace("read json config start: " ~ ConfigT.stringof);
    bool[string] used;

    static foreach (llmMemberName; __traits(allMembers, ConfigT)) {
        {
            alias member = __traits(getMember, conf, llmMemberName);
            static if (!isType!member) {
                alias Type = typeof(member);
                if (llmMemberName in json) {
                    try {
                        logger.tracef("using config value for %s:%s - %s",
                                ConfigT.stringof, llmMemberName, json[llmMemberName]);

                        used[llmMemberName] = true;
                        static if (is(Type : Path)) {
                            __traits(getMember, conf, llmMemberName) = json[llmMemberName].str.Path;
                        } else static if (is(Type == RagDatabaseConfig)) {
                            auto elem = json[llmMemberName];
                            validateRagDatabase(elem);
                            string path = elem["path"].str;
                            string desc = elem["description"].str;
                            __traits(getMember, conf, llmMemberName) = RagDatabaseConfig(path.Path,
                                    desc);
                        } else static if (is(Type == RagDatabaseConfig[][string])) {
                            foreach (key, ref JSONValue dbs; json[llmMemberName].object) {
                                RagDatabaseConfig[] configs;
                                foreach (db; dbs.array) {
                                    validateRagDatabase(db);
                                    string path = db["path"].str;
                                    string desc = db["description"].str;
                                    configs ~= RagDatabaseConfig(path.Path, desc);
                                }
                                __traits(getMember, conf, llmMemberName)[key] = configs;
                            }
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
                        } else static if (is(Type == CodeModelConfig[])) {
                            __traits(getMember, conf, llmMemberName) = json[llmMemberName].array.map!(
                                    a => jsonToConfig(CodeModelConfig.init, a)).array;
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

/// Validate LlmConfig after JSON parsing. Throws on validation failure.
void validateConfig(LlmConfig conf) {
    if (conf.codeModels.length <= 0)
        throw new Exception(
                "No code models configured. 'codeModels' array or 'codeModel' object is required in configuration.");

    // Validate activeCodeModelIndex is within bounds
    if (conf.activeCodeModelIndex < 0 || conf.activeCodeModelIndex >= conf.codeModels.length)
        throw new Exception(format!"activeCodeModelIndex %s is out of bounds (codeModels count: %s)"(
                conf.activeCodeModelIndex, conf.codeModels.length));

    // Validate each CodeModelConfig has required fields
    foreach (i, model; conf.codeModels) {
        if (model.name.empty)
            throw new Exception(format!"codeModels[%s].name must not be empty"(i));
        if (model.server.url.empty)
            throw new Exception(format!"codeModels[%s].server.url must not be empty"(i));
    }
}

alias jsonToLlmConfig = jsonToConfig!LlmConfig;

/// Returns the OpenAI API key from the OPENAI_API_KEY environment variable, or "" if not set.
string getEnvApiKey() {
    import std.process : environment;

    return environment.get("OPENAI_API_KEY", null);
}
