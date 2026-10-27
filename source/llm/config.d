module llm.config;

import logger = std.logger;
import std.algorithm : filter, map, sort, among;
import std.array : array, empty, appender, join;
import std.conv : to, text;
import std.datetime : Clock;
import std.file : readText, exists, mkdirRecurse, rename, rmdirRecurse, thisExePath, isDir;
import std.format : format;
import std.functional : toDelegate;
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

/// AGENTS.md prompt payload handed to the composer. payload empty (or
/// whitespace-only) = no AGENTS.md treatment (no injection, no instruction).
/// ragTopic empty = no pointer instruction (ragIndex=false, or full-text
/// treatment). New fields must be appended at the end: positional
/// constructions elsewhere rely on the field order.
struct AgentsMdPrompt {
    string payload; /// summary OR verbatim text (see AgentMdState.summary)
    string ragTopic; /// exact RAG topic name (agent_md_<8-hex>); empty = no pointer
}

/// RAG pointer instruction composed at build time with the exact topic name.
/// Retires the static AgentMdRagInstruction constant: the pointer now embeds
/// the checksum-derived topic name (agent_md_<8-hex>) instead of the
/// inaccurate "'AGENTS.md' source" wording.
private string composeAgentMdRagPointer(string topic) @safe pure {
    return "For detailed explanations, architecture justifications, or lengthy code "
        ~ "examples, query your RAG knowledge base for topic `"
        ~ topic ~ "`; however, you must obey the summarized rules listed above at all times.";
}

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

    Path uiInitFile;

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

        // Use only the cwd and the closest directory: a user who creates the
        // "memory" directory on purpose in the llmfun directory wants all
        // memories there, not in any other writable memory directory.
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

        if (uiInitFile.empty) {
            import my.xdg : xdgDataHome;

            uiInitFile = xdgDataHome ~ Path(ProgramName) ~ "ui.ini";
        }
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

    /// AGENTS.md prompt treatment: compression mode, threshold, RAG indexing.
    AgentsMdConfig agentsMd;

    /// Agent prompt filename searched for in promptDir.
    string agentPrompt = "AGENT.md";

    /// Reasoning-summary prompt filename searched for in promptDir.
    string reasoningSummaryPrompt = "REASONING_SUMMARY.md";

    CodeModelConfig[] codeModels;
    long activeCodeModelIndex = 0;

    /// Short-term memory lifetime in days. Memories older than this are promoted.
    uint shortTermDays = 7;
    /// Long-term memory lifetime in days. Older memories are demoted to archive.
    uint longTermDays = 100;
    /// Hours between MemoryTierActor passes. 0 = startup-only pass.
    uint tierPassIntervalHours = 6;

    SummaryModelConfig summaryModel;

    /// Optional dedicated vision model for image processing. When set, image analysis
    /// delegates to a separate model specialized for vision tasks.
    Nullable!VisionModelConfig visionModel;

    NudgeConfig nudges;

    string taskDoneTool = "taskDone";
    string[] hiddenTools = ["taskDone", "pipelineOutput"];

    invariant {
        assert(maxManifestSkills > 0, i"maxManifestSkills must be positive, got $(maxManifestSkills)"
                .text);
        assert(maxAlwaysApplyTokens >= 0, i"maxAlwaysApplyTokens must be non-negative (0 = unlimited), got $(
                maxAlwaysApplyTokens)".text);
        assert(shortTermDays > 0, i"shortTermDays must be positive, got $(shortTermDays)".text);
        assert(longTermDays > shortTermDays, i"longTermDays must be greater than shortTermDays (short=$(
                shortTermDays), long=$(longTermDays))".text);
    }

    EmbedConfig embedConfig;
    long embedDimensions() const @safe {
        return embedConfig.match!((LocalEmbedConfig a) => a.dimensions,
                (RemoteEmbedConfig a) => a.dimensions);
    }

    /// Return: the currently active code model config (value copy, no mutex needed).
    /// Non-const on purpose: a const accessor could not return a mutable value
    /// copy once CodeModelConfig grew a member with mutable indirections
    /// (Nullable!NudgeConfig - string[] ladders) - const(CodeModelConfig) does
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
            File(tempFile, "w").writeln(stateObj.toString(JSONOptions.doNotEscapeSlashes));
            rename(tempFile, stateFile);
        } catch (Exception e) {
            try {
                logger.tracef("Failed to save state: %s", e.msg);
            } catch (Exception e) {
            }
        }
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

    /// Compose system prompt: basePrompt -> alwaysApplyBlock -> agentsMd payload
    /// -> agentsMd instruction (pointer iff payload && ragTopic non-empty) -> manifestXml.
    string getPrompt(SkillManager skillManager, string promptName = null,
            bool addSkills = true, AgentsMdPrompt agentsMd = AgentsMdPrompt.init) {
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

        const hasAgentMd = !agentsMd.payload.strip.empty;
        string agentMdInstruction;
        if (hasAgentMd && !agentsMd.ragTopic.empty)
            agentMdInstruction = composeAgentMdRagPointer(agentsMd.ragTopic);

        if (hasAgentMd || !alwaysApplyBlock.empty || !manifestXml.empty) {
            string[] parts;
            parts ~= basePrompt;
            if (!alwaysApplyBlock.empty)
                parts ~= alwaysApplyBlock;
            if (hasAgentMd)
                parts ~= agentsMd.payload;
            if (!agentMdInstruction.empty)
                parts ~= agentMdInstruction;
            if (!manifestXml.empty)
                parts ~= manifestXml;

            fullPrompt = parts.join("\n\n").strip;
        }

        return fullPrompt;
    }

    Path[] skillPaths() @safe {
        return skillPathsUser ~ skillPathsSystem;
    }

    /** Check if the tool is the tool that mark a query loop as done.
     * Params:
     *  name: name of the tool the llm is using, can be camelCase or snake_case
     */
    bool isTaskDone(string name) {
        import llm.utility : toSnakeCase;

        static string toolName;
        static string toolSnakeCase;
        if (toolSnakeCase.empty || toolName != taskDoneTool) {
            toolName = taskDoneTool;
            toolSnakeCase = taskDoneTool.toSnakeCase;
        }
        return name == taskDoneTool || name == toolSnakeCase;
    }

    /** Check if the tool should be hidden in the UI.
     * Params:
     *  name: name of the tool the llm is using, can be camelCase or snake_case
     */
    bool isHiddenTool(string name) {
        import std.algorithm : canFind;
        import llm.utility : toSnakeCase;

        static string[] toolNames;
        static string[] toolsSnakeCase;
        if (toolsSnakeCase.empty || toolNames != hiddenTools) {
            toolNames = hiddenTools;
            toolsSnakeCase = hiddenTools.map!(a => a.toSnakeCase).array;
        }
        return hiddenTools.canFind(name) || toolsSnakeCase.canFind(name);
    }
}

@("taskDoneNames resolves the dual-axis taskDone identity from the registry")
unittest {
    LlmConfig conf;

    assert(conf.isTaskDone("taskDone"),
            "isTaskDone must match both name axes of the configured tool");
    assert(conf.isTaskDone("task_done"),
            "isTaskDone must match both name axes of the configured tool");
    assert(conf.isHiddenTool("taskDone"));
    assert(conf.isHiddenTool("task_done"));
    assert(conf.isHiddenTool("pipelineOutput"));
    assert(conf.isHiddenTool("pipeline_output"));

    // A second config with custom values must resolve against its own
    // fields, not against any other LlmConfig instance in the process.
    LlmConfig custom;
    custom.taskDoneTool = "customDone";
    custom.hiddenTools = ["customHidden"];
    assert(custom.isTaskDone("customDone"));
    assert(custom.isTaskDone("custom_done"));
    assert(!custom.isTaskDone("taskDone"),
            "custom config must not match the default tool's D axis");
    assert(!custom.isTaskDone("task_done"),
            "custom config must not match the default tool's snake axis");
    assert(custom.isHiddenTool("customHidden"));
    assert(custom.isHiddenTool("custom_hidden"));
    assert(!custom.isHiddenTool("taskDone"));
    assert(!custom.isHiddenTool("task_done"));
}

/// Tier subdirectories under each memoryArea dir.
Path memoryShortDir(Path area) => area ~ "short";
Path memoryLongDir(Path area) => area ~ "long";
Path memoryArchiveDir(Path area) => area ~ "archive";

/// Create `short/`, `long/` and `archive/` under every memory area dir.
/// Idempotent: existing tier dirs are left alone.
/// Non-fatal: an area that cannot be created is logged and skipped, so a read-only area never blocks startup; the tier pass retries on every run.
void ensureMemoryTierDirs(Path[] memoryAreas) {
    foreach (area; memoryAreas.filter!(a => !a.empty)) {
        foreach (dir; [
            memoryShortDir(area), memoryLongDir(area), memoryArchiveDir(area)
        ]) {
            if (dir.exists)
                continue;
            try {
                logger.trace("Creating directory ", dir);
                mkdirRecurse(dir);
            } catch (Exception e) {
                logger.warningf("failed to create memory tier directory '%s': %s", dir, e.msg);
            }
        }
    }
}

