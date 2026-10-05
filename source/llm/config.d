module llm.config;

import logger = std.logger;
import std.algorithm : filter, map, sort, among;
import std.array : array, empty, appender, join;
import std.conv : to, text;
import std.datetime : Clock;
import std.file : readText, exists, mkdirRecurse, rename, rmdirRecurse, thisExePath, isDir;
import std.format : format;
import std.json : JSONValue, JSONType, parseJSON, JSONOptions;
import std.path : dirName;
import std.string : toLower, startsWith, indexOf;
import std.sumtype : SumType, match;
import std.typecons : Nullable;

import my.path;

import llm.query : RequestConfig;
import llm.skill : SkillManager;
import llm.environment.config : EnvironmentBackend, ExecutionConfigState, loadExecutionBackends;
import llm.yaml : loadYamlValue;
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
    long queryFtl5Mode = 0; // 0: full mode, 1: limited ftl5
}

struct RagConfig {
    /// Sliding window overlap as percentage (0-99).
    /// 50 means each chunk overlaps 50% with the previous one.
    /// Must be in range [0, 99]. Value of 100 would cause infinite loop.
    long windowOverlapPercent = 50;

    long nBatch;

    long maxChunksPerTopic;

    invariant {
        assert(windowOverlapPercent >= 0 && windowOverlapPercent <= 99,
                "windowOverlapPercent must be in range [0, 99], got "
                ~ windowOverlapPercent.to!string);
    }
}

struct SandboxConfig {
    /// Default container options as tag -> CLI arguments map.
    /// Keys are logical group names (e.g., "security", "network", "mounts").
    /// Values are arrays of CLI arguments flattened into the command line.
    string[][string] defaultOptions;

    /// Maximum output bytes per stream (stdout/stderr)
    long maxOutputBytes = 1_048_576;

    /// Path to system execution environments config file (relative to config directory).
    /// Loaded as layer 1; user file entries override system entries with the same tag.
    string systemExecutionEnvironmentsFile;

    /// Path to user execution environments config file (relative to config directory).
    /// Loaded as layer 2; entries with the same tag override system entries.
    string userExecutionEnvironmentsFile;

    /// Execution environments (loaded from the configured environments file at startup).
    EnvironmentBackend[] executionEnvironments;

    /// Loading state of the execution environments config.
    ExecutionConfigState executionConfigState = ExecutionConfigState.notConfigured;

    invariant {
        assert(maxOutputBytes > 0, i"maxOutputBytes must be positive, got $(maxOutputBytes)".text);
    }
}

struct TuiConfig {
    /// Maximum TUI width in terminal columns. 0 = unlimited (default).
    /// Valid values: 0 or [40, 10000] (40 = C++ MIN_TERMINAL_WIDTH; 10000
    /// caps the long->int conversion at the C API boundary).
    long maxWidth = 0;
}

enum NudgeKind {
    keepReasoning,
    recovery
}

public immutable DefaultCompressionNudge = "NUDGE_COMPRESSION.md";

struct EscalationConfig {
    /// Soft-phase nudge files resolved through promptDir. Empty = kind default.
    string[] softNudges;
    /// Soft strikes before the hard phase. 0 = skip soft phase.
    long softStrikes = 2;
    /// Hard-phase nudge files. Empty = kind default.
    string[] hardNudges;
    /// Hard strikes before the turn fails. 0 = unlimited (backstop governs).
    long hardStrikes = 1;
    /// Master switch for the kind's ladder. False = no nudge, counter untouched, files not loaded (turn ends via backstops).
    bool enabled = true;
}

struct CompressionNudgeConfig {
    /// Gates the one-shot compression nudge. False = no nudge at any usage, template not loaded (the 90% forced point is not gated).
    bool enabled = true;
    /// Fraction of the context window that triggers the one-shot nudge.
    double threshold = 0.8;
    string prompt; // empty = shipped default file
}

struct FeedbackNudgeConfig {
    /// Gates the tool-call feedback warning. False = no warnings; intervalSecs/minToolCalls ignored.
    bool enabled = true;
    long intervalSecs = 900;
    long minToolCalls = 50;
}

struct NudgeConfig {
    EscalationConfig keepReasoning; // default files differ per kind:
    EscalationConfig recovery; // resolved by defaultNudgeFiles(kind)
    CompressionNudgeConfig compression;
    FeedbackNudgeConfig feedback;
}

/// Kind -> shipped default file names (config/prompt/):
///   keepReasoning: soft NUDGE_KEEP_REASONING_SOFT.md  hard NUDGE_KEEP_REASONING_HARD.md
///   recovery:      soft NUDGE_RECOVERY_SOFT.md        hard NUDGE_RECOVERY_HARD.md
string[] defaultNudgeFiles(NudgeKind kind, bool hard) @safe pure {
    final switch (kind) with (NudgeKind) {
    case keepReasoning:
        return hard ? ["NUDGE_KEEP_REASONING_HARD.md"] : [
            "NUDGE_KEEP_REASONING_SOFT.md"
        ];
    case recovery:
        return hard ? ["NUDGE_RECOVERY_HARD.md"] : ["NUDGE_RECOVERY_SOFT.md"];
    }
}

struct LlmConfig {
    Path dataDir = ProgramName ~ "/data";

    /// LLM saves memories to this area, persisted between runs.
    Path[] memoryArea;

    bool noMemory;

    Path[] promptDir;

    Path chatDir;
    /// Directory holding per-session dialogue RAG databases.
    /// Defaults to <dataDir>/dialogue (see resolvePaths).
    Path dialogueDir;
    /// Active chat session id (stored in state.json; empty = none).
    string activeChatSessionId;

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

        AbsolutePath[] prioConfCwdDirs = configSearch(ProgramName);
        if (cwdConfig) {
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
            memoryArea = memoryArea.map!(a => replaceMagicWord(a,
                    workArea.AbsolutePath).Path).array;
        }

        if (promptDir.empty) {
            promptDir = prioConfCwdDirs.map!(a => cast(Path)(a ~ "prompt")).array;
        } else {
            promptDir = promptDir.map!(a => replaceMagicWord(a, workArea.AbsolutePath).Path).array;
        }

        bool skillsFromDataSearch;
        if (skillPathsSystem.empty) {
            if (cwdConfig)
                skillPathsSystem ~= (ProgramName ~ "/skills").Path;
            dataSearch(ProgramName).resolve("skills".Path).match!((ResourceFile a) {
                skillPathsSystem ~= a.get;
            }, (_) {});
            skillsFromDataSearch = true;
        } else {
            skillPathsSystem = skillPathsSystem.map!(a => replaceMagicWord(a,
                    workArea.AbsolutePath).Path).array;
        }
        if (skillPathsUser.empty && !skillsFromDataSearch) {
            dataSearch(ProgramName).resolve("skills".Path).match!((ResourceFile a) {
                skillPathsUser ~= a.get;
            }, (_) {});
        } else {
            skillPathsUser = skillPathsUser.map!(a => replaceMagicWord(a,
                    workArea.AbsolutePath).Path).array;
        }

        auto localChat = (ProgramName ~ "/data/chat").Path;
        if (cwdConfig && chatDir.empty && localChat.exists && localChat.isDir) {
            chatDir = localChat;
        } else if (chatDir.empty || !chatDir.exists) {
            dataSearch(ProgramName).resolve("chat".Path).match!((ResourceFile a) {
                chatDir = a.get;
            }, (_) { chatDir = localChat; });
        }

        auto localDialogue = (ProgramName ~ "/data/dialogue").Path;
        if (cwdConfig && dialogueDir.empty && localDialogue.exists && localDialogue.isDir) {
            dialogueDir = localDialogue;
        } else if (dialogueDir.empty || !dialogueDir.exists) {
            dataSearch(ProgramName).resolve("dialogue".Path).match!((ResourceFile a) {
                dialogueDir = a.get;
            }, (_) { dialogueDir = localDialogue; });
        }

