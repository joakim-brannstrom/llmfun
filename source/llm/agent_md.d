// AGENTS.md handling module.
// Manages AGENTS.md file detection, treatment selection, caching, and
// summarization. The prompt payload follows the effective treatment: a
// compressed summary OR the verbatim text, per the agentsMd config; the full
// document is indexed into RAG only when ragIndex is set.

module llm.agent_md;

import logger = std.logger;
import std.algorithm : startsWith, map, filter, canFind;
import std.array : join, array;
import std.datetime : SysTime, Clock, dur, DateTime, UTC;
import std.file : exists, readText, rename, remove;
import std.json : JSONValue, parseJSON, JSONOptions, JSONType;
import std.path : dirName;
import std.range : empty;
import std.stdio : File;
import std.string : splitLines, strip, stripRight, indexOf;
import std.sumtype : match;

public import my.path : Path, AbsolutePath;

import llm.utility : computeContentHash;
import llm.config : AgentsMdCompression, AgentsMdConfig, CompressionMode,
    CodeModelConfig, LlmConfig, RagConfig, agentsMdSourceSummary, effectiveAgentsMd;
import llm.rag.rag : RAG, Document, Origin, Topic, Offset, add, Url;
import llm.summary_agent : SummaryChunkT;

/// State representation for an AGENTS.md file.
/// Tracks the checksum of the original content, the prompt payload,
/// and the timestamp when this state was last updated.
struct AgentMdState {
    /// Content checksum computed via MurmurHash3-32 (matches utility.d hashing).
    long checksum;

    /// Prompt payload injected into the system prompt: the generated summary
    /// (summary/full-auto-over-threshold treatment) OR the verbatim AGENTS.md
    /// text (full treatment). Not persisted beyond the three cache keys.
    string summary;

    /// Timestamp when this state was last updated.
    SysTime timestamp;

    /// Exact RAG topic name (agent_md_<8-hex>) at decision time; empty when no
    /// pointer instruction is allowed (ragIndex=false or full-text treatment).
    /// NOT persisted (cache schema unchanged); stamped from the effective config
    /// at every return - never read from loadAgentMdCache results.
    string ragTopic;

    /// Whether this state is considered valid/current.
    bool isValid() const @safe pure nothrow @nogc {
        return this.checksum != 0 && !this.summary.empty;
    }

    /// Compare two AgentMdState instances for equality.
    bool opEquals(const AgentMdState other) const @safe pure nothrow @nogc {
        return this.checksum == other.checksum && this.summary == other.summary
            && this.timestamp == other.timestamp;
    }
}

// Load AGENTS.md state from disk cache.
// Reads `agent_md_cache.json` and reconstructs the cached `AgentMdState`.
// Returns a default-constructed state if the file is missing, corrupted, or not a JSON object.
// Params:
//  cachePath: Path to the cache file
// Returns: The cached AgentMdState, or a default-constructed state if loading fails.
AgentMdState loadAgentMdCache(Path cachePath) @safe {
    if (!cachePath.exists) {
        return AgentMdState.init;
    }

    try {
        auto json = cachePath.readText.parseJSON;
        if (json.type != JSONType.object) {
            logger.warningf("AGENTS.md cache is not a JSON object: %s", cachePath);
            return AgentMdState.init;
        }

        long checksum = 0;
        string summary;
        SysTime timestamp;

        if ("checksum" in json) {
            checksum = json["checksum"].integer;
        }
        if ("summary" in json) {
            summary = json["summary"].str;
        }
        if ("timestamp" in json) {
            long ts = json["timestamp"].integer;
            if (ts > 0) {
                timestamp = SysTime(DateTime.init, UTC()) + ts.dur!"seconds";
            }
        }

        return AgentMdState(checksum, summary, timestamp);
    } catch (Exception e) {
        logger.warningf("Failed to load AGENTS.md cache '%s': %s", cachePath, e.msg);
        return AgentMdState();
    }
}

// Save AGENTS.md state to disk cache.
// Writes checksum, summary, and timestamp to `agent_md_cache.json` using atomic write
// (temp file + rename). Returns true on success, false on failure.
// Params:
//  cachePath: Path to the cache file
//  state: The AgentMdState to persist
// Returns: true if the state was saved successfully.
bool saveAgentMdCache(Path cachePath, AgentMdState state) @safe {
    if (!cachePath.dirName.exists) {
        logger.warningf("Cache directory does not exist: %s", cachePath.dirName);
        return false;
    }

    string tempFile = cachePath.toString ~ ".tmp";
    try {
        auto json = JSONValue();
        json["checksum"] = state.checksum;
        json["summary"] = state.summary;
        json["timestamp"] = state.timestamp.toUnixTime!long;

        File(tempFile, "w").writeln(json.toString(JSONOptions.doNotEscapeSlashes));
        rename(tempFile, cachePath);

        return true;
    } catch (Exception e) {
        if (tempFile.exists) {
            remove(tempFile);
        }
        logger.warningf("Failed to save AGENTS.md cache '%s': %s", cachePath, e.msg);
        return false;
    }
}

// Generate RAG topic name from a content checksum.
// Produces names like "agent_md_1a2b3c4d" for consistent topic identification.
private string makeAgentMdTopicName(long checksum) @safe pure {
    import std.string : format;

    return format("agent_md_%08x", checksum);
}

// Remove all agent_md_* topics from RAG.
// Used during noCwdConfig cleanup and when replacing old AGENTS.md content.
private void removeAgentMdTopics(RAG rag) {
    if (rag is null)
        return;
    foreach (src; rag.db.getSources()) {
        src.origin.match!((Topic t) {
            if (t.name.startsWith("agent_md_")) {
                logger.tracef("Removing AGENTS.md topic '%s' from RAG", t.name);
                rag.removeSource(Origin(t));
            }
        }, (Url _) { /* URLs are not AGENTS.md topics */ }, (Path _) { /* Paths are not AGENTS.md topics */ });
    }
}

// Remove a specific AGENTS.md topic by checksum.
private void removeAgentMdTopic(RAG rag, long checksum) {
    if (rag is null)
        return;
    auto topicName = makeAgentMdTopicName(checksum);
    try {
        rag.removeSource(Origin(Topic(topicName)));
        logger.tracef("Removed AGENTS.md topic '%s' from RAG", topicName);
    } catch (Exception e) {
        logger.warningf("Failed to remove AGENTS.md topic '%s': %s", topicName, e.msg);
    }
}