void makeDefaultFileStructure() {
    import std.file : mkdirRecurse;
    import my.xdg : xdgDataHome;

    auto programDataDir = xdgDataHome ~ Path(ProgramName);
    ensureMemoryTierDirs([programDataDir ~ Path("memory")]);

    foreach (path; [
        (programDataDir ~ Path("skills")), (programDataDir ~ Path("chat")),
        (programDataDir ~ Path("dialogue"))
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
        conf.dataDir ~ "dialogue"
    ].filter!(a => !a.empty && !a.exists)) {
        try {
            logger.info("Creating directory ", path);
            mkdirRecurse(path);
        } catch (Exception e) {
            logger.warning(e);
        }
    }
    if (!conf.dataDir.empty)
        ensureMemoryTierDirs([conf.dataDir ~ "memory"]);
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

/// A named group of tools. The group name is the activation key, the
/// description feeds discovery, and `tools` lists the member tool names. A
/// tool may be listed in multiple groups.
struct GroupConfig {
    string description;
    string[] tools;
}

struct ToolBrokerConfig {
    /// Kill switch: false means all tools are treated as ungrouped
    /// (alwaysOn) and discovery is inert.
    bool enabled = false;
    /// Named tool groups. The group name is the activation key, the
    /// description feeds discovery, and `tools` lists the member tool
    /// names. A tool may be listed in multiple groups.
    GroupConfig[string] groups;
    /// Group names whose members start hidden, activatable via
    /// listToolTags. Omitted or empty hides nothing.
    string[] hiddenTags;
    /// Tool names or group names forced visible. A group name un-hides
    /// the whole membership.
    string[] alwaysOn;
    /// Tools never hidden regardless of LlmConfig.toolFilter. A neverHide tool missing
    /// from the registry warns at startup.
    string[] neverHideTools = ["taskDone"];
    /// Raw YAML keys inside the `toolBroker` block that named no struct member -
    /// populated by applyConfig while parsing (the unknown-key warning loop
    /// collects them as it warns). The removed `toolTagDescriptions` key lands
    /// here, and the startup validator turns it into the groups migration
    /// warning. Parser-filled, not a hand-maintained presence list.
    string[] unknownYamlKeys;
}

/// AGENTS.md compression treatment under `mode: auto`. `auto` adapts to the
/// document size: verbatim injection when it fits the model's context window,
/// summary when it does not.
enum CompressionMode {
    /// Verbatim when size <= thresholdKb, summary when larger (the default).
    auto_,
    /// Never compress; verbatim injection regardless of size.
    full,
    /// Always compress (the old unconditional behavior).
    summary,
}

struct AgentsMdCompression {
    /// Auto = verbatim when size <= thresholdKb, summary when larger.
    CompressionMode mode = CompressionMode.auto_;
    /// kbyte threshold for the auto boundary (size <= thresholdKb*1024 -> full);
    /// also consulted by the over-size warning on the full path. 0 = legal
    /// (always-summary under auto); negative is a parse error.
    long thresholdKb = 20;
}

struct AgentsMdConfig {
    /// Master switch. False = complete opt-out: no injection, no RAG indexing,
    /// no cache activity; overrides ragIndex and everything else.
    bool enabled = true;
    AgentsMdCompression compression;
    /// Index the full doc into RAG regardless of prompt treatment.
    bool ragIndex = true;
}

/// Per-model override of the global `AgentsMd` block. Null = inherit the
/// global value; non-null replaces it. Per-model entries only restate keys
/// they want to differ - absent fields fall back to the global section
/// (per-field semantics, not whole-block).
struct AgentsMdOverride {
    Nullable!bool enabled;
    Nullable!CompressionMode mode;
    Nullable!long thresholdKb;
    Nullable!bool ragIndex;
}

/// Per-model override of the global `toolBroker` block. Null = no override
/// for that field; non-null replaces the global value. The `groups` map is
/// merged per entry by group name - a model entry with the same name
/// replaces the global entry wholesale.
struct ToolBrokerOverride {
    Nullable!bool enabled;
    Nullable!(string[]) hiddenTags;
    Nullable!(string[]) alwaysOn;
    Nullable!(GroupConfig[string]) groups;
}

/// Resolve the effective tool broker config for a model: built-in struct
/// defaults, then the global `toolBroker` block (applyConfig already folded
/// the global block over the defaults when parsing), then the model's
/// `toolBroker` override. Present override fields replace the global value;
/// absent fields keep it. The `groups` map is merged per entry by group
/// name - a model entry with the same name replaces the global entry
/// wholesale.
ToolBrokerConfig resolveToolBrokerConfig(const LlmConfig conf, const CodeModelConfig model) @safe {
    // Global layer: applyConfig parsed the global block over the built-in
    // defaults, so conf.toolBroker already holds layers 1 + 2.
    ToolBrokerConfig resolved;
    resolved.enabled = conf.toolBroker.enabled;
    resolved.hiddenTags = conf.toolBroker.hiddenTags.dup;
    resolved.alwaysOn = conf.toolBroker.alwaysOn.dup;
    resolved.neverHideTools = conf.toolBroker.neverHideTools.dup;
    foreach (name, group; conf.toolBroker.groups)
        resolved.groups[name] = GroupConfig(group.description, group.tools.dup);

    // Model layer: present fields replace, absent fields keep the global value.
    if (!model.toolBroker.enabled.isNull)
        resolved.enabled = model.toolBroker.enabled.get;
    if (!model.toolBroker.hiddenTags.isNull)
        resolved.hiddenTags = model.toolBroker.hiddenTags.get.dup;
    if (!model.toolBroker.alwaysOn.isNull)
        resolved.alwaysOn = model.toolBroker.alwaysOn.get.dup;
    if (!model.toolBroker.groups.isNull)
        foreach (name, group; model.toolBroker.groups.get)
            resolved.groups[name] = GroupConfig(group.description, group.tools.dup);
    return resolved;
}

/// Effective AGENTS.md treatment for a model: built-in struct defaults, then the
/// global `agentsMd` block (applyConfig already folded the global block over the
/// defaults when parsing), then the model's `agentsMd` override. Present override
/// fields replace the global value; absent fields keep it.
AgentsMdConfig effectiveAgentsMd(const LlmConfig conf, const CodeModelConfig model) @safe {
    // Global layer: applyConfig parsed the global block over the built-in
    // defaults, so conf.agentsMd already holds layers 1 + 2.
    AgentsMdConfig resolved;
    resolved.enabled = conf.agentsMd.enabled;
    resolved.compression.mode = conf.agentsMd.compression.mode;
    resolved.compression.thresholdKb = conf.agentsMd.compression.thresholdKb;
    resolved.ragIndex = conf.agentsMd.ragIndex;

    // Model layer: present fields replace, absent fields keep the global value.
    if (!model.agentsMd.enabled.isNull)
        resolved.enabled = model.agentsMd.enabled.get;
    if (!model.agentsMd.mode.isNull)
        resolved.compression.mode = model.agentsMd.mode.get;
    if (!model.agentsMd.thresholdKb.isNull)
        resolved.compression.thresholdKb = model.agentsMd.thresholdKb.get;
    if (!model.agentsMd.ragIndex.isNull)
        resolved.ragIndex = model.agentsMd.ragIndex.get;

    return resolved;
}

/// Inverse of compressionModeFromYaml: the YAML spelling of a CompressionMode
/// (`auto` is a D keyword, so the member is `auto_`). Used by the treatment
/// log's provenance summary.
private string compressionModeToYaml(CompressionMode mode) @safe pure nothrow {
    final switch (mode) with (CompressionMode) {
    case CompressionMode.auto_:
        return "auto";
    case CompressionMode.full:
        return "full";
    case CompressionMode.summary:
        return "summary";
    }
}

/// One-line provenance of the effective AGENTS.md treatment for a model: each
/// field's value with the config layer that supplied it (built-in default /
/// global `agentsMd` block / per-model override). A global-block value equal to
/// the built-in default is reported as "default" (the layers are value-identical
/// there). Feeds the treatment log line
/// in processAgentMd; the resolver effectiveAgentsMd itself stays silent
/// because it also runs on hot paths like model listings.
string agentsMdSourceSummary(const LlmConfig conf, const CodeModelConfig model) @safe {
    const resolved = effectiveAgentsMd(conf, model);
    const globalBlock = conf.agentsMd;
    const defaults = AgentsMdConfig.init;

    static string layer(bool perModel, bool restatedGlobal) @safe pure nothrow {
        return perModel ? "per-model" : restatedGlobal ? "global" : "default";
    }

    import std.string : format;

    return format!"enabled=%s (%s), mode=%s (%s), thresholdKb=%s (%s), ragIndex=%s (%s)"(
            resolved.enabled, layer(!model.agentsMd.enabled.isNull,
            globalBlock.enabled != defaults.enabled), compressionModeToYaml(resolved.compression.mode),
            layer(!model.agentsMd.mode.isNull,
                globalBlock.compression.mode != defaults.compression.mode),
            resolved.compression.thresholdKb, layer(!model.agentsMd.thresholdKb.isNull,
                globalBlock.compression.thresholdKb != defaults.compression.thresholdKb), resolved.ragIndex,
            layer(!model.agentsMd.ragIndex.isNull, globalBlock.ragIndex != defaults.ragIndex));
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

    /// Per-model tool broker override (see ToolBrokerOverride)
    ToolBrokerOverride toolBroker;

    /// Per-model AGENTS.md override (see AgentsMdOverride).
    AgentsMdOverride agentsMd;
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
/// (The config layers have a separate system/user split: `readConfig`'s
/// `includeSystemConfig` flag opts out of the system base layer.)
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

/// Load the layered configuration for `path` (layer 2) over the system base
/// config (layer 1), with the same system/user split as `readConfigInternal`.
///
/// `includeSystemConfig = false` skips layer-1 resolution entirely: no
/// `LLMFUN_SYSTEM_CONFIG` env read, no XDG default resolution, no base-config
/// file read, and no layer-1 log line. Production callers keep the default
/// (`true`); unittests opt out so no test reads the real machine config.
LlmConfig readConfig(Path path, bool silent = false, bool noCwdConfig,
        bool trustedConfig, Path userCliWorkArea = Path.init, bool includeSystemConfig = true) {
    import std.file : getcwd;
    import std.process : environment;
    import my.xdg : xdgConfigHome;

    Path systemConfigPath;
    if (includeSystemConfig) {
        systemConfigPath = environment.get("LLMFUN_SYSTEM_CONFIG",
                (xdgConfigHome ~ Path(ProgramName) ~ Path("config.yaml")).toString).Path;
    }
    return readConfigInternal(path: path, silent: silent, noCwdConfig: noCwdConfig, trustedConfig: trustedConfig,
            userCliWorkArea: userCliWorkArea, cwd: Path(getcwd()),
            systemConfigPath: systemConfigPath);
}

private LlmConfig readConfigInternal(Path path, bool silent = false, bool noCwdConfig,
        bool trustedConfig, Path userCliWorkArea = Path.init, Path cwd = Path.init,
        Path systemConfigPath = Path.init, bool includeSystemConfig = true) {
    import std.path : buildPath, dirName;

    LlmConfig conf;
    bool loadedAnyFile = false;
    string configDir = "."; // Directory of the last successfully loaded config file

    void layerOneLoad() {
        // Layer 1: Base config from LLMFUN_SYSTEM_CONFIG
        if (!includeSystemConfig || systemConfigPath.empty) {
            return;
        }
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

private EmbedConfig embedConfigFromValue(JSONValue json,
        void delegate(logger.LogLevel, string) warn = toDelegate(&defaultConfigWarnSink)) {
    import std.exception : enforce;

    if ("type" !in json) {
        throw new Exception("embedConfig missing required field 'type'");
    }

    string type = json["type"].str;
    json.object.remove("type");
    if (type == "remote") {
        return EmbedConfig(applyConfig!(RemoteEmbedConfig)(RemoteEmbedConfig.init, json, warn));
    }
    if (type == "local") {
        return EmbedConfig(applyConfig!(LocalEmbedConfig)(LocalEmbedConfig.init, json, warn));
    }
    throw new Exception("embedConfig: unknown type '" ~ type ~ "', expected 'remote' or 'local'");
}

/// Map a YAML `compression.mode` spelling to CompressionMode. The YAML
/// spellings match the member names except `auto`: it is a D keyword, so the
/// member is `auto_`. Unknown spellings are a parse error naming the valid
/// values.
private CompressionMode compressionModeFromYaml(string spelling) @safe {
    switch (spelling) {
    case "auto":
        return CompressionMode.auto_;
    case "full":
        return CompressionMode.full;
    case "summary":
        return CompressionMode.summary;
    default:
        throw new Exception(
                "invalid compression.mode '" ~ spelling ~ "' (valid: auto | full | summary)");
    }
}

private void defaultConfigWarnSink(logger.LogLevel lvl, string msg) {
    logger.logf(lvl, "%s", msg);
}

auto applyConfig(ConfigT)(ConfigT conf, JSONValue json,
        void delegate(logger.LogLevel, string) warn = toDelegate(&defaultConfigWarnSink)) {
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

                // The YAML key for this member: the member name itself.
                enum yamlKey = llmMemberName;

                if (yamlKey in json) {
                    try {
                        logger.tracef("using config value for %s:%s - %s",
                                ConfigT.stringof, llmMemberName, json[yamlKey]);

                        used[yamlKey] = true;
                        static if (is(Type : Path)) {
                            __traits(getMember, conf, llmMemberName) = json[yamlKey].str.Path;
                        } else static if (is(Type == RagDatabaseConfig)) {
                            auto elem = json[yamlKey];
                            validateRagDatabase(elem);
                            string path = elem["path"].str;
                            string desc = elem["description"].str;
                            __traits(getMember, conf, llmMemberName) = RagDatabaseConfig(path.Path,
                                    desc);
                        } else static if (is(Type == RagDatabaseConfig[][string])) {
                            foreach (key, ref JSONValue dbs; json[yamlKey].object) {
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
                            auto val = json[yamlKey];
                            if (val.type == JSONType.STRING) {
                                __traits(getMember, conf, llmMemberName) = [
                                    val.str.Path
                                ];
                            } else {
                                __traits(getMember, conf, llmMemberName) = val.array.map!(a => a.str.Path)
                                    .array;
                            }
                        } else static if (is(Type == CodeModelConfig[])) {
                            __traits(getMember, conf, llmMemberName) = json[yamlKey].array.map!(
                                    a => applyConfig(CodeModelConfig.init, a, warn)).array;
                        } else static if (is(Type : string)) {
                            __traits(getMember, conf, llmMemberName) = json[yamlKey].str;
                        } else static if (is(Type : bool)) {
                            __traits(getMember, conf, llmMemberName) = json[yamlKey].boolean;
                        } else static if (is(Type == CompressionMode)) {
                            // `auto` is a D keyword, so the member is `auto_`
                            // while the YAML spelling stays `auto`: map
                            // explicitly and reject unknown spellings naming
                            // the valid values.
                            __traits(getMember, conf, llmMemberName) = compressionModeFromYaml(
                                    json[yamlKey].str);
                        } else static if (is(Type == enum)) {
                            // Enum fields are configured by member name, e.g. `mode: mixed`.
                            __traits(getMember, conf, llmMemberName) = json[yamlKey].str.to!Type;
                        } else static if (isNullableType!Type && is(NullableInner!Type == long)) {
                            // Per-model AgentsMd override (Nullable!long
                            // thresholdKb): present scalar replaces, absent or
                            // JSON-null keeps the Nullable unset. Negative
                            // values are a parse error.
                            auto val = json[yamlKey];
                            if (val.type != JSONType.NULL) {
                                auto raw = val.integer;
                                static if (llmMemberName.among("thresholdKb")) {
                                    if (raw < 0)
                                        throw new Exception("config key '" ~ llmMemberName
                                                ~ "' must be non-negative, got " ~ raw.to!string);
                                }
                                __traits(getMember, conf, llmMemberName) = Nullable!long(raw);
                            }
                        } else static if (isFloatingPoint!Type) {
                            __traits(getMember, conf, llmMemberName) = json[yamlKey].floating;
                        } else static if (isIntegral!Type) {
                            auto raw = json[yamlKey].integer;
                            static if ((is(Type : uint) && llmMemberName.among("shortTermDays", "longTermDays",
                                    "tierPassIntervalHours")) || (is(Type == long)
                                    && llmMemberName.among("thresholdKb"))) {
                                if (raw < 0)
                                    throw new Exception(
                                            "config key '" ~ llmMemberName
                                            ~ "' must be non-negative, got " ~ raw.to!string);
                            }
                            import std.exception : enforce;

                            static if (is(Type : uint)) {
                                if (raw < 0) {
                                    throw new Exception(
                                            "config key '" ~ llmMemberName
                                            ~ "' must be non-negative");
                                }
                            }
                            __traits(getMember, conf, llmMemberName) = cast(Type) raw;
                        } else static if (is(Type == string[][string])) {
                            foreach (key, ref JSONValue valArr; json[yamlKey].object) {
                                string[] vals;
                                foreach (v; valArr.array) {
                                    vals ~= v.str;
                                }
                                __traits(getMember, conf, llmMemberName)[key] = vals;
                            }
                        } else static if (is(Type == string[string])) {
                            foreach (key, ref JSONValue val; json[yamlKey].object) {
                                __traits(getMember, conf, llmMemberName)[key] = val.str;
                            }
                        } else static if (isNullableType!Type && is(NullableInner!Type == bool)) {
                            // Nullable!bool (per-model toolBroker override):
                            // present scalar replaces, absent or JSON-null
                            // keeps the Nullable unset.
                            auto val = json[yamlKey];
                            if (val.type != JSONType.NULL) {
                                __traits(getMember, conf, llmMemberName) = Nullable!bool(
                                        val.boolean);
                            }
                        } else static if (isNullableType!Type
                                && is(NullableInner!Type == CompressionMode)) {
                            // Per-model AgentsMd override (Nullable!CompressionMode):
                            // present spelling replaces, absent or JSON-null
                            // keeps the Nullable unset.
                            auto val = json[yamlKey];
                            if (val.type != JSONType.NULL) {
                                __traits(getMember, conf, llmMemberName) = Nullable!CompressionMode(
                                        compressionModeFromYaml(val.str));
                            }
                        } else static if (isNullableType!Type && is(NullableInner!Type : string[])) {
                            alias InnerT = NullableInner!Type;
                            auto val = json[yamlKey];
                            if (val.type != JSONType.NULL) {
                                __traits(getMember, conf, llmMemberName) = Nullable!InnerT(val.array.map!(a => a.str)
                                        .array);
                            }
                        } else static if (is(Type == GroupConfig[string])) {
                            auto val = json[yamlKey];
                            if (val.type != JSONType.NULL) {
                                __traits(getMember, conf, llmMemberName) = Type.init;
                                foreach (key, ref JSONValue groupVal; val.object) {
                                    __traits(getMember, conf, llmMemberName)[key] = applyConfig(GroupConfig.init,
                                            groupVal, warn);
                                }
                            }
                        } else static if (isNullableType!Type
                                && is(NullableInner!Type == GroupConfig[string])) {
                            alias InnerT = NullableInner!Type;
                            auto val = json[yamlKey];
                            if (val.type != JSONType.NULL) {
                                InnerT groups;
                                foreach (key, ref JSONValue groupVal; val.object) {
                                    groups[key] = applyConfig(GroupConfig.init, groupVal, warn);
                                }
                                __traits(getMember, conf, llmMemberName) = Nullable!InnerT(groups);
                            }
                        } else static if (is(Type : string[])) {
                            __traits(getMember, conf, llmMemberName) = json[yamlKey].array.map!(a => a.str)
                                .array;
                        } else static if (is(Type : EmbedConfig)) {
                            __traits(getMember, conf, llmMemberName) = embedConfigFromValue(json[yamlKey],
                                    warn);
                        } else static if (isNullableType!Type
                                && isAggregateType!(NullableInner!Type)) {
                            // Handle Nullable!T for aggregate types (e.g., Nullable!VisionModelConfig)
                            alias InnerT = NullableInner!Type;
                            auto val = json[yamlKey];
                            if (val.type != JSONType.NULL) {
                                auto innerConf = InnerT.init;
                                __traits(getMember, conf, llmMemberName) = Nullable!InnerT(applyConfig(innerConf,
                                        val, warn));
                            }
                        } else static if (isAggregateType!Type) {
                            __traits(getMember, conf, llmMemberName) = applyConfig(__traits(getMember,
                                    conf, llmMemberName), *(yamlKey in json), warn);
                        }
                    } catch (Exception e) {
                        warn(logger.LogLevel.warning,
                                format!"unable to read '%s': %s"(llmMemberName, e.msg));
                    }
                } else {
                    logger.tracef("using default value for %s:%s", ConfigT.stringof, llmMemberName);
                }
            }
        }
    }

    foreach (k; json.object.byKey.filter!(a => a !in used)) {
        static if (is(typeof(conf.unknownYamlKeys)))
            conf.unknownYamlKeys ~= k;
        warn(logger.LogLevel.warning,
                format!"Unknown configuration key %s.%s"(ConfigT.stringof, k));
    }

    logger.trace("apply config done: " ~ ConfigT.stringof);
    return conf;
}

/// Rendered warnings for models configured without API keys. Called from
/// validateConfig(), which renders them via logger.warningf exactly as
/// before - production log output is byte-identical. The "To suppress these
/// warnings..." footer is part of the data (rendered at the boundary like
/// the rest).
private string[] checkApiKeyWarnings(LlmConfig conf) {
    string[] warnings;

    void warnOrNot(ref ServerConfig conf, string name, string helpText) {
        if (conf.warnIfNoApiKey && getEnvApiKey(conf.apiKeyEnv).empty) {
            warnings ~= i"No API key found in environment variable '$(conf.apiKeyEnv)' for $(
                    helpText): $(name)".text;
        }

        if (!conf.jsonFields.empty) {
            try {
                auto dummy = parseJSON(conf.jsonFields);
            } catch (Exception e) {
                warnings ~= i"Unable to parse jsonFields '$(conf.jsonFields)' in model config '$(
                        name)': $(e.msg)".text;
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

    if (!warnings.empty) {
        warnings ~= "To suppress these warnings, set 'warnIfNoApiKey' to false or provide the an environment variable with the API key.";
    }

    return warnings;
}

/// Validate one escalation ladder of the nudge policy:
/// non-negative strike counts, and - only when the kind is enabled -
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
private string[] validateNudgeConfig(string keyPath, in NudgeConfig n) {
    string[] warnings;
    validateNudgeEscalation(keyPath ~ ".keepReasoning", n.keepReasoning);
    validateNudgeEscalation(keyPath ~ ".recovery", n.recovery);

    immutable forcedCompression = 0.9; // the hard-coded forced point
    if (!(n.compression.threshold > 0.0) || !(n.compression.threshold < 1.0))
        throw new Exception(i"$(keyPath).compression.threshold must be in (0.0, 1.0) and below 0.9 to ever fire (values in [0.9, 1.0) only warn), got $(
                n.compression.threshold)".text);
    if (n.compression.enabled && n.compression.threshold >= forcedCompression)
        warnings ~= format("%s.compression.threshold %s is at or above the forced-compression point %.1f — the advisory compression nudge can never fire",
                keyPath, n.compression.threshold, forcedCompression);

    if (n.feedback.intervalSecs < 1)
        throw new Exception(i"$(keyPath).feedback.intervalSecs must be >= 1, got $(
                n.feedback.intervalSecs) (with enabled: false the value is ignored; 0 would warn after every tool call)"
                .text);
    if (n.feedback.minToolCalls < 1)
        throw new Exception(i"$(keyPath).feedback.minToolCalls must be >= 1, got $(
                n.feedback.minToolCalls) (with enabled: false the value is ignored; 0 would warn after every tool call)"
                .text);

    return warnings;
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
    // Warnings are data: the nudge and API-key validators collect the
    // rendered warnings; the boundary caller renders them stage by stage, so
    // each stage's warnings go out before the next validation can throw -
    // the emission order holds on failure paths too.
    foreach (w; validateNudgeConfig("nudges", conf.nudges))
        logger.warningf("%s", w);
    foreach (i, model; conf.codeModels)
        if (!model.nudges.isNull)
            foreach (w; validateNudgeConfig(i"codeModels[$(i)].nudges".text, model.nudges.get))
                logger.warningf("%s", w);
    foreach (w; checkApiKeyWarnings(conf))
        logger.warningf("%s", w);
}

/// The discovery meta-tools (listToolTags, then toolSearch). They
/// must stay out of the discovery vocabulary: a group
/// with a description that contains a meta-tool advertises it in
/// discovery (the vocabulary is what the model sees). A meta-tool
/// itself is infrastructure, not a capability - describing a group
/// around it entangles the vocabulary with plumbing, and if that group
/// is hidden the discovery tool starts hidden: the loop would die with
/// the very tools it is meant to reveal. Undescribed (or ungrouped)
/// meta-tool names stay silent here. The UDA-side test lives next to
/// the tools (tool_call/discovery.d + search.d); this is the registry-
/// side startup warning (warnings only).
///
/// Membership is checked config-side only (`g.tools`); runtime
/// `RegFunction.tags` naming a group (the MCP fallback) is not
/// consulted, so a runtime-tagged tool shadowing a meta-tool name
/// inside a described group would be advertised without this warning.
///
/// Takes the resolved block (the caller owns it) plus the warning
/// label ("toolBroker" for the global block, "model '<name>'
/// toolBroker" for a model's resolved view); the two meta-tool names
/// live ONLY here, so a third discovery meta-tool means touching one
/// line. The helper stays import-light and trivially testable.
string[] discoveryMetaToolTagWarnings(const ToolBrokerConfig resolved, string label) @safe {
    import std.algorithm : canFind;

    string[] warnings;
    foreach (name; ["listToolTags", "toolSearch"]) {
        string[] described;
        foreach (a; resolved.groups.byKeyValue.filter!(a => !a.value.description.empty
                && a.value.tools.canFind(name)))
            described ~= a.key;
        if (!described.empty) {
            auto list = described.join(", ");
            warnings ~= i"$(label): discovery meta-tool '$(name)' is described via group(s) [$(list)] - it would be advertised in discovery; keep meta-tools ungrouped (or undescribed)"
                .text;
        }
    }
    return warnings;
}

static import llm.tool_call;

/// Startup validation (neverHide + group sanity + typo protection).
/// Warnings only - never fatal. Not `pure`: reads the global tool
/// registry. Returns the warnings; the caller (Agent ctor) logs them.
/// Reads the tool registry (populated by shared static ctors, which run
/// before main), so it is only meaningful after module construction.
///
/// Per-model `toolBroker` overrides are validated too, on their RESOLVED
/// view (global + model merged): a model entry may name a group that only
/// the global block defines. A finding for the global block therefore
/// repeats under each overridden model's label - the model's effective
/// view genuinely has the issue, and fixing the global block clears it
/// for every model. The migration warning stays global-only.
///
/// Duplicate group names are NOT checked - within one YAML mapping the
/// composer collapses duplicate keys at load, and the global-vs-model
/// merge is per entry by name, so a duplicate cannot arise. Group
/// sanity + typo-protection rules are shared by the global `toolBroker`
/// block and each model's resolved view. `label` prefixes each warning
/// ("toolBroker" for the global block, "model '<name>' toolBroker" for a
/// model's resolved view) so the user can tell which block to fix.
private string[] brokerBlockWarnings(ToolBrokerConfig resolved,
        llm.tool_call.RegFunction[string] registry, string label) @safe {
    import std.algorithm : canFind, filter;
    import llm.tool_call : RegFunction;

    string[] warnings;

    // Group sanity: a listed tool that is not registered cannot be
    // grouped (a typo means it silently stays ungrouped/alwaysOn), and an
    // empty UNDESCRIBED group is inert config - an empty DESCRIBED group is
    // a valid runtime-vocabulary preset (MCP tools arrive via
    // onMcpServerConnected after construction). Group names are sorted for a
    // deterministic warning order.
    string[] groupNames;
    foreach (gname, _; resolved.groups)
        groupNames ~= gname;
    groupNames.sort;
    foreach (gname; groupNames) {
        auto g = resolved.groups[gname];
        if (g.tools.empty && g.description.empty) {
            warnings ~= i"$(label): group '$(gname)' has an empty tools list and an empty description - it is inert; give it tools or a description, or drop it"
                .text;
            continue;
        }
        foreach (tool; g.tools)
            if (tool !in registry)
                warnings ~= i"$(label): group '$(gname)' lists '$(tool)' which is not in the registry - a typo means it silently stays ungrouped (alwaysOn)"
                    .text;
    }

    // A hiddenTags or alwaysOn entry that names neither a defined group
    // nor a registered tool can never have an effect - almost certainly
    // a misspelling.
    foreach (entry; resolved.hiddenTags)
        if (entry !in resolved.groups && entry !in registry)
            warnings ~= i"$(label).hiddenTags: '$(entry)' names neither a group nor a registered tool"
                .text;
    foreach (entry; resolved.alwaysOn)
        if (entry !in resolved.groups && entry !in registry)
            warnings ~= i"$(label).alwaysOn: '$(entry)' names neither a group nor a registered tool"
                .text;

    // The discovery meta-tools must stay out of the discovery vocabulary
    // (see discoveryMetaToolTagWarnings); a described group containing one
    // warns.
    warnings ~= discoveryMetaToolTagWarnings(resolved, label);

    return warnings;
}

string[] validateToolBrokerConfig(LlmConfig conf) @safe {
    import std.algorithm : canFind;
    import llm.tool_call : RegFunction, getFunctions;

    string[] warnings;

    // Registry lookup: name -> tags (empty tags = alwaysOn).
    RegFunction[string] registry;
    foreach (func; getFunctions)
        registry[func.name] = func;

    // A neverHide tool is too important to hide silently - a name that is
    // not in the registry cannot be protected, so warn. Global block only:
    // a ToolBrokerOverride has no neverHideTools field.
    foreach (name; conf.toolBroker.neverHideTools.filter!(name => name !in registry)) {
        warnings ~= i"neverHideTools: tool '$(name)' is not in the registry - it cannot be protected from hiding"
            .text;
    }

    // The global block's rules, then each model override's resolved view
    // (see the per-model note in the doc). A fully-unset override resolves
    // to the global block and is skipped.
    warnings ~= brokerBlockWarnings(conf.toolBroker, registry, "toolBroker");
    foreach (model; conf.codeModels) {
        auto over = model.toolBroker;
        if (over.enabled.isNull && over.hiddenTags.isNull
                && over.alwaysOn.isNull && over.groups.isNull)
            continue;
        warnings ~= brokerBlockWarnings(resolveToolBrokerConfig(conf, model),
                registry, "model '" ~ model.modelName ~ "' toolBroker");
    }

    // Migration: a toolBroker block still carrying the removed
    // toolTagDescriptions key (collected by applyConfig at parse time)
    // points at the groups replacement.
    if (conf.toolBroker.unknownYamlKeys.canFind("toolTagDescriptions"))
        warnings ~= "toolBroker: 'toolTagDescriptions' was removed - move each tag's description to a 'groups' entry (group name: description, tools list)";

    return warnings;
}

alias applyLlmConfig = applyConfig!LlmConfig;

/// Returns the key from the environment variable, or "" if not set.
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

@("@{llmfun_workarea} in a value expands to the workarea path") unittest {
    auto result = replaceMagicWord!string("@{llmfun_workarea}/file.txt",
            AbsolutePath("/my/workarea"));
    assert(result == "/my/workarea/file.txt", result);
}

@("@{llmfun} in a value expands to the llmfun binary directory") unittest {
    auto result = replaceMagicWord!string("@{llmfun}/bin/tool", AbsolutePath("/my/workarea"));
    assert(result == i"$(thisExePath.dirName)/bin/tool".text);
}

@("a value with both magic words expands both") unittest {
    auto result = replaceMagicWord!string(
            "-v @{llmfun_workarea}:/work -v @{llmfun}/data:/data", AbsolutePath("/my/work"));
    assert(result == i"-v /my/work:/work -v $(thisExePath.dirName)/data:/data".text);
}

@("no magic words, values unchanged") unittest {
    auto result = replaceMagicWord!string("just a plain string", AbsolutePath("/my/workarea"));
    assert(result == "just a plain string");
}

@("an empty options map yields an empty result") unittest {
    string[][string] options;
    auto result = replaceContainerMagicWords(options, AbsolutePath("/work"));
    assert(result.length == 0);
}

@("keys pass through unchanged; only values expand") unittest {
    string[][string] options;
    options["@{llmfun_workarea}"] = ["@{llmfun_workarea}/path"];
    auto result = replaceContainerMagicWords(options, AbsolutePath("/work"));
    assert(result.length == 1);
    assert(result["@{llmfun_workarea}"][0] == "/work/path");
}

@("every value in an options array expands") unittest {
    string[][string] options;
    options["mounts"] = ["-v", "@{llmfun_workarea}:/w", "@{llmfun}/data:/d"];
    auto result = replaceContainerMagicWords(options, AbsolutePath("/work"));
    assert(result["mounts"][0] == "-v");
    assert(result["mounts"][1] == "/work:/w");
    assert(result["mounts"][2] == i"$(thisExePath.dirName)/data:/d".text);
}

@("a string[][string] options block loads into the options field") unittest {
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

@("a valid enum member name selects that member; an invalid one keeps the default") unittest {
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
    auto conf = applyConfig!(LocalEmbedConfig)(LocalEmbedConfig.init,
            parseJSON(`{"mode": "bogus"}`));
    assert(conf.mode == EmbedMode.cpu, "invalid mode must fall back to the default");
}

@("explicit config path always loads regardless of trusted-config") unittest {
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
            trustedConfig: false, includeSystemConfig: false);
    assert(conf.sandboxConfig.maxOutputBytes == 42);
}

@("--no-cwd-config skips CWD config entirely") unittest {
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

@("the shipped config/example.yaml loads with its defaultOptions map intact") unittest {
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

    // The tool-broker keys shipped in the example: groups + hiddenTags +
    // alwaysOn, an explicit kill switch (false, per model overridable), and
    // neverHideTools overriding the default (it un-hides taskDone).
    assert(!conf.toolBroker.enabled, "shipped example kill switch must be false");
    assert(conf.toolBroker.neverHideTools == ["taskDone"],
            "shipped example neverHideTools: " ~ conf.toolBroker.neverHideTools.to!string);

    // Groups: the shipped example defines workarea, pipeline, rag (the rag
    // group exists so the hiddenTags entry below names a defined group and
    // the example validates silently).
    assert(conf.toolBroker.groups.length == 3,
            "shipped example groups: " ~ conf.toolBroker.groups.keys.to!string);
    assert(conf.toolBroker.groups["workarea"] == GroupConfig("Files in the agent workarea: read/write/list/search",
            ["writeFile", "readFile", "editFile", "listDirectory", "grepFiles"]));
    assert(conf.toolBroker.groups["pipeline"].tools == ["pipelineOutput"]);
    assert(conf.toolBroker.groups["rag"].tools == [
        "queryBestMatch", "querySemantic", "queryTextSearch", "queryReadFile",
        "readRAGSource", "listRAGSources", "loadFileToRAG",
        "loadContentToRAG", "removeTopicFromRAG"
    ]);
    assert(conf.toolBroker.hiddenTags == ["workarea", "rag"]);
    assert(conf.toolBroker.alwaysOn == ["taskDone", "listToolTags"]);

    // Resolution: the model override wins per field - enabled flips to true,
    // the empty hiddenTags replaces the global non-empty list (unhide-all),
    // the workarea group is replaced wholesale, the other groups inherit.
    auto resolved = resolveToolBrokerConfig(conf, conf.codeModels[0]);
    assert(resolved.enabled, "model enabled: true must win");
    assert(resolved.hiddenTags.empty, "model hiddenTags: [] must replace the global list");
    assert(resolved.groups["workarea"] == GroupConfig("Fewer workarea tools for this model",
            ["readFile", "listDirectory"]));
    assert(resolved.groups["pipeline"].tools == ["pipelineOutput"],
            "groups the model does not name inherit the global entry");
    assert(resolved.groups["rag"].description == "RAG tools: search/add/remove/list");
    assert(resolved.alwaysOn == ["taskDone", "listToolTags"]);

    // Silent-valid rule: zero warnings for the global block and the model's
    // resolved view (per-model overrides are validated on their resolved view).
    auto warns = validateToolBrokerConfig(conf);
    assert(warns.empty, warns.to!string);
}

@("tui.maxWidth parses from YAML into LlmConfig.tui.maxWidth") unittest {
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

@("absent tui key keeps the default maxWidth = 0") unittest {
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

@("maxWidth below 40 is rejected by validateConfig") unittest {
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

@("maxWidth above 10000 is rejected by validateConfig") unittest {
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

@("the shipped example.yaml parses without the tui key; maxWidth stays 0") unittest {
    auto json = loadYamlValue(Path("config/example.yaml"));
    auto conf = applyLlmConfig(LlmConfig.init, json);
    assert(conf.tui.maxWidth == 0,
            "shipped example.yaml must keep default 0, got " ~ conf.tui.maxWidth.to!string);
}

@("an explicit dialogueDir in YAML round-trips unchanged") unittest {
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

    auto conf = readConfig(configFile.Path, silent: true, noCwdConfig: true,
            trustedConfig: false, includeSystemConfig: false);
    assert(conf.dialogueDir == tmpDir,
            "dialogueDir round-trip failed: " ~ conf.dialogueDir.to!string);
}

@("absent dialogueDir key defaults to the dialogue subdirectory of dataDir") unittest {
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
            trustedConfig: false, includeSystemConfig: false);
    assert(!conf.dialogueDir.empty, "dialogueDir should be non-empty after defaulting");
    assert(conf.dialogueDir == (conf.dataDir ~ "dialogue"),
            "dialogueDir default mismatch: " ~ conf.dialogueDir.to!string);
}

@("default reasoningSummaryPrompt ships as the named file") unittest {
    assert(LlmConfig().reasoningSummaryPrompt == "REASONING_SUMMARY.md");
}

@("an explicit reasoningSummaryPrompt in YAML round-trips unchanged") unittest {
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

    auto conf = readConfig(configFile.Path, silent: true, noCwdConfig: true,
            trustedConfig: false, includeSystemConfig: false);
    assert(conf.reasoningSummaryPrompt == "MY_PROMPT.md",
            "reasoningSummaryPrompt round-trip failed: " ~ conf.reasoningSummaryPrompt);
}

@("validateConfig accepts an empty dialogueDir and a safe explicit one") unittest {
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
    /// Minimal LlmConfig that passes validateConfig's non-nudge checks, so
    /// nudge-policy tests can mutate only the nudges block.
    private LlmConfig nudgesValidateTestConfig() {
        LlmConfig conf;
        conf.codeModels ~= CodeModelConfig(server: ServerConfig(url: "http://localhost:8080",
                warnIfNoApiKey: false), display: "test", modelName: "test");
        return conf;
    }
}

@("global nudges block parses into LlmConfig.nudges") unittest {
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

/// `nudges` absent everywhere -> the struct defaults survive.
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
@("a config without nudges gets the shipped default nudge files") unittest {
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

@("unknown key inside nudges warns instead of throwing") unittest {
    import std.path : buildPath;
    import std.stdio : File;

    // TODO: should use TestArea from test_util
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

    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    assert(conf.nudges.keepReasoning.softStrikes == 2);
    assert(conf.nudges.keepReasoning.hardStrikes == 1);
    assert(conf.codeModels[0].nudges.isNull);
}

/// The empty prompt means the shipped default file, resolved at load time.
@("default nudge policy shape passes validateConfig") unittest {
    validateConfig(nudgesValidateTestConfig());
}

/// A 0/0 ladder would fail on the first strike - a foot-gun, not a policy.
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

/// Disabled kinds are not validated: a disabled kind loads nothing and nudges nothing.
@("empty file names are rejected only for enabled kinds") unittest {
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

/// A threshold in [0.9, 1.0) only warns: the advisory nudge can never fire
/// before forced compression at 90%.
@("compression threshold: >= 0.9 warns, outside (0.0, 1.0) throws") unittest {
    import std.algorithm : canFind;

    auto conf = nudgesValidateTestConfig();
    validateConfig(conf); // default 0.8 is valid and silent

    // At/above the forced point: warning, not throw. The warning is data:
    // validateNudgeConfig returns it; the boundary caller
    // (validateConfig) renders it via logger.warningf.
    conf.nudges.compression.threshold = 0.95;
    auto warnings = validateNudgeConfig("nudges", conf.nudges);
    assert(warnings.canFind!(w => w.canFind("nudges.compression.threshold")),
            "threshold >= 0.9 must warn, got: " ~ warnings.join("\n"));

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

/// A floor of 0 would warn after every tool call; use `enabled: false` to
/// turn the warning off instead.
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

@("a per-model nudges violation is reported with the model's key path") unittest {
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

@("default nudge policy validates silently") unittest {
    assert(validateNudgeConfig("nudges", nudgesValidateTestConfig().nudges).empty);
}

@("toolBroker keys parse from YAML") unittest {
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
  groups:
    workarea:
      description: "Files in the agent workarea: read, write, list, search."
      tools: [writeFile, readFile, editFile, listDirectory, grepFiles]
    rag:
      description: "RAG knowledge base tools."
      tools: [queryBestMatch, queryTextSearch]
  hiddenTags: [workarea, rag]
  alwaysOn: [taskDone, listToolTags]
  neverHideTools:
    - taskDone
    - pipelineOutput
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
    toolBroker:
      enabled: true
      hiddenTags: []
      alwaysOn: [taskDone]
      groups:
        workarea:
          description: "Fewer workarea tools for this model."
          tools: [readFile, listDirectory]
`;
    File(tmpFile, "w").write(yaml);
    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    assert(!conf.toolBroker.enabled, "toolBroker.enabled: false must parse");
    assert(conf.toolBroker.groups["workarea"].description.canFind("workarea"),
            "group description must parse: " ~ conf.toolBroker.groups["workarea"].description);
    assert(conf.toolBroker.groups["workarea"].tools == [
        "writeFile", "readFile", "editFile", "listDirectory", "grepFiles"
    ], "group tools must parse: " ~ conf.toolBroker.groups["workarea"].tools.to!string);
    assert(conf.toolBroker.groups["rag"].tools == [
        "queryBestMatch", "queryTextSearch"
    ]);
    assert(conf.toolBroker.hiddenTags == ["workarea", "rag"],
            "hiddenTags must parse: " ~ conf.toolBroker.hiddenTags.to!string);
    assert(conf.toolBroker.alwaysOn == ["taskDone", "listToolTags"],
            "alwaysOn must parse: " ~ conf.toolBroker.alwaysOn.to!string);
    assert(conf.toolBroker.neverHideTools == ["taskDone", "pipelineOutput"],
            "explicit neverHideTools must parse: " ~ conf.toolBroker.neverHideTools.to!string);

    // Per-model override block.
    auto model = conf.codeModels[0];
    assert(model.toolBroker.enabled.get, "model toolBroker.enabled: true must parse");
    assert(model.toolBroker.hiddenTags.get.empty, "model-level empty hiddenTags must parse");
    assert(model.toolBroker.alwaysOn.get == ["taskDone"]);
    assert(
            model.toolBroker.groups.get["workarea"].description
            == "Fewer workarea tools for this model.");
    assert(model.toolBroker.groups.get["workarea"].tools == [
        "readFile", "listDirectory"
    ], "model groups must parse: " ~ model.toolBroker.groups.get["workarea"].tools.to!string);
}

@("absent toolBroker keys keep the struct defaults") unittest {
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
    assert(!conf.toolBroker.enabled, "default toolBroker.enabled must be false");
    assert(conf.toolBroker.groups.empty, "no groups by default");
    assert(conf.toolBroker.hiddenTags.empty, "no hiddenTags by default");
    assert(conf.toolBroker.alwaysOn.empty, "no alwaysOn by default");
    assert(conf.toolBroker.neverHideTools == ["taskDone"],
            "default neverHideTools must be [\"taskDone\"]");

    auto model = conf.codeModels[0];
    assert(model.toolBroker.enabled.isNull, "absent model toolBroker block stays unset");
    assert(model.toolBroker.hiddenTags.isNull);
    assert(model.toolBroker.groups.isNull);
}

@("per-model toolBroker overrides of any kind parse silently") unittest {
    import std.path : buildPath;

    // TODO: should use TestArea from test_util
    auto tmpDir = buildPath("llmfun_test", "config_toolbroker_override_kinds");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto parse = (string yamlText, string name) {
        import std.path : buildPath;
        import std.stdio : File;

        auto tmpFile = buildPath(tmpDir, name ~ ".yaml");
        File(tmpFile, "w").write(yamlText);
        return applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    };

    // All three Nullable kinds present, including the groups map.
    auto conf = parse(`codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
    toolBroker:
      enabled: true
      hiddenTags: [rag]
      alwaysOn: [taskDone]
      groups:
        workarea:
          description: "Fewer workarea tools for this model."
          tools: [readFile, listDirectory]
`, "present");
    auto model = conf.codeModels[0];
    assert(model.toolBroker.enabled.get, "Nullable!bool must parse");
    assert(model.toolBroker.hiddenTags.get == ["rag"], "Nullable!(string[]) must parse");
    assert(model.toolBroker.alwaysOn.get == ["taskDone"]);
    assert(model.toolBroker.groups.get["workarea"].tools == [
        "readFile", "listDirectory"
    ], "groups map must parse: " ~ model.toolBroker.groups.get.to!string);
    assert(
            model.toolBroker.groups.get["workarea"].description
            == "Fewer workarea tools for this model.");

    // The groups map absent: the field stays unset.
    auto confNoGroups = parse(`codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
    toolBroker:
      enabled: true
`, "groups_absent");
    assert(confNoGroups.codeModels[0].toolBroker.groups.isNull, "absent groups key stays unset");
    assert(confNoGroups.codeModels[0].toolBroker.enabled.get);
}

@("resolveToolBrokerConfig merges global and per-model toolBroker") unittest {
    LlmConfig conf;
    conf.toolBroker.enabled = true;
    conf.toolBroker.groups["workarea"] = GroupConfig("Files in the agent workarea.",
            ["writeFile", "readFile", "editFile"]);
    conf.toolBroker.groups["pipeline"] = GroupConfig("Pipeline tools.", [
        "pipelineOutput"
    ]);
    conf.toolBroker.hiddenTags = ["workarea", "pipeline"];
    conf.toolBroker.alwaysOn = ["taskDone"];

    // Absent model block: the global config passes through unchanged.
    auto resolved = resolveToolBrokerConfig(conf, CodeModelConfig.init);
    assert(resolved.enabled);
    assert(resolved.hiddenTags == ["workarea", "pipeline"]);
    assert(resolved.alwaysOn == ["taskDone"]);
    assert(resolved.groups["workarea"].tools == [
        "writeFile", "readFile", "editFile"
    ]);
    assert(resolved.groups["pipeline"].tools == ["pipelineOutput"]);
    assert(resolved.neverHideTools == ["taskDone"]);

    // Per-field overrides: scalar flip + empty model hiddenTags unhide-all.
    CodeModelConfig model;
    model.toolBroker.enabled = Nullable!bool(false);
    model.toolBroker.hiddenTags = Nullable!(string[])([]);
    model.toolBroker.alwaysOn = Nullable!(string[])(["readFile"]);
    resolved = resolveToolBrokerConfig(conf, model);
    assert(!resolved.enabled, "model enabled: false overrides global true");
    assert(resolved.hiddenTags.empty, "model empty hiddenTags replaces the global list");
    assert(resolved.alwaysOn == ["readFile"]);
    // Absent override fields keep the global value.
    assert(resolved.neverHideTools == ["taskDone"]);
    assert(resolved.groups["pipeline"].tools == ["pipelineOutput"]);

    // Per-entry groups merge: a model entry with the same name replaces the
    // global entry wholesale; other entries pass through.
    GroupConfig[string] modelGroups;
    model.toolBroker.groups = Nullable!(GroupConfig[string])(modelGroups);
    model.toolBroker.groups.get["workarea"] = GroupConfig(
            "Fewer workarea tools for this model.", [
        "readFile", "listDirectory"
    ]);
    resolved = resolveToolBrokerConfig(conf, model);
    assert(resolved.groups["workarea"].tools == ["readFile", "listDirectory"],
            "model group entry replaces the global entry wholesale");
    assert(resolved.groups["workarea"].description == "Fewer workarea tools for this model.");
    assert(resolved.groups["pipeline"].tools == ["pipelineOutput"], "global-only entry survives");
}

@("AgentsMd global block parses; absent block keeps the built-in defaults") unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "config_agentsmd_globals");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "test.yaml");

    // Absent block: the built-in defaults survive.
    string yaml = `codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(tmpFile, "w").write(yaml);
    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    assert(conf.agentsMd == AgentsMdConfig.init, "absent AgentsMd keeps the defaults");
    assert(conf.agentsMd.enabled);
    assert(conf.agentsMd.compression.mode == CompressionMode.auto_);
    assert(conf.agentsMd.compression.thresholdKb == 20);
    assert(conf.agentsMd.ragIndex);

    // Full explicit block: every field applies; thresholdKb: 0 is legal.
    yaml = `agentsMd:
  enabled: false
  compression:
    mode: full
    thresholdKb: 0
  ragIndex: false
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(tmpFile, "w").write(yaml);
    conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    assert(!conf.agentsMd.enabled);
    assert(conf.agentsMd.compression.mode == CompressionMode.full);
    assert(conf.agentsMd.compression.thresholdKb == 0,
            "thresholdKb: 0 must parse (always-summary under auto), got "
            ~ conf.agentsMd.compression.thresholdKb.to!string);
    assert(!conf.agentsMd.ragIndex);
}

@("effectiveAgentsMd resolves per-field: restated keys win, absent inherit global") unittest {
    import std.typecons : Nullable;

    // Absent global block: the built-in defaults are the base layer.
    auto conf = LlmConfig.init;
    auto resolved = effectiveAgentsMd(conf, CodeModelConfig.init);
    assert(resolved.enabled);
    assert(resolved.compression.mode == CompressionMode.auto_);
    assert(resolved.compression.thresholdKb == 20);
    assert(resolved.ragIndex);

    // Global block set + absent model block: the global passes through.
    conf.agentsMd.enabled = false;
    conf.agentsMd.compression.mode = CompressionMode.summary;
    conf.agentsMd.compression.thresholdKb = 100;
    conf.agentsMd.ragIndex = false;
    resolved = effectiveAgentsMd(conf, CodeModelConfig.init);
    assert(!resolved.enabled);
    assert(resolved.compression.mode == CompressionMode.summary);
    assert(resolved.compression.thresholdKb == 100);
    assert(!resolved.ragIndex);

    // Per-field overrides: restated keys replace the global value,
    // unspecified keys inherit it.
    CodeModelConfig model;
    model.agentsMd.mode = Nullable!CompressionMode(CompressionMode.full);
    model.agentsMd.thresholdKb = Nullable!long(5);
    resolved = effectiveAgentsMd(conf, model);
    assert(!resolved.enabled, "absent model enabled inherits the global false");
    assert(resolved.compression.mode == CompressionMode.full, "restated mode wins");
    assert(resolved.compression.thresholdKb == 5, "restated thresholdKb wins");
    assert(!resolved.ragIndex, "absent model ragIndex inherits the global false");
}

@("unknown AgentsMd keys warn via the existing unknown-key loop") unittest {
    import std.algorithm : canFind;
    import std.path : buildPath;
    import std.stdio : File;

    import llm.test_util : testArea;

    auto tmpDir = testArea("config_agentsmd_unknown");
    scope (exit)
        tmpDir.cleanup;
    auto tmpFile = buildPath(tmpDir, "test.yaml");
    string yaml = `agentsMd:
  enabled: true
  bogusKey: 1
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(tmpFile, "w").write(yaml);

    // Explicit collector instead of a sharedLog swap (AGENTS.md Testing rule).
    string[] warnings;
    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)),
            (logger.LogLevel lvl, string msg) { warnings ~= msg; });
    assert(conf.agentsMd.enabled, "the known key still parsed");
    assert(warnings.canFind!(w => w.canFind("bogusKey")), warnings.to!string);
    assert(warnings.canFind!(w => w.canFind("Unknown configuration key AgentsMdConfig.bogusKey")),
            warnings.to!string);

}

@("thresholdKb: -1 is a parse error; thresholdKb: 0 is legal") unittest {
    import std.algorithm : canFind;
    import std.path : buildPath;
    import std.stdio : File;

    import llm.test_util : testArea;

    auto tmpDir = testArea("config_agentsmd_threshold");
    scope (exit)
        tmpDir.cleanup;
    auto tmpFile = buildPath(tmpDir, "test.yaml");

    // Explicit collector instead of a sharedLog swap (AGENTS.md Testing rule).
    string[] warnings;

    // Negative: parse error - the invalid value is not applied and the
    // warning names the key; the field keeps its default.
    string yaml = `agentsMd:
  compression:
    mode: summary
    thresholdKb: -1
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(tmpFile, "w").write(yaml);
    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)),
            (logger.LogLevel lvl, string msg) { warnings ~= msg; });
    assert(conf.agentsMd.compression.thresholdKb == 20,
            "the rejected value must not be applied, got "
            ~ conf.agentsMd.compression.thresholdKb.to!string);
    assert(warnings.canFind!(w => w.canFind("thresholdKb") && w.canFind("-1")), warnings.to!string);
    assert(warnings.canFind!(w => w.canFind("non-negative")), warnings.to!string);

    // Zero is legal (always-summary under auto).
    yaml = `agentsMd:
  compression:
    thresholdKb: 0
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(tmpFile, "w").write(yaml);
    warnings = null;
    conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)),
            (logger.LogLevel lvl, string msg) { warnings ~= msg; });
    assert(conf.agentsMd.compression.thresholdKb == 0,
            "thresholdKb: 0 must parse, got " ~ conf.agentsMd.compression.thresholdKb.to!string);
}

@("unknown compression.mode is a parse error naming the valid values") unittest {
    import std.algorithm : canFind;
    import std.exception : collectException;
    import std.path : buildPath;
    import std.stdio : File;

    import llm.test_util : testArea;

    // The mapping helper rejects unknown spellings naming the valid values.
    auto e = collectException(compressionModeFromYaml("bogus"));
    assert(e !is null, "unknown compression.mode must be a parse error");
    assert(e.msg.canFind("auto | full | summary"), e.msg);

    auto tmpDir = testArea("config_agentsmd_mode");
    scope (exit)
        tmpDir.cleanup;
    auto tmpFile = buildPath(tmpDir, "test.yaml");

    // Explicit collector instead of a sharedLog swap (AGENTS.md Testing rule).
    string[] warnings;

    // End to end: the bad spelling is a per-field parse error - the mode
    // field keeps its enum default, the sibling keys still apply, and the
    // warning names the valid values.
    string yaml = `agentsMd:
  enabled: false
  compression:
    mode: bogus
    thresholdKb: 7
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(tmpFile, "w").write(yaml);
    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)),
            (logger.LogLevel lvl, string msg) { warnings ~= msg; });
    assert(!conf.agentsMd.enabled, "AgentsMd-level siblings still parse");
    assert(conf.agentsMd.compression.mode == CompressionMode.auto_,
            "the rejected field keeps its default");
    assert(conf.agentsMd.compression.thresholdKb == 7,
            "sibling keys still parse, got " ~ conf.agentsMd.compression.thresholdKb.to!string);
    assert(warnings.canFind!(w => w.canFind("auto | full | summary")), warnings.to!string);
}

/// Regression: a YAML float whose value differs from the struct default must
/// actually apply - the duplicate empty isFloatingPoint branch used to make
/// the assignment unreachable (silently keeping the default).
@("a YAML float value that differs from the struct default is applied") unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "config_float_applies");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto tmpFile = buildPath(tmpDir, "test.yaml");
    string yaml = `nudges:
  compression:
    threshold: 0.75
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(tmpFile, "w").write(yaml);

    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    assert(conf.nudges.compression.threshold == 0.75,
            "float config value must override the struct default, got "
            ~ conf.nudges.compression.threshold.to!string);
}

@("tool broker validation warns on invalid configs and stays silent on valid ones") unittest {
    import std.algorithm : canFind;

    // Shipped defaults validate silently: taskDone is registered and
    // untagged (registered by tool_call's module ctor, which runs first).
    auto conf = LlmConfig.init;
    assert(validateToolBrokerConfig(conf).empty);

    // An unknown neverHide name cannot be protected - warn.
    conf.toolBroker.neverHideTools = ["taskDone", "bogus_no_hide"];
    auto warnings = validateToolBrokerConfig(conf);
    assert(warnings.length == 1, warnings.to!string);
    assert(warnings[0].canFind("bogus_no_hide"), warnings.to!string);

    // A group listing an unregistered tool warns; an empty
    // undescribed group warns; a group listing a registered tool stays
    // silent; an empty DESCRIBED group stays silent (the MCP preset shape).
    conf = LlmConfig.init;
    conf.toolBroker.groups = [
        "workarea": GroupConfig("Workarea tools.", ["readFile", "bogus_tool"]),
        "empty": GroupConfig("", []),
        "preset": GroupConfig("Nothing here.", []),
        "good": GroupConfig("Fine.", ["taskDone"]),
    ];
    warnings = validateToolBrokerConfig(conf);
    assert(warnings.length == 2, warnings.to!string);
    assert(warnings[0].canFind("empty"), warnings.to!string);
    assert(warnings[1].canFind("workarea"), warnings.to!string);
    assert(warnings[1].canFind("bogus_tool"), warnings.to!string);

    // A fully valid config (groups + hiddenTags + alwaysOn + neverHide all
    // consistent) validates silently.
    conf = LlmConfig.init;
    conf.toolBroker.groups = [
        "workarea": GroupConfig("Workarea tools.", ["readFile", "taskDone"])
    ];
    conf.toolBroker.hiddenTags = ["workarea"];
    conf.toolBroker.alwaysOn = ["taskDone"];
    conf.toolBroker.neverHideTools = ["taskDone", "readFile"];
    assert(validateToolBrokerConfig(conf).empty);

    // A hiddenTags entry that names neither a group nor a registered
    // tool warns; alwaysOn likewise. A defined group name and a registered
    // tool name stay silent.
    conf = LlmConfig.init;
    conf.toolBroker.groups["workarea"] = GroupConfig("Workarea tools.", [
        "taskDone"
    ]);
    conf.toolBroker.hiddenTags = ["workarea", "ghost_group"];
    conf.toolBroker.alwaysOn = ["taskDone", "ghost_tool"];
    warnings = validateToolBrokerConfig(conf);
    assert(warnings.length == 2, warnings.to!string);
    assert(warnings[0].canFind("hiddenTags"), warnings.to!string);
    assert(warnings[0].canFind("ghost_group"), warnings.to!string);
    assert(warnings[1].canFind("alwaysOn"), warnings.to!string);
    assert(warnings[1].canFind("ghost_tool"), warnings.to!string);

    // A per-model toolBroker override is validated on its resolved view:
    // an override listing an unregistered tool warns with the model's label;
    // a fully-unset override resolves to the global block and stays silent.
    conf = LlmConfig.init;
    conf.toolBroker.groups["workarea"] = GroupConfig("Workarea tools.", [
        "taskDone"
    ]);
    conf.codeModels.length = 1;
    conf.codeModels[0].modelName = "test";
    conf.codeModels[0].toolBroker.groups = Nullable!(GroupConfig[string])(
            [
        "workarea": GroupConfig("Workarea tools.", [
            "taskDone", "ghost_model_tool"
        ])
    ]);
    warnings = validateToolBrokerConfig(conf);
    assert(warnings.length == 1, warnings.to!string);
    assert(warnings[0].canFind("model 'test'"), warnings.to!string);
    assert(warnings[0].canFind("ghost_model_tool"), warnings.to!string);

    // A fully-unset override resolves to the global block - no duplicate
    // warnings, silence.
    conf = LlmConfig.init;
    conf.toolBroker.groups["workarea"] = GroupConfig("Workarea tools.", [
        "taskDone"
    ]);
    conf.codeModels.length = 1;
    conf.codeModels[0].modelName = "test";
    warnings = validateToolBrokerConfig(conf);
    assert(warnings.empty, warnings.to!string);
}

// The warning path: applyConfig's unknown-key loop collects the orphan key
// into ToolBrokerConfig.unknownYamlKeys, and the startup validator turns it
// into the warning.
@("a leftover toolTagDescriptions key warns with a pointer to groups") unittest {
    import std.algorithm : canFind;
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "config_toolbroker_migration");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "test.yaml");
    string yaml = `toolBroker:
  toolTagDescriptions:
    workarea: "Files in the agent workarea."
`;
    File(tmpFile, "w").write(yaml);
    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    auto warnings = validateToolBrokerConfig(conf);
    assert(warnings.length == 1, warnings.to!string);
    assert(warnings[0].canFind("toolTagDescriptions"), warnings.to!string);
    assert(warnings[0].canFind("groups"), warnings.to!string);
}

@("a meta-tool in a described group warns; ungrouped or empty-description groups stay silent") unittest {
    import std.algorithm : canFind;

    // Silent: no groups at all.
    assert(discoveryMetaToolTagWarnings(ToolBrokerConfig.init, "toolBroker").empty);

    // Silent: the meta-tool is ungrouped, or in a group whose description is
    // empty. A described group without the meta-tool is silent too.
    auto conf = ToolBrokerConfig.init;
    conf.groups["vague"] = GroupConfig("", ["listToolTags"]);
    conf.groups["other"] = GroupConfig("Other tools.", ["taskDone"]);
    assert(discoveryMetaToolTagWarnings(conf, "toolBroker").empty);

    // Warn: the meta-tool is a member of a described group.
    conf.groups["vague"].description = "Tag tools.";
    auto warnings = discoveryMetaToolTagWarnings(conf, "toolBroker");
    assert(warnings.length == 1, warnings.to!string);
    assert(warnings[0].canFind("listToolTags"), warnings.to!string);
    assert(warnings[0].canFind("vague"), warnings.to!string);
}

@("default LlmConfig carries the time-based tier knobs") unittest {
    auto conf = LlmConfig.init;
    assert(conf.shortTermDays == 7);
    assert(conf.longTermDays == 100);
    assert(conf.tierPassIntervalHours == 6);
}

@("memory tier dir helpers append the tier name to the area dir") unittest {
    assert(memoryShortDir(Path("a")).toString == "a/short");
    assert(memoryLongDir(Path("a")).toString == "a/long");
    assert(memoryArchiveDir(Path("a")).toString == "a/archive");
}

@("makeLocalSetupFileStructure creates the tier subdirs and is idempotent") unittest {
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "config_tier_dirs");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto conf = LlmConfig.init;
    conf.dataDir = Path(buildPath(tmpDir, "data"));
    conf.workArea = Path(buildPath(tmpDir, "workarea"));
    makeLocalSetupFileStructure(conf);
    makeLocalSetupFileStructure(conf);

    foreach (tier; ["short", "long", "archive"])
        assert(buildPath(tmpDir, "data", "memory", tier).exists, tier);
}

@("ensureMemoryTierDirs creates the tier subdirs under EVERY memory area and is idempotent") unittest {
    import std.path : buildPath;
    import llm.test_util : testArea;

    auto tmpDir = testArea("ensure_tier_dirs");
    scope (exit)
        tmpDir.cleanup;

    // Two areas, like the default resolution (local data dir + XDG home).
    auto areas = [
        Path(buildPath(tmpDir, "local", "memory")),
        Path(buildPath(tmpDir, "xdg", "memory"))
    ];
    // Pre-existing structure in one area only: the other must still be fixed.
    mkdirRecurse((areas[1] ~ "short").toString);

    ensureMemoryTierDirs(areas);
    ensureMemoryTierDirs(areas); // idempotent

    foreach (area; areas)
        foreach (tier; ["short", "long", "archive"])
            assert((area ~ tier).exists, area.toString ~ "/" ~ tier);
}

@("per-model compression.mode: bogus is a parse error naming the valid values") unittest {
    import std.algorithm : canFind;
    import std.path : buildPath;
    import std.stdio : File;

    import llm.test_util : testArea;

    auto tmpDir = testArea("config_agentsmd_bogus_mode");
    scope (exit)
        tmpDir.cleanup;
    auto tmpFile = buildPath(tmpDir, "test.yaml");

    // Explicit collector instead of a sharedLog swap (AGENTS.md Testing rule).
    string[] warnings;

    // Per-model block: the bogus mode spelling is rejected with a warning, so
    // that Nullable override field stays unset and the model inherits the
    // global value. (The global-block half of this scenario is covered by the
    // existing `unknown compression.mode` unittest above.)
    string yaml = `agentsMd:
  compression:
    mode: summary
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
    agentsMd:
      mode: bogus
`;
    File(tmpFile, "w").write(yaml);
    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)),
            (logger.LogLevel lvl, string msg) { warnings ~= msg; });
    assert(conf.agentsMd.compression.mode == CompressionMode.summary);
    assert(conf.codeModels[0].agentsMd.mode.isNull,
            "the rejected override must not be applied, got "
            ~ conf.codeModels[0].agentsMd.mode.to!string);
    assert(warnings.canFind!(w => w.canFind("bogus")
            && w.canFind("auto | full | summary")), warnings.to!string);
}