        if (nudges.keepReasoning.softNudges.empty)
            nudges.keepReasoning.softNudges = defaultNudgeFiles(NudgeKind.keepReasoning, false);
        if (nudges.keepReasoning.hardNudges.empty)
            nudges.keepReasoning.hardNudges = defaultNudgeFiles(NudgeKind.keepReasoning, true);
        if (nudges.recovery.softNudges.empty)
            nudges.recovery.softNudges = defaultNudgeFiles(NudgeKind.recovery, false);
        if (nudges.recovery.hardNudges.empty)
            nudges.recovery.hardNudges = defaultNudgeFiles(NudgeKind.recovery, true);
        if (nudges.compression.prompt.empty)
            nudges.compression.prompt = DefaultCompressionNudge;
    }

    /// Directory where the LLM can work with assets, create files etc.
    Path workArea = ProgramName ~ "/workarea";

    SandboxConfig sandboxConfig;

    ToolLimits toolLimits;
    TuiConfig tui;

    ToolFilter toolFilter;
    RagFilter ragFilter;

    /// Broker kill switch + tuning-free config.
    ToolBrokerConfig toolBroker;

    RagConfig ragConfig;

    /// Agent prompt filename searched for in promptDir.
    string agentPrompt = "AGENT.md";

    /// Reasoning-summary prompt filename searched for in promptDir.
    string reasoningSummaryPrompt = "REASONING_SUMMARY.md";

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

    NudgeConfig nudges;

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
    /// Non-const on purpose: a const accessor could not return a mutable value
    /// copy once CodeModelConfig grew a member with mutable indirections
    /// (Nullable!NudgeConfig - string[] ladders) — const(CodeModelConfig) does
    /// not implicitly convert to CodeModelConfig.
    CodeModelConfig activeCodeModel() @safe {
        if (codeModels.length == 0)
            throw new Exception("No code models configured");
        if (activeCodeModelIndex < 0 || activeCodeModelIndex >= codeModels.length)
            throw new Exception(i"Active code model index $(activeCodeModelIndex) is out of bounds (count: $(
                    codeModels.length))".text);
        return codeModels[activeCodeModelIndex];
    }

    /// Return: the name of the active model.
    string activeModelName() @safe {
        return activeCodeModel().modelName;
    }

    /// Return: the display name of the active model.
    string activeModelDisplayName() @safe {
        return activeCodeModel().display;
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
            if (model.display.toLower == lowerName) {
                matchCount++;
                matchIndex = i;
            }
        }

        if (matchCount == 0) {
            return i"No model matches '$(name)'. Available models: $(
                    codeModels.map!(m => m.display))".text;
        }
        if (matchCount > 1) {
            return i"Ambiguous model name '$(name)'. Matches: $(
                    codeModels.filter!(m => m.display.toLower == lowerName)
                    .map!(m => m.display))".text;
        }

        activeCodeModelIndex = matchIndex;
        saveState();
        return null;
    }

    /// List all configured model names with index and active indicator.
    string[] listModels() const @safe {
        auto app = appender!(string[])();
        foreach (i, model; codeModels) {
            app.put(i"$(model.display) (index: $(i))$(i == activeCodeModelIndex ? " [active]" : "")"
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
            import llm.utility : getValue;

            activeChatSessionId = getValue!(string)(json, v => v["activeChatSessionId"].str, "");
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
            stateObj["activeChatSessionId"] = activeChatSessionId;
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

    /// Read a prompt file raw from promptDir. No composition: unlike
    /// getPrompt, no skills/agentMd/rag blocks are appended. THROWS when the
    /// file is missing (mirrors getBasePrompt).
    string readPromptFile(string name) {
        import llm.vfs : FlatVfs;

        auto vfs = FlatVfs(promptDir);
        return vfs.read(name).match!((string a) => a, (_) {
            logger.warningf("Prompt '%s' not found", name);
            throw new Exception("Prompt file not found: " ~ name);
            return null;
        });
    }

    /// Compose system prompt: basePrompt → alwaysApplyBlock → agentMdSummary → ragInstruction → manifestXml.
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
        (xdgDataHome ~ Path(ProgramName) ~ Path("skills")),
        (xdgDataHome ~ Path(ProgramName) ~ Path("chat")),
        (xdgDataHome ~ Path(ProgramName) ~ Path("dialogue"))
    ].filter!(a => !a.exists)) {
        try {
            logger.trace("Creating directory ", path);
            mkdirRecurse(path);
        } catch (Exception e) {
            logger.warning(e);
        }
    }
}