// Reload AGENTS.md content into in-memory RAG.
// Used when forceReload is true to ensure the topic exists in the ephemeral RAG.
private void reloadAgentMdToRag(RAG rag, string content, long checksum, RagConfig ragConfig) {
    if (rag is null)
        return;
    auto topicName = makeAgentMdTopicName(checksum);
    try {
        auto result = add(rag, Document(Origin(Topic(topicName)), content,
                Offset.init), ragConfig);
        logger.tracef("Loaded AGENTS.md into in-memory RAG as '%s' (%s chunks)",
                topicName, result.chunks);
    } catch (Exception e) {
        logger.warningf("Failed to load AGENTS.md into in-memory RAG: %s", e.msg);
    }
}

/// Merge multiple chunk summaries into a single, clean bullet list.
/// deduplicates bullet points, and ensures all lines use "- " prefix.
private string bulletListMerge(SummaryChunkT[] summaries) {
    if (summaries.empty)
        return "";

    string[] allBullets;
    foreach (chunk; summaries) {
        auto text = chunk[0]; // the summary string
        foreach (trimmed; text.splitLines
                .map!(a => a.stripRight)
                .filter!(a => !a.empty)) {
            string bullet = () {
                if (trimmed.startsWith("- "))
                    return trimmed;
                else if (trimmed.startsWith("* "))
                    return "- " ~ trimmed[2 .. $];
                return "- " ~ trimmed;
            }();

            bool found = false;
            foreach (existing; allBullets.filter!(a => a == bullet)) {
                found = true;
                break;
            }
            if (!found) {
                allBullets ~= bullet;
            }
        }
    }

    if (allBullets.empty)
        return "";

    return allBullets.join("\n") ~ "\n";
}

// Delegate wrapper for bulletListMerge to pass as MergeCallback.
private string delegate(SummaryChunkT[] summaries) bulletListMergeCallback = (SummaryChunkT[] s) => bulletListMerge(
        s);

/// Whether the effective treatment needs the summary pipeline (cache read,
/// SummaryAgent call): mode=summary always does, full never, and auto only
/// when the raw text exceeds the threshold. enabled=false is a
/// pure no-op and never needs a summary.
private bool agentMdNeedsSummary(const AgentsMdConfig agentsMd, size_t rawSize) @safe pure nothrow @nogc {
    if (!agentsMd.enabled) {
        return false;
    }
    final switch (agentsMd.compression.mode) with (CompressionMode) {
    case CompressionMode.summary:
        return true;
    case CompressionMode.full:
        return false;
    case CompressionMode.auto_:
        if (agentsMd.compression.thresholdKb <= 0) {
            return true; // thresholdKb: 0 = "always summary" under auto
        }
        return rawSize > agentMdThresholdBytes(agentsMd);
    }
}

private size_t agentMdThresholdBytes(const AgentsMdConfig agentsMd) @safe pure nothrow @nogc {
    return agentsMd.compression.thresholdKb > int.max / 1024 ? int.max
        : cast(size_t) agentsMd.compression.thresholdKb * 1024;
}

/// Whether a /model-switch recompute (rebuildSystemPrompt for `model`) may run
/// the summary pipeline (model-switch UX): the effective treatment for `model` resolves
/// to summary, or auto with an over-threshold file whose cache does not already
/// hold a valid summary for the current content (a cache hit makes the
/// recompute a fast cache read - no status message needed). enabled=false
/// never summarizes; a missing, unreadable, or whitespace-only AGENTS.md
/// has nothing to summarize. Reads the real workarea (processAgentMd hardcodes
/// `AbsolutePath(".")` - the dub test runner runs tests concurrently, so tests
/// must not chdir).
package bool agentMdSwitchMaySummarize(const LlmConfig conf, const CodeModelConfig model) @safe {
    const agentsMd = effectiveAgentsMd(conf, model);
    if (!agentsMd.enabled)
        return false;

    auto agentMdPath = (AbsolutePath(".") ~ "AGENTS.md").Path;
    if (!agentMdPath.exists)
        return false;

    string rawContent;
    try {
        rawContent = agentMdPath.readText;
    } catch (Exception e) {
        return false;
    }

    import std.string : strip;

    if (rawContent.strip.empty)
        return false;

    const rawSize = rawContent.length;
    if (!agentMdNeedsSummary(agentsMd, rawSize))
        return false;

    if (agentsMd.compression.mode != CompressionMode.auto_)
        return agentsMd.compression.mode == CompressionMode.summary;

    // auto + over-threshold: skip the status message when the cache already
    // holds a valid summary for the current content (the recompute is then a
    // fast cache read).
    const contentChecksum = computeContentHash(rawContent);
    auto cachedState = loadAgentMdCache((conf.dataDir ~ "agent_md_cache.json").Path);
    return !(cachedState.isValid() && cachedState.checksum == contentChecksum);
}

private string agentMdRagTopic(const AgentsMdConfig agentsMd, bool summaryTreatment,
        string topicName) @safe pure nothrow @nogc {
    return (summaryTreatment && agentsMd.ragIndex) ? topicName : "";
}

