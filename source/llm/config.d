module llm.config;

import logger = std.logger;
import std.algorithm : filter, map;
import std.array : array, empty, appender, empty;
import std.conv : to;
import std.file : readText, exists, mkdirRecurse, rename;
import std.format : format;
import std.datetime : Clock;
import std.json : JSONValue, JSONType, parseJSON;
import std.sumtype : SumType, match;
import std.string : toLower, startsWith;

import my.path;

import llm.query : RequestConfig;
public import llm.common.embedder;
public import llm.common.config;

immutable ProgramName = "llmfun";

/// Minimum interval (in seconds) between session count increments.
private enum SessionCountMinIntervalSec = 3 * 3600;

struct RagDatabaseConfig {
    Path path;
    string description;
}

struct ToolLimits {
    long readFileMaxLines = 20;
    long editFileMaxLines = 80;
    long maxDirEntries = 50;
    long grepMaxResults = 1000;
    long maxSummaryLength = 200;
    long maxTopicLength = 100;
    long maxTopK = 20;
    long maxArgLength = 200;
}

struct RagConfig {
    /// Sliding window overlap as percentage (0-99).
    /// 50 means each chunk overlaps 50% with the previous one.
    /// Must be in range [0, 99]. Value of 100 would cause infinite loop.
    long windowOverlapPercent = 50;

    invariant {
        assert(windowOverlapPercent >= 0 && windowOverlapPercent <= 99,
                "windowOverlapPercent must be in range [0, 99], got "
                ~ windowOverlapPercent.to!string);
    }
}

struct LlmConfig {
    Path dataDir = ProgramName ~ "/data";

    // LLM save a memory to this file which is used between runs.
    Path[] memoryArea;

    bool noMemory;

    Path scratchArea = ProgramName ~ "/data/scratch";

    Path[] thinkingTemplatesDir;

    Path[] promptDir;

    Path[] skillPaths;
    long maxManifestSkills = 200;
    long maxAlwaysApplyTokens = 4000;
    bool disableSkills = false;

    RagDatabaseConfig ragPrimary = RagDatabaseConfig((ProgramName ~ "/data/rag.sqlite3").Path,
            "Recent project source code, documentation and files added with tools loadFileToRAG, loadContentToRAG");
    RagDatabaseConfig[][string] ragSecondary;

    void resolvePaths(bool cwdConfig) {
        import my.resource;
        import my.optional;

        AbsolutePath[] prioDataCwdDirs = dataSearch(ProgramName);
        AbsolutePath[] prioConfCwdDirs = configSearch(ProgramName);
        if (cwdConfig) {
            prioDataCwdDirs = AbsolutePath(ProgramName ~ "/data") ~ prioDataCwdDirs;
            prioConfCwdDirs = AbsolutePath(ProgramName ~ "/config") ~ prioConfCwdDirs;
        }

        // only use cwd and closest directory.
        // This is based on the assumption that if a user create the directory
        // "memory" on purpose in the llmfun directory they want all memories
        // to be in that directory and not in any other writable memory
        // directory.
        if (memoryArea.empty) {
            if (cwdConfig)
                memoryArea ~= (ProgramName ~ "/data/memory").Path;
            dataSearch(ProgramName).resolve("memory".Path).match!((ResourceFile a) {
                memoryArea ~= a.get;
            }, (_) {});
        }

        if (thinkingTemplatesDir.empty) {
            thinkingTemplatesDir = prioConfCwdDirs.map!(a => cast(Path)(a ~ "thinking")).array;
        }

        if (promptDir.empty) {
            promptDir = prioConfCwdDirs.map!(a => cast(Path)(a ~ "prompt")).array;
        }

        if (skillPaths.empty) {
            if (cwdConfig)
                skillPaths ~= (ProgramName ~ "/skills").Path;
            dataSearch(ProgramName).resolve("skills".Path).match!((ResourceFile a) {
                skillPaths ~= a.get;
            }, (_) {});
        }

        scratchArea = prioDataCwdDirs.resolve("scratch".Path)
            .orElse(ResourceFile(scratchArea.AbsolutePath)).get.Path;
    }