void makeLocalSetupFileStructure(LlmConfig conf) {
    import std.file : mkdirRecurse;

    foreach (path; [
        conf.dataDir, conf.workArea, conf.dataDir ~ "chat",
        conf.dataDir ~ "memory", conf.dataDir ~ "dialogue"
    ].filter!(a => !a.empty && !a.exists)) {
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

struct ToolBrokerConfig {
    /// Kill switch: false ⇒ all tools treated as untagged (alwaysOn) and
    /// discovery inert.
    bool enabled = true;
    /// tag - human-readable description. Unknown keys warn at startup (typo protection).
    string[string] toolTagDescriptions;
    /// Tools never hidden regardless of LlmConfig.toolFilter. A neverHide tool missing
    /// from the registry, or one that is tagged, warns at startup.
    string[] neverHideTools = ["taskDone"];
}

struct CodeModelConfig {
    ServerConfig server;
    /// What is shown to the user
    string display;
    /// The name of the model in the request to the server
    string modelName;
    double temp = 0.0;
    long contextSize;
    long maxTokens;
    /// Per-model nudge override. Whole-block semantics: when set,
    /// this is the model's ENTIRE policy - fields not restated here fall
    /// back to struct defaults, NOT to the global LlmConfig.nudges.
    /// Omit the block entirely to inherit the global policy.
    Nullable!NudgeConfig nudges;
}

struct SummaryModelConfig {
    ServerConfig server;
    string modelName;
    string prompt = "SUMMARY.md";
    double temp = 0.0;
    long contextSize;
    long contextChunkSize = 32768;
    long maxTokens;
}

struct VisionModelConfig {
    ServerConfig server;
    string modelName;
    string systemPrompt;
    double temp = 0.0;
    long contextSize;
    long maxTokens;
    long timeoutSecs = 60;

    invariant {
        assert(!modelName.empty, "Vision model name must not be empty");
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
        case EndpointType.openAiv1:
            break;
        case EndpointType.llamaCpp:
            break;
        case EndpointType.deepseek:
            if (maxTokens == -1)
                j["max_tokens"] = null;
            break;
        }

        if (!cfg.jsonFields.empty) {
            try {
                auto fields = parseJSON(cfg.jsonFields);
                foreach (key, value; fields.object) {
                    j[key] = value;
                }
            } catch (Exception e) {
                logger.warningf("Unable to parse jsonFields '%s' in model config '%s': %s",
                        cfg.jsonFields, model, e.msg);
            }
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
         verbosity: cast(int) conf.server.httpVerbosity,
         apiKey: conf.server.apiKeyEnv.empty ? "" : getEnvApiKey(conf.server.apiKeyEnv),
         header: makeHeader(conf.modelName, conf.temp, conf.maxTokens, conf.server));
    // dfmt on
}

/// Load execution environments into `conf.sandboxConfig`, mirroring the image
/// catalog loading done in `readConfigInternal`. Supports system/user split
/// with user entries overriding system entries by tag.
///
/// When `systemExecutionEnvironmentsFile` or `userExecutionEnvironmentsFile`
/// are configured in `SandboxConfig`, they are loaded from paths relative to
/// `configDir` and merged (user overrides system).
///
/// A missing config file disables command execution (empty backend list,
/// `ExecutionConfigState.notConfigured`). `loadExecutionBackends` handles all
/// logging; this function only applies the results.
private void loadExecutionEnvironments(ref LlmConfig conf,
        Path explicitConfigFile, string configDir, bool loadedAnyFile) {
    import std.path : buildPath;

    // If system/user files are configured, use them with merge logic.
    if (!conf.sandboxConfig.systemExecutionEnvironmentsFile.empty
            || !conf.sandboxConfig.userExecutionEnvironmentsFile.empty) {

        EnvironmentBackend[] systemEntries;
        ExecutionConfigState systemState = ExecutionConfigState.notConfigured;
        string systemDefaultTag;

        if (!conf.sandboxConfig.systemExecutionEnvironmentsFile.empty) {
            try {
                auto systemPath = AbsolutePath(buildPath(configDir,
                        conf.sandboxConfig.systemExecutionEnvironmentsFile));
                systemEntries = loadExecutionBackends(systemPath,
                        conf.sandboxConfig.defaultOptions, systemState, systemDefaultTag);
            } catch (Exception e) {
                logger.warningf("Failed to load system execution environments '%s': %s",
                        conf.sandboxConfig.systemExecutionEnvironmentsFile, e.msg);
                systemState = ExecutionConfigState.loadFailed;
            }
        }

        EnvironmentBackend[] userEntries;
        ExecutionConfigState userState = ExecutionConfigState.notConfigured;
        string userDefaultTag;

        if (!conf.sandboxConfig.userExecutionEnvironmentsFile.empty) {
            try {
                auto userPath = AbsolutePath(buildPath(configDir,
                        conf.sandboxConfig.userExecutionEnvironmentsFile));
                userEntries = loadExecutionBackends(userPath,
                        conf.sandboxConfig.defaultOptions, userState, userDefaultTag);
            } catch (Exception e) {
                logger.warningf("Failed to load user execution environments '%s': %s",
                        conf.sandboxConfig.userExecutionEnvironmentsFile, e.msg);
                userState = ExecutionConfigState.loadFailed;
            }
        }

        if (systemState == ExecutionConfigState.loaded || userState == ExecutionConfigState.loaded) {
            EnvironmentBackend[string] merged;

            // User entries take priority (inserted first, not overwritten by system)
            foreach (entry; userEntries) {
                if (entry.tag !in merged) {
                    merged[entry.tag] = entry;
                }
            }

            // System entries fill gaps not covered by user entries
            foreach (entry; systemEntries) {
                if (entry.tag !in merged) {
                    merged[entry.tag] = entry;
                }
            }

            conf.sandboxConfig.executionEnvironments = merged.byValue.array;
            conf.sandboxConfig.executionConfigState = ExecutionConfigState.loaded;
        } else {
            conf.sandboxConfig.executionConfigState = ExecutionConfigState.loadFailed;
        }

        return;
    }
}

// Configuration format decision (2026-08): llmfun reads ONLY YAML config
// files (config.yaml / .llmfun.yaml / any -c path). Legacy JSON names
// (.llmfun.json, config.json) are never read — no fallback, no deprecation
// window, no migration. Machine-managed JSON (state.json, chat sessions,
// monitor.jsonl, protocol payloads) is unaffected.
LlmConfig readConfig(Path path, bool silent = false, bool noCwdConfig,
        bool trustedConfig, Path userCliWorkArea = Path.init) {
    import std.file : getcwd;
    import std.process : environment;
    import my.xdg : xdgConfigHome;

    auto systemConfigPath = environment.get("LLMFUN_SYSTEM_CONFIG",
            (xdgConfigHome ~ Path(ProgramName) ~ Path("config.yaml")).toString).Path;
    return readConfigInternal(path: path, silent: silent, noCwdConfig: noCwdConfig, trustedConfig: trustedConfig,
            userCliWorkArea: userCliWorkArea, cwd: Path(getcwd()),
            systemConfigPath: systemConfigPath);
}

private LlmConfig readConfigInternal(Path path, bool silent = false, bool noCwdConfig,
        bool trustedConfig, Path userCliWorkArea = Path.init, Path cwd = Path.init,
        Path systemConfigPath = Path.init) {
    import std.path : buildPath, dirName;

    LlmConfig conf;
    bool loadedAnyFile = false;
    string configDir = "."; // Directory of the last successfully loaded config file

    void layerOneLoad() {
        // Layer 1: Base config from LLMFUN_SYSTEM_CONFIG
        if (systemConfigPath.exists) {
            logger.infof(!silent, "Reading base configuration from %s", systemConfigPath);
            try {
                conf = applyLlmConfig(conf, loadYamlValue(systemConfigPath));
                loadedAnyFile = true;
                configDir = systemConfigPath.dirName;
            } catch (Exception e) {
                logger.errorf(!silent, "Failed to load base config %s: %s",
                        systemConfigPath, e.msg);
            }
        } else {
            logger.infof(!silent,
                    "No base configuration found (LLMFUN_SYSTEM_CONFIG not set or file missing)");
        }
    }

    void layerTwoLoad() {
        // Layer 2: Overlay config. Skip CWD config if workArea == CWD unless --trusted-config.
        Path overlayPath;

        if (!path.empty) {
            overlayPath = path; // from -c/--config (explicit path, always trusted)
        } else if (!noCwdConfig) {
            // Determine effective workArea for the CWD check.
            // CLI-specified workArea (-w) takes priority over config file workArea.
            Path effectiveWorkArea = !userCliWorkArea.empty ? userCliWorkArea : conf.workArea;
            bool workAreaIsCwd = effectiveWorkArea.toString.among(".", "./")
                || AbsolutePath(effectiveWorkArea) == AbsolutePath(cwd);
            if (workAreaIsCwd && !trustedConfig) {
                logger.warningf(!silent, "Skipping CWD config: workarea equals CWD (%s). Use --trusted-config to allow loading .llmfun.yaml from CWD, or --no-cwd-config to suppress this warning.",
                        cwd);
                overlayPath = Path.init;
            } else {
                overlayPath = buildPath(cwd, ".llmfun.yaml").Path;
            }
        }

        if (!overlayPath.empty && overlayPath.exists) {
            logger.infof(!silent, "Reading project configuration from '%s'", overlayPath);
            try {
                conf = applyLlmConfig(conf, loadYamlValue(overlayPath));
                loadedAnyFile = true;
                configDir = overlayPath.dirName;
            } catch (Exception e) {
                logger.errorf(!silent, "Failed to load project config %s: %s", overlayPath, e.msg);
            }
        } else if (!overlayPath.empty) {
            logger.tracef(!silent, "No project configuration found at %s", overlayPath);
        }
    }

    layerOneLoad();
    layerTwoLoad();
    if (loadedAnyFile) {
        validateConfig(conf);
    }

    conf.resolvePaths(!noCwdConfig);
    conf.loadState();

    // Load execution environments: system layer 1, user layer 2.
    // User environment entries override system entries with the same tag.
    loadExecutionEnvironments(conf, path, configDir, loadedAnyFile);
    return conf;
}

private EmbedConfig embedConfigFromValue(JSONValue json) {
    import std.exception : enforce;

    if ("type" !in json) {
        throw new Exception("embedConfig missing required field 'type'");
    }

    string type = json["type"].str;
    json.object.remove("type");
    if (type == "remote") {
        return EmbedConfig(applyConfig!(RemoteEmbedConfig)(RemoteEmbedConfig.init, json));
    }
    if (type == "local") {
        return EmbedConfig(applyConfig!(LocalEmbedConfig)(LocalEmbedConfig.init, json));
    }
    throw new Exception("embedConfig: unknown type '" ~ type ~ "', expected 'remote' or 'local'");
}

auto applyConfig(ConfigT)(ConfigT conf, JSONValue json) {
    import std.traits;

    template NullableInner(T) {
        static if (is(T == Nullable!U_, U_))
            alias NullableInner = U_;
        else
            alias NullableInner = void;
    }

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

    logger.trace("apply config start: " ~ ConfigT.stringof);
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
                                    a => applyConfig(CodeModelConfig.init, a)).array;
                        } else static if (is(Type : string)) {
                            __traits(getMember, conf, llmMemberName) = json[llmMemberName].str;
                        } else static if (is(Type : bool)) {
                            __traits(getMember, conf, llmMemberName) = json[llmMemberName].boolean;
                        } else static if (is(Type == enum)) {
                            // Enum fields are configured by member name, e.g. `mode: mixed`.
                            __traits(getMember, conf, llmMemberName) = json[llmMemberName]
                                .str.to!Type;
                        } else static if (isFloatingPoint!Type) {
                            __traits(getMember, conf, llmMemberName) = json[llmMemberName].floating;
                        } else static if (isIntegral!Type) {
                            __traits(getMember, conf, llmMemberName) = cast(Type) json[llmMemberName]
                                .integer;
                        } else static if (is(Type == string[][string])) {
                            foreach (key, ref JSONValue valArr; json[llmMemberName].object) {
                                string[] vals;
                                foreach (v; valArr.array) {
                                    vals ~= v.str;
                                }
                                __traits(getMember, conf, llmMemberName)[key] = vals;
                            }
                        } else static if (is(Type == string[string])) {
                            foreach (key, ref JSONValue val; json[llmMemberName].object) {
                                __traits(getMember, conf, llmMemberName)[key] = val.str;
                            }
                        } else static if (is(Type : string[])) {
                            __traits(getMember, conf, llmMemberName) = json[llmMemberName].array.map!(a => a.str)
                                .array;
                        } else static if (is(Type : EmbedConfig)) {
                            __traits(getMember, conf, llmMemberName) = embedConfigFromValue(
                                    json[llmMemberName]);
                        } else static if (isNullableType!Type
                                && isAggregateType!(NullableInner!Type)) {
                            // Handle Nullable!T for aggregate types (e.g., Nullable!VisionModelConfig)
                            alias InnerT = NullableInner!Type;
                            auto val = json[llmMemberName];
                            if (val.type != JSONType.NULL) {
                                auto innerConf = InnerT.init;
                                __traits(getMember, conf, llmMemberName) = Nullable!InnerT(applyConfig(innerConf,
                                        val));
                            }
                        } else static if (isAggregateType!Type) {
                            __traits(getMember, conf, llmMemberName) = applyConfig(__traits(getMember,
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
        logger.warningf("Unknown configuration key %s.%s", ConfigT.stringof, k);
    }

    logger.trace("apply config done: " ~ ConfigT.stringof);
    return conf;
}

/// Emit warnings for models configured without API keys.
/// Called from validateConfig() after all hard validation checks.
/// No-op when warnIfNoApiKey is false or OPENAI_API_KEY env var is set.
private void checkApiKeyWarnings(LlmConfig conf) {
    bool warned;

    void warnOrNot(ref ServerConfig conf, string name, string helpText) {
        if (conf.warnIfNoApiKey && getEnvApiKey(conf.apiKeyEnv).empty) {
            logger.warningf("No API key found in environment variable '%s' for %s: %s",
                    conf.apiKeyEnv, helpText, name);
            warned = true;
        }

        if (!conf.jsonFields.empty) {
            try {
                auto dummy = parseJSON(conf.jsonFields);
            } catch (Exception e) {
                logger.warningf("Unable to parse jsonFields '%s' in model config '%s': %s",
                        conf.jsonFields, name, e.msg);
                warned = true;
            }
        }
    }

    foreach (model; conf.codeModels) {
        warnOrNot(model.server, model.display, "code model");
    }

    if (!conf.summaryModel.server.url.empty) {
        warnOrNot(conf.summaryModel.server, conf.summaryModel.modelName, "summary model");
    }

    if (!conf.visionModel.isNull && !conf.visionModel.get.server.url.empty) {
        warnOrNot(conf.visionModel.get.server, conf.visionModel.get.modelName, "vision model");
    }

    conf.embedConfig.match!((RemoteEmbedConfig r) {
        warnOrNot(r.server, r.modelName, "embed model");
    }, (LocalEmbedConfig) {} // No API key needed for local embed
    );

    if (warned) {
        logger.warningf("To suppress these warnings, set 'warnIfNoApiKey' to false or provide the an environment variable with the API key.");
    }
}

/// Validate one escalation ladder of the nudge policy:
/// non-negative strike counts, and — only when the kind is enabled —
/// non-empty user-supplied file names. Empty lists are allowed (they resolve
/// to the kind's shipped defaults at load time); files of disabled kinds are
/// not validated here. `keyPath` names the offending block in messages
/// (e.g. "nudges.keepReasoning").
private void validateNudgeEscalation(string keyPath, in EscalationConfig esc) {
    if (esc.softStrikes < 0)
        throw new Exception(i"$(keyPath).softStrikes must be >= 0, got $(esc.softStrikes)".text);
    if (esc.hardStrikes < 0)
        throw new Exception(i"$(keyPath).hardStrikes must be >= 0, got $(esc.hardStrikes)".text);
    if (esc.softStrikes == 0 && esc.hardStrikes == 0)
        throw new Exception(keyPath
                ~ ": softStrikes and hardStrikes must not both be 0 (the escalation ladder would fail on the first strike)");
    if (esc.enabled) {
        import std.string : strip;

        foreach (i, file; esc.softNudges)
            if (file.strip().empty)
                throw new Exception(i"$(keyPath).softNudges[$(i)] must not be an empty file name"
                        .text);
        foreach (i, file; esc.hardNudges)
            if (file.strip().empty)
                throw new Exception(i"$(keyPath).hardNudges[$(i)] must not be an empty file name"
                        .text);
    }
}

/// Validate a whole nudge policy (NudgeConfig structs): both escalation
/// ladders, the compression threshold range and its `>= 0.9` warning, and the
/// feedback floors. `keyPath` names the block in messages (e.g. "nudges" for
/// the global default, "codeModels[0].nudges" for a per-model wholesale
/// override). Files of disabled kinds are neither loaded nor validated here.
private void validateNudgeConfig(string keyPath, in NudgeConfig n) {
    validateNudgeEscalation(keyPath ~ ".keepReasoning", n.keepReasoning);
    validateNudgeEscalation(keyPath ~ ".recovery", n.recovery);

    immutable forcedCompression = 0.9; // the hard-coded forced point
    if (!(n.compression.threshold > 0.0) || !(n.compression.threshold < 1.0))
        throw new Exception(i"$(keyPath).compression.threshold must be in (0.0, 1.0) and below 0.9 to ever fire (values in [0.9, 1.0) only warn), got $(
                n.compression.threshold)".text);
    if (n.compression.enabled && n.compression.threshold >= forcedCompression)
        logger.warningf("%s.compression.threshold %s is at or above the forced-compression point %.1f — the advisory compression nudge can never fire",
                keyPath, n.compression.threshold, forcedCompression);

    if (n.feedback.intervalSecs < 1)
        throw new Exception(i"$(keyPath).feedback.intervalSecs must be >= 1, got $(
                n.feedback.intervalSecs) (with enabled: false the value is ignored; 0 would warn after every tool call)"
                .text);
    if (n.feedback.minToolCalls < 1)
        throw new Exception(i"$(keyPath).feedback.minToolCalls must be >= 1, got $(
                n.feedback.minToolCalls) (with enabled: false the value is ignored; 0 would warn after every tool call)"
                .text);
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
        if (model.modelName.empty)
            throw new Exception(i"codeModels[$(i)].modelName must not be empty".text);
        if (model.display.empty)
            throw new Exception(i"codeModels[$(i)].display must not be empty".text);
        if (model.server.url.empty)
            throw new Exception(i"codeModels[$(i)].server.url must not be empty for $(
                    model.modelName)".text);
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

    if (conf.tui.maxWidth != 0 && (conf.tui.maxWidth < 40 || conf.tui.maxWidth > 10_000))
        throw new Exception(i"tui.maxWidth must be 0 or in [40, 10000], got $(conf.tui.maxWidth)"
                .text);

    // Nudge policy (NudgeConfig structs), global default and per-model
    // wholesale overrides. Files of disabled kinds are neither loaded
    // nor validated here.
    validateNudgeConfig("nudges", conf.nudges);
    foreach (i, model; conf.codeModels)
        if (!model.nudges.isNull)
            validateNudgeConfig(i"codeModels[$(i)].nudges".text, model.nudges.get);

    // Emit warnings for missing API keys (after all hard validation)
    checkApiKeyWarnings(conf);
}