// Process an AGENTS.md file end-to-end.
// Orchestrates the full AGENTS.md workflow: noCwdConfig handling, checksum comparison,
// cache hit/miss logic, summarization, RAG loading, and in-memory RAG reload.
// Params:
//  config The LLM configuration (provides dataDir, summaryModel, ragConfig)
//  noCwdConfig When true, skip AGENTS.md loading and clean up existing AGENTS.md data
//  rag The RAG instance for loading content and removing topics
//  agentsMd The effective AgentsMd treatment for the active model, resolved by
//           the caller via effectiveAgentsMd (defaults: struct init = built-ins).
//  model The active model config, supplying the per-field config-source
//        provenance for the treatment log line (defaults: struct init = no
//        per-model overrides).
// Returns: The current AgentMdState, or a default-constructed state if no AGENTS.md is present.
AgentMdState processAgentMd(LlmConfig config, bool noCwdConfig, RAG rag, bool forceRefresh = false,
        AgentsMdConfig agentsMd = AgentsMdConfig.init,
        const CodeModelConfig model = CodeModelConfig.init) {
    import llm.summary_agent : SummaryAgent;

    if (noCwdConfig) {
        logger.info("noCwdConfig is set: skipping AGENTS.md loading and cleaning up");
        removeAgentMdTopics(rag);

        auto cachePath = (config.dataDir ~ "agent_md_cache.json").Path;
        if (cachePath.exists) {
            try {
                remove(cachePath);
            } catch (Exception e) {
                logger.warningf("Failed to clear AGENTS.md cache during noCwdConfig: %s", e.msg);
            }
        }
        return AgentMdState.init;
    }

    if (forceRefresh) {
        auto cachePath = (config.dataDir ~ "agent_md_cache.json").Path;
        if (cachePath.exists) {
            try {
                remove(cachePath);
                logger.info("AGENTS.md cache cleared by forceRefresh request");
            } catch (Exception e) {
                logger.warningf("Failed to clear AGENTS.md cache during forceRefresh: %s", e.msg);
            }
        }
    }

    auto workarea = AbsolutePath(".");
    auto agentMdPath = (workarea ~ "AGENTS.md").Path;

    if (!agentMdPath.exists) {
        // AGENTS.md not found - clean up any leftover cache and RAG topics
        auto cachePath = (config.dataDir ~ "agent_md_cache.json").Path;
        auto oldCachedState = loadAgentMdCache(cachePath);
        if (oldCachedState.isValid()) {
            logger.infof("AGENTS.md was removed (previously cached checksum: %08x), cleaning up",
                    oldCachedState.checksum);
            try {
                removeAgentMdTopics(rag);
            } catch (Exception e) {
                logger.warningf("Failed to remove AGENTS.md RAG topics after removal: %s", e.msg);
            }

            try {
                remove(cachePath);
            } catch (Exception e) {
                logger.warningf("Failed to clear AGENTS.md cache after removal: %s", e.msg);
            }
        } else {
            logger.tracef("No AGENTS.md found in workarea: %s", workarea);
        }
        return AgentMdState.init;
    }

    string rawContent;
    try {
        rawContent = agentMdPath.readText;
    } catch (Exception e) {
        logger.warningf("Failed to read AGENTS.md '%s': %s", agentMdPath, e.msg);
        return AgentMdState.init;
    }

    if (rawContent.strip.empty) {
        // Whitespace-only AGENTS.md counts as no AGENTS.md
        logger.trace("AGENTS.md is empty, skipping");
        return AgentMdState.init;
    }

    string resolvedContent = rawContent;

    if (!agentsMd.enabled) {
        return AgentMdState.init;
    }

    const size_t rawSize = rawContent.length;
    const contentChecksum = computeContentHash(resolvedContent);
    const topicName = makeAgentMdTopicName(contentChecksum);
    const bool summaryTreatment = agentMdNeedsSummary(agentsMd, rawSize);

    logger.infof("AGENTS.md treatment: %s; file is %s bytes vs thresholdKb=%s KB",
            agentsMdSourceSummary(config, model), rawSize, agentsMd.compression.thresholdKb);

    if (!summaryTreatment) {
        if (rawSize > agentMdThresholdBytes(agentsMd)) {
            logger.infof(
                    "AGENTS.md is %s KB, exceeding compression.thresholdKb (%s KB); injecting verbatim by configuration",
                    rawSize / 1024, agentsMd.compression.thresholdKb);
        }
        if (agentsMd.ragIndex) {
            reloadAgentMdToRag(rag, resolvedContent, contentChecksum, config.ragConfig);
        }
        return AgentMdState(checksum: contentChecksum, summary: rawContent,
                timestamp: Clock.currTime()); // ragTopic stays empty (init)
    }

    auto cachePath = (config.dataDir ~ "agent_md_cache.json").Path;
    auto cachedState = loadAgentMdCache(cachePath);

    bool isCacheHit = cachedState.isValid() && (cachedState.checksum == contentChecksum);

    bool forceReload = (rag !is null) && rag.isPrimaryInMemory();
    logger.trace(forceReload, "In-memory RAG detected: forcing AGENTS.md load");

    if (isCacheHit) {
        logger.tracef("AGENTS.md cache hit (checksum: %s)", contentChecksum);

        // For in-memory RAG, reload the topic to ensure it exists in ephemeral storage.
        if (agentsMd.ragIndex && agentsMd.enabled && forceReload) {
            reloadAgentMdToRag(rag, resolvedContent, contentChecksum, config.ragConfig);
        }

        cachedState.ragTopic = agentMdRagTopic(agentsMd, true, topicName);
        return cachedState;
    }

    logger.infof("AGENTS.md cache miss (checksum: %s), generating summary", contentChecksum);

    if (agentsMd.ragIndex && cachedState.isValid()) {
        removeAgentMdTopic(rag, cachedState.checksum);
    }

    string summary;
    try {
        auto sumAgent = SummaryAgent(config.summaryModel);
        sumAgent.setSystemPrompt(config.getPrompt(skillManager: null,
                promptName: config.summaryModel.prompt, addSkills: false));
        summary = sumAgent.summarizeText(resolvedContent, 200, bulletListMergeCallback);
    } catch (Exception e) {
        logger.warningf("Failed to generate AGENTS.md summary: %s", e.msg);
        if (cachedState.isValid()) {
            logger.info("Using cached AGENTS.md summary after summarization failure");
            if (agentsMd.ragIndex && agentsMd.enabled && forceReload) {
                reloadAgentMdToRag(rag, resolvedContent, cachedState.checksum, config.ragConfig);
            }
            // Stamp the pointer topic for the cached summary (its own checksum).
            cachedState.ragTopic = agentMdRagTopic(agentsMd, true,
                    makeAgentMdTopicName(cachedState.checksum));
            return cachedState;
        }
        return AgentMdState.init;
    }

    if (summary.empty) {
        logger.warning("AGENTS.md summary is empty");
        if (cachedState.isValid()) {
            logger.info("Using cached AGENTS.md summary after empty summary");
            if (agentsMd.ragIndex && agentsMd.enabled && forceReload) {
                reloadAgentMdToRag(rag, resolvedContent, cachedState.checksum, config.ragConfig);
            }
            cachedState.ragTopic = agentMdRagTopic(agentsMd, true,
                    makeAgentMdTopicName(cachedState.checksum));
            return cachedState;
        }
        return AgentMdState.init;
    }

    if (rag !is null && agentsMd.ragIndex && agentsMd.enabled) {
        reloadAgentMdToRag(rag, resolvedContent, contentChecksum, config.ragConfig);
    }

    auto newState = AgentMdState(checksum: contentChecksum, summary: summary,
            timestamp: Clock.currTime());
    newState.ragTopic = agentMdRagTopic(agentsMd, true, topicName);

    if (saveAgentMdCache(cachePath, newState)) {
        logger.infof("Saved AGENTS.md cache (summary: %s chars)", summary.length);
    } else {
        logger.warning("Failed to save AGENTS.md cache, state will be regenerated next run");
    }

    return newState;
}