@("per-model agentsMd YAML block parses; effectiveAgentsMd layers it over the global") unittest {
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "config_agentsmd_model");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "test.yaml");

    // Global block + per-model per-field override: restated keys win,
    // unspecified keys inherit the global block.
    string yaml = `agentsMd:
  enabled: false
  compression:
    mode: summary
    thresholdKb: 100
  ragIndex: false
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
    agentsMd:
      mode: full
      thresholdKb: 5
`;
    File(tmpFile, "w").write(yaml);
    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    auto resolved = effectiveAgentsMd(conf, conf.codeModels[0]);
    assert(!resolved.enabled, "absent model enabled inherits the global false");
    assert(resolved.compression.mode == CompressionMode.full, "restated mode wins");
    assert(resolved.compression.thresholdKb == 5, "restated thresholdKb wins");
    assert(!resolved.ragIndex, "absent model ragIndex inherits the global false");

    // A model without an agentsMd block inherits the global as-is.
    resolved = effectiveAgentsMd(conf, CodeModelConfig.init);
    assert(!resolved.enabled);
    assert(resolved.compression.mode == CompressionMode.summary);
    assert(resolved.compression.thresholdKb == 100);
    assert(!resolved.ragIndex);

    // Absent global block: the built-in defaults are the base layer.
    yaml = `codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
    agentsMd:
      enabled: true
`;
    File(tmpFile, "w").write(yaml);
    conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    resolved = effectiveAgentsMd(conf, conf.codeModels[0]);
    assert(resolved.enabled, "restated model enabled wins over the built-in default");
    assert(resolved.compression.mode == CompressionMode.auto_);
    assert(resolved.compression.thresholdKb == 20);
    assert(resolved.ragIndex);
}

