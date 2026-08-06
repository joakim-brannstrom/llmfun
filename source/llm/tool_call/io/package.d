module llm.tool_call.io;

import logger = std.logger;
import std.algorithm : map, filter, startsWith, count, joiner, endsWith, splitter, canFind, min;
import std.array : empty, appender, array, join;
import std.conv : to, text;
import std.exception : enforce;
import std.file : readText, exists, mkdirRecurse, getSize, remove, dirEntries, SpanMode;
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
    if (msg.canFind("ambiguous targeting"))
        return "ambiguous_targeting";
    if (msg.canFind("no targeting method specified"))
        return "missing_targeting";
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