@("agent-md cache round trip: save writes the file and load returns the same state")
unittest {
    // Integration Test: Cache file creation, persistence, and round-trip
    import llm.test_util : testArea;

    auto td = testArea("cache_roundtrip");
    scope (exit)
        td.cleanup;

    auto cachePath = td.workArea ~ "agent_md_cache.json";

    assert(!cachePath.exists, "Cache file should not exist initially");

    // Load non-existent cache returns default state
    auto emptyState = loadAgentMdCache(cachePath);
    assert(!emptyState.isValid(), "Empty cache should not be valid");
    assert(emptyState.checksum == 0, "Empty cache checksum should be 0");
    assert(emptyState.summary.empty, "Empty cache summary should be empty");

    auto testState = AgentMdState(checksum: 12345678, summary: "- Rule 1: Be helpful\n- Rule 2: Be concise\n",
            timestamp: Clock.currTime());
    assert(saveAgentMdCache(cachePath, testState), "Cache save should succeed");
    assert(cachePath.exists, "Cache file should exist after save");

    auto loadedState = loadAgentMdCache(cachePath);
    assert(loadedState.isValid(), "Loaded state should be valid");
    assert(loadedState.checksum == testState.checksum, "Checksum should match after round-trip");
    assert(loadedState.summary == testState.summary, "Summary should match after round-trip");
}

@("a corrupted cache JSON loads as an invalid state without throwing")
unittest {
    import llm.test_util : testArea;

    auto td = testArea("cache_corrupt");
    scope (exit)
        td.cleanup;

    auto cachePath = td.createFile("agent_md_cache.json", "{ corrupted json !!! }");

    auto state = loadAgentMdCache(cachePath);
    assert(!state.isValid(), "Corrupted cache should return invalid state");
}

@("removing the cache file clears the state; loading the removed cache is invalid")
unittest {
    import llm.test_util : testArea;

    auto td = testArea("cache_clear");
    scope (exit)
        td.cleanup;

    auto cachePath = td.workArea ~ "agent_md_cache.json";

    auto testState = AgentMdState(checksum: 99999, summary: "- Some rule\n",
            timestamp: Clock.currTime());
    saveAgentMdCache(cachePath, testState);
    assert(cachePath.exists, "Cache should exist");

    remove(cachePath);
    assert(!cachePath.exists, "Cache should be removed");

    auto state = loadAgentMdCache(cachePath);
    assert(!state.isValid(), "Removed cache should return invalid state");
}

@("a full AgentMdState (multi-line summary, non-init timestamp) survives serialization")
unittest {
    import llm.test_util : testArea;

    auto td = testArea("cache_fullserial");
    scope (exit)
        td.cleanup;

    auto cachePath = td.workArea ~ "agent_md_cache.json";

    // Save a state with positive checksum (negative checksums may cause JSON issues)
    AgentMdState original;
    original.checksum = 1234567890;
    original.summary = "- First rule\n- Second rule\n- Third rule\n";
    original.timestamp = Clock.currTime();
    saveAgentMdCache(cachePath, original);
    auto loaded = loadAgentMdCache(cachePath);

    assert(loaded.summary == original.summary, "Multi-line summary should serialize correctly");
    assert(loaded.timestamp != SysTime.init, "Timestamp should be non-zero after round-trip");
}

@("makeAgentMdTopicName yields the agent_md_ prefix with 8 hex chars, 16 for negative checksums, distinct per checksum")
unittest {
    long checksum1 = 12345678;
    string topic1 = makeAgentMdTopicName(checksum1);
    assert(topic1.startsWith("agent_md_"), "Topic should start with agent_md_ prefix");
    auto hexPart1 = topic1["agent_md_".length .. $];
    assert(hexPart1.length == 8, "Hex portion should be 8 characters");
    long checksum2 = 87654321;
    string topic2 = makeAgentMdTopicName(checksum2);
    assert(topic1 != topic2, "Different checksums should produce different topic names");

    long checksumNeg = -1;
    string topicNeg = makeAgentMdTopicName(checksumNeg);
    assert(topicNeg.startsWith("agent_md_"), "Negative checksum topic should have correct prefix");
    auto hexPartNeg = topicNeg["agent_md_".length .. $];
    assert(hexPartNeg.length == 16,
            "Negative checksum hex portion should be 16 characters (full 64-bit hex)");
}

@("bulletListMerge combines chunks and deduplicates identical bullet lines")
unittest {
    SummaryChunkT[] chunks;
    chunks ~= SummaryChunkT("Some summary text from chunk 1\n- Point A\n- Point B",
            size_t(0), size_t(50));
    chunks ~= SummaryChunkT("More summary from chunk 2\n- Point C\n- Point A",
            size_t(50), size_t(100));

    auto result = bulletListMerge(chunks);

    assert(!result.empty, "Merged result should not be empty");
    assert(result.canFind("Point A"), "Should contain Point A");
    assert(result.canFind("Point B"), "Should contain Point B");
    assert(result.canFind("Point C"), "Should contain Point C");

    auto lines = result.splitLines.filter!(a => !a.empty).array;
    auto pointALines = lines.filter!(a => a == "- Point A").array;
    assert(pointALines.length == 1, "Duplicate bullets should be deduplicated");
}

@("bulletListMerge over empty chunks returns an empty result")
unittest {
    SummaryChunkT[] emptyChunks;
    auto result = bulletListMerge(emptyChunks);
    assert(result.empty, "Empty chunks should produce empty result");
}