@("agentsMdSourceSummary reports per-field provenance: per-model, global, default") unittest {
    import std.algorithm.searching : canFind;
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "config_agentsmd_source");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "test.yaml");

    // Global block restates mode (global layer), per-model restates thresholdKb
    // (per-model layer); enabled and ragIndex come from the built-in defaults.
    string yaml = `agentsMd:
  compression:
    mode: summary
codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
    agentsMd:
      thresholdKb: 5
`;
    File(tmpFile, "w").write(yaml);
    auto conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));

    auto source = agentsMdSourceSummary(conf, conf.codeModels[0]);
    assert(source.canFind("mode=summary (global)"), source);
    assert(source.canFind("thresholdKb=5 (per-model)"), source);
    assert(source.canFind("enabled=true (default)"), source);
    assert(source.canFind("ragIndex=true (default)"), source);

    // No agentsMd anywhere: everything reports the built-in default layer.
    yaml = `codeModels:
  - modelName: test
    display: test42
    server:
      url: http://localhost:8080
`;
    File(tmpFile, "w").write(yaml);
    conf = applyLlmConfig(LlmConfig.init, loadYamlValue(Path(tmpFile)));
    source = agentsMdSourceSummary(conf, conf.codeModels[0]);
    assert(source == "enabled=true (default), mode=auto (default), "
            ~ "thresholdKb=20 (default), ragIndex=true (default)", source);
}