/// The discovery meta-tools (listToolTags, step 1; toolSearch,
/// step 2). They must stay untagged (⇒ alwaysOn): tagged, they
/// would be discovery-gated out of existence — the discovery loop would die
/// with the very tools it is meant to reveal. The UDA-side test lives next to
/// the tools (tool_call/discovery.d + search.d); this is the registry-side
/// startup warning (warnings only).
///
/// Takes the whole registry snapshot as name → comma-joined tags (the caller
/// owns the RegFunction import and builds it); the two meta-tool names live
/// ONLY here, so a third discovery meta-tool means touching one line. The
/// helper stays import-light and trivially testable. An absent name, or one
/// with empty tags (⇒ alwaysOn), is silent here.
string[] discoveryMetaToolTagWarnings(const(string[string]) metaTags) @safe {
    string[] warnings;
    foreach (name; ["listToolTags", "toolSearch"]) {
        if (auto tags = name in metaTags) {
            auto tagList = *tags;
            if (!tagList.empty)
                warnings ~= i"discovery meta-tool '$(name)' is tagged [$(tagList)] — it would be hidden until its tag is activated, disabling discovery; keep it untagged"
                    .text;
        }
    }
    return warnings;
}

/// Startup validation (neverHide + typo protection). Warnings only —
/// never fatal. Not `pure`: reads the global tool registry. Returns the
/// warnings; the caller (Agent ctor) logs them.
/// Reads the tool registry (populated by shared static ctors, which run
/// before main), so it is only meaningful after module construction.
string[] validateToolBrokerConfig(const LlmConfig conf) @safe {
    import llm.tool_call : RegFunction, getFunctions;
    import llm.tool_call.tags : knownToolTagNames, unknownToolTags;

    string[] warnings;

    // Registry lookup: name -> tags (empty tags = alwaysOn).
    RegFunction[string] registry;
    foreach (func; getFunctions)
        registry[func.name] = func;

    // A neverHide tool is too important to hide silently — a name that is
    // not in the registry cannot be protected, so warn.
    // A tagged neverHide tool is NOT discovery-gated (selectTools exempts
    // neverHide names from the tag filter), so tagging it has no hiding
    // effect — the tag is pointless; warn.
    foreach (name; conf.toolBroker.neverHideTools) {
        auto func = name in registry;
        if (func is null) {
            warnings ~= i"neverHideTools: tool '$(name)' is not in the registry — it cannot be protected from hiding"
                .text;
        } else if (!func.tags.empty) {
            auto tagList = func.tags.join(", ");
            warnings ~= i"neverHideTools: tool '$(name)' is tagged [$(tagList)] — tagging it has no hiding effect: neverHide exempts it from tag gating, so it stays always-visible"
                .text;
        }
    }

    // Typo protection: a toolTagDescriptions key that is not
    // a KnownToolTag member never reaches any tool — almost certainly a
    // misspelling.
    auto descTags = conf.toolBroker.toolTagDescriptions.byKey.array;
    foreach (tag; unknownToolTags(descTags))
        warnings ~= i"toolTagDescriptions: unknown tag '$(tag)' (known tags: $(knownToolTagNames()))"
            .text;

    // The discovery meta-tools must stay untagged — a
    // tagged meta-tool would be discovery-gated out of existence. One
    // name → comma-joined tags snapshot over the whole registry; the
    // meta-tool names themselves live only inside the helper, so a
    // third meta-tool is a one-line change there. An untagged (or absent)
    // name joins as "" (empty) and is silent.
    string[string] metaTags;
    foreach (f; registry.values)
        metaTags[f.name] = f.tags.join(", ");
    warnings ~= discoveryMetaToolTagWarnings(metaTags);

    return warnings;
}

alias applyLlmConfig = applyConfig!LlmConfig;

/// Returns the key from the enviroment variable, or "" if not set.
string getEnvApiKey(string envVariable) {
    import std.process : environment;

    return environment.get(envVariable, null);
}

/// Replace magic words in text with actual paths.
/// Supports @{llmfun} (binary directory) and @{llmfun_workarea} (workarea path).
auto replaceMagicWord(T)(T variable, AbsolutePath workArea) @safe nothrow {
    import std.path : dirName;
    import std.file : thisExePath;
    import std.string : replace;

    immutable BinaryMagic = "@{llmfun}";
    immutable WorkareaMagic = "@{llmfun_workarea}";

    string s;
    static if (is(T == Path) || is(T == AbsolutePath)) {
        s = variable.toString;
    } else {
        s = variable;
    }

    auto result = s.replace(WorkareaMagic, workArea.toString);
    try {
        result = result.replace(BinaryMagic, thisExePath.dirName);
    } catch (Exception e) {
        try {
            logger.warningf("Unable to replace %s with llmfun executables path in variable with content: %s",
                    WorkareaMagic, variable);
        } catch (Exception e) {
        }
    }

    static if (!is(T : string))
        return T(result);
    else
        return result;
}

/// Apply magic word substitution to all values in an options map.
/// Keys are not modified. Supports @{llmfun} and @{llmfun_workarea}.
string[][string] replaceContainerMagicWords(string[][string] options, AbsolutePath workArea) @safe nothrow {
    string[][string] result;
    try {
        foreach (key, values; options) {
            string[] newValues;
            foreach (v; values) {
                newValues ~= replaceMagicWord(v, workArea);
            }
            result[key] = newValues;
        }
    } catch (Exception e) {
        // fix for ldc-1.40. Remove when min compiler is updated
    }
    return result;
}

/// Test: @{llmfun_workarea} replaced with workarea path.
unittest {
    auto result = replaceMagicWord!string("@{llmfun_workarea}/file.txt",
            AbsolutePath("/my/workarea"));
    assert(result == "/my/workarea/file.txt", result);
}

/// Test: @{llmfun} replaced with binary directory.
unittest {
    auto result = replaceMagicWord!string("@{llmfun}/bin/tool", AbsolutePath("/my/workarea"));
    assert(result == i"$(thisExePath.dirName)/bin/tool".text);
}

/// Test: both magic words in same value.
unittest {
    auto result = replaceMagicWord!string(
            "-v @{llmfun_workarea}:/work -v @{llmfun}/data:/data", AbsolutePath("/my/work"));
    assert(result == i"-v /my/work:/work -v $(thisExePath.dirName)/data:/data".text);
}

/// Test: no magic words — values unchanged.
unittest {
    auto result = replaceMagicWord!string("just a plain string", AbsolutePath("/my/workarea"));
    assert(result == "just a plain string");
}

/// Test: replaceContainerMagicWords with empty options returns empty.
unittest {
    string[][string] options;
    auto result = replaceContainerMagicWords(options, AbsolutePath("/work"));
    assert(result.length == 0);
}

/// Test: keys are not modified in replaceContainerMagicWords.
unittest {
    string[][string] options;
    options["@{llmfun_workarea}"] = ["@{llmfun_workarea}/path"];
    auto result = replaceContainerMagicWords(options, AbsolutePath("/work"));
    assert(result.length == 1);
    assert(result["@{llmfun_workarea}"][0] == "/work/path");
}

/// Test: multiple values in array each processed.
unittest {
    string[][string] options;
    options["mounts"] = ["-v", "@{llmfun_workarea}:/w", "@{llmfun}/data:/d"];
    auto result = replaceContainerMagicWords(options, AbsolutePath("/work"));
    assert(result["mounts"][0] == "-v");
    assert(result["mounts"][1] == "/work:/w");
    assert(result["mounts"][2] == i"$(thisExePath.dirName)/data:/d".text);
}