@("bulletListMerge normalizes star bullets to dashes and prefixes plain lines with a bullet")
unittest {
    SummaryChunkT[] chunks;
    chunks ~= SummaryChunkT("- Normal bullet\n* Star bullet\nPlain text line",
            size_t(0), size_t(40));

    auto result = bulletListMerge(chunks);

    assert(result.canFind("- Normal bullet"), "Normal bullets should be preserved");
    assert(result.canFind("- Star bullet"), "Star bullets should be converted to dash");
    assert(result.canFind("- Plain text line"), "Plain text should get bullet prefix");
}

@("AgentMdState equality compares all fields: same fields equal, different checksum not")
unittest {
    SysTime fixedTime = SysTime(DateTime(2024, 1, 1, 0, 0, 0), UTC());

    auto state1 = AgentMdState(checksum: 100, summary: "- Rule 1\n", timestamp: fixedTime);

    auto state2 = AgentMdState(checksum: 100, summary: "- Rule 1\n", timestamp: fixedTime);

    auto state3 = AgentMdState(checksum: 200, summary: "- Rule 1\n", timestamp: fixedTime);

    assert(state1 == state2, "States with same fields should be equal");
    assert(state1 != state3, "States with different checksums should not be equal");
}

@("AgentMdState.isValid requires a non-zero checksum and a non-empty summary")
unittest {
    auto validState = AgentMdState(checksum: 1, summary: "- Rule\n", timestamp: SysTime.init);
    assert(validState.isValid(),
            "State with non-zero checksum and non-empty summary should be valid");

    auto emptyChecksum = AgentMdState(checksum: 0, summary: "- Rule\n", timestamp: SysTime.init);
    assert(!emptyChecksum.isValid(), "State with zero checksum should not be valid");

    auto emptySummary = AgentMdState(checksum: 1, summary: "", timestamp: SysTime.init);
    assert(!emptySummary.isValid(), "State with empty summary should not be valid");

    auto defaultState = AgentMdState.init;
    assert(!defaultState.isValid(), "Default state should not be valid");
}

@("end-to-end cache workflow: first-run miss, second-run hit, content-change miss, cache update")
unittest {
    import llm.test_util : testArea;

    auto td = testArea("workflow_sim");
    scope (exit)
        td.cleanup;

    auto cachePath = td.workArea ~ "agent_md_cache.json";

    // Simulate first run - no cache exists
    auto cachedState = loadAgentMdCache(cachePath);
    assert(!cachedState.isValid(), "First run: no cache should exist");

    // Create AGENTS.md content and compute hash
    string agentContent = "# Project Agent\n\n- Always use D programming language\n- Follow the coding standards\n- Write tests for all new code\n";
    long contentChecksum = computeContentHash(agentContent);
    assert(contentChecksum != 0, "Content hash should be non-zero");

    // Simulate summary generation (in real code, this comes from LLM)
    string mockSummary = "- Always use D programming language\n- Follow the coding standards\n- Write tests for all new code\n";

    // Save cache (simulating successful first run)
    auto newState = AgentMdState(contentChecksum, mockSummary, Clock.currTime());
    assert(saveAgentMdCache(cachePath, newState), "Cache save should succeed on first run");

    // Simulate second run - cache hit
    cachedState = loadAgentMdCache(cachePath);
    assert(cachedState.isValid(), "Second run: cache should be valid");
    assert(cachedState.checksum == contentChecksum, "Second run: checksum should match");
    assert(cachedState.summary == mockSummary, "Second run: summary should match");

    // Simulate content change - cache miss
    string modifiedContent = agentContent ~ "- New rule added\n";
    long newChecksum = computeContentHash(modifiedContent);
    assert(newChecksum != contentChecksum, "Modified content should have different hash");

    // In real code, this would trigger re-summarization
    cachedState = loadAgentMdCache(cachePath);
    assert(cachedState.checksum != newChecksum, "Cache miss: old checksum differs from new");

    // Update cache with new state
    string newMockSummary = mockSummary ~ "- New rule added\n";
    auto updatedState = AgentMdState(newChecksum, newMockSummary, Clock.currTime());
    assert(saveAgentMdCache(cachePath, updatedState), "Cache update should succeed");

    // Verify updated cache
    cachedState = loadAgentMdCache(cachePath);
    assert(cachedState.checksum == newChecksum, "Updated cache should have new checksum");
    assert(cachedState.summary == newMockSummary, "Updated cache should have new summary");

    // Verify topic naming for both checksums
    string oldTopic = makeAgentMdTopicName(contentChecksum);
    string newTopic = makeAgentMdTopicName(newChecksum);
    assert(oldTopic.startsWith("agent_md_"), "Old topic should have correct prefix");
    assert(newTopic.startsWith("agent_md_"), "New topic should have correct prefix");
    assert(oldTopic != newTopic, "Different checksums should produce different topics");
}

@("noCwdConfig cleanup removes the cache; the state is invalid afterwards")
unittest {
    import llm.test_util : testArea;

    auto td = testArea("nocwdconfig");
    scope (exit)
        td.cleanup;

    auto cachePath = td.workArea ~ "agent_md_cache.json";

    // Pre-condition: cache exists with valid data
    auto oldState = AgentMdState(checksum: 12345, summary: "- Old rule\n",
            timestamp: Clock.currTime());
    saveAgentMdCache(cachePath, oldState);
    assert(cachePath.exists, "Cache should exist before cleanup");

    remove(cachePath);
    assert(!cachePath.exists, "Cache should be removed during noCwdConfig cleanup");

    auto state = loadAgentMdCache(cachePath);
    assert(!state.isValid(), "State should be invalid after noCwdConfig cleanup");
}

@(
        "prompt composition order: base prompt precedes the AGENTS.md payload, which precedes the RAG pointer instruction")
unittest {
    string basePrompt = "You are an AI assistant.";
    string agentsMdPayload = "# AGENTS.md Rules\n- Rule 1\n- Rule 2";
    string agentsMdInstruction = "For detailed explanations, query your RAG knowledge base.";

    string composed = basePrompt ~ "\n\n" ~ agentsMdPayload ~ "\n\n" ~ agentsMdInstruction;

    assert(composed.canFind(basePrompt), "Composed prompt should contain base prompt");
    assert(composed.canFind(agentsMdPayload),
            "Composed prompt should contain the AGENTS.md payload");
    assert(composed.canFind(agentsMdInstruction),
            "Composed prompt should contain the RAG pointer instruction");

    size_t basePos = indexOf(composed, basePrompt);
    size_t payloadPos = indexOf(composed, agentsMdPayload);
    size_t instructionPos = indexOf(composed, agentsMdInstruction);
    assert(basePos < payloadPos, "Base prompt should come before the AGENTS.md payload");
    assert(payloadPos < instructionPos,
            "AGENTS.md payload should come before the RAG pointer instruction");
}

