module llm.config;

import logger = std.logger;
import std.algorithm : filter, map;
import std.array : array, empty, appender, join;
import std.conv : to, text;
import std.file : readText, exists, mkdirRecurse, rename, rmdirRecurse;
import std.format : format;
import std.datetime : Clock;
import std.json : JSONValue, JSONType, parseJSON, JSONOptions;
import std.sumtype : SumType, match;
import std.string : toLower, startsWith;
import std.typecons : Nullable;

import my.path;

import llm.query : RequestConfig;
import llm.skill : SkillManager;
public import llm.common.embedder;
public import llm.common.config;

immutable ProgramName = "llmfun";

/// Minimum interval (in seconds) between session count increments.
private enum SessionCountMinIntervalSec = 3 * 3600;

/// RAG query instruction appended to system prompt when AGENTS.md summary is present.
private immutable AgentMdRagInstruction = "For detailed explanations, architecture justifications, or lengthy code examples, " ~ "query your RAG knowledge base using the 'AGENTS.md' source. However, you must obey the " ~ "compressed rules listed above at all times, even if you don't retrieve the full file.";

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

/// Single entry in the curated image catalog.
struct ImageCatalogEntry {
    /// Full image reference (e.g., "python:3.11-slim")
    string name;

    /// Human-readable description of what this image is for
    string description;

    /// Tags for categorization and filtering (e.g., ["python", "dev"])
    string[] tags;

    /// If set the workarea is mounted inside the container at this destination
    string workareaMountDest;

    /// Mount the containers root filesystem read-only
    bool readOnly = true;

    /// If network is allowed
    bool allowNetwork;

    invariant {
        assert(name.length > 0, "ImageCatalogEntry name must not be empty");
    }
}

/// Loading state of the image catalog.
enum CatalogState {
    /// No catalog file configured - falls back to allowedImages
    notConfigured,
    /// Catalog loaded successfully - validates against it
    loaded,
    /// Catalog file existed but failed to parse - falls back to allowedImages
    loadFailed
}

struct SandboxConfig {
    /// Container runtime CLI command (e.g., "docker" or "podman")
    string runtimeCli = "docker";

    /// Default container image when none specified
    string defaultImage;

    /// Execution timeout in seconds
    long timeoutSeconds = 60;

    /// Memory limit for containers (e.g., "256m", "1g")
    string memoryLimit = "256m";

    /// CPU limit for containers (e.g., "0.5", "1", "2")
    string cpuLimit = "0.5";

    /// tmpfs mount options for /tmp inside container
    string tmpfsOptions = "rw,noexec,nosuid,size=64m";

    /// User namespace mapping (UID:GID)
    string userNs = "1000:1000";

    /// Maximum output bytes per stream (stdout/stderr)
    long maxOutputBytes = 1_048_576;

    /// Curated image catalog entries (loaded from imageCatalogFile at startup)
    ImageCatalogEntry[] imageCatalog;

    /// Path to image catalog JSON file (relative to config directory)
    string imageCatalogFile;

    /// Loading state of the image catalog
    CatalogState catalogState = CatalogState.notConfigured;

    invariant {
        assert(runtimeCli.length > 0, "runtimeCli must not be empty");
        assert(timeoutSeconds > 0, i"timeoutSeconds must be positive, got $(timeoutSeconds)".text);
        assert(maxOutputBytes > 0, i"maxOutputBytes must be positive, got $(maxOutputBytes)".text);
    }
}

struct LlmConfig {
    Path dataDir = ProgramName ~ "/data";

    /// LLM save a memory to this file which is used between runs.
    Path[] memoryArea;

    bool noMemory;

    Path scratchArea;

    Path[] promptDir;