version (unittest) {
    /// Minimal LlmConfig whose promptDir holds only AGENT.md, with skills
    /// disabled so getPrompt never touches the (null) skill manager.
    private LlmConfig agentMdComposeTestConfig(string promptDir) {
        LlmConfig conf;
        conf.disableSkills = true;
        conf.promptDir = [promptDir.Path];
        return conf;
    }
}

@("getPrompt with an empty payload injects nothing") unittest {
    import std.file : write;

    import llm.test_util : testArea;

    auto td = testArea("getprompt_empty_payload");
    scope (exit)
        td.cleanup;
    write(td.workArea ~ "AGENT.md", "Base prompt.");
    auto conf = agentMdComposeTestConfig(td.workArea);

    auto prompt = conf.getPrompt(null, "AGENT.md", addSkills: true, agentsMd: AgentsMdPrompt.init);
    assert(prompt == "Base prompt.", "empty payload must inject nothing");
}

@("getPrompt with a whitespace-only payload injects nothing") unittest {
    import std.file : write;

    import llm.test_util : testArea;

    auto td = testArea("getprompt_whitespace_payload");
    scope (exit)
        td.cleanup;
    write(td.workArea ~ "AGENT.md", "Base prompt.");
    auto conf = agentMdComposeTestConfig(td.workArea);

    auto prompt = conf.getPrompt(null, "AGENT.md", addSkills: true, agentsMd: AgentsMdPrompt("  \n\t",
            "agent_md_0a1b2c3d"));
    assert(prompt == "Base prompt.", "whitespace-only payload counts as absent");
}