/// Test: applyConfig parses string[][string] correctly.
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    struct TestConfig {
        string[][string] options;
    }

    auto tmpDir = buildPath("llmfun_test", "config_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "test.yaml");
    string yaml = `options:
  security: ["--read-only"]
  network: ["--network", "none"]
`;
    File(tmpFile, "w").write(yaml);
    auto conf = applyConfig!(TestConfig)(TestConfig.init, loadYamlValue(Path(tmpFile)));
    assert(conf.options.length == 2);
    assert(conf.options["security"] == ["--read-only"]);
    assert(conf.options["network"] == ["--network", "none"]);
}

/// Test: enum-valued config fields parse from the member name string and keep
/// the default when the value is not a valid member.
unittest {
    auto ec = embedConfigFromValue(parseJSON(`{"type": "local", "mode": "cpu"}`));
    ec.match!((LocalEmbedConfig l) {
        assert(l.mode == EmbedMode.cpu, "mode: cpu must parse");
    }, (RemoteEmbedConfig) { assert(false, "expected a local embed config"); });

    ec = embedConfigFromValue(parseJSON(`{"type": "local", "mode": "gpu"}`));
    ec.match!((LocalEmbedConfig l) {
        assert(l.mode == EmbedMode.gpu, "mode: gpu must parse");
    }, (RemoteEmbedConfig) { assert(false, "expected a local embed config"); });

    // Absent mode keeps the default.
    ec = embedConfigFromValue(parseJSON(`{"type": "local"}`));
    ec.match!((LocalEmbedConfig l) {
        assert(l.mode == EmbedMode.cpu, "absent mode must default to cpu");
    }, (RemoteEmbedConfig) { assert(false, "expected a local embed config"); });

    // Invalid member name: warns (not fails silently) and keeps the default.
    import llm.agent.nudges : sharedLogSwapMutex;
    import std.algorithm : canFind;
    import std.array : join;

    synchronized (sharedLogSwapMutex) {
        auto prevLog = logger.sharedLog;
        auto cap = cast(shared) new CfgLogCapture();
        logger.sharedLog = cap;
        scope (exit)
            logger.sharedLog = prevLog;

        auto conf = applyConfig!(LocalEmbedConfig)(LocalEmbedConfig.init,
                parseJSON(`{"mode": "bogus"}`));
        assert(conf.mode == EmbedMode.cpu, "invalid mode must fall back to the default");
        assert(canFind((cast() cap).takeLines().join("\n"),
                "unable to read 'mode'"), "invalid mode must warn");
    }
}

/// Test: Explicit config path always loads regardless of trusted-config.
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "trustedconfig_1");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    // Create a config file with a custom value we can check
    auto configFile = buildPath(tmpDir, "test_config.yaml");
    string configYaml = `sandboxConfig:
  maxOutputBytes: 42
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(configFile, "w").write(configYaml);

    // Explicit config path should always load
    auto conf = readConfig(configFile.Path, silent: true, noCwdConfig: false,
            trustedConfig: false);
    assert(conf.sandboxConfig.maxOutputBytes == 42);
}

/// Test: --no-cwd-config skips CWD config entirely.
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "trustedconfig_2");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto configFile = buildPath(tmpDir, ".llmfun.yaml");
    string configYaml = `codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(configFile, "w").write(configYaml);

    // With --no-cwd-config, no CWD config should be loaded
    auto conf = readConfigInternal(Path.init, silent: true, noCwdConfig: true, trustedConfig: false,
            userCliWorkArea: tmpDir.Path, cwd: tmpDir.Path, systemConfigPath: Path.init);
    assert(conf.codeModels.empty, "No config should have been loaded: " ~ conf.to!string);

    // With workArea == CWD and no --trusted-config, the CWD config is skipped.
    conf = readConfigInternal(Path.init, silent: true, noCwdConfig: false, trustedConfig: false,
            userCliWorkArea: tmpDir.Path, cwd: tmpDir.Path, systemConfigPath: Path.init);
    assert(conf.codeModels.empty,
            "CWD config must be skipped when workArea == CWD without --trusted-config: "
            ~ conf.to!string);

    // with trusted it should load
    conf = readConfigInternal(Path.init, silent: true, noCwdConfig: false, trustedConfig: true,
            userCliWorkArea: tmpDir.Path, cwd: tmpDir.Path, systemConfigPath: Path.init);
    assert(!conf.codeModels.empty, "Config should have been loaded: " ~ conf.to!string);
}

/// Test: a legacy `.llmfun.json` alone is ignored — no fallback read.
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "legacy_ignored_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    // Only the old JSON name exists in this directory.
    auto legacyFile = buildPath(tmpDir, ".llmfun.json");
    string legacyJson = `{"codeModels":[{"name":"legacy","server":{"url":"http://localhost:8080"}}]}`;
    File(legacyFile, "w").write(legacyJson);

    // Even with --trusted-config, the legacy JSON file must not be loaded.
    auto conf = readConfigInternal(Path.init, silent: true, noCwdConfig: false, trustedConfig: true,
            userCliWorkArea: tmpDir.Path, cwd: tmpDir.Path, systemConfigPath: Path.init);
    assert(conf.codeModels.empty, "Legacy .llmfun.json must be ignored: " ~ conf.to!string);
}

/// Test: layer 1 loads `config.yaml` (or any YAML name) from LLMFUN_SYSTEM_CONFIG.
unittest {
    import std.path : buildPath;
    import std.process : environment;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "default_config_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto configFile = buildPath(tmpDir, "my_base.yaml");
    string configYaml = `warnIfNoApiKey: false
codeModels:
  - modelName: base
    display: test42
    server:
      url: http://localhost:8080
`;
    File(configFile, "w").write(configYaml);

    // Point LLMFUN_SYSTEM_CONFIG at the YAML file; restore the old value after.
    string oldValue;
    if ("LLMFUN_SYSTEM_CONFIG" in environment)
        oldValue = environment["LLMFUN_SYSTEM_CONFIG"];
    scope (exit) {
        if (oldValue is null)
            environment.remove("LLMFUN_SYSTEM_CONFIG");
        else
            environment["LLMFUN_SYSTEM_CONFIG"] = oldValue;
    }

    environment["LLMFUN_SYSTEM_CONFIG"] = configFile;
    auto conf = readConfig(Path.init, silent: true, noCwdConfig: true, trustedConfig: false);
    assert(conf.codeModels.length == 1, "Layer 1 YAML config must be loaded: " ~ conf.to!string);
    assert(conf.codeModels[0].modelName == "base");
}

/// Test: the shipped config/example.yaml loads with a complete defaultOptions
/// map (guards the YAML quoting: unquoted "0.5"/"60" would become non-strings
/// and the merger would silently drop the whole map, incl. 06_network).
unittest {
    // Parse guard: the shipped file must load, and every defaultOptions item
    // must be a string (the merger's .str access would throw otherwise).
    auto json = loadYamlValue(Path("config/example.yaml"));
    foreach (key, val; json["sandboxConfig"]["defaultOptions"].object) {
        foreach (item; val.array) {
            assert(item.type == JSONType.STRING,
                    "defaultOptions['" ~ key ~ "'] item must be a string: " ~ item.to!string);
        }
    }

    // Full pipeline: the merged config must keep every defaultOptions entry.
    auto conf = readConfigInternal(Path("config/example.yaml"), silent: true, noCwdConfig: true,
            trustedConfig: false, userCliWorkArea: Path.init, cwd: Path.init,
            systemConfigPath: Path.init);
    auto opts = conf.sandboxConfig.defaultOptions;
    assert(opts.length == 8, "defaultOptions must have 8 keys, got " ~ opts.length.to!string);
    assert(opts["00_subcommand"] == ["run"]);
    assert(opts["01_cleanup"] == ["--rm"]);
    assert(opts["02_user"] == ["--user", "1000:1000"]);
    assert(opts["03_resources"] == ["--memory", "256m", "--cpus", "0.5"]);
    assert(opts["04_tmpfs"] == ["--tmpfs", "/tmp:rw,noexec,nosuid,size=64m"]);
    assert(opts["05_timeout"] == ["--stop-timeout", "60"]);
    assert(opts["06_network"] == ["--network", "none"]);
    assert(opts["entrypoint_shell"] == ["sh", "-c"]);

    // The tool-broker keys shipped in the example: neverHideTools overrides
    // the default (it un-hides the memory tools) and the tag vocabulary lists
    // every known tool tag.
    assert(conf.toolBroker.enabled, "toolBroker.enabled must default to true");
    assert(conf.toolBroker.neverHideTools == [
        "taskDone", "readMemory", "getMemoryTopics", "writeMemory"
    ], "shipped example neverHideTools: " ~ conf.toolBroker.neverHideTools.to!string);
    assert(conf.toolBroker.toolTagDescriptions.length == 10,
            "shipped example tag vocabulary: " ~ conf.toolBroker.toolTagDescriptions.to!string);
    import std.algorithm : canFind;

    assert(conf.toolBroker.toolTagDescriptions["workarea"].canFind("workarea"));
}

/// Test: tui.maxWidth parses from YAML into LlmConfig.tui.maxWidth.
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "tuintest_parse");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "test.yaml");
    string yaml = `tui:
  maxWidth: 255
`;
    File(tmpFile, "w").write(yaml);
    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    assert(conf.tui.maxWidth == 255, "expected 255, got " ~ conf.tui.maxWidth.to!string);
}