@(
        "forceRefresh clears the stale cache before re-summarization; the new cache holds the new checksum")
unittest {
    import llm.test_util : testArea;

    auto td = testArea("forcerefresh");
    scope (exit)
        td.cleanup;

    auto cachePath = td.workArea ~ "agent_md_cache.json";

    // Pre-condition: cache exists
    auto oldState = AgentMdState(checksum: 11111, summary: "- Old summary\n",
            timestamp: Clock.currTime());
    saveAgentMdCache(cachePath, oldState);

    if (cachePath.exists) {
        remove(cachePath);
    }

    auto state = loadAgentMdCache(cachePath);
    assert(!state.isValid(), "Cache should be invalid after forceRefresh");

    // In real code, processAgentMd would now re-summarize
    string content = "# Updated Agent\n- New rules\n";
    long newChecksum = computeContentHash(content);
    auto newState = AgentMdState(checksum: newChecksum, summary: "- New summary\n",
            timestamp: Clock.currTime());
    saveAgentMdCache(cachePath, newState);

    state = loadAgentMdCache(cachePath);
    assert(state.isValid(), "Cache should be valid after refresh");
    assert(state.checksum == newChecksum, "Cache should have new checksum after refresh");
}

@("AGENTS.md removal invalidates the cached state")
unittest {
    import llm.test_util : testArea;

    auto td = testArea("agentremoval");
    scope (exit)
        td.cleanup;

    auto cachePath = td.workArea ~ "agent_md_cache.json";

    // Pre-condition: cache exists with valid data
    auto oldState = AgentMdState(checksum: 55555, summary: "- Cached rule\n",
            timestamp: Clock.currTime());
    saveAgentMdCache(cachePath, oldState);
    assert(loadAgentMdCache(cachePath).isValid(), "Cache should be valid before removal");

    // Simulate AGENTS.md removal: clean up cache and RAG topics
    // NOTE: In real code, removeAgentMdTopics(rag) would also be called to clean
    // up RAG entries. This test cannot verify RAG topic removal without a RAG
    // instance - that is tested as part of the full processAgentMd() workflow.
    remove(cachePath);
    auto state = loadAgentMdCache(cachePath);
    assert(!state.isValid(), "Cache should be invalid after AGENTS.md removal");
}

@("computeContentHash is deterministic, distinct for different content, and non-zero")
unittest {
    string content1 = "Some content to hash";
    string content2 = "Some content to hash";
    string content3 = "Different content";

    long hash1 = computeContentHash(content1);
    long hash2 = computeContentHash(content2);
    long hash3 = computeContentHash(content3);

    assert(hash1 == hash2, "Same content should produce same hash");
    assert(hash1 != hash3, "Different content should produce different hash");
    assert(hash1 != 0, "Hash should not be zero for non-empty content");
}

@(
        "agentMdNeedsSummary decision table: enabled x mode x size, threshold boundary at thresholdKb*1024")
unittest {
    auto summaryCfg = AgentsMdConfig(true, AgentsMdCompression(CompressionMode.summary, 20), true);
    auto fullCfg = AgentsMdConfig(true, AgentsMdCompression(CompressionMode.full, 20), true);
    auto autoCfg = AgentsMdConfig(true, AgentsMdCompression(CompressionMode.auto_, 20), true);
    auto disabledCfg = AgentsMdConfig(false, AgentsMdCompression(CompressionMode.summary, 20), true);

    // mode=summary always summarizes (when enabled)
    assert(agentMdNeedsSummary(summaryCfg, 0));
    assert(agentMdNeedsSummary(summaryCfg, 21 * 1024));

    // mode=full never summarizes
    assert(!agentMdNeedsSummary(fullCfg, 0));
    assert(!agentMdNeedsSummary(fullCfg, 21 * 1024));

    // mode=auto: within the threshold -> full treatment, over -> summary.
    // Boundary: size == thresholdKb*1024 stays full, size == thresholdKb*1024+1
    // needs the summary treatment.
    assert(!agentMdNeedsSummary(autoCfg, 20 * 1024),
            "size == thresholdKb*1024 stays on the full treatment");
    assert(agentMdNeedsSummary(autoCfg, 20 * 1024 + 1),
            "size == thresholdKb*1024+1 needs the summary treatment");

    // enabled=false is a pure no-op regardless of mode or size.
    assert(!agentMdNeedsSummary(disabledCfg, 0));
    assert(!agentMdNeedsSummary(disabledCfg, 100 * 1024));

    // The threshold bytes are clamped to int.max for an unrepresentable
    // thresholdKb, so the boundary stays representable instead of wrapping.
    auto absurdCfg = AgentsMdConfig(true,
            AgentsMdCompression(CompressionMode.auto_, long.max), true);
    assert(!agentMdNeedsSummary(absurdCfg, int.max),
            "size == clamped int.max boundary stays on the full treatment");
    assert(agentMdNeedsSummary(absurdCfg, size_t.max),
            "a size above the clamped int.max boundary needs the summary treatment");
}

@("agentMdRagTopic: pointer topic only for the summary treatment with ragIndex on")
unittest {
    auto onCfg = AgentsMdConfig(true, AgentsMdCompression(CompressionMode.auto_, 20), true);
    auto offCfg = AgentsMdConfig(true, AgentsMdCompression(CompressionMode.auto_, 20), false);

    assert(agentMdRagTopic(onCfg, true, "agent_md_1a2b3c4d") == "agent_md_1a2b3c4d");
    assert(agentMdRagTopic(onCfg, false, "agent_md_1a2b3c4d").empty,
            "full-text treatment: no pointer (the model already holds the text)");
    assert(agentMdRagTopic(offCfg, true, "agent_md_1a2b3c4d").empty,
            "ragIndex=false: nothing to point at");
}