    // Directory where the LLM can work with assets, create files etc.
    Path workArea = ProgramName ~ "/workarea";

    string containerCmd = "podman";

    ToolLimits toolLimits;

    ToolFilter toolFilter;
    RagFilter ragFilter;

    RagConfig ragConfig;

    // Searched for in promptDir
    string agentPrompt = "AGENT.md";

    CodeModelConfig[] codeModels;
    long activeCodeModelIndex = 0;

    // Tracks total session starts (incremented at the beginning of each session).
    uint sessionCount = 0;
    // Prevents concurrent or crash-retry consolidation. Cleared on load if stale.
    bool isConsolidating = false;
    // Default trigger threshold (every N sessions). 0 means disabled.
    uint consolidationInterval = 10;
    // Unix epoch seconds of the last session count increment. 0 means never incremented.
    long lastSessionCountUpdate = 0;

    SummaryModelConfig summaryModel;

    // If true, emit a warning when no API key is configured for a model server. Defaults to true.
    bool warnIfNoApiKey = true;

    invariant {
        assert(maxManifestSkills > 0,
                "maxManifestSkills must be positive, got " ~ maxManifestSkills.to!string);
        assert(maxAlwaysApplyTokens >= 0,
                "maxAlwaysApplyTokens must be non-negative (0 = unlimited), got "
                ~ maxAlwaysApplyTokens.to!string);
    }

    EmbedConfig embedConfig;
    long embedDimensions() const @safe {
        return embedConfig.match!((LocalEmbedConfig a) => a.dimensions,
                (RemoteEmbedConfig a) => a.dimensions);
    }

    /// Return: the currently active code model config (value copy, no mutex needed).
    CodeModelConfig activeCodeModel() const @safe {
        if (codeModels.length == 0)
            throw new Exception("No code models configured");
        if (activeCodeModelIndex < 0 || activeCodeModelIndex >= codeModels.length)
            throw new Exception(format!"Active code model index %s is out of bounds (count: %s)"(
                    activeCodeModelIndex, codeModels.length));
        return codeModels[activeCodeModelIndex];
    }