/// Test: absent tui key keeps the default maxWidth = 0.
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "tuintest_absent");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "test.yaml");
    string yaml = `codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(tmpFile, "w").write(yaml);
    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    assert(conf.tui.maxWidth == 0, "expected default 0, got " ~ conf.tui.maxWidth.to!string);
}

/// Test: maxWidth below 40 is rejected by validateConfig.
unittest {
    import std.path : buildPath;
    import std.stdio : File;
    import std.algorithm : canFind;

    auto tmpDir = buildPath("llmfun_test", "tuintest_toosmall");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "test.yaml");
    string yaml = `codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
tui:
  maxWidth: 20
`;
    File(tmpFile, "w").write(yaml);
    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    assert(conf.tui.maxWidth == 20, "parse must accept 20, got " ~ conf.tui.maxWidth.to!string);
    bool threw;
    string msg;
    try {
        validateConfig(conf);
    } catch (Exception e) {
        threw = true;
        msg = e.msg;
    }
    assert(threw, "validateConfig must throw for maxWidth < 40");
    assert(canFind(msg, "tui.maxWidth"), "unexpected error message: " ~ msg);
}

/// Test: maxWidth above 10000 is rejected by validateConfig.
unittest {
    import std.path : buildPath;
    import std.stdio : File;
    import std.algorithm : canFind;

    auto tmpDir = buildPath("llmfun_test", "tuintest_toobig");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "test.yaml");
    string yaml = `codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
tui:
  maxWidth: 999999
`;
    File(tmpFile, "w").write(yaml);
    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    assert(conf.tui.maxWidth == 999999,
            "parse must accept 999999, got " ~ conf.tui.maxWidth.to!string);
    bool threw;
    string msg;
    try {
        validateConfig(conf);
    } catch (Exception e) {
        threw = true;
        msg = e.msg;
    }
    assert(threw, "validateConfig must throw for maxWidth > 10000");
    assert(canFind(msg, "tui.maxWidth"), "unexpected error message: " ~ msg);
}

/// Test: shipped config/example.yaml still parses; tui key absent → maxWidth 0.
unittest {
    auto json = loadYamlValue(Path("config/example.yaml"));
    auto conf = applyLlmConfig(LlmConfig.init, json);
    assert(conf.tui.maxWidth == 0,
            "shipped example.yaml must keep default 0, got " ~ conf.tui.maxWidth.to!string);
}

unittest {
    // Round-trip: an explicit dialogueDir in YAML is preserved through
    // applyConfig (reflection) and not overwritten by resolvePaths (it is set).
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "dialogueDir_roundtrip_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto configFile = buildPath(tmpDir, "test.yaml");
    string yaml = i`dialogueDir: $(tmpDir)
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080`
        .text;
    File(configFile, "w").write(yaml);

    auto conf = readConfig(configFile.Path, silent: true, noCwdConfig: true, trustedConfig: false);
    assert(conf.dialogueDir == tmpDir,
            "dialogueDir round-trip failed: " ~ conf.dialogueDir.to!string);
}

unittest {
    // Absent key -> resolvePaths defaults dialogueDir to dataDir ~ "dialogue".
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "dialogueDir_default_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto configFile = buildPath(tmpDir, "test.yaml");
    string yaml = `codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(configFile, "w").write(yaml);

    auto conf = readConfig(configFile.Path, silent: true, noCwdConfig: false,
            trustedConfig: false);
    assert(!conf.dialogueDir.empty, "dialogueDir should be non-empty after defaulting");
    assert(conf.dialogueDir == (conf.dataDir ~ "dialogue"),
            "dialogueDir default mismatch: " ~ conf.dialogueDir.to!string);
}

unittest {
    // Default: the reasoning-summary prompt ships as the named file.
    assert(LlmConfig().reasoningSummaryPrompt == "REASONING_SUMMARY.md");
}

unittest {
    // Round-trip: an explicit reasoningSummaryPrompt in YAML is preserved
    // through applyConfig (reflection) and not overwritten by resolvePaths.
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "reasoningSummaryPrompt_roundtrip_" ~ __LINE__
            .to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto configFile = buildPath(tmpDir, "test.yaml");
    string yaml = `reasoningSummaryPrompt: MY_PROMPT.md
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(configFile, "w").write(yaml);

    auto conf = readConfig(configFile.Path, silent: true, noCwdConfig: true, trustedConfig: false);
    assert(conf.reasoningSummaryPrompt == "MY_PROMPT.md",
            "reasoningSummaryPrompt round-trip failed: " ~ conf.reasoningSummaryPrompt);
}

unittest {
    // validateConfig accepts the default (empty) and a safe dialogueDir
    import std.exception : assertThrown;

    LlmConfig conf;
    conf.codeModels ~= CodeModelConfig(server: ServerConfig(url: "http://localhost:8080"),
            display: "test", modelName: "test");

    // Empty (the pre-resolution default) is accepted.
    conf.dialogueDir = Path.init;
    validateConfig(conf);

    // A safe explicit value is accepted.
    conf.dialogueDir = (Path("llmfun/data/dialogue"));
    validateConfig(conf);
}

version (unittest) {
    /// Test seam: a Logger that captures formatted messages so a test can
    /// assert on emitted log lines. Installed via the std.logger `sharedLog`
    /// swap (same pattern as D7LogCapture in llm.rag.dialogue_worker).
    private class CfgLogCapture : logger.Logger {
        import core.sync.mutex : Mutex;
        import std.array : Appender;

        private {
            Appender!(string[]) lines;
            Mutex mtx;
        }

        this(const logger.LogLevel lvl = logger.LogLevel.all) {
            super(lvl);
            this.mtx = new Mutex;
        }

        override void writeLogMsg(ref LogEntry payload) @trusted {
            mtx.lock_nothrow();
            scope (exit)
                mtx.unlock_nothrow();
            lines.put(payload.msg);
        }

        string[] takeLines() {
            mtx.lock_nothrow();
            scope (exit)
                mtx.unlock_nothrow();
            auto tmp = lines[];
            lines.clear();
            return tmp;
        }
    }

    /// Minimal LlmConfig that passes validateConfig's non-nudge checks, so
    /// nudge-policy tests can mutate only the nudges block.
    private LlmConfig nudgesValidateTestConfig() {
        LlmConfig conf;
        conf.codeModels ~= CodeModelConfig(server: ServerConfig(url: "http://localhost:8080",
                warnIfNoApiKey: false), display: "test", modelName: "test");
        return conf;
    }
}

/// a global default `nudges:` block parses; every field lands in
/// LlmConfig.nudges (the config schema).
@("global nudges block parses into LlmConfig.nudges")
unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "config_nudges_global");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto tmpFile = buildPath(tmpDir, "test.yaml");
    string yaml = `nudges:
  keepReasoning:
    enabled: true
    softNudges: [NUDGE_KEEP_REASONING_SOFT.md]
    softStrikes: 2
    hardNudges: [NUDGE_KEEP_REASONING_HARD.md]
    hardStrikes: 1
  recovery:
    softNudges: [NUDGE_RECOVERY_SOFT.md]
    softStrikes: 2
    hardNudges: [NUDGE_RECOVERY_HARD.md]
    hardStrikes: 1
  compression:
    enabled: true
    threshold: 0.8
    prompt: NUDGE_COMPRESSION.md
  feedback:
    enabled: true
    intervalSecs: 900
    minToolCalls: 50
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(tmpFile, "w").write(yaml);

    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    auto n = conf.nudges;
    assert(n.keepReasoning.enabled);
    assert(n.keepReasoning.softNudges == ["NUDGE_KEEP_REASONING_SOFT.md"]);
    assert(n.keepReasoning.softStrikes == 2);
    assert(n.keepReasoning.hardNudges == ["NUDGE_KEEP_REASONING_HARD.md"]);
    assert(n.keepReasoning.hardStrikes == 1);
    assert(n.recovery.softNudges == ["NUDGE_RECOVERY_SOFT.md"]);
    assert(n.recovery.softStrikes == 2);
    assert(n.recovery.hardNudges == ["NUDGE_RECOVERY_HARD.md"]);
    assert(n.recovery.hardStrikes == 1);
    assert(n.compression.enabled);
    assert(n.compression.threshold == 0.8);
    assert(n.compression.prompt == "NUDGE_COMPRESSION.md");
    assert(n.feedback.enabled);
    assert(n.feedback.intervalSecs == 900);
    assert(n.feedback.minToolCalls == 50);
}

/// `nudges` absent everywhere → the struct defaults survive.
@("nudges absent everywhere keeps the struct defaults") unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "config_nudges_defaults");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto tmpFile = buildPath(tmpDir, "test.yaml");
    string yaml = `codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(tmpFile, "w").write(yaml);

    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    auto n = conf.nudges;
    assert(n.keepReasoning.enabled);
    assert(n.keepReasoning.softNudges.empty);
    assert(n.keepReasoning.softStrikes == 2);
    assert(n.keepReasoning.hardStrikes == 1);
    assert(n.recovery.softNudges.empty);
    assert(n.recovery.softStrikes == 2);
    assert(n.recovery.hardNudges.empty);
    assert(n.recovery.hardStrikes == 1);
    assert(n.compression.enabled);
    assert(n.compression.threshold == 0.8);
    assert(n.compression.prompt.empty);
    assert(n.feedback.enabled);
    assert(n.feedback.intervalSecs == 900);
    assert(n.feedback.minToolCalls == 50);
}

/// `nudges` absent everywhere -> readConfig still materializes the shipped
/// default prompt files (resolvePaths), unlike the raw struct defaults above.
@("readConfig materializes the shipped default nudge files") unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "config_nudges_resolved");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto tmpFile = buildPath(tmpDir, "test.yaml");
    string yaml = `codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
      warnIfNoApiKey: false
embedConfig:
  type: remote
  server:
    warnIfNoApiKey: false