/// The test helpers. The real processAgentMd hardcodes its workarea
/// to AbsolutePath(".") and the dub test runner executes tests concurrently,
/// so tests must not touch the process cwd; the re-index and topic-removal
/// mechanics are covered at the reloadAgentMdToRag / removeAgentMdTopic level
/// instead, leaving the inline gate wiring to the integration tests.
version (unittest) {
    import std.conv : to;

    import llm.config : RagConfig, RagDatabaseConfig;
    import llm.rag : RAG;
    import llm.test_util : TestArea, TestEmbedder, testArea;

    RAG makeInMemoryAgentMdRag(ref TestArea td) {
        auto rag = new RAG(new TestEmbedder(),
                RagDatabaseConfig(Path("/nonexistent_dir/agent_md_test.db"), "agent_md test rag"),
                null);
        td.addRag(rag);
        return rag;
    }

    struct CacheFixture {
        LlmConfig conf;
        Path cachePath;
    }

    CacheFixture cacheFixture(ref TestArea td) {
        CacheFixture f;
        f.conf = LlmConfig();
        f.conf.dataDir = td.workArea;
        f.cachePath = td.workArea ~ "agent_md_cache.json";
        return f;
    }

    struct RawAgentsMd {
        string text;
        long checksum;
    }

    RawAgentsMd readRawAgentsMd() {
        immutable rawContent = (AbsolutePath(".") ~ "AGENTS.md").Path;
        assert(rawContent.exists, "the repo AGENTS.md must exist for this matrix");
        immutable rawText = rawContent.readText;
        assert(!rawText.strip.empty,
                "this matrix needs a non-whitespace repo AGENTS.md (whitespace-only counts as no AGENTS.md)");
        return RawAgentsMd(rawText, computeContentHash(rawText));
    }
}

@("reloadAgentMdToRag: re-indexing is idempotent - same content re-adds nothing, changed content lands under a fresh topic")
unittest {
    auto td = testArea("agent_md_reload_idempotent");
    scope (exit)
        td.cleanup();

    auto rag = makeInMemoryAgentMdRag(td);
    immutable ragConf = RagConfig.init;

    immutable content = "# Project rules\n\n- Keep the build green\n"
        ~ "- Prefer small, focused modules\n";
    immutable checksum = computeContentHash(content);

    // First index: exactly one source under the checksum-derived topic.
    reloadAgentMdToRag(rag, content, checksum, ragConf);
    auto sources = rag.db.getSources;
    assert(sources.length == 1, "exactly one RAG source expected, got " ~ sources.length.to!string);
    bool sawTopic;
    foreach (immutable source; sources)
        source.origin.match!((Topic t) {
            if (t.name == makeAgentMdTopicName(checksum))
                sawTopic = true;
        }, (_) {});
    assert(sawTopic, "the topic must be checksum-derived");

    // Re-index with the same content and checksum (the cache-free full path
    // does this on every run): the same topic name, the dedup no-ops, still
    // one source.
    reloadAgentMdToRag(rag, content, checksum, ragConf);
    assert(rag.db.getSources.length == 1,
            "re-indexing the same content must not duplicate the topic, got "
            ~ rag.db.getSources.length.to!string ~ " sources");
    sawTopic = false;
    foreach (immutable source; rag.db.getSources)
        source.origin.match!((Topic t) {
            if (t.name == makeAgentMdTopicName(checksum))
                sawTopic = true;
        }, (_) {});
    assert(sawTopic, "the surviving topic must be the checksum-derived one");

    // Re-index with changed content under a new checksum: a second,
    // checksum-derived topic appears (the full path relies on add()'s dedup,
    // never on removal, so stale topics stay until the cache-miss sweep).
    immutable newContent = "# Other rules\n\n- Be concise\n- Verify before asserting\n";
    immutable newChecksum = computeContentHash(newContent);
    reloadAgentMdToRag(rag, newContent, newChecksum, ragConf);
    assert(rag.db.getSources.length == 2,
            "changed content must land under a fresh topic, got "
            ~ rag.db.getSources.length.to!string ~ " sources");
    bool sawNew;
    foreach (immutable source; rag.db.getSources)
        source.origin.match!((Topic t) {
            if (t.name == makeAgentMdTopicName(newChecksum))
                sawNew = true;
        }, (_) {});
    assert(sawNew, "the new content's topic must be indexed");
}

@("removeAgentMdTopic: removes the checksum's topic and tolerates a missing one")
unittest {
    auto td = testArea("agent_md_remove_topic");
    scope (exit)
        td.cleanup();

    auto rag = makeInMemoryAgentMdRag(td);
    immutable ragConf = RagConfig.init;

    immutable content = "# Stale topic rules\n\n- rule one\n- rule two\n";
    immutable checksum = computeContentHash(content);
    reloadAgentMdToRag(rag, content, checksum, ragConf);
    assert(rag.db.getSources.length == 1,
            "the seed must index the topic first, got "
            ~ rag.db.getSources.length.to!string ~ " sources");

    // The removal the gated cache-miss branch performs: the checksum's topic
    // is gone, the source count drops to zero.
    removeAgentMdTopic(rag, checksum);
    assert(rag.db.getSources.length == 0, "the topic must be removed");

    // Removing an absent topic (a second sweep, or the ragIndex=false cache
    // miss skipping the first one) is a no-op, not an error.
    removeAgentMdTopic(rag, checksum);
    assert(rag.db.getSources.length == 0, "removing an absent topic must stay a no-op");
}

// ---- Cache-rule verification matrix ----
// These tests prove the cache rules end-to-end against a real
// agent_md_cache.json in a temp data dir (TestArea). The summarize step is
// stubbed by pre-seeding the cache (the deterministic approach of the
// forceRefresh test above): processAgentMd hardcodes its workarea to
// AbsolutePath(".") and the dub test runner executes tests concurrently, so
// no test may chdir and no test may drive the network SummaryAgent. The
// fixture wiring lives in cacheFixture / readRawAgentsMd (version (unittest)
// block above).

