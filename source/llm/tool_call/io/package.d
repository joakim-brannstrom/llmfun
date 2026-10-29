module llm.tool_call.io;

import logger = std.logger;
import std.algorithm : map, filter, startsWith, count, joiner, endsWith, splitter, canFind, min;
import std.array : empty, appender, array, join;
import std.conv : to, text;
import std.exception : enforce;
import std.file : readText, exists, mkdirRecurse, remove, dirEntries, SpanMode;
import std.json : JSONValue, parseJSON, JSONOptions, JSONType;
import std.path : relativePath;
import std.process : execute;
import std.range : enumerate;
import std.regex : Regex, regex;
import std.stdio : File;
import std.string : splitLines, indexOf, strip, split, replace, toLower, chomp;
import std.sumtype : match;

import my.path : AbsolutePath;

import llm.config : ToolLimits;
import llm.tool_call : Context;

interface FileContext : Context {
    bool isPathInsideWorkArea(AbsolutePath path);
    AbsolutePath workArea();
    ToolLimits getToolLimits();

    /// Session-scoped file dirtiness tracking: editFile/writeFile
    /// call markDirty(path) on success (non-dryRun); readFile calls observe
    /// on success. A byLine edit on a dirty path fails loudly, forcing a
    /// re-read first. State is bounded: oldest clean entries are evicted,
    /// dirty entries are never evicted.
    void markDirty(AbsolutePath path);
    void observe(AbsolutePath path);
    bool isDirty(AbsolutePath path);
    /// Content hash at last observation; -1 when never observed (the
    /// best-effort staleness cross-check skips such paths).
    long observedHash(AbsolutePath path);
}

mixin template FileDirtyTracking() {
    import logger = std.logger;
    import llm.utility : computeContentHash;
    import llm.tool_call.io : sanitizeLogPath;

    private {
        long[AbsolutePath] dirtyHash; // computeContentHash at last observation
        bool[AbsolutePath] dirty; // dirtied since last observation
        AbsolutePath[] dirtyOrder; // insertion order, for clean-entry eviction
        bool[AbsolutePath] inOrder; // dirtyOrder membership (no duplicates)
    }

    private void update(AbsolutePath path) {
        import std.file : readText;

        try {
            dirtyHash[path] = computeContentHash(readText(path.toString));
        } catch (Exception e) {
            // The file may have vanished mid-session; keep the dirty flag but
            // no snapshot — the best-effort cross-check skips paths without
            // one.
            dirtyHash.remove(path);
            logger.tracef("read failed for %s: %s", sanitizeLogPath(path.toString), e.msg);
        }
        if (path !in inOrder) {
            inOrder[path] = true;
            dirtyOrder ~= path;
        }
    }

    override void markDirty(AbsolutePath path) {
        update(path);
        dirty[path] = true;
        maybeEvictDirtyEntries;
    }

    override void observe(AbsolutePath path) {
        update(path);
        dirty.remove(path);
        maybeEvictDirtyEntries;
    }

    override bool isDirty(AbsolutePath path) {
        return (path in dirty) !is null;
    }

    override long observedHash(AbsolutePath path) {
        auto p = path in dirtyHash;
        return p ? *p : -1;
    }

    /// Bounded state: evict the oldest CLEAN entries only - a dirty entry is
    /// never evicted (that would re-enable stale byLine targeting).
    private void maybeEvictDirtyEntries() {
        import std.algorithm : countUntil;

        enum maxTrackedFiles = 256;

        if (dirtyOrder.length < maxTrackedFiles)
            return;

        // TODO: use a map with time instead of the order array, making
        // eviction O(1). Also evict a few extra entries so a full cache does
        // not need eviction on every operation.
        while (dirtyOrder.length > (maxTrackedFiles - 10)) {
            auto idx = dirtyOrder.countUntil!(p => (p in dirty) is null);
            if (idx == -1)
                return; // every tracked entry is dirty: keep them all
            const victim = dirtyOrder[idx];
            foreach (i; idx .. dirtyOrder.length - 1)
                dirtyOrder[i] = dirtyOrder[i + 1];
            dirtyOrder = dirtyOrder[0 .. $ - 1];
            dirtyHash.remove(victim);
            inOrder.remove(victim);
        }
    }
}

/// Sanitize a path parameter for log output: never log absolute paths or
/// parent-traversal segments, only the workarea-relative form.
string sanitizeLogPath(string path) @safe {
    import std.algorithm : among;
    import std.path : pathSplitter, dirSeparator;

    if (path.empty)
        return "(workarea)";
    const norm = path.pathSplitter.filter!(a => !a.among(".", "..", "/")).join(dirSeparator);
    return norm.empty ? "(workarea)" : norm;
}

/// Classify a tool error message into a stable category for structured logs.
/// Order matters: more specific patterns are checked first.
// TODO: this is very bad design how classifyEditError. It should instead be a
// custom Exception with an enum. A change to the error message will lead to
// "other".
string classifyEditError(string msg) @safe {
    if (msg.canFind("the edit was refused before any change"))
        return "fragment_guard";
    if (msg.canFind("pass count="))
        return "auto_count_required";
    if (msg.canFind("ambiguous targeting"))
        return "ambiguous_targeting";
    if (msg.canFind("no targeting method specified"))
        return "missing_targeting";
    if (msg.canFind("line numbers may be stale"))
        return "stale_byline_target";
    if (msg.canFind("not found"))
        return "search_not_found";
    if (msg.canFind("matchIndex="))
        return "match_index_oob";
    if (msg.canFind("Context mismatch"))
        return "context_mismatch";
    if (msg.canFind("exceeds file length"))
        return "range_oob";
    if (msg.canFind("Invalid hunk") || msg.canFind("hunk header")
            || msg.canFind("Hunk tries to go backward")
            || msg.canFind("Unexpected end of file") || msg.canFind("must start with"))
        return "invalid_diff";
    if (msg.canFind("must be") || msg.canFind("may NOT")
            || msg.canFind("cannot be used") || msg.canFind("cannot be combined")
            || msg.canFind("not supported")
            || msg.canFind("must not be empty") || msg.canFind("must contain"))
        return "invalid_parameter";
    return "other";
}

/// True when the file's last byte is '\n'. Used to preserve trailing-newline
/// state across edits and to make dryRun previews byte-accurate.
bool fileEndsWithNewline(string path) {
    import std.file : read;

    auto data = cast(ubyte[]) read(path);
    return data.length > 0 && data[$ - 1] == '\n';
}

void writeLines(AbsolutePath path, string[] lines, bool lastLineNoNewline = false) {
    auto f = File(path.toString, "w");
    foreach (i, line; lines) {
        if (lastLineNoNewline && i == lines.length - 1)
            f.write(line);
        else
            f.writeln(line);
    }
}

/// Edit mode for the file-editing tools: replace, remove, append,
/// insert_before, insert_after.
enum EditMode {
    replace,
    remove,
    append,
    insert_before,
    insert_after
}

/**
 * Parse a mode string into an EditMode enum value.
 * Accepts both kebab-case (e.g. "insert-before").
 * Normalizes input by removing hyphens and converting to lowercase before matching.
 * Throws on invalid mode.
 */
EditMode parseEditMode(string modeStr) @safe {
    try {
        return replace(modeStr, "-", "_").toLower.to!EditMode;
    } catch (Exception e) {
        logger.info(e.msg);
    }
    throw new Exception("Invalid edit mode '" ~ modeStr
            ~ "'. Valid modes: replace, remove, append, insert_before, insert_after");
}