`;
    File(tmpFile, "w").write(yaml);

    auto conf = readConfigInternal(Path(tmpFile), silent: true, noCwdConfig: true,
            trustedConfig: false, userCliWorkArea: Path.init, cwd: Path.init,
            systemConfigPath: Path.init);
    auto n = conf.nudges;
    assert(n.keepReasoning.softNudges == ["NUDGE_KEEP_REASONING_SOFT.md"]);
    assert(n.keepReasoning.hardNudges == ["NUDGE_KEEP_REASONING_HARD.md"]);
    assert(n.recovery.softNudges == ["NUDGE_RECOVERY_SOFT.md"]);
    assert(n.recovery.hardNudges == ["NUDGE_RECOVERY_HARD.md"]);
    assert(n.compression.prompt == "NUDGE_COMPRESSION.md");
    assert(n.keepReasoning.softStrikes == 2);
    assert(n.recovery.hardStrikes == 1);
}

/// a model without its own `nudges:` block stays null (inherit the
/// global policy) even when the global default is customized.
@("model without its own nudges block inherits the global policy") unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "config_nudges_inherit");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto tmpFile = buildPath(tmpDir, "test.yaml");
    string yaml = `nudges:
  keepReasoning:
    softStrikes: 7
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(tmpFile, "w").write(yaml);

    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    assert(conf.nudges.keepReasoning.softStrikes == 7);
    assert(conf.codeModels[0].nudges.isNull,
            "model without its own nudges block must inherit the global policy");
}

/// Test: a per-model `nudges:` block parses through the Nullable!NudgeConfig
/// branch and is WHOLESALE: restated fields win over the global default,
/// fields not restated fall back to struct defaults, NOT to the global values.
@(
        "per-model nudges block parses wholesale; restated wins, missing fields fall back to struct defaults") unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "config_nudges_permodel");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto tmpFile = buildPath(tmpDir, "test.yaml");
    string yaml = `nudges:
  keepReasoning:
    softStrikes: 7
  recovery:
    softStrikes: 3
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
    nudges:
      keepReasoning:
        softNudges: [MY_SOFT.md]
        softStrikes: 5
`;
    File(tmpFile, "w").write(yaml);

    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    // Global default customized...
    assert(conf.nudges.keepReasoning.softStrikes == 7);
    assert(conf.nudges.recovery.softStrikes == 3);

    // ...but the model's block is its ENTIRE policy:
    auto mn = conf.codeModels[0].nudges;
    assert(!mn.isNull, "per-model nudges must parse into the Nullable field");
    // Restated fields take the per-model values, not the global ones.
    assert(mn.get.keepReasoning.softStrikes == 5, "per-model value must win over the global 7");
    assert(mn.get.keepReasoning.softNudges == ["MY_SOFT.md"]);
    // Fields not restated in the model block fall back to STRUCT defaults,
    // NOT to the customized global values (recovery.softStrikes: global 3).
    assert(mn.get.recovery.softStrikes == 2,
            "unrestated fields must use struct defaults, not the global 3");
    assert(mn.get.recovery.softNudges.empty);
    assert(mn.get.keepReasoning.hardStrikes == 1);
    assert(mn.get.compression.threshold == 0.8);
    assert(mn.get.feedback.intervalSecs == 900);
    assert(mn.get.feedback.minToolCalls == 50);
}

/// an unknown key inside `nudges:` logs the existing "Unknown
/// configuration key" warning (applyConfig) instead of throwing; recognized
/// sibling keys still land.
@("unknown key inside nudges warns instead of throwing") unittest {
    import llm.agent.nudges : sharedLogSwapMutex;
    import std.algorithm : canFind;
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "config_nudges_unknownkey");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto tmpFile = buildPath(tmpDir, "test.yaml");
    string yaml = `nudges:
  keepReasoning:
    softStrikes: 2
  bogusKey: 1
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(tmpFile, "w").write(yaml);

    synchronized (sharedLogSwapMutex) {
        auto prevLog = logger.sharedLog;
        auto cap = cast(shared) new CfgLogCapture();
        logger.sharedLog = cap;
        scope (exit)
            logger.sharedLog = prevLog;

        auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
        assert(conf.nudges.keepReasoning.softStrikes == 2);
        assert(conf.nudges.keepReasoning.hardStrikes == 1);
        assert(conf.codeModels[0].nudges.isNull);
        assert(canFind((cast() cap).takeLines(), "Unknown configuration key NudgeConfig.bogusKey"),
                "unknown key inside nudges: must warn, not throw");
    }
}

/// validateConfig accepts the default (untouched) nudge policy shape —
/// shipped defaults 2/1, threshold 0.8, feedback 900/50, empty prompt/ladders
/// (baseline; empty prompt = shipped default file, resolved at load time).
@("default nudge policy shape passes validateConfig") unittest {
    validateConfig(nudgesValidateTestConfig());
}

/// a 0/0 escalation ladder is rejected — the ladder would fail on the
/// first strike (the math makes this a foot-gun, not a policy).
@("0/0 escalation ladder is rejected") unittest {
    import std.algorithm : canFind;

    auto conf = nudgesValidateTestConfig();
    conf.nudges.keepReasoning.softStrikes = 0;
    conf.nudges.keepReasoning.hardStrikes = 0;

    bool threw;
    string msg;
    try {
        validateConfig(conf);
    } catch (Exception e) {
        threw = true;
        msg = e.msg;
    }
    assert(threw, "validateConfig must throw for a 0/0 escalation ladder");
    assert(canFind(msg, "nudges.keepReasoning"),
            "error message must name the offending key path, got: " ~ msg);
}

/// negative strike counts are rejected, with the offending key path
/// named in the message.
@("negative strike counts are rejected with the key path named") unittest {
    import std.algorithm : canFind;

    auto conf = nudgesValidateTestConfig();
    conf.nudges.recovery.softStrikes = -1;

    bool threw;
    string msg;
    try {
        validateConfig(conf);
    } catch (Exception e) {
        threw = true;
        msg = e.msg;
    }
    assert(threw, "validateConfig must throw for a negative softStrikes");
    assert(canFind(msg, "nudges.recovery.softStrikes"),
            "error message must name the offending key path, got: " ~ msg);

    conf = nudgesValidateTestConfig();
    conf.nudges.recovery.hardStrikes = -3;
    threw = false;
    try {
        validateConfig(conf);
    } catch (Exception e) {
        threw = true;
        msg = e.msg;
    }
    assert(threw, "validateConfig must throw for a negative hardStrikes");
    assert(canFind(msg, "nudges.recovery.hardStrikes"),
            "error message must name the offending key path, got: " ~ msg);
}

/// Test: user-supplied EMPTY file names are rejected only for enabled kinds;
/// files of disabled kinds are not validated (semantics: disabled kind
/// loads nothing and nudges nothing).
@("empty file names rejected only for enabled kinds") unittest {
    auto conf = nudgesValidateTestConfig();
    conf.nudges.keepReasoning.enabled = false;
    conf.nudges.keepReasoning.softNudges = ["", "GARBAGE.md"];
    conf.nudges.keepReasoning.hardNudges = [" "];
    validateConfig(conf); // must not throw

    conf = nudgesValidateTestConfig();
    conf.nudges.keepReasoning.enabled = true;
    conf.nudges.keepReasoning.softNudges = ["OK.md", ""];
    bool threw;
    string msg;
    try {
        validateConfig(conf);
    } catch (Exception e) {
        threw = true;
        msg = e.msg;
    }
    assert(threw, "validateConfig must throw for an empty file name in an enabled kind");
    import std.algorithm : canFind;

    assert(canFind(msg, "nudges.keepReasoning.softNudges[1]"),
            "error message must name the offending entry, got: " ~ msg);
}

/// compression threshold — (0.0, 0.9) valid; a value in [0.9, 1.0)
/// elicits a warning (not a throw) that the advisory nudge can never fire
/// before forced compression at 90%; outside (0.0, 1.0) throws.
@("compression threshold: >= 0.9 warns, outside (0.0, 1.0) throws") unittest {
    import llm.agent.nudges : sharedLogSwapMutex;
    import std.algorithm : canFind;

    auto conf = nudgesValidateTestConfig();
    validateConfig(conf); // default 0.8 is valid and silent

    // At/above the forced point: warning, not throw.
    conf.nudges.compression.threshold = 0.95;
    synchronized (sharedLogSwapMutex) {
        auto prevLog = logger.sharedLog;
        auto cap = cast(shared) new CfgLogCapture();
        logger.sharedLog = cap;
        scope (exit)
            logger.sharedLog = prevLog;

        validateConfig(conf);
        auto lines = (cast() cap).takeLines().join("\n");
        assert(lines.canFind("nudges.compression.threshold"),
                "threshold >= 0.9 must warn, got: " ~ lines);
    }

    // Outside (0.0, 1.0): throw.
    conf.nudges.compression.threshold = 0.0;
    bool threw;
    try {
        validateConfig(conf);
    } catch (Exception e) {
        threw = true;
    }
    assert(threw, "validateConfig must throw for threshold 0.0");

    conf.nudges.compression.threshold = 1.0;
    threw = false;
    try {
        validateConfig(conf);
    } catch (Exception e) {
        threw = true;
    }
    assert(threw, "validateConfig must throw for threshold 1.0");
}

/// Test: feedback floors — 0 would warn after every tool call, so both values
/// must be >= 1 (use `enabled: false` to turn the warning off instead).
@("feedback floors of 0 are rejected") unittest {
    auto conf = nudgesValidateTestConfig();
    conf.nudges.feedback.intervalSecs = 0;
    bool threw;
    string msg;
    try {
        validateConfig(conf);
    } catch (Exception e) {
        threw = true;
        msg = e.msg;
    }
    assert(threw, "validateConfig must throw for intervalSecs 0");
    import std.algorithm : canFind;

    assert(canFind(msg, "nudges.feedback.intervalSecs"),
            "error message must name the offending key path, got: " ~ msg);

    conf = nudgesValidateTestConfig();
    conf.nudges.feedback.minToolCalls = 0;
    threw = false;
    try {
        validateConfig(conf);
    } catch (Exception e) {
        threw = true;
        msg = e.msg;
    }
    assert(threw, "validateConfig must throw for minToolCalls 0");
    assert(canFind(msg, "nudges.feedback.minToolCalls"),
            "error message must name the offending key path, got: " ~ msg);
}

/// Test: per-model `nudges:` wholesale overrides are validated like the
/// global block — a 0/0 ladder, a bad compression threshold, or a feedback
/// floor violation inside a model block fails startup with the model's key
/// path in the message.
@("per-model nudges overrides validated with the model key path") unittest {
    import std.algorithm : canFind;

    auto conf = nudgesValidateTestConfig();
    conf.codeModels[0].nudges = NudgeConfig(keepReasoning: EscalationConfig(softStrikes: 0,
            hardStrikes: 0));

    bool threw;
    string msg;
    try {
        validateConfig(conf);
    } catch (Exception e) {
        threw = true;
        msg = e.msg;
    }
    assert(threw, "validateConfig must throw for a 0/0 ladder in a per-model override");
    assert(canFind(msg, "codeModels[0].nudges.keepReasoning"),
            "error message must name the per-model key path, got: " ~ msg);

    conf = nudgesValidateTestConfig();
    conf.codeModels[0].nudges = NudgeConfig(compression: CompressionNudgeConfig(enabled: true,
            threshold: 1.5));
    threw = false;
    try {
        validateConfig(conf);
    } catch (Exception e) {
        threw = true;
        msg = e.msg;
    }
    assert(threw, "validateConfig must throw for a per-model threshold outside (0.0, 1.0)");
    assert(canFind(msg, "codeModels[0].nudges.compression.threshold"),
            "error message must name the per-model key path, got: " ~ msg);

    conf = nudgesValidateTestConfig();
    conf.codeModels[0].nudges = NudgeConfig(feedback: FeedbackNudgeConfig(enabled: true,
            intervalSecs: 900, minToolCalls: 0));
    threw = false;
    try {
        validateConfig(conf);
    } catch (Exception e) {
        threw = true;
        msg = e.msg;
    }
    assert(threw, "validateConfig must throw for a per-model feedback floor violation");
    assert(canFind(msg, "codeModels[0].nudges.feedback.minToolCalls"),
            "error message must name the per-model key path, got: " ~ msg);
}

/// Test: a valid per-model `nudges:` override passes, and files of DISABLED
/// kinds inside a per-model block are not validated either (same semantics as
/// the global policy).
@("valid per-model override passes; disabled kinds not validated") unittest {
    auto conf = nudgesValidateTestConfig();
    conf.codeModels[0].nudges = NudgeConfig(keepReasoning: EscalationConfig(softNudges: [
        "MY_SOFT.md"
    ], softStrikes: 5));
    validateConfig(conf); // must not throw

    conf = nudgesValidateTestConfig();
    conf.codeModels[0].nudges = NudgeConfig(keepReasoning: EscalationConfig(enabled: false,
            softNudges: ["", "GARBAGE.md"]));
    validateConfig(conf); // disabled kind inside a per-model block: not validated
}

/// Test: the default (untouched) nudge policy shape validates SILENTLY — no
/// spurious nudge warning on the shipped defaults.
@("default nudge policy validates silently") unittest {
    import llm.agent.nudges : sharedLogSwapMutex;
    import std.algorithm : canFind;

    synchronized (sharedLogSwapMutex) {
        auto prevLog = logger.sharedLog;
        auto cap = cast(shared) new CfgLogCapture();
        logger.sharedLog = cap;
        scope (exit)
            logger.sharedLog = prevLog;

        validateConfig(nudgesValidateTestConfig());
        auto lines = (cast() cap).takeLines().join("\n");
        assert(!lines.canFind("nudges."),
                "default nudge policy must validate silently, got: " ~ lines);
    }
}

/// Test: the Tool Broker config keys parse from YAML, nested under
/// toolBroker: enabled (kill-switch, default true), toolTagDescriptions
/// (string[string] tag -> description), neverHideTools (default ["taskDone"]).
@("tool broker config keys parse from YAML") unittest {
    import std.algorithm : canFind;
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "config_toolbroker_parse");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "test.yaml");
    string yaml = `toolBroker:
  enabled: false
  toolTagDescriptions:
    workarea: "Files in the agent workarea: read, write, list, search."
    rag: "RAG knowledge base tools."
  neverHideTools:
    - taskDone
    - pipelineOutput
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(tmpFile, "w").write(yaml);
    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    assert(!conf.toolBroker.enabled, "toolBroker.enabled: false must parse");
    assert(conf.toolBroker.toolTagDescriptions["workarea"].canFind("workarea"),
            "toolTagDescriptions must parse: " ~ conf.toolBroker.toolTagDescriptions.to!string);
    assert(conf.toolBroker.toolTagDescriptions["rag"] == "RAG knowledge base tools.");
    assert(conf.toolBroker.neverHideTools == ["taskDone", "pipelineOutput"],
            "explicit neverHideTools must parse: " ~ conf.toolBroker.neverHideTools.to!string);
}