    /// Return: the name of the active model.
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
        return null;
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
            if ("sessionCount" in json) {
                sessionCount = cast(uint) json["sessionCount"].integer;
            }
            if ("isConsolidating" in json) {
                auto val = json["isConsolidating"].boolean;
                if (val) {
                    logger.warning("Found stale consolidation lock - clearing");
                }
                isConsolidating = false; // Always clear - stale lock recovery
            }
            if ("consolidationInterval" in json) {
                consolidationInterval = cast(uint) json["consolidationInterval"].integer;
            }
            if ("lastSessionCountUpdate" in json) {
                lastSessionCountUpdate = cast(long) json["lastSessionCountUpdate"].integer;
            }
        } catch (Exception e) {
            logger.tracef("Failed to load state: %s", e.msg);
        }
    }

    /// Save state to llmfun/data/state.json. Only saves if directory exists.
    void saveState() const @safe nothrow {
        import std.stdio : File;

        if (!dataDir.exists) {
            return;
        }

        try {
            auto stateFile = dataDir ~ "state.json";
            string tempFile = stateFile.toString ~ ".tmp";
            JSONValue stateObj;
            stateObj["activeCodeModelIndex"] = activeCodeModelIndex;
            stateObj["sessionCount"] = sessionCount;
            stateObj["isConsolidating"] = isConsolidating;
            stateObj["consolidationInterval"] = consolidationInterval;
            stateObj["lastSessionCountUpdate"] = lastSessionCountUpdate;
            File(tempFile, "w").writeln(stateObj.toString);
            rename(tempFile, stateFile);
        } catch (Exception e) {
            try {
                logger.tracef("Failed to save state: %s", e.msg);
            } catch (Exception e) {
            }
        }
    }

    /// Increments session count and begins consolidation if it should trigger.
    /// Returns true if consolidation was triggered (lock acquired).
    /// Persists state immediately.
    bool beginConsolidation() @safe nothrow {
        long nowSec = Clock.currTime().toUnixTime!long;
        long diff = nowSec - lastSessionCountUpdate;

        if (diff < 0) {
            try {
                logger.tracef("Session count timestamp appears to be in the future (diff=%s). Resetting timer.",
                        diff);
            } catch (Exception e) {
            }
            lastSessionCountUpdate = nowSec;
        } else if (diff >= SessionCountMinIntervalSec) {
            sessionCount++;
            lastSessionCountUpdate = nowSec;
        } else {
            try {
                logger.tracef("Skipping session count increment: only %s seconds since last update (threshold: %s)",
                        diff, SessionCountMinIntervalSec);
            } catch (Exception e) {
            }
        }

        bool trigger = shouldConsolidateInternal();
        if (trigger) {
            isConsolidating = true;
        }
        saveState();
        return trigger;
    }

    bool shouldConsolidate() const @safe nothrow {
        return !isConsolidating && shouldConsolidateInternal;
    }

    /// Internal check without the isConsolidating guard (used after increment).
    private bool shouldConsolidateInternal() const @safe nothrow {
        if (consolidationInterval == 0 || sessionCount == 0) {
            return false;
        }
        return sessionCount % consolidationInterval == 0;
    }

    /// Clears consolidation lock after completion (success or failure). Persists immediately.
    void clearConsolidationLock() @safe {
        isConsolidating = false;
        sessionCount++;
        saveState();
    }

    RagDatabaseConfig[] getRagSecondary() @safe {
        import std.algorithm : joiner;

        return ragSecondary.byValue.joiner.array;
    }

    string getPrompt(string prompt) {
        import llm.vfs : FlatVfs;

        auto vfs = FlatVfs(promptDir);
        return vfs.read(prompt).match!((string a) => a, (_) {
            logger.warningf("Prompt '%s' not found", prompt);
            throw new Exception("System prompt not found: " ~ prompt);
            return null;
        });
    }
}

void makeDefaultFileStructure() {
    import std.file : mkdirRecurse;
    import my.xdg : xdgDataHome;

    foreach (path; [
        (xdgDataHome ~ Path(ProgramName) ~ Path("memory")),
        (xdgDataHome ~ Path(ProgramName) ~ Path("skills"))
    ].filter!(a => !a.exists)) {
        try {
            logger.trace("Creating directory ", path);
            mkdirRecurse(path);
        } catch (Exception e) {
            logger.warning(e);
        }
    }
}

void makeLocalSetupFileStructure(LlmConfig conf, bool rag = false) {
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

RequestConfig toRequestConfig(ConfigT)(ConfigT conf) {
    JSONValue makeHeader(string model, double temp, long maxTokens, ServerConfig cfg) {
        import std.math : isNaN;
        import std.array : empty;

        JSONValue j;

        if (!model.empty)
            j["model"] = model;
        if (!temp.isNaN)
            j["temperature"] = temp;
        if (maxTokens != 0)
            j["max_tokens"] = maxTokens;

        final switch (cfg.toType) {
        case EndpointType.unknown:
            break;
        case EndpointType.llamaCpp:
            if (conf.reasoningBudget != 0 || conf.preserveThinking) {
                j["chat_template_kwargs"] = JSONValue.emptyObject;
                if (conf.reasoningBudget != 0)
                    j["chat_template_kwargs"]["reasoning_budget"] = conf.reasoningBudget;
                if (conf.preserveThinking)
                    j["chat_template_kwargs"]["preserve_thinking"] = true;
            }
            break;
        case EndpointType.deepseek:
            if (conf.reasoningBudget != 0 || conf.preserveThinking) {
                j["thinking"] = JSONValue.emptyObject;
                j["thinking"]["type"] = "enabled";
            }
            if (conf.reasoningBudget >= 4096) {
                j["reasoning_effort"] = "max";
            } else if (conf.reasoningBudget >= 2048) {
                j["reasoning_effort"] = "high";
            }
            if (maxTokens == -1)
                j["max_tokens"] = null;
            break;
        }

        return j;
    }

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
         header: makeHeader(conf.name, conf.temp, conf.maxTokens, conf.server));
    // dfmt on
}