@("getPrompt injects the summary payload and a pointer instruction naming the exact RAG topic") unittest {
    import std.algorithm : canFind;
    import std.file : write;

    import llm.test_util : testArea;

    auto td = testArea("getprompt_summary_pointer");
    scope (exit)
        td.cleanup;
    write(td.workArea ~ "AGENT.md", "Base prompt.");
    auto conf = agentMdComposeTestConfig(td.workArea);

    auto prompt = conf.getPrompt(null, "AGENT.md", addSkills: true, agentsMd: AgentsMdPrompt(
            "# AGENTS.md Rules\n- Rule 1", "agent_md_0a1b2c3d"));

    assert(prompt.canFind("# AGENTS.md Rules"), "summary payload injected");
    assert(prompt.canFind("query your RAG knowledge base for topic `agent_md_0a1b2c3d`"),
            "pointer instruction must embed the exact RAG topic name");
    assert(indexOf(prompt, "Base prompt.") < indexOf(prompt, "# AGENTS.md Rules")
            && indexOf(prompt, "# AGENTS.md Rules") < indexOf(prompt,
                "query your RAG knowledge base"), "order: base → payload → pointer instruction");
}

@("getPrompt with an empty ragTopic emits no pointer instruction") unittest {
    import std.algorithm : canFind;
    import std.file : write;

    import llm.test_util : testArea;

    auto td = testArea("getprompt_summary_no_pointer");
    scope (exit)
        td.cleanup;
    write(td.workArea ~ "AGENT.md", "Base prompt.");
    auto conf = agentMdComposeTestConfig(td.workArea);

    auto prompt = conf.getPrompt(null, "AGENT.md", addSkills: true, agentsMd: AgentsMdPrompt(
            "# AGENTS.md Rules", ""));

    assert(prompt.canFind("# AGENTS.md Rules"), "payload still injected");
    assert(!prompt.canFind("query your RAG knowledge base"),
            "no pointer instruction without a ragTopic");
}

@("getPrompt with an empty payload injects nothing even with a ragTopic set") unittest {
    import std.file : write;

    import llm.test_util : testArea;

    auto td = testArea("getprompt_empty_payload_ragtopic");
    scope (exit)
        td.cleanup;
    write(td.workArea ~ "AGENT.md", "Base prompt.");
    auto conf = agentMdComposeTestConfig(td.workArea);

    auto prompt = conf.getPrompt(null, "AGENT.md", addSkills: true, agentsMd: AgentsMdPrompt("",
            "agent_md_0a1b2c3d"));

    assert(prompt == "Base prompt.", "empty payload wins: no injection, no instruction");
}

@("composeAgentMdRagPointer embeds the topic name in the pointer wording") unittest {
    const instruction = composeAgentMdRagPointer("agent_md_0a1b2c3d");
    assert(instruction == "For detailed explanations, architecture justifications, or lengthy code examples, "
            ~ "query your RAG knowledge base for topic `agent_md_0a1b2c3d`; however, you must obey the "
            ~ "summarized rules listed above at all times.");
}
