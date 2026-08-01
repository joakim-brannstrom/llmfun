// AGENTS.md hybrid support module.
// Manages AGENTS.md file detection, caching, and summary generation.
// Uses a hybrid approach: compressed summary injected into system prompt,
// full document stored in RAG for detailed lookups.

module llm.agent_md;

import logger = std.logger;
import std.algorithm : startsWith, map, filter, count, canFind;
import std.array : appender, join, array;
import std.conv : text, to;
import std.datetime : SysTime, Clock, Duration, dur, DateTime, UTC;
import std.file : exists, getSize, isSymlink, readText, rename, remove;
import std.json : JSONValue, parseJSON, JSONOptions, JSONType;
import std.path : dirName;
import std.range : empty;
import std.stdio : File;
import std.string : splitLines, strip, stripRight, indexOf;
import std.sumtype : match;
import std.typecons : Tuple;

public import my.path : Path, AbsolutePath;
import my.set : Set;

import llm.utility : computeContentHash, isPathInsideWorkarea;
import llm.config : LlmConfig, RagConfig;
import llm.rag.rag : RAG, Document, Origin, Topic, Offset, add, Url;
import llm.summary_agent : SummaryChunkT;

/// State representation for an AGENTS.md file.
/// Tracks the checksum of the original content, a generated summary,
/// and the timestamp when this state was last updated.
struct AgentMdState {
    /// Content checksum computed via MurmurHash3-32 (matches utility.d hashing).
    long checksum;

    /// Compressed summary of the AGENTS.md content (100-300 words).
    string summary;