/// Test: absent keys keep the struct defaults — toolBroker.enabled true,
/// neverHideTools ["taskDone"], toolTagDescriptions empty.
@("tool broker config defaults hold") unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "config_toolbroker_defaults");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "test.yaml");
    string yaml = `codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(tmpFile, "w").write(yaml);
    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    assert(conf.toolBroker.enabled, "default toolBroker.enabled must be true");
    assert(conf.toolBroker.neverHideTools == ["taskDone"],
            "default neverHideTools must be [\"taskDone\"]");
    assert(conf.toolBroker.toolTagDescriptions.empty, "no toolTagDescriptions by default");
}

/// Test: validateToolBrokerConfig — warnings only, never fatal.
/// (1) A neverHide name missing from the registry warns (it cannot be
///     protected from hiding).
/// (2) A tagged neverHide tool warns (discovery gating runs before
///     hiding, so it can still disappear from the tool list).
/// (3) The shipped defaults (registered, untagged names) validate silently —
///     including the discovery meta-tools (listToolTags + toolSearch, the
///     Guard: untagged meta-tools emit no warning).
/// (4) A toolTagDescriptions key outside KnownToolTag warns (typo guard);
///     known keys are silent.
@("tool broker startup validation") unittest {
    import std.algorithm : canFind;

    import llm.tool_call : Context, ExecuteFuncResult, RegFunction, addFunction, toParams;

    // Fixture (mirrors llm.tool_call.tests.d): an empty params struct plus a
    // (Context, JSONValue)-shaped callback matching RegFunction.callback;
    // static nested so &cb is a function pointer. Registered via addFunction,
    // which dedupes by name (order-proof). The registry entry is a deliberate,
    // benign test leak (mirrors tool_call/tests.d:56-57) — the tagged-tool
    // tests should know it exists.
    struct BrokerValidateParams {
    }

    static ExecuteFuncResult brokerValidateCallback(Context ctx, JSONValue args) {
        return ExecuteFuncResult("ok", true);
    }

    addFunction(RegFunction(name: "broker_validate_tagged_fixture", desc: "validateToolBrokerConfig fixture", params: toParams!BrokerValidateParams,
            callback: &brokerValidateCallback, tags: ["workarea"]));

    // (3) Shipped defaults validate silently: taskDone is registered and
    // untagged (registered by tool_call's module ctor, which runs first).
    auto conf = LlmConfig.init;
    assert(validateToolBrokerConfig(conf).empty);

    // (1) An unknown neverHide name cannot be protected — warn.
    conf.toolBroker.neverHideTools = ["taskDone", "bogus_no_hide"];
    auto warnings = validateToolBrokerConfig(conf);
    assert(warnings.length == 1, warnings.to!string);
    assert(warnings[0].canFind("bogus_no_hide"), warnings.to!string);

    // (2) A tagged neverHide tool would still be discovery-gated — warn.
    conf.toolBroker.neverHideTools = ["broker_validate_tagged_fixture"];
    warnings = validateToolBrokerConfig(conf);
    assert(warnings.length == 1, warnings.to!string);
    assert(warnings[0].canFind("broker_validate_tagged_fixture"), warnings.to!string);
    assert(warnings[0].canFind("tagged"), warnings.to!string);

    // (4) Typo guard: an unknown toolTagDescriptions key warns, known keys silent.
    conf = LlmConfig.init;
    conf.toolBroker.toolTagDescriptions = [
        "workarea": "Workarea tools.",
        "bogusTag": "typo"
    ];
    warnings = validateToolBrokerConfig(conf);
    assert(warnings.length == 1, warnings.to!string);
    assert(warnings[0].canFind("bogusTag"), warnings.to!string);

    conf.toolBroker.toolTagDescriptions = [
        "workarea": "Workarea tools.",
        "rag": "RAG tools."
    ];
    assert(validateToolBrokerConfig(conf).empty);
}

/// Test: discoveryMetaToolTagWarnings — the discovery-meta-tool guard
/// A TAGGED meta-tool warns; untagged (empty joined tags) and
/// registry-missing names are silent (the neverHide loop above owns
/// registry-missing).
@("discovery meta-tool tag guard") unittest {
    import std.algorithm : canFind;

    string[string] metaTags;
    metaTags["listToolTags"] = "";
    assert(discoveryMetaToolTagWarnings(metaTags).empty); // registered, untagged

    metaTags["toolSearch"] = "workarea";
    auto warnings = discoveryMetaToolTagWarnings(metaTags);
    assert(warnings.length == 1, warnings.to!string);
    assert(warnings[0].canFind("toolSearch"), warnings.to!string);
    assert(warnings[0].canFind("workarea"), warnings.to!string);

    // A name missing from the registry is silent here (the registry-missing
    // warning is the neverHide loop's concern).
    assert(discoveryMetaToolTagWarnings(["ghost": "rag"]).empty);
}