    Path[] skillPathsUser;
    Path[] skillPathsSystem;
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
        } else {
            memoryArea = memoryArea.map!(a => replaceMagicWord(a)).array;
        }

        if (promptDir.empty) {
            promptDir = prioConfCwdDirs.map!(a => cast(Path)(a ~ "prompt")).array;
        } else {
            promptDir = promptDir.map!(a => replaceMagicWord(a)).array;
        }

        if (skillPathsSystem.empty) {
            if (cwdConfig)
                skillPathsSystem ~= (ProgramName ~ "/skills").Path;
            dataSearch(ProgramName).resolve("skills".Path).match!((ResourceFile a) {
                skillPathsSystem ~= a.get;
            }, (_) {});
        } else {
            skillPathsSystem = skillPathsSystem.map!(a => replaceMagicWord(a)).array;
        }
        skillPathsUser = skillPathsUser.map!(a => replaceMagicWord(a)).array;

        if (scratchArea == Path.init) {
            scratchArea = prioDataCwdDirs.resolve("scratch".Path)
                .orElse(ResourceFile(scratchArea.AbsolutePath)).get.Path;
        }
    }

    /// Directory where the LLM can work with assets, create files etc.
    Path workArea = ProgramName ~ "/workarea";

    SandboxConfig sandboxConfig;

    ToolLimits toolLimits;

    ToolFilter toolFilter;
    RagFilter ragFilter;

    RagConfig ragConfig;

    /// Searched for in promptDir
    string agentPrompt = "AGENT.md";

    CodeModelConfig[] codeModels;
    long activeCodeModelIndex = 0;

    /// Tracks total session starts (incremented at the beginning of each session).
    uint sessionCount = 0;
    /// Prevents concurrent or crash-retry consolidation. Cleared on load if stale.
    bool isConsolidating = false;
    /// Default trigger threshold (every N sessions). 0 means disabled.
    uint consolidationInterval = 10;
    /// Unix epoch seconds of the last session count increment. 0 means never incremented.
    long lastSessionCountUpdate = 0;

    SummaryModelConfig summaryModel;

    /// Optional dedicated vision model for image processing. When set, image analysis
    /// delegates to a separate model specialized for vision tasks.
    Nullable!VisionModelConfig visionModel;

    /// If true, emit a warning when no API key is configured for a model server. Defaults to true.
    bool warnIfNoApiKey = true;

    invariant {
        assert(maxManifestSkills > 0, i"maxManifestSkills must be positive, got $(maxManifestSkills)"
                .text);
        assert(maxAlwaysApplyTokens >= 0, i"maxAlwaysApplyTokens must be non-negative (0 = unlimited), got $(
                maxAlwaysApplyTokens)".text);
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
            throw new Exception(i"Active code model index $(activeCodeModelIndex) is out of bounds (count: $(
                    codeModels.length))".text);
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
            return i"No model matches '$(name)'. Available models: $(codeModels.map!(m => m.name))"
                .text;
        }
        if (matchCount > 1) {
            return i"Ambiguous model name '$(name)'. Matches: $(
                    codeModels.filter!(m => m.name.toLower == lowerName)
                    .map!(m => m.name))".text;
        }

        activeCodeModelIndex = matchIndex;
        saveState();
        return null;
    }

    /// List all configured model names with index and active indicator.
    string[] listModels() const @safe {
        auto app = appender!(string[])();
        foreach (i, model; codeModels) {
            app.put(i"$(model.name) (index: $(i))$(i == activeCodeModelIndex ? " [active]" : "")"
                    .text);
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
            File(tempFile, "w").writeln(stateObj.toString(JSONOptions.doNotEscapeSlashes));
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

    private string getBasePrompt(string prompt) {
        import llm.vfs : FlatVfs;

        auto vfs = FlatVfs(promptDir);
        return vfs.read(prompt).match!((string a) => a, (_) {
            logger.warningf("Prompt '%s' not found", prompt);
            throw new Exception("System prompt not found: " ~ prompt);
            return null;
        });
    }

    // Compose system prompt with skills
    // Composition order: basePrompt → alwaysApplyBlock → agentMdSummary → ragInstruction → memory → timestamp → manifestXml
    string getPrompt(SkillManager skillManager, string promptName = null,
            bool addSkills = true, string agentMdSummary = null) {
        import std.string : strip;
        import llm.skill : buildAlwaysApplyBlock;

        string basePrompt = promptName.empty ? getBasePrompt(agentPrompt) : getBasePrompt(
                promptName);

        string fullPrompt = basePrompt;

        string alwaysApplyBlock;
        string manifestXml;

        if (!disableSkills && addSkills) {
            alwaysApplyBlock = buildAlwaysApplyBlock(skillManager.getAlwaysApplySkills(),
                    maxAlwaysApplyTokens);

            manifestXml = skillManager.getManifestXml(maxManifestSkills);
        }

        // Build the prompt in the defined composition order
        bool hasAgentMd = !agentMdSummary.empty;
        string ragInstruction = hasAgentMd ? AgentMdRagInstruction : "";

        if (hasAgentMd || !alwaysApplyBlock.empty || !manifestXml.empty) {
            string[] parts;
            parts ~= basePrompt;
            if (!alwaysApplyBlock.empty)
                parts ~= alwaysApplyBlock;
            if (hasAgentMd)
                parts ~= agentMdSummary;
            if (hasAgentMd)
                parts ~= ragInstruction;
            if (!manifestXml.empty)
                parts ~= manifestXml;

            fullPrompt = parts.join("\n\n").strip;
        }

        return fullPrompt;
    }

    Path[] skillPaths() @safe {
        return skillPathsUser ~ skillPathsSystem;
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

struct VisionModelConfig {
    ServerConfig server;
    string name;
    string systemPrompt;
    double temp;
    long contextSize;
    long maxTokens;
    long reasoningBudget;
    bool preserveThinking;
    long timeoutSecs = 60;

    invariant {
        assert(!name.empty, "Vision model name must not be empty");
        assert(!server.url.empty, "Vision model server URL must not be empty");
        assert(temp >= 0.0 && temp <= 2.0, i"Temperature must be in [0.0, 2.0], got $(temp)".text);
        assert(contextSize > 0, i"Context size must be positive, got $(contextSize)".text);
        assert(timeoutSecs > 0 && timeoutSecs <= 3600, i"Timeout must be in (0, 3600], got $(
                timeoutSecs)".text);
    }
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

/// Load the image catalog from a JSON file.
/// Supports wrapper object with version + entries formats.
/// Updates state to reflect loading outcome.
ImageCatalogEntry[] loadImageCatalog(Path filePath, ref CatalogState state) {
    import my.set : Set;
    import llm.utility : getValue;

    if (!filePath.exists) {
        logger.warningf("Image catalog file not found: %s - falling back to allow-list", filePath);
        state = CatalogState.notConfigured;
        return null;
    }

    string content;
    try {
        content = readText(filePath);
    } catch (Exception e) {
        logger.warningf("Failed to read image catalog %s: %s - falling back to allow-list",
                filePath, e.msg);
        state = CatalogState.loadFailed;
        return null;
    }

    JSONValue json;
    try {
        json = parseJSON(content);
    } catch (Exception e) {
        logger.warningf("Failed to parse image catalog %s: %s - falling back to allow-list",
                filePath, e.msg);
        state = CatalogState.loadFailed;
        return [];
    }

    // Validate v1 format: object with "entries" array
    JSONValue entriesJson;
    if (json.type == JSONType.OBJECT && "entries" in json) {
        // { "version": 1, "entries": [...] }
        entriesJson = json["entries"];
        if ("version" in json) {
            auto ver = json["version"].integer;
            if (ver != 1) {
                logger.warningf("Image catalog version %s is unknown - attempting to parse anyway",
                        ver);
            }
        }
    } else {
        logger.warningf("Image catalog %s has invalid format - expected v1 object with 'entries' field. Falling back to allow-list.",
                filePath);
        state = CatalogState.loadFailed;
        return null;
    }
    if (entriesJson.type != JSONType.ARRAY) {
        logger.warningf("Image catalog entries in %s is not an array - falling back to allow-list",
                filePath);
        state = CatalogState.loadFailed;
        return null;
    }

    ImageCatalogEntry[] result;
    Set!string seenNames;

    foreach (entry; entriesJson.array) {
        if (entry.type != JSONType.OBJECT) {
            logger.warningf("Skipping non-object entry in image catalog");
            continue;
        }

        // Parse name (required)
        string name = getValue(entry, (v) => v["name"].str, null);
        if (name.empty) {
            logger.warningf("Skipping catalog entry with empty or invalid 'name' field");
            continue;
        }

        // Check for duplicates
        if (name in seenNames) {
            logger.warningf("Duplicate image name in catalog '%s' - keeping first occurrence",
                    name);
            continue;
        }
        seenNames.add(name);

        // Parse optional fields
        string workareaMountDest = getValue(entry, (v) => v["workareaMountDest"].str, null);
        bool readOnly = getValue(entry, (v) => v["readOnly"].boolean,
                ImageCatalogEntry.init.readOnly);
        bool allowNetwork = getValue(entry, (v) => v["allowNetwork"].boolean,
                ImageCatalogEntry.init.allowNetwork);
        string description = getValue(entry, (v) => v["description"].str, null);
        string[] tags = getValue(entry, (v) => v["tags"].array, null).filter!(
                a => a.type == JSONType.STRING)
            .map!(a => a.str)
            .array;

        result ~= ImageCatalogEntry(name: name, description: description, tags: tags,
                workareaMountDest: workareaMountDest, readOnly: readOnly,
                allowNetwork: allowNetwork);
    }

    if (result.length > 100) {
        logger.warningf("Image catalog contains %s entries (exceeds 100) - this may impact performance",
                result.length);
    }

    state = CatalogState.loaded;
    logger.infof("Loaded %s entries from image catalog %s", result.length, filePath);

    return result;
}

LlmConfig readConfig(Path path, bool silent = false, bool noCwdConfig = false) {
    import std.process : environment;
    import std.path : dirName;
    import my.xdg : xdgConfigHome;

    LlmConfig conf;
    bool loadedAnyFile = false;
    string configDir = "."; // Directory of the last successfully loaded config file

    // Layer 1: Base config from LLMFUN_DEFAULT_CONFIG
    auto basePath = environment.get("LLMFUN_DEFAULT_CONFIG",
            (xdgConfigHome ~ Path(ProgramName) ~ Path("config.json")).toString).Path;
    if (basePath.exists) {
        logger.infof(!silent, "Reading base configuration from %s", basePath);
        try {
            conf = jsonToLlmConfig(conf, readText(basePath.toString).parseJSON);
            loadedAnyFile = true;
            configDir = basePath.dirName;
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
            configDir = overlayPath.dirName;
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

    // Load image catalog if configured
    if (!conf.sandboxConfig.imageCatalogFile.empty) {
        // Determine config directory for resolving relative paths
        try {
            import std.path : buildPath;

            auto catalogPath = AbsolutePath(buildPath(configDir,
                    conf.sandboxConfig.imageCatalogFile));
            conf.sandboxConfig.imageCatalog = loadImageCatalog(catalogPath,
                    conf.sandboxConfig.catalogState);
        } catch (Exception e) {
            logger.warningf("Failed to validate catalog path '%s': %s - falling back to allow-list",
                    conf.sandboxConfig.imageCatalogFile, e.msg);
            conf.sandboxConfig.catalogState = CatalogState.notConfigured;
        }
    }

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

    // Helper: extract inner type from Nullable!T, or void if not Nullable
    template NullableInner(T) {
        static if (is(T == Nullable!U_, U_))
            alias NullableInner = U_;
        else
            alias NullableInner = void;
    }

    // Helper: check if type is Nullable!Something
    template isNullableType(T) {
        enum isNullableType = is(T == Nullable!U_, U_);
    }

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
                        } else static if (is(Type == ImageCatalogEntry[])) {
                            __traits(getMember, conf, llmMemberName) = json[llmMemberName].array.map!(
                                    a => jsonToConfig(ImageCatalogEntry.init, a)).array;
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
                        } else static if (isNullableType!Type
                                && isAggregateType!(NullableInner!Type)) {
                            // Handle Nullable!T for aggregate types (e.g., Nullable!VisionModelConfig)
                            alias InnerT = NullableInner!Type;
                            auto val = json[llmMemberName];
                            if (val.type != JSONType.NULL) {
                                auto innerConf = InnerT.init;
                                __traits(getMember, conf, llmMemberName) = Nullable!InnerT(jsonToConfig(innerConf,
                                        val));
                            }
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

    if (!conf.visionModel.isNull && !conf.visionModel.get.server.url.empty
            && conf.visionModel.get.server.apiKey.empty) {
        logger.warningf("No API key configured for vision model '%s'", conf.visionModel.get.name);
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
        throw new Exception(i"activeCodeModelIndex $(conf.activeCodeModelIndex) is out of bounds (codeModels count: $(
                conf.codeModels.length))".text);

    foreach (i, model; conf.codeModels) {
        if (model.name.empty)
            throw new Exception(i"codeModels[$(i)].name must not be empty".text);
        if (model.server.url.empty)
            throw new Exception(i"codeModels[$(i)].server.url must not be empty for $(model.name)"
                    .text);
    }

    if (!conf.visionModel.isNull) {
        auto vm = conf.visionModel.get;
        if (vm.server.url.empty)
            throw new Exception("visionModel.server.url must not be empty");
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

auto replaceMagicWord(T)(T variable) {
    import std.file : thisExePath;
    import std.path : dirName;
    import std.string : replace;

    immutable BinaryMagic = "@{llmfun}";
    auto binaryDir = thisExePath.dirName;

    static if (is(T == Path) || is(T == AbsolutePath)) {
        return T(variable.toString.replace(BinaryMagic, binaryDir));
    } else {
        return variable.replace(BinaryMagic, binaryDir);
    }
}

/// Test: loadImageCatalog returns empty array when file not found.
unittest {
    CatalogState state;
    auto result = loadImageCatalog("/nonexistent/path/to/catalog.json".Path, state);
    assert(result.length == 0);
    assert(state == CatalogState.notConfigured);
}

/// Test: loadImageCatalog returns empty array with loadFailed on invalid JSON.
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "catalog_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "bad.json");
    File(tmpFile, "w").write("{ invalid json }");

    CatalogState state;
    auto result = loadImageCatalog(tmpFile.Path, state);
    assert(result.length == 0);
    assert(state == CatalogState.loadFailed);
}

/// Test: loadImageCatalog parses v1 format correctly.
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "catalog_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "catalog.json");
    string json = `{"version":1,"entries":[{"name":"alpine:latest","description":"Minimal Linux","tags":["linux"]},{"name":"python:3.11-slim","description":"Python runtime","tags":["python","dev"]}]}`;
    File(tmpFile, "w").write(json);

    CatalogState state;
    auto result = loadImageCatalog(tmpFile.Path, state);
    assert(state == CatalogState.loaded);
    assert(result.length == 2);
    assert(result[0].name == "alpine:latest");
    assert(result[0].description == "Minimal Linux");
    assert(result[0].tags.length == 1);
    assert(result[0].tags[0] == "linux");
    assert(result[1].name == "python:3.11-slim");
    assert(result[1].tags.length == 2);
}

/// Test: loadImageCatalog handles missing optional fields (description, tags).
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "catalog_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "catalog.json");
    string json = `{"version":1,"entries":[{"name":"alpine:latest"},{"name":"python:3.11-slim","description":"Python runtime"},{"name":"node:20-alpine","tags":["nodejs"]}]}`;
    File(tmpFile, "w").write(json);

    CatalogState state;
    auto result = loadImageCatalog(tmpFile.Path, state);
    assert(state == CatalogState.loaded);
    assert(result.length == 3);
    assert(result[0].description == "");
    assert(result[0].tags.length == 0);
    assert(result[1].description == "Python runtime");
    assert(result[1].tags.length == 0);
    assert(result[2].tags.length == 1);
    assert(result[2].tags[0] == "nodejs");
}

/// Test: loadImageCatalog skips entries with missing or empty name.
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "catalog_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "catalog.json");
    string json = `{"version":1,"entries":[{"name":"alpine:latest","description":"Good"},{"description":"Missing name"},{"name":"","description":"Empty name"},{"name":"python:3.11-slim","description":"Good2"}]}`;
    File(tmpFile, "w").write(json);

    CatalogState state;
    auto result = loadImageCatalog(tmpFile.Path, state);
    assert(state == CatalogState.loaded);
    assert(result.length == 2);
    assert(result[0].name == "alpine:latest");
    assert(result[1].name == "python:3.11-slim");
}

/// Test: loadImageCatalog detects duplicate names and keeps first.
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "catalog_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "catalog.json");
    string json = `{"version":1,"entries":[{"name":"alpine:latest","description":"First"},{"name":"alpine:latest","description":"Duplicate"}]}`;
    File(tmpFile, "w").write(json);

    CatalogState state;
    auto result = loadImageCatalog(tmpFile.Path, state);
    assert(state == CatalogState.loaded);
    assert(result.length == 1);
    assert(result[0].description == "First");
}

/// Test: loadImageCatalog rejects invalid format (no entries field).
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "catalog_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "catalog.json");
    string json = `{"version":1,"items":[]}`;
    File(tmpFile, "w").write(json);

    CatalogState state;
    auto result = loadImageCatalog(tmpFile.Path, state);
    assert(result.empty);
    assert(state == CatalogState.loadFailed);
}

/// Test: loadImageCatalog returns loadFailed on empty file.
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "catalog_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "empty.json");
    File(tmpFile, "w").write("");

    CatalogState state;
    auto result = loadImageCatalog(tmpFile.Path, state);
    assert(result.empty);
    assert(state == CatalogState.loadFailed);
}

/// Test: loadImageCatalog handles empty entries array.
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "catalog_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "empty_entries.json");
    string json = `{"version":1,"entries":[]}`;
    File(tmpFile, "w").write(json);

    CatalogState state;
    auto result = loadImageCatalog(tmpFile.Path, state);
    assert(result.empty);
    assert(state == CatalogState.loaded);
}