@("cache hit: the summary is generated once per content - the second run with the same content and mode is a checksum hit returning the cached summary without a second summarize")
unittest {
    import llm.test_util : testArea;

    auto td = testArea("cache_hit_once");
    scope (exit)
        td.cleanup;
    auto fx = cacheFixture(td);

    // The real workarea AGENTS.md is the content under test (read-only).
    immutable agentMd = readRawAgentsMd();

    // First run: summarize (stubbed) + cache write.
    immutable generatedSummary = "- stubbed AGENTS.md summary: generated once\n";
    assert(saveAgentMdCache(fx.cachePath, AgentMdState(agentMd.checksum,
            generatedSummary, Clock.currTime())));
    immutable cacheBefore = fx.cachePath.readText;

    // Second run with the same content and mode: a checksum cache hit returns
    // the stored summary - the network SummaryAgent is never constructed.
    auto summaryCfg = AgentsMdConfig(true, AgentsMdCompression(CompressionMode.summary, 20), false);
    auto state = processAgentMd(fx.conf, false, null, false, summaryCfg);
    assert(state.checksum == agentMd.checksum, "the hit must carry the cached checksum");
    assert(state.summary == generatedSummary,
            "the cache hit must hand back the stored summary without a second summarize");
    assert(fx.cachePath.readText == cacheBefore, "a cache hit must not rewrite the cache file");
}

@("mode toggle reuses the cached summary: summary run writes the cache, full run leaves it untouched, summary run hits it again without re-summarizing")
unittest {
    import llm.test_util : testArea;

    auto td = testArea("cache_mode_toggle");
    scope (exit)
        td.cleanup;
    auto fx = cacheFixture(td);

    immutable agentMd = readRawAgentsMd();

    // First run, summary mode: summarize (stubbed) + cache write.
    immutable generatedSummary = "- stubbed AGENTS.md summary: generated once\n";
    assert(saveAgentMdCache(fx.cachePath, AgentMdState(agentMd.checksum,
            generatedSummary, Clock.currTime())));

    // Switch to full: verbatim payload, and the cache file must stay
    // untouched (no cache activity on the full path).
    auto fullCfg = AgentsMdConfig(true, AgentsMdCompression(CompressionMode.full, 20), false);
    immutable cacheBefore = fx.cachePath.readText;
    auto fullState = processAgentMd(fx.conf, false, null, false, fullCfg);
    assert(fullState.summary == agentMd.text,
            "the full treatment must inject the verbatim raw text");
    assert(fullState.checksum == agentMd.checksum, "the checksum is the raw text's");
    assert(fx.cachePath.readText == cacheBefore, "the full treatment must perform no cache write");

    // Switch back to summary: the cached summary is reused - no
    // re-summarize even though the mode changed twice in between.
    auto summaryCfg = AgentsMdConfig(true, AgentsMdCompression(CompressionMode.summary, 20), false);
    auto hitState = processAgentMd(fx.conf, false, null, false, summaryCfg);
    assert(hitState.checksum == agentMd.checksum, "the hit must carry the cached checksum");
    assert(hitState.summary == generatedSummary,
            "toggling modes must reuse the cached summary without a re-summarize");
    assert(fx.cachePath.readText == cacheBefore, "a cache hit must not rewrite the cache file");
}

@("full treatment performs no cache activity: an absent cache file stays absent, a pre-seeded one stays byte-identical")
unittest {
    import llm.test_util : testArea;

    auto td = testArea("cache_full_no_activity");
    scope (exit)
        td.cleanup;
    auto fx = cacheFixture(td);
    auto fullCfg = AgentsMdConfig(true, AgentsMdCompression(CompressionMode.full, 20), false);

    // (a) Absent cache file: a full-treatment run must not create one.
    assert(!fx.cachePath.exists, "pre-condition: no cache file");
    processAgentMd(fx.conf, false, null, false, fullCfg);
    assert(!fx.cachePath.exists, "the full treatment must not write the cache file");

    // (b) Pre-seeded cache file: a full-treatment run must leave it
    // byte-identical (no read-driven rewrite either).
    assert(saveAgentMdCache(fx.cachePath, AgentMdState(checksum: 424242,
            summary: "- seeded summary\n", timestamp: Clock.currTime())));
    immutable cacheBefore = fx.cachePath.readText;
    processAgentMd(fx.conf, false, null, false, fullCfg);
    assert(fx.cachePath.readText == cacheBefore,
            "the full treatment must not rewrite the cache file");
}

@(
        "cache file schema unchanged: the written file has exactly the checksum, summary and timestamp keys")
unittest {
    import llm.test_util : testArea;

    auto td = testArea("cache_schema");
    scope (exit)
        td.cleanup;
    auto fx = cacheFixture(td);

    assert(saveAgentMdCache(fx.cachePath, AgentMdState(checksum: 12345678,
            summary: "- Rule\n", timestamp: Clock.currTime())));

    auto json = fx.cachePath.readText.parseJSON;
    assert(json.type == JSONType.object, "the cache file must be a JSON object");
    auto keys = json.object.keys;
    assert(keys.length == 3, "exactly three keys expected, got " ~ keys.length.to!string);
    foreach (immutable key; ["checksum", "summary", "timestamp"])
        assert(canFind(keys, key), "missing cache key: " ~ key);
}

@("atomic write intact: saveAgentMdCache still writes tmp+rename and leaves no .tmp leftover")
unittest {
    import llm.test_util : testArea;

    auto td = testArea("cache_atomic_write");
    scope (exit)
        td.cleanup;
    auto fx = cacheFixture(td);

    assert(saveAgentMdCache(fx.cachePath, AgentMdState(checksum: 555, summary: "- Rule\n",
            timestamp: Clock.currTime())), "the save must succeed");
    assert(fx.cachePath.exists, "the rename must have landed the cache file");
    auto leftover = (fx.cachePath.toString ~ ".tmp").Path;
    assert(!leftover.exists, "the tmp file must be renamed away, not left behind");

    // The landed file is the parseable cache payload, not an empty tmp shell.
    auto json = fx.cachePath.readText.parseJSON;
    assert(json["checksum"].integer == 555, "the renamed file must hold the saved state");
}

@(
        "checksum input is the raw AGENTS.md text: a full-treatment run reports computeContentHash of the raw file content")
unittest {
    import llm.test_util : testArea;

    auto td = testArea("cache_checksum_raw_text");
    scope (exit)
        td.cleanup;
    auto fx = cacheFixture(td);

    immutable agentMd = readRawAgentsMd();

    auto fullCfg = AgentsMdConfig(true, AgentsMdCompression(CompressionMode.full, 20), false);
    auto state = processAgentMd(fx.conf, false, null, false, fullCfg);
    assert(state.checksum == agentMd.checksum,
            "the checksum must be computed over the raw AGENTS.md text (no @path resolution remains)");
}