LlmConfig readConfig(Path path, bool silent = false, bool noCwdConfig = false) {
    import std.process : environment;
    import my.xdg : xdgConfigHome;

    LlmConfig conf;
    bool loadedAnyFile = false;

    // Layer 1: Base config from LLMFUN_DEFAULT_CONFIG
    auto basePath = environment.get("LLMFUN_DEFAULT_CONFIG",
            (xdgConfigHome ~ Path(ProgramName) ~ Path("config.json")).toString).Path;
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

    if (loadedAnyFile) {
        validateConfig(conf);
    }

    conf.resolvePaths(!noCwdConfig);
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

/// Emit warnings for models configured without API keys.
/// Called from validateConfig() after all hard validation checks.
/// No-op when warnIfNoApiKey is false or OPENAI_API_KEY env var is set.
private void checkApiKeyWarnings(LlmConfig conf) {
    if (!conf.warnIfNoApiKey || !getEnvApiKey.empty)
        return;

    bool warned;

    foreach (model; conf.codeModels.filter!(a => a.server.apiKey.empty)) {
        logger.warningf("No API key configured for code model '%s'", model.name);
        warned = true;
    }

    if (!conf.summaryModel.server.url.empty && conf.summaryModel.server.apiKey.empty) {
        logger.warningf("No API key configured for summary model '%s'", conf.summaryModel.name);
        warned = true;
    }

    conf.embedConfig.match!((RemoteEmbedConfig r) {
        if (r.server.apiKey.empty) {
            logger.warningf("No API key configured for remote embed model '%s'", r.name);
            warned = true;
        }
    }, (LocalEmbedConfig) {} // No API key needed for local embed
    );

    if (warned)
        logger.warningf(
                "To suppress these warnings, set 'warnIfNoApiKey' to false or provide OPENAI_API_KEY.");
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

    if (conf.toolLimits.readFileMaxLines < 1)
        throw new Exception("toolLimits.readFileMaxLines must be >= 1");
    if (conf.toolLimits.editFileMaxLines < 1)
        throw new Exception("toolLimits.editFileMaxLines must be >= 1");
    if (conf.toolLimits.maxDirEntries < 1)
        throw new Exception("toolLimits.maxDirEntries must be >= 1");
    if (conf.toolLimits.grepMaxResults < 1)
        throw new Exception("toolLimits.grepMaxResults must be >= 1");
    if (conf.toolLimits.maxSummaryLength < 1)
        throw new Exception("toolLimits.maxSummaryLength must be >= 1");
    if (conf.toolLimits.maxTopicLength < 1)
        throw new Exception("toolLimits.maxTopicLength must be >= 1");
    if (conf.toolLimits.maxTopK < 1)
        throw new Exception("toolLimits.maxTopK must be >= 1");
    if (conf.toolLimits.maxArgLength < 1)
        throw new Exception("toolLimits.maxArgLength must be >= 1");

    // Emit warnings for missing API keys (after all hard validation)
    checkApiKeyWarnings(conf);
}

alias jsonToLlmConfig = jsonToConfig!LlmConfig;

/// Returns the OpenAI API key from the OPENAI_API_KEY environment variable, or "" if not set.
string getEnvApiKey() {
    import std.process : environment;

    return environment.get("OPENAI_API_KEY", null);
}