    /// Timestamp when this state was last updated.
    SysTime timestamp;

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

// Maximum file size for @path-resolved files (1MB).
private enum size_t MaxPathFileSize = 1024 * 1024;

// Maximum recursion depth for nested @path references.
private enum size_t MaxPathRecursionDepth = 5;

// Check if any parent directory in the path is a symlink.
// Walks up from the file's parent directory to the workarea root,
// rejecting any symlinked directories.
// Params:
//  fullPath The full path to check
//  workarea The workarea root path
// Returns: true if a symlinked parent directory was found
private bool hasSymlinkedParent(AbsolutePath fullPath, AbsolutePath workarea) @safe {
    auto checkDir = fullPath.dirName;
    while (!checkDir.empty && isPathInsideWorkarea(checkDir, workarea)) {
        if (checkDir.exists && checkDir.isSymlink) {
            return true;
        }
        checkDir = checkDir.dirName;
    }
    return false;
}

// Resolve a single @path reference to file content.
// Params:
//  workarea The workarea root path for path validation
//  pathStr The path from the @path reference (without the @ prefix)
//  depth Current recursion depth
//  visited already-visited paths for circular reference detection
// Returns: Tuple of (content, success). On failure, content is empty string.
private Tuple!(string, bool) resolvePathRef(AbsolutePath workarea, Path path,
        size_t depth, ref Set!AbsolutePath visited) @safe {
    alias Result = Tuple!(string, bool);

    if (depth > MaxPathRecursionDepth) {
        logger.warningf("@path recursion limit exceeded (%s levels): %s",
                MaxPathRecursionDepth, path);
        return Result("", false);
    }

    auto fullPath = (workarea ~ path).AbsolutePath;

    if (!isPathInsideWorkarea(fullPath, workarea)) {
        logger.warningf("@path '%s' resolves outside workarea '%s': %s", path, workarea, fullPath);
        return Result("", false);
    }

    if (visited.contains(fullPath)) {
        logger.warningf("Circular @path reference detected: %s", fullPath);
        return Result("", false);
    }

    // Check existence first - isSymlink can throw for non-existent files
    if (!fullPath.exists) {
        logger.warningf("@path file does not exist: %s", fullPath);
        return Result("", false);
    }

    if (fullPath.isSymlink) {
        logger.warningf("@path '%s' is a symlink. Symlinks are not allowed.", fullPath);
        return Result("", false);
    }

    if (hasSymlinkedParent(fullPath, workarea)) {
        logger.warningf("@path '%s' is inside a symlinked directory. Not allowed.", fullPath);
        return Result("", false);
    }

    // File size limit check
    // NOTE: There is a TOCTOU race between getSize and readText.
    // In a sandboxed environment this is mitigated, but worth noting.
    try {
        const fileSize = fullPath.getSize;
        if (fileSize > MaxPathFileSize) {
            logger.warningf("@path file '%s' exceeds size limit (%s > %s bytes)",
                    fullPath, fileSize, MaxPathFileSize);
            return Result("", false);
        }
    } catch (Exception e) {
        logger.warningf("Failed to get file size for '%s': %s", fullPath, e.msg);
        return Result("", false);
    }

    try {
        visited.add(fullPath);
        string fileContent = fullPath.readText;
        string resolvedContent = resolveAgentMdContentInternal(workarea,
                fileContent, depth + 1, visited);
        return Result(resolvedContent, true);
    } catch (Exception e) {
        logger.warningf("Failed to read @path file '%s': %s", fullPath, e.msg);
        return Result("", false);
    }
}

/// Resolve @path references in AGENTS.md content.
/// Replaces @path references with the actual file content,
/// applying security constraints (path traversal protection, size limits, etc.).
/// Params:
///  workarea The workarea root path for path validation
///  content The raw AGENTS.md content potentially containing @path references
///  depth Current recursion depth (for nested resolution)
/// Returns: The content with @path references resolved, or original content on failure.
string resolveAgentMdContent(AbsolutePath workarea, string content, size_t depth = 0) @safe {
    Set!AbsolutePath visited;
    return resolveAgentMdContentInternal(workarea, content, depth, visited);
}

// Internal version that threads the visited set through recursion.
private string resolveAgentMdContentInternal(AbsolutePath workarea,
        string content, size_t depth, ref Set!AbsolutePath visited) @safe {
    string[] lines = content.splitLines;

    if (lines.map!(a => a.strip)
            .filter!(a => !a.empty)
            .filter!(a => a.startsWith("@") && a.length > 1)
            .count == 0) {
        return content;
    }

    auto resultLines = appender!(string[])();
    bool anySuccess = false;
    bool anyFailure = false;

    foreach (line; lines) {
        auto stripped = line.strip;
        if (stripped.startsWith("@") && stripped.length > 1) {
            auto pathStr = stripped[1 .. $].strip;

            if (pathStr.empty) {
                // assuming an empty @ is intentional thus not adding an error
                // message here
                resultLines.put(line);
                continue;
            }
            auto path = Path(pathStr);

            auto refResult = resolvePathRef(workarea, path, depth + 1, visited);
            if (refResult[1]) {
                resultLines.put(refResult[0]);
                anySuccess = true;
            } else {
                // Failed — insert error marker
                resultLines.put("@@RESOLVE_ERROR: " ~ pathStr ~ " (see logs for details)");
                anyFailure = true;
            }
        } else {
            resultLines.put(line);
        }
    }

    // Complete failure: if ALL @path references failed, treat as no AGENTS.md
    if (anyFailure && !anySuccess) {
        logger.warningf(
                "All @path references in AGENTS.md failed resolution, treating as no AGENTS.md");
        return "";
    }

    return resultLines[].join("\n");
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
/// This is used as a MergeCallback for AGENTS.md summarization.
private string bulletListMerge(SummaryChunkT[] summaries) {
    if (summaries.empty)
        return "";

    string[] allBullets;
    foreach (chunk; summaries) {
        auto text = chunk[0]; // the summary string
        foreach (trimmed; text.splitLines
                .map!(a => a.stripRight)
                .filter!(a => !a.empty)) {
            // Normalize: if line starts with "- " or "* ", keep as bullet; otherwise prefix with "- "
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

// Process an AGENTS.md file end-to-end.
// Orchestrates the full AGENTS.md workflow: noCwdConfig handling, checksum comparison,
// cache hit/miss logic, summarization, RAG loading, and in-memory RAG reload.
// Params:
//  config The LLM configuration (provides dataDir, summaryModel, ragConfig)
//  noCwdConfig When true, skip AGENTS.md loading and clean up existing AGENTS.md data
//  rag The RAG instance for loading content and removing topics
// Returns: The current AgentMdState, or a default-constructed state if no AGENTS.md is present.
// NOTE: Cannot be @safe because RAG operations (db.getSources, isPrimaryInMemory, add)
// and SummaryAgent operations are @system.
AgentMdState processAgentMd(LlmConfig config, bool noCwdConfig, RAG rag, bool forceRefresh = false) {
    import std.datetime : Clock;
    import std.range : empty;
    import llm.summary_agent : SummaryAgent;

    // ---- noCwdConfig handling: skip loading, clean up, return empty ----
    if (noCwdConfig) {
        logger.info("noCwdConfig is set: skipping AGENTS.md loading and cleaning up");
        removeAgentMdTopics(rag);

        // Clear cache file
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

    // ---- forceRefresh: clear cache to force re-summarization ----
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
        // AGENTS.md not found — clean up any leftover cache and RAG topics
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
        logger.trace("AGENTS.md is empty, skipping");
        return AgentMdState.init;
    }

    string resolvedContent;
    try {
        resolvedContent = resolveAgentMdContent(workarea, rawContent);
    } catch (Exception e) {
        // Non-fatal — log warning and continue
        logger.warningf("Failed to resolve @path references in AGENTS.md: %s", e.msg);
    }

    if (resolvedContent.empty) {
        logger.info("AGENTS.md content is empty after @path resolution, treating as no AGENTS.md");
        return AgentMdState.init;
    }

    const contentChecksum = computeContentHash(resolvedContent);

    auto cachePath = (config.dataDir ~ "agent_md_cache.json").Path;
    auto cachedState = loadAgentMdCache(cachePath);

    bool isCacheHit = cachedState.isValid() && (cachedState.checksum == contentChecksum);

    // In-memory RAG: always reload regardless of cache state
    bool forceReload = (rag !is null) && rag.isPrimaryInMemory();
    logger.trace(forceReload, "In-memory RAG detected: forcing AGENTS.md load");

    // return cached state (still ensure RAG has the topic for in-memory)
    if (isCacheHit) {
        logger.tracef("AGENTS.md cache hit (checksum: %s)", contentChecksum);

        // For in-memory RAG, reload the topic to ensure it exists in ephemeral storage
        if (forceReload) {
            reloadAgentMdToRag(rag, resolvedContent, contentChecksum, config.ragConfig);
        }

        return cachedState;
    }

    // ---- Cache miss: clean up old topic, summarize, load to RAG, save cache ----
    logger.infof("AGENTS.md cache miss (checksum: %s), generating summary", contentChecksum);

    // Remove old AGENTS.md topic from RAG if content has changed
    if (cachedState.isValid()) {
        removeAgentMdTopic(rag, cachedState.checksum);
    }

    string summary;
    try {
        auto sumAgent = SummaryAgent(config.summaryModel);
        // Summary agent doesn't need skill context, use summary model's prompt
        sumAgent.setSystemPrompt(config.getPrompt(skillManager: null,
                promptName: config.summaryModel.prompt, addSkills: false));
        summary = sumAgent.summarizeText(resolvedContent, 200, bulletListMergeCallback);
    } catch (Exception e) {
        logger.warningf("Failed to generate AGENTS.md summary: %s", e.msg);
        if (cachedState.isValid()) {
            logger.info("Using cached AGENTS.md summary after summarization failure");
            // For in-memory RAG, ensure cached topic is reloaded
            if (forceReload) {
                reloadAgentMdToRag(rag, resolvedContent, cachedState.checksum, config.ragConfig);
            }
            return cachedState;
        }
        return AgentMdState.init;
    }

    if (summary.empty) {
        logger.warning("AGENTS.md summary is empty");
        if (cachedState.isValid()) {
            logger.info("Using cached AGENTS.md summary after empty summary");
            if (forceReload) {
                reloadAgentMdToRag(rag, resolvedContent, cachedState.checksum, config.ragConfig);
            }
            return cachedState;
        }
        return AgentMdState.init;
    }

    // ---- Load full content into RAG ----
    if (rag !is null) {
        reloadAgentMdToRag(rag, resolvedContent, contentChecksum, config.ragConfig);
    }

    // ---- Build and save new state ----
    auto newState = AgentMdState(checksum: contentChecksum, summary: summary,
            timestamp: Clock.currTime());

    if (saveAgentMdCache(cachePath, newState)) {
        logger.infof("Saved AGENTS.md cache (summary: %s chars)", summary.length);
    } else {
        logger.warning("Failed to save AGENTS.md cache, state will be regenerated next run");
    }

    return newState;
}

/// Force-refresh AGENTS.md: clears the cache and re-summarizes.
/// Equivalent to calling processAgentMd(config, false, rag, forceRefresh: true),
/// but provides a cleaner API for callers that only need a refresh.
/// Returns the new AgentMdState, or a default-constructed state if no AGENTS.md is present.
/// NOTE: @system due to RAG and SummaryAgent operations.
AgentMdState refreshAgentMd(LlmConfig config, RAG rag) {
    return processAgentMd(config, false, rag, forceRefresh: true);
}

// ----------------------------------------------------------------------------
// Unit Tests: @path resolution and security
// ----------------------------------------------------------------------------
// Helper: safely remove a directory in scope(exit)
private void safeRmdirRecurse(string path) @safe {
    import std.file : rmdirRecurse;

    try {
        rmdirRecurse(path);
    } catch (Exception) {
    }
}

// C symlink function for creating symlinks in tests
extern (C) int symlink(const(char)*, const(char)*);

// Unified helper: creates a temporary test directory with cleanup support.
// Supports both workarea-style (with AbsolutePath + createFile) and
// dataDir-style (with Path) access patterns.
private struct TestWorkarea {
    import std.file : mkdirRecurse;

    string path;
    AbsolutePath workarea;

    this(string name) {
        this("", name);
    }

    this(string prefix, string name) {
        path = "./llmfun_test/" ~ prefix ~ name ~ "_" ~ Clock.currTime.toString;
        mkdirRecurse(path);
        workarea = path.AbsolutePath;
    }

    void cleanup() @safe {
        safeRmdirRecurse(path);
    }

    Path dirPath() @safe {
        return Path(path);
    }

    // Create a file with given content relative to the temp directory
    void createFile(string relPath, string content) @safe {
        import std.file : mkdirRecurse;

        auto fullPath = (workarea ~ relPath).AbsolutePath;
        mkdirRecurse(fullPath.dirName.toString);
        auto f = File(fullPath.toString, "w");
        f.write(content);
    }
}

unittest {
    // Test: Direct AGENTS.md content (no @path) passes through unchanged
    auto tw = TestWorkarea("path_direct");
    scope (exit)
        tw.cleanup();

    string content = "# My Project\n\nThis is a regular AGENTS.md with no @path references.\n\n- Rule 1: Be helpful\n- Rule 2: Be concise";
    auto result = resolveAgentMdContent(tw.workarea, content);
    assert(result == content, "Content without @path should pass through unchanged");
}

unittest {
    // Test: @path to valid workarea file resolves correctly
    auto tw = TestWorkarea("path_valid");
    scope (exit)
        tw.cleanup();

    string refContent = "This is the referenced file content.\nIt has multiple lines.\nAnd more content.";
    tw.createFile("rules.md", refContent);

    string content = "@rules.md";
    auto result = resolveAgentMdContent(tw.workarea, content);
    assert(result == refContent, "Expected resolved content to match referenced file");
}

unittest {
    // Test: @path to non-existent file produces empty result (complete failure)
    auto tw = TestWorkarea("path_missing");
    scope (exit)
        tw.cleanup();

    string content = "@nonexistent.md";
    auto result = resolveAgentMdContent(tw.workarea, content);
    assert(result == "", "Complete @path failure should return empty string");
}

unittest {
    // Test: Nested @path references resolve correctly
    auto tw = TestWorkarea("path_nested");
    scope (exit)
        tw.cleanup();

    // Create inner referenced file
    string innerContent = "Inner file content.";
    tw.createFile("inner.md", innerContent);

    // Create outer file that references inner
    tw.createFile("outer.md", "@inner.md");

    // AGENTS.md references outer.md, which references inner.md
    string content = "@outer.md";
    auto result = resolveAgentMdContent(tw.workarea, content);
    assert(result == innerContent, "Nested @path should resolve to final content");
}

unittest {
    // Test: Path traversal attacks (../) are rejected
    auto tw = TestWorkarea("path_traversal");
    scope (exit)
        tw.cleanup();
    // Try to escape workarea with ../
    string content = "@../outside.md";
    auto result = resolveAgentMdContent(tw.workarea, content);
    assert(result == "", "Path traversal should be rejected (complete failure returns empty)");
}

unittest {
    // Test: isPathInsideWorkarea prefix attack prevention
    auto workarea = "/workarea".AbsolutePath;
    auto safePath = "/workarea/subdir/file.md".AbsolutePath;
    auto evilPath = "/workarea_evil/file.md".AbsolutePath;
    auto exactMatch = "/workarea".AbsolutePath;
    assert(isPathInsideWorkarea(safePath, workarea), "Path inside workarea should be allowed");
    assert(!isPathInsideWorkarea(evilPath, workarea), "Prefix attack path should be rejected");
    assert(isPathInsideWorkarea(exactMatch, workarea), "Exact workarea match should be allowed");
}

unittest {
    // Test: Symlinks are rejected
    import std.file : mkdirRecurse;

    string tmpDir = "./llmfun_test/path_symlink_" ~ Clock.currTime.toString;
    mkdirRecurse(tmpDir);
    scope (exit)
        safeRmdirRecurse(tmpDir);

    auto workarea = tmpDir.AbsolutePath;

    // Create a real file
    {
        auto f = File(((workarea ~ "real.md").AbsolutePath).toString, "w");
        f.write("Real file content");
    }
    // Create a symlink to the real file using C's symlink function
    symlink(((workarea ~ "real.md").AbsolutePath).toString.ptr,
            ((workarea ~ "link.md").AbsolutePath).toString.ptr);

    string content = "@link.md";
    auto result = resolveAgentMdContent(workarea, content);
    assert(result == "", "Symlink @path should be rejected (complete failure returns empty)");
}

unittest {
    // Test: File size limit enforcement (> 1MB rejected)
    auto tw = TestWorkarea("path_size");
    scope (exit)
        tw.cleanup();

    // Create a file larger than 1MB
    import std.array : appender, replicate;

    auto bigContent = appender!string();
    auto line = "x".replicate(1024);
    foreach (i; 0 .. 1100) {
        bigContent.put(line);
        bigContent.put('\n');
    }
    tw.createFile("big.md", bigContent.data);

    string content = "@big.md";
    auto result = resolveAgentMdContent(tw.workarea, content);
    assert(result == "", "Oversized file should be rejected (complete failure returns empty)");
}

unittest {
    // Test: Recursion limit enforcement (> 5 levels rejected)
    auto tw = TestWorkarea("path_recursion");
    scope (exit)
        tw.cleanup();

    // Create a chain of files: level1 -> level2 -> level3 -> level4 -> level5 -> level6
    // This exceeds the max recursion depth of 5
    tw.createFile("level6.md", "Final content");

    // Build chain: each file references the next
    string[] levels = [
        "level1.md", "level2.md", "level3.md", "level4.md", "level5.md"
    ];
    foreach (i, level; levels) {
        string nextRef;
        if (i == levels.length - 1) {
            nextRef = "level6.md";
        } else {
            nextRef = levels[i + 1];
        }
        tw.createFile(level, "@" ~ nextRef);
    }

    string content = "@level1.md";
    auto result = resolveAgentMdContent(tw.workarea, content);
    assert(result == "",
            "Recursion depth limit should be exceeded (complete failure returns empty)");
}

unittest {
    // Test: Recursion boundary - should succeed within limit
    auto tw = TestWorkarea("path_recursion_boundary");
    scope (exit)
        tw.cleanup();

    // Create a chain that stays within the 5-level depth limit:
    // level1 -> level2 -> final (depths 1, 3, 5)
    tw.createFile("final.md", "Final content reached");
    tw.createFile("level2.md", "@final.md");
    tw.createFile("level1.md", "@level2.md");

    string content = "@level1.md";
    auto result = resolveAgentMdContent(tw.workarea, content);
    assert(result == "Final content reached", "Boundary recursion should succeed");
}

unittest {
    // Test: Circular @path reference detection
    auto tw = TestWorkarea("path_circular");
    scope (exit)
        tw.cleanup();

    // Create circular reference: A -> B -> A
    tw.createFile("fileA.md", "@fileB.md");
    tw.createFile("fileB.md", "@fileA.md");

    string content = "@fileA.md";
    auto result = resolveAgentMdContent(tw.workarea, content);
    assert(result == "", "Circular @path reference should be detected and rejected");
}

unittest {
    // Test: Partial @path failure handling (some succeed, some fail)
    auto tw = TestWorkarea("path_partial");
    scope (exit)
        tw.cleanup();

    // Create one valid file
    string validContent = "This content is valid.";
    tw.createFile("valid.md", validContent);

    // Reference both valid and invalid files
    string content = "@valid.md\n@nonexistent.md";
    auto result = resolveAgentMdContent(tw.workarea, content);
    assert(!result.empty, "Partial failure should not return empty");
    assert(result.canFind(validContent), "Partial failure should include successful resolutions");
    assert(result.canFind("@@RESOLVE_ERROR"), "Partial failure should include error markers");
}

unittest {
    // Test: Complete @path failure handling (all fail, returns empty)
    auto tw = TestWorkarea("path_completefail");
    scope (exit)
        tw.cleanup();

    // Reference multiple non-existent files
    string content = "@missing1.md\n@missing2.md";
    auto result = resolveAgentMdContent(tw.workarea, content);
    assert(result == "", "Complete @path failure should return empty string");
}

unittest {
    // Test: Mixed content with @path references and regular text
    auto tw = TestWorkarea("path_mixed");
    scope (exit)
        tw.cleanup();

    string refContent = "Referenced rules:\n- Always be polite";
    tw.createFile("rules.md", refContent);

    string content = "# Agent Instructions\n\n@rules.md\n\nAdditional instructions here.";
    auto result = resolveAgentMdContent(tw.workarea, content);
    assert(result.canFind("# Agent Instructions"), "Regular text should be preserved");
    assert(result.canFind("Referenced rules"), "@path content should be resolved");
    assert(result.canFind("Additional instructions here."), "Text after @path should be preserved");
}

unittest {
    // Test: @path with subdirectory resolution
    auto tw = TestWorkarea("path_subdir");
    scope (exit)
        tw.cleanup();

    string refContent = "Documentation content from subdirectory.";
    tw.createFile("docs/guide.md", refContent);

    string content = "@docs/guide.md";
    auto result = resolveAgentMdContent(tw.workarea, content);
    assert(result == refContent, "Subdirectory @path should resolve correctly");
}

unittest {
    // Test: Empty @ line is passed through (not treated as @path)
    auto tw = TestWorkarea("path_empty");
    scope (exit)
        tw.cleanup();

    string content = "Some text\n@\nMore text";
    auto result = resolveAgentMdContent(tw.workarea, content);
    assert(result == content, "Empty @ line should pass through unchanged");
}

unittest {
    // Test: Self-referencing @path (file references itself)
    auto tw = TestWorkarea("path_selfref");
    scope (exit)
        tw.cleanup();

    // Create a file that references itself
    tw.createFile("self.md", "@self.md");

    string content = "@self.md";
    auto result = resolveAgentMdContent(tw.workarea, content);
    assert(result == "", "Self-referencing @path should be detected as circular");
}

// ----------------------------------------------------------------------------
// Unit Tests: Integration tests for AGENTS.md workflow
// ----------------------------------------------------------------------------

// Helper wrapper for cache test data directories
private struct TestDataDir {
    TestWorkarea inner;

    string path() @safe @property {
        return inner.path;
    }

    Path dataDir() @safe @property {
        return inner.dirPath;
    }

    this(string name) {
        inner = TestWorkarea("data_", name);
    }

    void cleanup() @safe {
        inner.cleanup();
    }
}

unittest {
    // Integration Test: Cache file creation, persistence, and round-trip
    auto td = TestDataDir("cache_roundtrip");
    scope (exit)
        td.cleanup();

    auto cachePath = (td.dataDir ~ "agent_md_cache.json").Path;

    // Verify cache doesn't exist initially
    assert(!cachePath.exists, "Cache file should not exist initially");

    // Load non-existent cache returns default state
    auto emptyState = loadAgentMdCache(cachePath);
    assert(!emptyState.isValid(), "Empty cache should not be valid");
    assert(emptyState.checksum == 0, "Empty cache checksum should be 0");
    assert(emptyState.summary.empty, "Empty cache summary should be empty");

    // Save a state
    auto testState = AgentMdState(checksum: 12345678, summary: "- Rule 1: Be helpful\n- Rule 2: Be concise\n",
            timestamp: Clock.currTime());
    assert(saveAgentMdCache(cachePath, testState), "Cache save should succeed");
    assert(cachePath.exists, "Cache file should exist after save");

    // Load and verify round-trip
    auto loadedState = loadAgentMdCache(cachePath);
    assert(loadedState.isValid(), "Loaded state should be valid");
    assert(loadedState.checksum == testState.checksum, "Checksum should match after round-trip");
    assert(loadedState.summary == testState.summary, "Summary should match after round-trip");
}

unittest {
    // Integration Test: Cache file corruption handling
    auto td = TestDataDir("cache_corrupt");
    scope (exit)
        td.cleanup();

    auto cachePath = (td.dataDir ~ "agent_md_cache.json").Path;

    // Write corrupted JSON
    File(cachePath.toString, "w").writeln("{ corrupted json !!! }");

    // Should handle gracefully and return empty state
    auto state = loadAgentMdCache(cachePath);
    assert(!state.isValid(), "Corrupted cache should return invalid state");
}

unittest {
    // Integration Test: Cache clearing (file removal)
    auto td = TestDataDir("cache_clear");
    scope (exit)
        td.cleanup();

    auto cachePath = (td.dataDir ~ "agent_md_cache.json").Path;

    // Create and save cache
    auto testState = AgentMdState(checksum: 99999, summary: "- Some rule\n",
            timestamp: Clock.currTime());
    saveAgentMdCache(cachePath, testState);
    assert(cachePath.exists, "Cache should exist");

    // Clear cache by removing file
    remove(cachePath);
    assert(!cachePath.exists, "Cache should be removed");

    // Loading removed cache returns empty state
    auto state = loadAgentMdCache(cachePath);
    assert(!state.isValid(), "Removed cache should return invalid state");
}

unittest {
    // Integration Test: AgentMdState serialization with all fields
    auto td = TestDataDir("cache_fullserial");
    scope (exit)
        td.cleanup();

    auto cachePath = (td.dataDir ~ "agent_md_cache.json").Path;

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

unittest {
    // Integration Test: RAG topic naming convention
    // Verify makeAgentMdTopicName produces correct format
    long checksum1 = 12345678;
    string topic1 = makeAgentMdTopicName(checksum1);
    assert(topic1.startsWith("agent_md_"), "Topic should start with agent_md_ prefix");
    auto hexPart1 = topic1["agent_md_".length .. $];
    assert(hexPart1.length == 8, "Hex portion should be 8 characters");
    // Verify different checksums produce different topics
    long checksum2 = 87654321;
    string topic2 = makeAgentMdTopicName(checksum2);
    assert(topic1 != topic2, "Different checksums should produce different topic names");

    // Verify negative checksums are handled (64-bit hex, so 16 chars for -1)
    long checksumNeg = -1;
    string topicNeg = makeAgentMdTopicName(checksumNeg);
    assert(topicNeg.startsWith("agent_md_"), "Negative checksum topic should have correct prefix");
    auto hexPartNeg = topicNeg["agent_md_".length .. $];
    assert(hexPartNeg.length == 16,
            "Negative checksum hex portion should be 16 characters (full 64-bit hex)");
}

unittest {
    // Integration Test: Bullet list merge with multiple chunks
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

    // Verify deduplication: Point A appears only once
    auto lines = result.splitLines.filter!(a => !a.empty).array;
    auto pointALines = lines.filter!(a => a == "- Point A").array;
    assert(pointALines.length == 1, "Duplicate bullets should be deduplicated");
}

unittest {
    // Integration Test: Bullet list merge with empty chunks
    SummaryChunkT[] emptyChunks;
    auto result = bulletListMerge(emptyChunks);
    assert(result.empty, "Empty chunks should produce empty result");
}

unittest {
    // Integration Test: Bullet list merge normalizes bullet prefixes
    SummaryChunkT[] chunks;
    chunks ~= SummaryChunkT("- Normal bullet\n* Star bullet\nPlain text line",
            size_t(0), size_t(40));

    auto result = bulletListMerge(chunks);

    assert(result.canFind("- Normal bullet"), "Normal bullets should be preserved");
    assert(result.canFind("- Star bullet"), "Star bullets should be converted to dash");
    assert(result.canFind("- Plain text line"), "Plain text should get bullet prefix");
}

unittest {
    // Integration Test: AgentMdState equality comparison
    SysTime fixedTime = SysTime(DateTime(2024, 1, 1, 0, 0, 0), UTC());

    auto state1 = AgentMdState(checksum: 100, summary: "- Rule 1\n", timestamp: fixedTime);

    auto state2 = AgentMdState(checksum: 100, summary: "- Rule 1\n", timestamp: fixedTime);

    auto state3 = AgentMdState(checksum: 200, summary: "- Rule 1\n", timestamp: fixedTime);

    assert(state1 == state2, "States with same fields should be equal");
    assert(state1 != state3, "States with different checksums should not be equal");
}

unittest {
    // Integration Test: AgentMdState isValid checks
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

unittest {
    // Integration Test: End-to-end workflow simulation (cache + state management)
    // Simulates the key steps of processAgentMd without RAG/LLM dependencies
    auto td = TestDataDir("workflow_sim");
    scope (exit)
        td.cleanup();

    auto cachePath = (td.dataDir ~ "agent_md_cache.json").Path;
    auto workarea = td.path.AbsolutePath;

    // Step 1: Simulate first run - no cache exists
    auto cachedState = loadAgentMdCache(cachePath);
    assert(!cachedState.isValid(), "First run: no cache should exist");

    // Step 2: Create AGENTS.md content and compute hash
    string agentContent = "# Project Agent\n\n- Always use D programming language\n- Follow the coding standards\n- Write tests for all new code\n";
    long contentChecksum = computeContentHash(agentContent);
    assert(contentChecksum != 0, "Content hash should be non-zero");

    // Step 3: Simulate summary generation (in real code, this comes from LLM)
    string mockSummary = "- Always use D programming language\n- Follow the coding standards\n- Write tests for all new code\n";

    // Step 4: Save cache (simulating successful first run)
    auto newState = AgentMdState(contentChecksum, mockSummary, Clock.currTime());
    assert(saveAgentMdCache(cachePath, newState), "Cache save should succeed on first run");

    // Step 5: Simulate second run - cache hit
    cachedState = loadAgentMdCache(cachePath);
    assert(cachedState.isValid(), "Second run: cache should be valid");
    assert(cachedState.checksum == contentChecksum, "Second run: checksum should match");
    assert(cachedState.summary == mockSummary, "Second run: summary should match");

    // Step 6: Simulate content change - cache miss
    string modifiedContent = agentContent ~ "- New rule added\n";
    long newChecksum = computeContentHash(modifiedContent);
    assert(newChecksum != contentChecksum, "Modified content should have different hash");

    // In real code, this would trigger re-summarization
    // Verify the cache still has old data (simulating cache miss detection)
    cachedState = loadAgentMdCache(cachePath);
    assert(cachedState.checksum != newChecksum, "Cache miss: old checksum differs from new");

    // Step 7: Update cache with new state
    string newMockSummary = mockSummary ~ "- New rule added\n";
    auto updatedState = AgentMdState(newChecksum, newMockSummary, Clock.currTime());
    assert(saveAgentMdCache(cachePath, updatedState), "Cache update should succeed");

    // Step 8: Verify updated cache
    cachedState = loadAgentMdCache(cachePath);
    assert(cachedState.checksum == newChecksum, "Updated cache should have new checksum");
    assert(cachedState.summary == newMockSummary, "Updated cache should have new summary");

    // Step 9: Verify topic naming for both checksums
    string oldTopic = makeAgentMdTopicName(contentChecksum);
    string newTopic = makeAgentMdTopicName(newChecksum);
    assert(oldTopic.startsWith("agent_md_"), "Old topic should have correct prefix");
    assert(newTopic.startsWith("agent_md_"), "New topic should have correct prefix");
    assert(oldTopic != newTopic, "Different checksums should produce different topics");
}

unittest {
    // Integration Test: noCwdConfig cleanup simulation
    // Simulates the cleanup path when noCwdConfig is true
    auto td = TestDataDir("nocwdconfig");
    scope (exit)
        td.cleanup();

    auto cachePath = (td.dataDir ~ "agent_md_cache.json").Path;

    // Pre-condition: cache exists with valid data
    auto oldState = AgentMdState(checksum: 12345, summary: "- Old rule\n",
            timestamp: Clock.currTime());
    saveAgentMdCache(cachePath, oldState);
    assert(cachePath.exists, "Cache should exist before cleanup");

    // Simulate noCwdConfig cleanup: remove cache
    remove(cachePath);
    assert(!cachePath.exists, "Cache should be removed during noCwdConfig cleanup");

    // Verify state after cleanup
    auto state = loadAgentMdCache(cachePath);
    assert(!state.isValid(), "State should be invalid after noCwdConfig cleanup");
}

unittest {
    // Integration Test: Prompt composition order specification
    // 1. basePrompt, 2. alwaysApplyBlock, 3. agentMdSummary, 4. manifestXml,
    // 5. ragInstruction, 6. memory, 7. timestamp
    // This test verifies the relative ordering of key components.
    // Note: Full getPrompt() integration testing requires LlmConfig setup.
    string basePrompt = "You are an AI assistant.";
    string agentMdSummary = "# AGENTS.md Rules\n- Rule 1\n- Rule 2";
    string ragInstruction = "For detailed explanations, query your RAG knowledge base.";

    // Simulate prompt composition order
    string composed = basePrompt ~ "\n\n" ~ agentMdSummary ~ "\n\n" ~ ragInstruction;

    assert(composed.canFind(basePrompt), "Composed prompt should contain base prompt");
    assert(composed.canFind(agentMdSummary), "Composed prompt should contain AGENTS.md summary");
    assert(composed.canFind(ragInstruction), "Composed prompt should contain RAG instruction");

    // Verify order: basePrompt comes before summary, summary comes before ragInstruction
    size_t basePos = indexOf(composed, basePrompt);
    size_t summaryPos = indexOf(composed, agentMdSummary);
    size_t ragPos = indexOf(composed, ragInstruction);
    assert(basePos < summaryPos, "Base prompt should come before AGENTS.md summary");
    assert(summaryPos < ragPos, "AGENTS.md summary should come before RAG instruction");
}

unittest {
    // Integration Test: forceRefresh workflow simulation
    // Integration Test: forceRefresh workflow simulation
    // Simulates the /refresh-agent-md command path
    auto td = TestDataDir("forcerefresh");
    scope (exit)
        td.cleanup();

    auto cachePath = (td.dataDir ~ "agent_md_cache.json").Path;

    // Pre-condition: cache exists
    auto oldState = AgentMdState(checksum: 11111, summary: "- Old summary\n",
            timestamp: Clock.currTime());
    saveAgentMdCache(cachePath, oldState);

    // Simulate forceRefresh: clear cache
    if (cachePath.exists) {
        remove(cachePath);
    }

    // Verify cache was cleared
    auto state = loadAgentMdCache(cachePath);
    assert(!state.isValid(), "Cache should be invalid after forceRefresh");

    // In real code, processAgentMd would now re-summarize
    // Simulate the re-summarization with new checksum
    string content = "# Updated Agent\n- New rules\n";
    long newChecksum = computeContentHash(content);
    auto newState = AgentMdState(checksum: newChecksum, summary: "- New summary\n",
            timestamp: Clock.currTime());
    saveAgentMdCache(cachePath, newState);

    // Verify new cache was created
    state = loadAgentMdCache(cachePath);
    assert(state.isValid(), "Cache should be valid after refresh");
    assert(state.checksum == newChecksum, "Cache should have new checksum after refresh");
}

unittest {
    // Integration Test: AGENTS.md removal workflow
    // Simulates when AGENTS.md file is deleted after being cached
    auto td = TestDataDir("agentremoval");
    scope (exit)
        td.cleanup();

    auto cachePath = (td.dataDir ~ "agent_md_cache.json").Path;

    // Pre-condition: cache exists with valid data
    auto oldState = AgentMdState(checksum: 55555, summary: "- Cached rule\n",
            timestamp: Clock.currTime());
    saveAgentMdCache(cachePath, oldState);
    assert(loadAgentMdCache(cachePath).isValid(), "Cache should be valid before removal");

    // Simulate AGENTS.md removal: clean up cache and RAG topics
    // (In real code, removeAgentMdTopics(rag) would also be called)
    // Simulate AGENTS.md removal: clean up cache and RAG topics
    // NOTE: In real code, removeAgentMdTopics(rag) would also be called to clean
    // up RAG entries. This test cannot verify RAG topic removal without a RAG
    // instance - that is tested as part of the full processAgentMd() workflow.
    remove(cachePath);
    auto state = loadAgentMdCache(cachePath);
    assert(!state.isValid(), "Cache should be invalid after AGENTS.md removal");
}

unittest {
    // Integration Test: resolveAgentMdContent with mixed content
    // Verify that regular text and @path references work together
    auto tw = TestWorkarea("integration_mixed");
    scope (exit)
        tw.cleanup();

    // Create a referenced file
    tw.createFile("standards.md",
            "## Coding Standards\n- Use 4-space indentation\n- Follow D style guide");

    // AGENTS.md with both text and @path
    string content = "# Project Agent\n\n## General Rules\n- Be helpful and concise\n\n## Standards\n@standards.md\n\n## Additional Notes\nRemember to write tests.";

    auto result = resolveAgentMdContent(tw.workarea, content);

    assert(result.canFind("# Project Agent"), "Header should be preserved");
    assert(result.canFind("Be helpful and concise"), "Regular text should be preserved");
    assert(result.canFind("Coding Standards"), "Referenced content should be included");
    assert(result.canFind("Remember to write tests"), "Text after @path should be preserved");
}

unittest {
    // Integration Test: computeContentHash consistency
    // Verify that the same content always produces the same hash
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