/// Test: loadImageCatalog handles entries with empty tags array.
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "catalog_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "empty_tags.json");
    string json = `{"version":1,"entries":[{"name":"alpine:latest","description":"Minimal","tags":[]}]}`;
    File(tmpFile, "w").write(json);

    CatalogState state;
    auto result = loadImageCatalog(tmpFile.Path, state);
    assert(result.length == 1);
    assert(state == CatalogState.loaded);
    assert(result[0].name == "alpine:latest");
    assert(result[0].tags.length == 0);
}

/// Test: loadImageCatalog warns on large catalog (>100 entries) but still loads.
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "catalog_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "large.json");

    // Build a catalog with 105 entries
    string entries;
    foreach (i; 0 .. 105) {
        if (i > 0)
            entries ~= ",";
        entries ~= `{"name":"image` ~ i.to!string
            ~ `:latest","description":"Image ` ~ i.to!string ~ `","tags":["test"]}`;
    }
    string json = `{"version":1,"entries":[` ~ entries ~ `]}`;
    File(tmpFile, "w").write(json);

    CatalogState state;
    auto result = loadImageCatalog(tmpFile.Path, state);
    assert(result.length == 105);
    assert(state == CatalogState.loaded);
}

/// Test: loadImageCatalog handles unknown version (warns, parses anyway).
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "catalog_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "unknown_version.json");
    string json = `{"version":99,"entries":[{"name":"alpine:latest","description":"Test","tags":["linux"]}]}`;
    File(tmpFile, "w").write(json);

    CatalogState state;
    auto result = loadImageCatalog(tmpFile.Path, state);
    assert(result.length == 1);
    assert(state == CatalogState.loaded);
    assert(result[0].name == "alpine:latest");
}
