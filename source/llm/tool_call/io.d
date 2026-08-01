module llm.tool_call.io;

import logger = std.logger;
import std.algorithm : map, filter, startsWith, count, joiner, endsWith, splitter, canFind;
import std.array : empty, appender, array, join;
import std.conv : to, text;
import std.exception : enforce;
import std.file : readText, exists, mkdirRecurse, getSize, remove, dirEntries, SpanMode;
import std.format : format, formattedWrite;
import std.json : JSONValue, parseJSON, JSONOptions;
import std.path : relativePath;
import std.process : execute;
import std.range : enumerate;
import std.regex : Regex, regex;
import std.stdio : File;
import std.string : splitLines, indexOf, strip, split, replace, toLower, chomp;
import std.sumtype : match;

import my.path : AbsolutePath;

import llm.tool_call;
import llm.tool_call.utility;
import llm.config : ToolLimits;

mixin RegisterLlmFunctions!();

immutable MaxLines = 20;

interface FileContext : Context {
    bool isPathInsideWorkArea(AbsolutePath path);
    AbsolutePath workArea();
    ToolLimits getToolLimits();
}

struct RemoveFileParams {
    string path;
}

@Function("Remove file")
ExecuteFuncResult removeFile(Context baseCtx, RemoveFileParams params) {
    mixin(baseContextToSpecific!FileContext);

    auto path_ = pathToWorkarea(ctx, params.path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    try {
        remove(path_);
        return ExecuteFuncResult("OK", success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: failed to remove file '$(params.path)': $(e.msg)".text,
                success: false);
    }
}

struct WriteFileParams {
    string path;
    string content;
}

@Function("Write content to a file, creating it (including parent directories) if it does not exist. Returns OK or error message")
ExecuteFuncResult writeFile(Context baseCtx, WriteFileParams params) {
    mixin(baseContextToSpecific!FileContext);

    auto path_ = pathToWorkarea(ctx, params.path);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }
    if (!ctx.workArea.exists) {
        return ExecuteFuncResult("error: creating file is blocked. writeFile is disabled",
                success: false);
    }

    try {
        if (path_ != ctx.workArea && !path_.dirName.exists) {
            mkdirRecurse(path_.dirName.toString);
        }
        File(path_.toString, "w").write(params.content);
        return ExecuteFuncResult("OK", success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: failed writing content to file '$(params.path)': $(e.msg)".text,
                success: false);
    }
}

struct ReadFileParams {
    string path;

    @ParamDescription("First line to read, 1-based")
    long startLine;

    @ParamDescription("Number of lines to read")
    long count;

    @ParamDescription("Set to true to prefix each line with its line number (e.g. \"1→ ...\")")
    @ParamOptional bool appendLoc = true;
}

@Function("Read the contents of a file.")
ExecuteFuncResult readFile(Context baseCtx, ReadFileParams params) {

    mixin(baseContextToSpecific!FileContext);

    auto json = JSONValue.emptyObject;

    auto path_ = pathToWorkarea(ctx, params.path, checkExist: true);
    if (!path_.valid) {
        json["error"] = path_.errorMsg;
        return ExecuteFuncResult(json.toString(JSONOptions.doNotEscapeSlashes), success: false);
    }
    auto maxLines = ctx.getToolLimits().readFileMaxLines;
    if (maxLines <= 0) {
        logger.warning("readFileMaxLines is ", maxLines, ", falling back to default ", MaxLines);
        maxLines = MaxLines;
    }
    if (auto err = validateLineRange(params.startLine, params.count, maxLines)) {
        json["error"] = err;
        return ExecuteFuncResult(json.toString(JSONOptions.doNotEscapeSlashes), success: false);
    }
    try {
        if (getSize(path_) == 0) {
            json["content"] = "";
            return ExecuteFuncResult(json.toString(JSONOptions.doNotEscapeSlashes), success: true);
        }

        auto buf = appender!(string)();
        const firstIdx = params.startLine - 1;
        const lastIndex = firstIdx + params.count;
        foreach (line; File(path_).byLine.enumerate.filter!(a => a.index >= firstIdx
                && a.index < lastIndex)) {
            if (params.appendLoc) {
                formattedWrite(buf, "%s→", line.index + 1);
            }
            buf.put(line.value);
            buf.put('\n');
        }
        json["content"] = buf[];
        return ExecuteFuncResult(json.toString(JSONOptions.doNotEscapeSlashes), success: true);
    } catch (Exception e) {
        json["error"] = i"error: failed reading $(params.count) lines starting at line $(
                params.startLine) from file '$(params.path)': $(e.msg)".text;
        return ExecuteFuncResult(json.toString(JSONOptions.doNotEscapeSlashes), success: false);
    }
}

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

unittest {
    assert(parseEditMode("replace") == EditMode.replace);
    assert(parseEditMode("remove") == EditMode.remove);
    assert(parseEditMode("append") == EditMode.append);
    assert(parseEditMode("insert-after") == EditMode.insert_after);
    assert(parseEditMode("insert-before") == EditMode.insert_before);
    assert(parseEditMode("REPLACE") == EditMode.replace); // case-insensitive
    assert(parseEditMode("Remove") == EditMode.remove); // case-insensitive

    bool threw = false;
    try {
        parseEditMode("invalid");
    } catch (Exception) {
        threw = true;
    }
    assert(threw, "should throw on invalid mode");
}

/**
 * Perform a case-sensitive substring search across file lines.
 * Returns the 0-based index of the first line containing the marker,
 * or -1 if not found.
 *
 * Parameters:
 *     lines:  Array of file lines to search
 *     marker: Substring to search for (case-sensitive)
 *
 * Returns:
 *     0-based index of the first matching line, or -1 if not found
 */
long findMarkerLine(string[] lines, string marker) @safe {
    enforce(marker.length > 0, "marker must not be empty");
    foreach (i, line; lines) {
        if (line.canFind(marker)) {
            return i;
        }
    }
    return -1;
}

unittest {
    string[] lines = ["hello world", "foo bar", "hello again"];

    // Exact substring match
    assert(findMarkerLine(lines, "hello world") == 0);

    // Substring match (marker is part of line)
    assert(findMarkerLine(lines, "world") == 0);
    assert(findMarkerLine(lines, "foo") == 1);

    // Multiple matches - should return first (index 0)
    assert(findMarkerLine(lines, "hello") == 0);

    // Case-sensitive: "Foo" does not match "foo"
    assert(findMarkerLine(lines, "Foo") == -1);
    assert(findMarkerLine(lines, "HELLO") == -1);

    // Not found
    assert(findMarkerLine(lines, "xyz") == -1);

    // Empty marker throws exception
    {
        bool threw = false;
        try {
            findMarkerLine(lines, "");
        } catch (Exception) {
            threw = true;
        }
        assert(threw, "empty marker should throw");
    }

    // Empty lines array
    assert(findMarkerLine(cast(string[])[], "xyz") == -1);
}

/**
 * Range representing a matched code block within a file.
 *
 * Fields:
 *     start: 0-based start index in the file lines (inclusive)
 *     end:   0-based end index in the file lines (exclusive)
 *     found: true if a matching code block was found
 */
struct CodeBlockRange {
    size_t start;
    size_t end;
    bool found;
}

/**
 * Find a code block in file lines using trimmed equality matching.
 *
 * Matching algorithm:
 * 1. Skip leading empty lines in searchLines to find the anchor line.
 * 2. Anchor line matches when fileLine.strip == searchLine.strip (trimmed equality).
 * 3. Subsequent non-empty search lines must match corresponding file lines via trimmed equality.
 * 4. Empty search lines are flexible — they are skipped and do not consume file lines.
 *
 * Parameters:
 *     fileLines:   Array of file lines to search in
 *     searchLines: Array of search lines to match (the code block to find)
 *
 * Returns:
 *     CodeBlockRange with start/end indices if found, or found=false if not found
 *
 * Throws:
 *     Exception if searchLines is empty or contains only empty/whitespace lines
 */
CodeBlockRange findCodeBlock(const(char[])[] fileLines, const(char[])[] searchLines) @safe {
    if (searchLines.length == 0) {
        return CodeBlockRange(0, 0, false);
    }

    // Find anchor (first non-empty line in searchLines after stripping)
    size_t anchorIdx = 0;
    while (anchorIdx < searchLines.length && searchLines[anchorIdx].strip.length == 0) {
        anchorIdx++;
    }

    if (anchorIdx >= searchLines.length) {
        return CodeBlockRange(0, 0, false);
    }

    auto anchorStripped = searchLines[anchorIdx].strip;

    // Search for anchor in fileLines
    foreach (i, fileLine; fileLines) {
        if (fileLine.strip != anchorStripped) {
            continue;
        }

        // Found anchor at position i — try to match remaining search lines
        size_t filePos = i + 1;
        bool allMatch = true;

        foreach (searchIdx; anchorIdx + 1 .. searchLines.length) {
            auto searchStripped = searchLines[searchIdx].strip;
            if (searchStripped.length == 0) {
                // Empty search line is flexible — skip without consuming file lines
                continue;
            }

            if (filePos >= fileLines.length) {
                allMatch = false;
                break;
            }

            if (fileLines[filePos].strip != searchStripped) {
                allMatch = false;
                break;
            }

            filePos++;
        }

        if (allMatch) {
            return CodeBlockRange(i, filePos, true);
        }
    }

    return CodeBlockRange(0, 0, false);
}

unittest {
    // === Exact match ===
    {
        string[] fileLines = ["foo", "bar", "baz"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 2);
    }

    // === Whitespace tolerance via strip ===
    {
        string[] fileLines = ["    foo", "  bar  ", "baz"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 2);
    }

    // === Tabs vs spaces tolerance ===
    {
        string[] fileLines = ["\t\tfoo", "\tbar"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 2);
    }

    // === Comment false positive prevention ===
    // Searching for "foo" should NOT match "// call fooBar()"
    {
        string[] fileLines = ["// call fooBar()", "other"];
        string[] searchLines = ["foo"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(!result.found);
    }

    // === Not found ===
    {
        string[] fileLines = ["hello", "world"];
        string[] searchLines = ["foo"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(!result.found);
    }

    // === First match when multiple matches exist ===
    {
        string[] fileLines = ["foo", "bar", "foo", "bar", "extra"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 2);
    }

    // === Empty search block returns not found ===
    {
        auto result = findCodeBlock(["foo"], cast(string[])[]);
        assert(!result.found);
    }

    // === All-empty search block returns not found ===
    {
        auto result = findCodeBlock(["foo"], ["", "  ", "\t"]);
        assert(!result.found);
    }

    // === Leading empty lines in search content ===
    {
        string[] fileLines = ["foo", "bar"];
        string[] searchLines = ["", "", "foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 2);
    }

    // === Multi-line search block matching ===
    {
        string[] fileLines = ["function foo() {", "    return bar;", "}"];
        string[] searchLines = ["function foo() {", "    return bar;", "}"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 3);
    }

    // === Empty search lines in middle (flexible) ===
    {
        string[] fileLines = ["function foo() {", "    return bar;", "}"];
        string[] searchLines = [
            "function foo() {", "", "    return bar;", "", "}"
        ];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 3);
    }

    // === Match not at start of file ===
    {
        string[] fileLines = ["header", "foo", "bar", "footer"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 1);
        assert(result.end == 3);
    }

    // === Single-line search block ===
    {
        string[] fileLines = ["hello", "world", "foo"];
        string[] searchLines = ["world"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 1);
        assert(result.end == 2);
    }

    // === Empty file lines ===
    {
        auto result = findCodeBlock(cast(string[])[], ["foo"]);
        assert(!result.found);
    }
    // === Search block longer than file ===
    {
        string[] fileLines = ["foo"];
        string[] searchLines = ["foo", "bar", "baz"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(!result.found);
    }

    // === Anchor at end of file with trailing search lines ===
    {
        string[] fileLines = ["header", "anchor"];
        string[] searchLines = ["anchor", "trailing"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(!result.found);
    }

    // === CRLF line endings tolerance ===
    {
        string[] fileLines = ["foo\r", "bar\r"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 2);
    }
}

/**
 * Edit a file in memory by finding a marker line and applying an edit mode.
 *
 * Parameters:
 *     fileLines: Array of file lines to edit
 *     mode: Edit mode determining how to apply the edit
 *     content: Content to insert (split into lines for multi-line content)
 *     marker: String to search for in file lines (case-sensitive substring match)
 *
 * Returns: Modified lines array with the edit applied.
 *
 * Mode behavior:
 *     replace:      Replace the marker line with content lines
 *     remove:       Remove the marker line (content must be empty)
 *     append:       Keep marker line, add content lines after it
 *     insert_after:  Same as append
 *     insert_before: Add content lines before marker line, keep marker line
 *
 * Throws: Exception if marker not found or if remove mode has non-empty content.
 */
string[] editFileByMarkerMemory(string[] fileLines, EditMode mode, string content, string marker) @safe {
    // Fail fast: validate marker before calling findMarkerLine
    enforce(marker.length > 0, "marker must not be empty");
    enforce(!(mode == EditMode.remove && content.length > 0), "remove mode requires empty content");

    long markerIndex = findMarkerLine(fileLines, marker);
    enforce(markerIndex >= 0, i"Marker not found: '$(marker)'".text);

    // Use chomp to prevent splitLines from creating a trailing empty element
    // when content ends with a newline (e.g. "line1\nline2\n" => ["line1","line2"])
    auto contentLines = content.length > 0 ? content.chomp.splitLines : null;

    auto lines = appender!(string[])();
    foreach (i, line; fileLines) {
        if (i == markerIndex) {
            final switch (mode) {
            case EditMode.replace:
                lines.put(contentLines);
                break;
            case EditMode.remove:
                // Skip the marker line entirely
                break;
            case EditMode.append:
            case EditMode.insert_after:
                lines.put(line.idup);
                lines.put(contentLines);
                break;
            case EditMode.insert_before:
                lines.put(contentLines);
                lines.put(line.idup);
                break;
            }
        } else {
            lines.put(line.idup);
        }
    }
    return lines[];
}

unittest {
    // === Replace mode: single-line content ===
    {
        string[] fileLines = ["hello", "world", "foo", "bar"];
        auto res = editFileByMarkerMemory(fileLines, EditMode.replace, "replaced", "world");
        assert(res.length == 4);
        assert(res[0] == "hello");
        assert(res[1] == "replaced");
        assert(res[2] == "foo");
        assert(res[3] == "bar");
    }

    // === Replace mode: multi-line content ===
    {
        string[] fileLines = ["hello", "world", "foo"];
        auto res = editFileByMarkerMemory(fileLines, EditMode.replace, "line1\nline2", "world");
        assert(res.length == 4);
        assert(res[0] == "hello");
        assert(res[1] == "line1");
        assert(res[2] == "line2");
        assert(res[3] == "foo");
    }

    // === Remove mode ===
    {
        string[] fileLines = ["hello", "world", "foo"];
        auto res = editFileByMarkerMemory(fileLines, EditMode.remove, "", "world");
        assert(res.length == 2);
        assert(res[0] == "hello");
        assert(res[1] == "foo");
    }

    // === Remove mode: non-empty content throws ===
    {
        bool threw = false;
        try {
            editFileByMarkerMemory(["hello", "world"], EditMode.remove, "not empty", "world");
        } catch (Exception) {
            threw = true;
        }
        assert(threw, "remove mode with non-empty content should throw");
    }

    // === Append mode: keep marker, add content after ===
    {
        string[] fileLines = ["hello", "world", "foo"];
        auto res = editFileByMarkerMemory(fileLines, EditMode.append, "appended", "world");
        assert(res.length == 4);
        assert(res[0] == "hello");
        assert(res[1] == "world");
        assert(res[2] == "appended");
        assert(res[3] == "foo");
    }

    // === Append mode: multi-line content ===
    {
        string[] fileLines = ["hello", "world", "foo"];
        auto res = editFileByMarkerMemory(fileLines, EditMode.append, "line1\nline2", "world");
        assert(res.length == 5);
        assert(res[0] == "hello");
        assert(res[1] == "world");
        assert(res[2] == "line1");
        assert(res[3] == "line2");
        assert(res[4] == "foo");
    }

    // === InsertAfter mode: same as append ===
    {
        string[] fileLines = ["hello", "world", "foo"];
        auto res = editFileByMarkerMemory(fileLines, EditMode.insert_after, "after", "world");
        assert(res.length == 4);
        assert(res[0] == "hello");
        assert(res[1] == "world");
        assert(res[2] == "after");
        assert(res[3] == "foo");
    }

    // === InsertBefore mode: add content before, keep marker ===
    {
        string[] fileLines = ["hello", "world", "foo"];
        auto res = editFileByMarkerMemory(fileLines, EditMode.insert_before, "before", "world");
        assert(res.length == 4);
        assert(res[0] == "hello");
        assert(res[1] == "before");
        assert(res[2] == "world");
        assert(res[3] == "foo");
    }

    // === InsertBefore mode: multi-line content ===
    {
        string[] fileLines = ["hello", "world", "foo"];
        auto res = editFileByMarkerMemory(fileLines, EditMode.insert_before,
                "line1\nline2", "world");
        assert(res.length == 5);
        assert(res[0] == "hello");
        assert(res[1] == "line1");
        assert(res[2] == "line2");
        assert(res[3] == "world");
        assert(res[4] == "foo");
    }

    // === Marker not found throws ===
    {
        bool threw = false;
        try {
            editFileByMarkerMemory(["hello", "world"], EditMode.replace, "x", "notfound");
        } catch (Exception) {
            threw = true;
        }
        assert(threw, "marker not found should throw");
    }

    // === Empty marker throws ===
    {
        bool threw = false;
        try {
            editFileByMarkerMemory(["hello"], EditMode.replace, "x", "");
        } catch (Exception) {
            threw = true;
        }
        assert(threw, "empty marker should throw");
    }

    // === Non-marker lines preserved unchanged ===
    {
        string[] fileLines = ["line1", "line2", "line3", "line4"];
        auto res = editFileByMarkerMemory(fileLines, EditMode.replace, "new", "line2");
        assert(res[0] == "line1");
        assert(res[1] == "new");
        assert(res[2] == "line3");
        assert(res[3] == "line4");
    }

    // === Marker at start of file ===
    {
        string[] fileLines = ["marker", "line2", "line3"];
        auto res = editFileByMarkerMemory(fileLines, EditMode.replace, "new", "marker");
        assert(res.length == 3);
        assert(res[0] == "new");
        assert(res[1] == "line2");
        assert(res[2] == "line3");
    }

    // === Marker at end of file ===
    {
        string[] fileLines = ["line1", "line2", "marker"];
        auto res = editFileByMarkerMemory(fileLines, EditMode.replace, "new", "marker");
        assert(res.length == 3);
        assert(res[0] == "line1");
        assert(res[1] == "line2");
        assert(res[2] == "new");
    }

    // === First match when multiple lines contain marker ===
    {
        string[] fileLines = ["hello world", "goodbye world", "end"];
        auto res = editFileByMarkerMemory(fileLines, EditMode.replace, "replaced", "world");
        assert(res.length == 3);
        assert(res[0] == "replaced");
        assert(res[1] == "goodbye world");
        assert(res[2] == "end");
    }

    // === Replace mode: empty content (effectively removes marker line) ===
    {
        string[] fileLines = ["hello", "world", "foo"];
        auto res = editFileByMarkerMemory(fileLines, EditMode.replace, "", "world");
        assert(res.length == 2);
        assert(res[0] == "hello");
        assert(res[1] == "foo");
    }

    // === Empty file lines ===
    {
        bool threw = false;
        try {
            editFileByMarkerMemory(cast(string[])[], EditMode.replace, "x", "marker");
        } catch (Exception) {
            threw = true;
        }
        assert(threw, "empty file lines with marker should throw");
    }

    // === Trailing newline in content handled correctly ===
    {
        string[] fileLines = ["hello", "world", "foo"];
        auto res = editFileByMarkerMemory(fileLines, EditMode.replace, "line1\nline2\n", "world");
        assert(res.length == 4, res.length.to!string);
        assert(res[0] == "hello");
        assert(res[1] == "line1");
        assert(res[2] == "line2");
        assert(res[3] == "foo");
    }

    // === Trailing newline in append mode ===
    {
        string[] fileLines = ["hello", "world", "foo"];
        auto res = editFileByMarkerMemory(fileLines, EditMode.append, "line1\nline2\n", "world");
        assert(res.length == 5, res.length.to!string);
        assert(res[0] == "hello");
        assert(res[1] == "world");
        assert(res[2] == "line1");
        assert(res[3] == "line2");
        assert(res[4] == "foo");
    }
}

/**
 * Find the first occurrence of a code block in file lines and replace it
 * with replacement lines.
 *
 * Uses findCodeBlock (trimmed equality matching) to locate the search block,
 * then replaces the matched range with the replacement lines.
 *
 * Parameters:
 *     fileLines:   Array of file lines to edit
 *     searchLines: Array of search lines to find (the code block to locate)
 *     replaceLines: Array of replacement lines to insert in place of the match
 *
 * Returns: Modified lines array with the first occurrence replaced.
 *
 * Throws: Exception if searchLines is empty or if the search block is not found.
 */
int searchAndReplaceMemory(string[] fileLines, string[] searchLines,
        string[] replaceLines, ref string[] res) @safe {
    auto range = findCodeBlock(fileLines, searchLines);
    if (!range.found)
        return 0;

    auto result = appender!(string[])();
    result.put(fileLines[0 .. range.start]);
    result.put(replaceLines);
    result.put(fileLines[range.end .. $]);
    res = result[];
    return 1;
}

unittest {
    // === Single-line replace ===
    {
        string[] res;
        string[] fileLines = ["hello", "world", "foo"];
        string[] searchLines = ["world"];
        string[] replaceLines = ["replaced"];
        const count = searchAndReplaceMemory(fileLines, searchLines, replaceLines, res);
        assert(count == 1);
        assert(res.length == 3);
        assert(res[0] == "hello");
        assert(res[1] == "replaced");
        assert(res[2] == "foo");
    }

    // === Multi-line replace ===
    {
        string[] res;
        string[] fileLines = ["header", "line1", "line2", "line3", "footer"];
        string[] searchLines = ["line1", "line2", "line3"];
        string[] replaceLines = ["replacement"];
        const count = searchAndReplaceMemory(fileLines, searchLines, replaceLines, res);
        assert(count == 1);
        assert(res.length == 3);
        assert(res[0] == "header");
        assert(res[1] == "replacement");
        assert(res[2] == "footer");
    }

    // === Multi-line replace with multi-line replacement ===
    {
        string[] res;
        string[] fileLines = ["a", "b", "c", "d"];
        string[] searchLines = ["b", "c"];
        string[] replaceLines = ["x", "y", "z"];
        const count = searchAndReplaceMemory(fileLines, searchLines, replaceLines, res);
        assert(count == 1);
        assert(res.length == 5);
        assert(res[0] == "a");
        assert(res[1] == "x");
        assert(res[2] == "y");
        assert(res[3] == "z");
        assert(res[4] == "d");
    }

    // === Whitespace tolerance via trimmed equality ===
    {
        string[] res;
        string[] fileLines = ["    foo", "    bar", "baz"];
        string[] searchLines = ["foo", "bar"];
        string[] replaceLines = ["replaced"];
        const count = searchAndReplaceMemory(fileLines, searchLines, replaceLines, res);
        assert(count == 1);
        assert(res.length == 2);
        assert(res[0] == "replaced");
        assert(res[1] == "baz");
    }

    // === Only first occurrence replaced (not all) ===
    {
        string[] res;
        string[] fileLines = ["foo", "bar", "foo", "bar", "end"];
        string[] searchLines = ["foo", "bar"];
        string[] replaceLines = ["replaced"];
        const count = searchAndReplaceMemory(fileLines, searchLines, replaceLines, res);
        assert(count == 1);
        assert(res.length == 4);
        assert(res[0] == "replaced");
        assert(res[1] == "foo");
        assert(res[2] == "bar");
        assert(res[3] == "end");
    }

    // === Replace at start of file ===
    {
        string[] res;
        string[] fileLines = ["foo", "bar", "baz"];
        string[] searchLines = ["foo"];
        string[] replaceLines = ["replaced"];
        const count = searchAndReplaceMemory(fileLines, searchLines, replaceLines, res);
        assert(count == 1);
        assert(res.length == 3);
        assert(res[0] == "replaced");
        assert(res[1] == "bar");
        assert(res[2] == "baz");
    }

    // === Replace at end of file ===
    {
        string[] res;
        string[] fileLines = ["foo", "bar", "baz"];
        string[] searchLines = ["baz"];
        string[] replaceLines = ["replaced"];
        const count = searchAndReplaceMemory(fileLines, searchLines, replaceLines, res);
        assert(count == 1);
        assert(res.length == 3);
        assert(res[0] == "foo");
        assert(res[1] == "bar");
        assert(res[2] == "replaced");
    }

    // === Replace entire file content ===
    {
        string[] res;
        string[] fileLines = ["foo", "bar"];
        string[] searchLines = ["foo", "bar"];
        string[] replaceLines = ["replaced"];
        const count = searchAndReplaceMemory(fileLines, searchLines, replaceLines, res);
        assert(count == 1);
        assert(res.length == 1);
        assert(res[0] == "replaced");
    }

    // === Empty replacement (effectively removes search block) ===
    {
        string[] res;
        string[] fileLines = ["header", "foo", "bar", "footer"];
        string[] searchLines = ["foo", "bar"];
        string[] replaceLines = cast(string[])[];
        const count = searchAndReplaceMemory(fileLines, searchLines, replaceLines, res);
        assert(count == 1);
        assert(res.length == 2);
        assert(res[0] == "header");
        assert(res[1] == "footer");
    }

    // === Search block not found throws ===
    {
        string[] res;
        const count = searchAndReplaceMemory(["hello", "world"], ["notfound"], [
            "replaced"
        ], res);
        assert(count == 0, "search block not found should throw");
    }

    // === Empty search block throws (inherited from findCodeBlock) ===
    {
        string[] res;
        const count = searchAndReplaceMemory(["hello"], cast(string[])[], [
            "replaced"
        ], res);
        assert(count == 0, "empty search block should throw");
    }

    // === Empty file with non-empty search block throws ===
    {
        string[] res;
        const count = searchAndReplaceMemory(cast(string[])[], ["something"], [
            "replaced"
        ], res);
        assert(count == 0, "empty file with non-empty search should throw not found");
    }

    // === Comment false positive prevention (inherited from findCodeBlock) ===
    // Searching for "foo" should NOT match "// call fooBar()"
    {
        string[] res;
        const count = searchAndReplaceMemory(["// call fooBar()", "other"],
                ["foo"], ["replaced"], res);
        assert(count == 0, "should not match comments via trimmed equality");
    }
}

/**
 * Replace all non-overlapping occurrences of a search block with replacement lines.
 *
 * Scans the file from the beginning, finding each occurrence of the search block
 * via findCodeBlock and replacing it with the replacement lines. After each
 * replacement, the search resumes from the line immediately after the replacement
 * (non-overlapping behavior).
 *
 * Parameters:
 *     fileLines:    Array of file lines to search and modify
 *     searchLines:  Array of search lines to match (the code block to find)
 *     replaceLines: Array of replacement lines (used for each match)
 *     result:       Output parameter receiving the modified lines array
 *
 * Returns:
 *     The number of replacements made (always >= 1)
 *
 * Throws:
 *     Exception if searchLines is empty
 *     Exception if zero matches are found in the file
 */
size_t searchAndReplaceAllMemory(string[] fileLines, string[] searchLines,
        string[] replaceLines, ref string[] result) @safe {
    auto app = appender!(string[])();
    size_t pos = 0;
    size_t count = 0;
    auto repl = replaceLines;

    while (pos < fileLines.length) {
        auto r = findCodeBlock(fileLines[pos .. $], searchLines);
        if (!r.found) {
            // No more matches — append remaining lines
            app.put(fileLines[pos .. $]);
            break;
        }

        // r.start and r.end are relative to fileLines[pos .. $]
        // Convert to absolute positions
        auto absStart = pos + r.start;
        auto absEnd = pos + r.end;

        enforce(absEnd > pos, "findCodeBlock returned zero-length match");
        // Lines before the match
        app.put(fileLines[pos .. absStart]);
        // Replacement content
        app.put(repl);

        pos = absEnd;
        count++;
    }

    enforce(count > 0, "Search block not found in file");

    result = app[];
    return count;
}

unittest {
    // === Zero matches throws exception ===
    {
        bool threw = false;
        string[] result;
        try {
            searchAndReplaceAllMemory(["hello", "world"], ["notfound"], [
                "replaced"
            ], result);
        } catch (Exception) {
            threw = true;
        }
        assert(threw, "zero matches should throw");
    }

    // === Single match replaced correctly ===
    {
        string[] fileLines = ["header", "foo", "bar", "footer"];
        string[] searchLines = ["foo", "bar"];
        string[] replaceLines = ["replaced"];
        string[] result;
        auto count = searchAndReplaceAllMemory(fileLines, searchLines, replaceLines, result);
        assert(count == 1);
        assert(result.length == 3);
        assert(result[0] == "header");
        assert(result[1] == "replaced");
        assert(result[2] == "footer");
    }

    // === Multiple matches all replaced ===
    {
        string[] fileLines = ["foo", "bar", "between", "foo", "bar", "end"];
        string[] searchLines = ["foo", "bar"];
        string[] replaceLines = ["replaced"];
        string[] result;
        auto count = searchAndReplaceAllMemory(fileLines, searchLines, replaceLines, result);
        assert(count == 2);
        assert(result.length == 4);
        assert(result[0] == "replaced");
        assert(result[1] == "between");
        assert(result[2] == "replaced");
        assert(result[3] == "end");
    }

    // === Non-overlapping: search continues from after replacement ===
    {
        string[] fileLines = ["foo", "bar", "foo", "bar", "foo", "bar"];
        string[] searchLines = ["foo", "bar"];
        string[] replaceLines = ["x"];
        string[] result;
        auto count = searchAndReplaceAllMemory(fileLines, searchLines, replaceLines, result);
        assert(count == 3);
        assert(result.length == 3);
        assert(result[0] == "x");
        assert(result[1] == "x");
        assert(result[2] == "x");
    }

    // === Multi-line replacement content ===
    {
        string[] fileLines = ["a", "target", "a", "target", "a"];
        string[] searchLines = ["target"];
        string[] replaceLines = ["rep1", "rep2"];
        string[] result;
        auto count = searchAndReplaceAllMemory(fileLines, searchLines, replaceLines, result);
        assert(count == 2);
        assert(result.length == 7);
        assert(result[0] == "a");
        assert(result[1] == "rep1");
        assert(result[2] == "rep2");
        assert(result[3] == "a");
        assert(result[4] == "rep1");
        assert(result[5] == "rep2");
        assert(result[6] == "a");
    }

    // === Whitespace tolerance via trimmed equality ===
    {
        string[] fileLines = [
            "    foo", "    bar", "baz", "    foo", "    bar", "end"
        ];
        string[] searchLines = ["foo", "bar"];
        string[] replaceLines = ["replaced"];
        string[] result;
        auto count = searchAndReplaceAllMemory(fileLines, searchLines, replaceLines, result);
        assert(count == 2);
        assert(result.length == 4);
        assert(result[0] == "replaced");
        assert(result[1] == "baz");
        assert(result[2] == "replaced");
        assert(result[3] == "end");
    }

    // === Replace at start of file ===
    {
        string[] fileLines = ["foo", "bar", "foo", "bar"];
        string[] searchLines = ["foo"];
        string[] replaceLines = ["replaced"];
        string[] result;
        auto count = searchAndReplaceAllMemory(fileLines, searchLines, replaceLines, result);
        assert(count == 2);
        assert(result.length == 4);
        assert(result[0] == "replaced");
        assert(result[1] == "bar");
        assert(result[2] == "replaced");
        assert(result[3] == "bar");
    }

    // === Replace at end of file ===
    {
        string[] fileLines = ["start", "foo", "foo"];
        string[] searchLines = ["foo"];
        string[] replaceLines = ["replaced"];
        string[] result;
        auto count = searchAndReplaceAllMemory(fileLines, searchLines, replaceLines, result);
        assert(count == 2);
        assert(result.length == 3);
        assert(result[0] == "start");
        assert(result[1] == "replaced");
        assert(result[2] == "replaced");
    }

    // === Empty replacement (effectively removes all matches) ===
    {
        string[] fileLines = ["header", "foo", "middle", "foo", "footer"];
        string[] searchLines = ["foo"];
        string[] replaceLines = cast(string[])[];
        string[] result;
        auto count = searchAndReplaceAllMemory(fileLines, searchLines, replaceLines, result);
        assert(count == 2);
        assert(result.length == 3);
        assert(result[0] == "header");
        assert(result[1] == "middle");
        assert(result[2] == "footer");
    }

    // === Empty search block throws (inherited from findCodeBlock) ===
    {
        bool threw = false;
        string[] result;
        try {
            searchAndReplaceAllMemory(["hello"], cast(string[])[], ["replaced"], result);
        } catch (Exception) {
            threw = true;
        }
        assert(threw, "empty search block should throw");
    }

    // === Empty file with non-empty search throws ===
    {
        bool threw = false;
        string[] result;
        try {
            searchAndReplaceAllMemory(cast(string[])[], ["something"], [
                "replaced"
            ], result);
        } catch (Exception) {
            threw = true;
        }
        assert(threw, "empty file with non-empty search should throw not found");
    }

    // === Comment false positive prevention (inherited from findCodeBlock) ===
    {
        bool threw = false;
        string[] result;
        try {
            searchAndReplaceAllMemory(["// call fooBar()", "other"], ["foo"],
                    ["replaced"], result);
        } catch (Exception) {
            threw = true;
        }
        assert(threw, "should not match comments via trimmed equality");
    }

    // === Original array not mutated ===
    {
        string[] fileLines = ["a", "target", "b", "target", "c"];
        string[] original = fileLines.dup;
        string[] searchLines = ["target"];
        string[] replaceLines = ["replaced"];
        string[] result;
        searchAndReplaceAllMemory(fileLines, searchLines, replaceLines, result);
        assert(fileLines == original, "original fileLines should not be mutated");
    }
}

struct EditFileParams {
    @ParamDescription("Path to the file (relative to workarea)")
    string path;

    @ParamDescription("Content to insert; must be empty string for delete mode")
    string content;

    @ParamDescription(`"replace", "remove", or "append"`)
    string mode;

    @ParamDescription("First line of the range (1-based)")
    long startLine;

    @ParamDescription("Number of lines to read")
    long count;
}

@Function("Edit a file by applying a change. " ~ "\"replace\" (replace lines with content), "
        ~ "\"remove\" (remove lines, content must be empty string), "
        ~ "\"append\" (insert content after the startLine). \n"
        ~ "startLine and count are ignored when mode is \"append\".")
ExecuteFuncResult editFile(Context baseCtx, EditFileParams params) {
    mixin(baseContextToSpecific!FileContext);

    auto editFileMaxLines = ctx.getToolLimits().editFileMaxLines;
    if (editFileMaxLines <= 0) {
        logger.warningf("editFileMaxLines is %s, falling back to default %s",
                editFileMaxLines, MaxLines * 4);
        editFileMaxLines = MaxLines * 4;
    }

    auto path_ = pathToWorkarea(ctx, params.path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    try {
        const mode_ = parseEditMode(params.mode);
        if (mode_ == EditMode.insert_before || mode_ == EditMode.insert_after) {
            return ExecuteFuncResult(i"error: mode '$(params.mode)' is not supported by editFile. Use editByMarker instead"
                    .text, success: false);
        }
        if (mode_ == EditMode.remove && !params.content.empty) {
            return ExecuteFuncResult("error: parameter mode is 'remove' but content is not empty",
                    success: false);
        }
        if (mode_ == EditMode.append) {
            params.count = 1; // fix bug where if count is >1 it removes lines
        } else {
            if (auto err = validateLineRange(params.startLine, params.count, editFileMaxLines)) {
                return ExecuteFuncResult(err, success: false);
            }
        }

        auto fileLines = File(path_.toString);
        auto res = editFileMemory(fileLines.byLine, mode_, params.content,
                params.startLine, params.count);
        fileLines.close;

        writeLines(path_, res);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: failed to edit $(params.count) lines starting at line $(
                params.startLine) in file '$(params.path)' with mode $(params.mode): $(e.msg)".text,
                success: false);
    }
    return ExecuteFuncResult("OK", success: true);
}

struct EditFileByMarkerParams {
    @ParamDescription("Path to the file (relative to workarea)")
    string path;

    @ParamDescription("Content to insert (must be empty for remove mode)")
    string content;

    @ParamDescription(
            "Edit mode: replace, remove, append, insert_before/insert-before, or insert_after/insert-after")
    string mode;

    @ParamDescription("The text to search for (case-sensitive substring match)")
    string marker;

    @ParamDescription("Preview the result without writing to disk")
    @ParamOptional bool dryRun;
}

@Function("Edit a file by finding a marker line and applying an edit mode. "
        ~ "Finds the first line containing the marker string and applies the specified mode. "
        ~ "Modes: replace (replace marker line with content), remove (delete marker line, content must be empty), "
        ~ "append (keep marker line, add content after), insert_after (same as append), "
        ~ "insert_before (add content before marker line, keep marker line). "
        ~ "Returns a JSON object with fields: "
        ~ "matchedAt (1-based line number where marker was found), linesChanged (int, delta of added minus removed lines). "
        ~ " When dryRun is true, also includes: preview (string).")
ExecuteFuncResult editFileByMarker(Context baseCtx, EditFileByMarkerParams params) {
    mixin(baseContextToSpecific!FileContext);

    auto path_ = pathToWorkarea(ctx, params.path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }
    if (params.marker.empty) {
        return ExecuteFuncResult("error: marker must not be empty", success: false);
    }

    EditMode mode_;
    try {
        mode_ = parseEditMode(params.mode);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: invalid mode '$(params.mode)': $(e.msg)".text,
                success: false);
    }

    if (mode_ == EditMode.remove && params.content.length > 0) {
        return ExecuteFuncResult("error: parameter mode is 'remove' but content is not empty",
                success: false);
    }

    try {
        auto fileLines = File(path_.toString).byLineCopy.array;
        const markerIndex = findMarkerLine(fileLines, params.marker);

        auto res = editFileByMarkerMemory(fileLines, mode_, params.content, params.marker);

        auto json = JSONValue.emptyObject;
        json["matchedAt"] = markerIndex + 1;
        json["linesChanged"] = cast(long) res.length - cast(long) fileLines.length;

        if (params.dryRun) {
            json["preview"] = res.join("\n");
        } else {
            writeLines(path_, res);
        }
        return ExecuteFuncResult(json.toString(JSONOptions.doNotEscapeSlashes), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: failed to edit file '$(params.path)' by marker '$(
                params.marker)' with mode '$(params.mode)': $(e.msg)".text, success: false);
    }
}

struct SearchAndReplaceParams {
    @ParamDescription("Path to the file (relative to workarea)")
    string path;

    @ParamDescription(
            "The text block to search for (trimmed equality matching, empty lines are flexible)")
    string searchContent;

    @ParamDescription("The replacement text (replaces the matched block)")
    string replaceContent;

    @ParamDescription("Replace all non-overlapping occurrences of a search block with replacement content. After each replacement, the search resumes from the line after the replacement (non-overlapping)")
    @ParamOptional bool replaceAll;

    @ParamDescription("Preview the result without writing to disk")
    @ParamOptional bool dryRun;
}

@Function("Replace the first occurrence, if replaceAll is false, of a search block with replacement content. Uses trimmed equality matching: each non-empty search line must match a file line after stripping whitespace. Empty search lines are flexible (they don't need to match). Returns a JSON object with fields: replacements (int, number of replacements made). When dryRun is true, also includes: preview (string), matchedAt (1-based line number of first matched line), linesChanged (int, delta of added minus removed lines).")
ExecuteFuncResult searchAndReplace(Context baseCtx, SearchAndReplaceParams params) {
    mixin(baseContextToSpecific!FileContext);

    auto path_ = pathToWorkarea(ctx, params.path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }
    if (params.searchContent.empty) {
        return ExecuteFuncResult("error: searchContent must not be empty", success: false);
    }

    // Use chomp to prevent splitLines from creating a trailing empty element
    auto searchLines = params.searchContent.chomp.splitLines;

    // Check for whitespace-only search content (mirrors findCodeBlock's check)
    bool hasNonEmptyLine = false;
    foreach (line; searchLines) {
        if (line.strip.length > 0) {
            hasNonEmptyLine = true;
            break;
        }
    }
    if (!hasNonEmptyLine) {
        return ExecuteFuncResult(
                "error: searchContent must contain at least one non-empty line (all lines are empty or whitespace)",
                success: false);
    }

    try {
        auto replaceLines = params.replaceContent.length > 0
            ? params.replaceContent.chomp.splitLines : null;
        auto fileLines = File(path_.toString).byLineCopy.array;

        string[] res;
        ulong count;
        if (params.replaceAll) {
            count = searchAndReplaceAllMemory(fileLines, searchLines, replaceLines, res);
        } else {
            count = searchAndReplaceMemory(fileLines, searchLines, replaceLines, res);
        }

        if (count == 0) {
            return ExecuteFuncResult(i"error: search block not found in file '$(params.path)'".text,
                    success: false);
        }

        auto json = JSONValue.emptyObject;

        if (params.dryRun) {
            if (params.replaceAll) {
                return ExecuteFuncResult("error: dryRun and replaceAll cannot be used together",
                        success: false);
            }
            auto range = findCodeBlock(fileLines, searchLines);
            json["matchedAt"] = cast(long)(range.start + 1);
            json["linesChanged"] = cast(long) res.length - cast(long) fileLines.length;
            json["preview"] = res.join("\n");
        } else {
            writeLines(path_, res);
        }

        json["replacements"] = count;
        return ExecuteFuncResult(json.toString(JSONOptions.doNotEscapeSlashes), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: failed to search and replace in file '$(params.path)': $(
                e.msg)".text, success: false);
    }
}

string[] editFileMemory(RangeT)(RangeT fileLines, EditMode mode, string content,
        long startLine, long count) @trusted {
    --startLine;
    long endLine = startLine + count;
    auto lines = appender!(string[])();
    foreach (txtLine; fileLines.enumerate) {
        if (txtLine.index >= startLine && txtLine.index < endLine) {
            final switch (mode) {
            case EditMode.replace:
                if (txtLine.index == startLine)
                    lines.put(content);
                break;
            case EditMode.remove:
                break;
            case EditMode.append:
                if (txtLine.index == startLine) {
                    lines.put(txtLine.value.idup);
                    lines.put(content);
                }
                break;
            case EditMode.insert_before:
            case EditMode.insert_after:
                // Reserved for future marker-based editing tools (editByMarker)
                enforce(false,
                        "insert_before/insert_after modes are not supported by editFile; use marker-based editing");
                break;
            }
        } else {
            lines.put(txtLine.value.idup);
        }
    }
    return lines[];
}

unittest {
    auto lines = ["hello", "world", "world", "is", "beautiful"];

    auto res = editFileMemory(lines, EditMode.replace, "earth", 2, 1);
    assert(res.length == 5, res.length.to!string);
    assert(res[1] == "earth", res.to!string);
    assert(res[2] == "world", res.to!string);

    res = editFileMemory(lines, EditMode.remove, null, 2, 1);
    assert(res.length == 4, res.length.to!string);
    assert(res[1] == "world", res.to!string);
    assert(res[2] == "is", res.to!string);

    res = editFileMemory(lines, EditMode.append, "cat", 2, 1);
    assert(res.length == 6, res.to!string);
    assert(res[0] == "hello", res.to!string);
    assert(res[1] == "world", res.to!string);
    assert(res[2] == "cat", res.to!string);
    assert(res[3] == "world", res.to!string);
}

struct ApplyDiffParams {
    @ParamDescription("Path to the file (relative to workarea)")
    string path;

    @ParamDescription("Unified diff patch to apply")
    string diff;

    @ParamDescription("Preview the result without writing to disk")
    @ParamOptional bool dryRun;
}

@Function("Apply a unified diff patch to a file. " ~ "Diff: each hunk starts with `@@ -oldStart[,oldCount] +newStart[,newCount] @@`, followed by lines starting " ~ "with ' ' (context), '-' (remove) or '+' (add). The diff must match the *exact* " ~ "current content of the file; use `readFile` first to obtain it. " ~ "Returns a JSON object with fields: linesChanged (int). When dryRun is true, also includes: preview (string), linesChanged (int, delta of added minus removed lines).")
ExecuteFuncResult applyDiff(Context baseCtx, ApplyDiffParams params) {
    mixin(baseContextToSpecific!FileContext);

    auto path_ = pathToWorkarea(ctx, params.path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }
    if (params.diff.empty) {
        return ExecuteFuncResult("error: diff must not be empty", success: false);
    }
    try {
        auto fileLines = File(path_.toString).byLineCopy.array;
        auto diffLines = params.diff.splitLines.filter!(a => !a.empty).array;
        auto result = applyDiffMemory(fileLines, diffLines);

        auto json = JSONValue.emptyObject;
        if (params.dryRun) {
            json["preview"] = result.join("\n");
            json["linesChanged"] = cast(long) result.length - cast(long) fileLines.length;
        } else {
            writeLines(path_, result);
        }

        return ExecuteFuncResult(json.toString(JSONOptions.doNotEscapeSlashes), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: failed applying diff to file '$(params.path)': $(e.msg)".text,
                success: false);
    }
}

string[] applyDiffMemory(string[] fileLines, string[] diffLines) @safe {
    size_t fileIdx = 0; // current position in fileLines (0‑based)
    string[] result;
    size_t lineIdx = 0;

    // 1. Skip leading --- / +++ headers
    while (lineIdx < diffLines.length && (diffLines[lineIdx].startsWith("---")
            || diffLines[lineIdx].startsWith("+++"))) {
        lineIdx++;
    }

    // 2. Must start with a hunk header after headers
    if (lineIdx >= diffLines.length || !diffLines[lineIdx].startsWith("@@"))
        throw new Exception("Diff does not contain any hunk header (@@ ... @@)");

    while (lineIdx < diffLines.length) {
        auto line = diffLines[lineIdx];
        if (!line.startsWith("@@"))
            throw new Exception(i"Expected hunk header but got: $(line)".text);

        // Parse header: "@@ -oldStart[,oldCount] +newStart[,newCount] @@"
        auto secondAt = line.indexOf("@@", 2);
        enforce(secondAt != -1, i"Invalid hunk header (missing closing @@): $(line)".text);
        auto header = line[2 .. secondAt].strip;
        auto parts = header.split;
        enforce(parts.length >= 2, i"Invalid hunk header format: $(line)".text);

        // Old range
        auto oldRange = parts[0];
        enforce(oldRange.startsWith("-"), i"Old range must start with '-': $(line)".text);
        oldRange = oldRange[1 .. $];
        long oldStart;
        size_t oldCount;
        if (oldRange.indexOf(',') != -1) {
            auto rp = oldRange.split(",");
            oldStart = rp[0].to!long;
            oldCount = rp[1].to!size_t;
        } else {
            oldStart = oldRange.to!long;
            oldCount = 1;
        }
        size_t oldPos = cast(size_t)(oldStart - 1); // 0‑based index in fileLines

        // New range
        auto newRange = parts[1];
        enforce(newRange.startsWith("+"), i"New range must start with '+': $(line)".text);
        newRange = newRange[1 .. $];
        long newStart;
        size_t newCount;
        if (newRange.indexOf(',') != -1) {
            auto rp = newRange.split(",");
            newStart = rp[0].to!long;
            newCount = rp[1].to!size_t;
        } else {
            newStart = newRange.to!long;
            newCount = 1;
        }
        if (oldPos < fileIdx)
            throw new Exception(i"Hunk tries to go backward (oldStart=$(oldStart), current file position=$(
                    fileIdx + 1))".text);

        // Copy lines from current position up to the start of this hunk
        while (fileIdx < oldPos) {
            if (fileIdx >= fileLines.length) {
                logger.tracef("Unexpected error. fileIdx:%s fileLines:%s\nfileLines:%s\ndiffLines:%s",
                        fileIdx, fileLines.length, fileLines, diffLines);
                throw new Exception("Unexpected error. Unable to apply diff");
            }
            result ~= fileLines[fileIdx];
            fileIdx++;
        }

        lineIdx++; // consume the '@@' line
        size_t processedOld = 0; // count of '-' and ' ' lines in this hunk
        size_t processedNew = 0; // count of '+' and ' ' lines in this hunk

        // Process hunk body lines
        while (lineIdx < diffLines.length && !diffLines[lineIdx].startsWith("@@")
                && !diffLines[lineIdx].startsWith("---") && !diffLines[lineIdx].startsWith("+++")) {

            auto hunkLine = diffLines[lineIdx];
            if (hunkLine.empty) {
                lineIdx++;
                continue;
            }

            auto firstChar = hunkLine[0];
            auto content = hunkLine[1 .. $];

            switch (firstChar) {
            case ' ': // context line - present in both old and new
                enforce(fileIdx < fileLines.length, i"Unexpected end of file at line $(fileIdx + 1) (hunk context)"
                        .text);
                enforce(fileLines[fileIdx] == content, i"Context mismatch at line $(fileIdx + 1): expected '$(
                        content)' but found '$(fileLines[fileIdx])'".text);
                result ~= fileLines[fileIdx];
                fileIdx++;
                processedOld++;
                processedNew++;
                break;
            case '-': // removal line (consumed from old file, not added to result)
                fileIdx++;
                processedOld++;
                break;
            case '+': // addition line (goes into result, does not consume old file line)
                result ~= content;
                processedNew++;
                break;
            default:
                throw new Exception(i"Invalid hunk line (must start with ' ', '-' or '+'): $(
                        hunkLine)".text);
            }
            lineIdx++;
        }

        // Validate the counts declared in the header
        enforce(processedOld == oldCount,
                i"Hunk @@ -$(oldStart),$(oldCount) +$(newStart),$(newCount) @@ consumes $(
                    processedOld) old lines but header declares $(oldCount)".text);
        enforce(processedNew == newCount,
                i"Hunk @@ -$(oldStart),$(oldCount) +$(newStart),$(newCount) @@ produces $(
                    processedNew) new lines but header declares $(newCount)".text);
    }

    // Append remaining file lines after the last hunk
    while (fileIdx < fileLines.length) {
        result ~= fileLines[fileIdx];
        fileIdx++;
    }

    return result;
}

unittest {
    // Multi-hunk test: lines between hunks must be preserved
    // This reproduces the bug where fileLineIdx is reset at each hunk,
    // skipping lines between hunks
    auto content2 = [
        `writeln("Line 1");`, `writeln("Line 2");`, `writeln("Line 3");`,
        `writeln("Line 4");`, `writeln("Line 5");`, `writeln("Line 6");`,
        `writeln("Line 7");`, `writeln("Line 8");`, `writeln("Line 9");`,
        `writeln("Line 10");`
    ];

    // Hunk 1: modify only line 4 (1 line)
    // Hunk 2: modify only line 8 (1 line)
    // Lines 5, 6, 7 are between hunks and must be preserved
    auto diff2 = [
        "--- old.txt", "+++ new.txt", "@@ -4,1 +4,1 @@", `-writeln("Line 4");`,
        `+writeln("Line 44");`, "@@ -8,1 +8,1 @@", `-writeln("Line 8");`,
        `+writeln("Line 88");`,
    ];

    auto result2 = applyDiffMemory(content2, diff2);

    // Expected: all 10 lines with lines 4 and 8 modified
    assert(result2.length == 10,
            "Multi-hunk: expected 10 lines but got " ~ result2.length.to!string);
    assert(result2[0] == `writeln("Line 1");`);
    assert(result2[1] == `writeln("Line 2");`);
    assert(result2[2] == `writeln("Line 3");`);
    assert(result2[3] == `writeln("Line 44");`);
    assert(result2[4] == `writeln("Line 5");`);
    assert(result2[5] == `writeln("Line 6");`);
    assert(result2[6] == `writeln("Line 7");`);
    assert(result2[7] == `writeln("Line 88");`);
    assert(result2[8] == `writeln("Line 9");`);
    assert(result2[9] == `writeln("Line 10");`);
}

// Test: context lines with additions (Bug #1: processedNew must count context lines)
unittest {
    // dfmt off
    auto content = [
        "int main() {",
        "    int a = 1;",
        "    int b = 2;",
        "    int c = 3;",
        "    return 0;",
        "}"
    ];

    // Patch from @@ -1,6 +1,6 @@ — only one line changed, rest is context
    auto diff = [
        "--- a/test.c",
        "+++ b/test.c",
        "@@ -1,6 +1,6 @@",
        " int main() {",
        "-    int a = 1;",
        "+    int a = 10;",
        "     int b = 2;",
        "     int c = 3;",
        "     return 0;",
        " }",
    ];
    // dfmt on

    auto result = applyDiffMemory(content, diff);
    assert(result.length == 6,
            "Context+additions: expected 6 lines but got " ~ result.length.to!string);
    assert(result[0] == "int main() {");
    assert(result[1] == "    int a = 10;");
    assert(result[2] == "    int b = 2;");
    assert(result[3] == "    int c = 3;");
    assert(result[4] == "    return 0;");
    assert(result[5] == "}");
}

// Test: context lines with removals
unittest {
    // dfmt off
    auto content = [
        "int main() {",
        "    int a = 1;",
        "    int b = 2;",
        "    int c = 3;",
        "    return 0;",
        "}"
    ];

    // Remove one line, rest is context
    auto diff = [
        "--- a/test.c",
        "+++ b/test.c",
        "@@ -1,6 +1,5 @@",
        " int main() {",
        "-    int a = 1;",
        "     int b = 2;",
        "     int c = 3;",
        "     return 0;",
        " }",
    ];
    // dfmt on

    auto result = applyDiffMemory(content, diff);
    assert(result.length == 5,
            "Context+removals: expected 5 lines but got " ~ result.length.to!string);
    assert(result[0] == "int main() {");
    assert(result[1] == "    int b = 2;");
    assert(result[2] == "    int c = 3;");
    assert(result[3] == "    return 0;");
    assert(result[4] == "}");
}

// Test: context-only patch (no additions or removals, just verifying context matches)
unittest {
    auto content = ["line1", "line2", "line3",];

    auto diff = [
        "--- a/test.txt", "+++ b/test.txt", "@@ -1,3 +1,3 @@", " line1", " line2",
        " line3",
    ];

    auto result = applyDiffMemory(content, diff);
    assert(result.length == 3, "Context-only: expected 3 lines but got " ~ result.length.to!string);
    assert(result[0] == "line1");
    assert(result[1] == "line2");
    assert(result[2] == "line3");
}

unittest {
    // Test: deletion only (no context lines)
    auto content3 = [`Line A`, `Line B`, `Line C`, `Line D`, `Line E`];
    auto diff3 = ["--- old.txt", "+++ new.txt", "@@ -3,1 +2,0 @@", `-Line C`];
    auto result3 = applyDiffMemory(content3, diff3);
    assert(result3.length == 4, "Delete: expected 4 lines but got " ~ result3.length.to!string);
    assert(result3[0] == `Line A`);
    assert(result3[1] == `Line B`, result3[1]);
    assert(result3[2] == `Line D`);
    assert(result3[3] == `Line E`);
}

unittest {
    // Test: multiple additions in one hunk
    auto content4 = [`A`, `B`, `C`];
    auto diff4 = [
        "--- old.txt", "+++ new.txt", "@@ -1,1 +1,3 @@", `-A`, `+A1`, `+A2`, `+A3`,
    ];
    auto result4 = applyDiffMemory(content4, diff4);
    assert(result4.length == 5, "Additions: expected 5 lines but got " ~ result4.length.to!string);
    assert(result4[0] == `A1`);
    assert(result4[1] == `A2`);
    assert(result4[2] == `A3`);
    assert(result4[3] == `B`);
    assert(result4[4] == `C`);
}

unittest {
    // Test: hunk header declares 3 old lines, but only 2 removal lines are present.
    // This simulates an LLM that forgets a context or deletion line.
    auto content = ["line1", "line2", "line3", "line4", "line5"];

    auto diff = [
        "--- a.txt", "+++ b.txt", "@@ -2,3 +2,2 @@", // expects 3 old lines (line2, line3, line4)
        "-line2", // only two '-' lines supplied
        "-line3", "+newline", "@@ -5,1 +5,1 @@", " line5"
    ];

    // The fixed function must throw because processedOld (2) != oldCount (3)

    try {
        auto res = applyDiffMemory(content, diff);
        assert(false, "should have thrown an exception: " ~ res.to!string);
    } catch (Exception e) {
        assert(e.msg.indexOf("consumes") != -1 && e.msg.indexOf("old lines") != -1,
                "Error message must indicate old line count mismatch, but got: " ~ e.msg);
    }
}

struct ListDirectoryParams {
    @ParamDescription("Path to the directory")
    string path;

    @ParamDescription("Set to true for recursive scan")
    @ParamOptional bool recursive;
}

@Function("List files in directory as JSON array of paths, types and sizes. Up to max entries are returned for recursive scan or error.")
ExecuteFuncResult listDirectory(Context baseCtx, ListDirectoryParams params) {
    mixin(baseContextToSpecific!FileContext);

    auto maxDirEntries = ctx.getToolLimits().maxDirEntries;

    auto path_ = pathToWorkarea(ctx, params.path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    try {
        JSONValue[] rval;
        foreach (a; dirEntries(path_.toString, params.recursive ? SpanMode.depth : SpanMode.shallow)) {
            if (params.recursive && rval.length > maxDirEntries) {
                return ExecuteFuncResult(i"error: failed listing directory recursive: more than $(
                        maxDirEntries) entries in the result".text, success: false);
            }

            auto e = JSONValue.emptyObject;
            e["path"] = a.name.relativePath(ctx.workArea.toString).JSONValue;
            e["type"] = (a.isDir ? "dir" : "file").JSONValue;
            if (a.isFile)
                e["size"] = a.size.JSONValue;
            rval ~= e;
        }
        return ExecuteFuncResult(JSONValue(rval)
                .toString(JSONOptions.doNotEscapeSlashes), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: failed listing directory '$(params.path)': $(e.msg)".text,
                success: false);
    }
}

struct GrepFilesParams {
    @ParamDescription("Path to search in")
    string path;

    @ParamDescription("Pattern to search for")
    string pattern;

    @ParamDescription("Maximum number of results to return")
    @ParamOptional long maxResults = 20;
}

@Function(
        "Search for a pattern in files at path. Returns up to maxResults matching lines with file and line number")
ExecuteFuncResult grepFiles(Context baseCtx, GrepFilesParams params) {
    mixin(baseContextToSpecific!FileContext);

    auto grepMaxResults = ctx.getToolLimits().grepMaxResults;

    if (params.maxResults > grepMaxResults) {
        return ExecuteFuncResult(i"error: requested maxResults $(params.maxResults) exceeds limit $(
                grepMaxResults)".text, success: false);
    }
    if (params.maxResults < 1) {
        return ExecuteFuncResult("error: maxResults must be >= 1", success: false);
    }

    auto path_ = pathToWorkarea(ctx, params.path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    auto cmd = [
        "grep", "-rn", "-m", params.maxResults.to!string, "-E", params.pattern,
        path_.toString
    ];
    auto result = execute(cmd);
    if (result.status == 0) {
        string rval = result.output.strip.replace(path_.toString, params.path);
        const results = rval.splitter('\n').count;
        if (results > grepMaxResults) {
            return ExecuteFuncResult(format!"error: %s results exceeds max allowed %s"(results,
                    grepMaxResults), success: false);
        }
        if (rval.empty) {
            return ExecuteFuncResult(i"error: no matches found searching in path '$(params.path)' with pattern '$(
                    params.pattern)'".text, success: false);
        }
        return ExecuteFuncResult(rval, success: true);
    }
    return ExecuteFuncResult(format!"error: failed to execute '%(%-s %)': %s"(
            cmd[0 .. $ - 1] ~ params.path, result.output.strip), success: false);
}

struct CountLinesInFileParams {
    @ParamDescription("Path to the file")
    string path;
}

@Function("Count number of lines in file. Return number or error message")
ExecuteFuncResult countLinesInFile(Context baseCtx, CountLinesInFileParams params) {
    mixin(baseContextToSpecific!FileContext);

    auto path_ = pathToWorkarea(ctx, params.path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    try {
        return ExecuteFuncResult(File(path_).byLine.count.to!string, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: $(e.msg)".text, success: false);
    }
}

struct Md5HashFileParams {
    @ParamDescription("Path to the file")
    string path;
}

@Function("Calculate the MD5 hash of a file. Returns a hexadecimal string.")
ExecuteFuncResult md5HashFile(Context baseCtx, Md5HashFileParams params) {
    import std.base64 : Base64;
    import std.digest : toHexString;
    import std.digest.md : md5Of;
    import std.file : read;

    mixin(baseContextToSpecific!FileContext);

    auto path_ = pathToWorkarea(ctx, params.path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    try {
        auto content = read(path_.toString);
        return ExecuteFuncResult(content.md5Of.toHexString.idup, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: $(e.msg)".text, success: false);
    }
}

interface VisionContext : Context {
    bool isPathInsideWorkArea(AbsolutePath path);
    AbsolutePath workArea();
    bool addVisionImage(AbsolutePath path, string query) nothrow;
}

struct LoadImageApiParams {
    @ParamDescription("Path to the image file")
    string path;

    @ParamDescription("Query to include with the image message")
    @ParamOptional string query;
}

// TODO: update supported formats by checking what stb_image supports.
@Function("Load an image from path into the vision context for interpretation. Supported formats are jpg, png, bmp, gif. The image will be attached to the next user message. The query will be part of the image message.")
ExecuteFuncResult loadImageApi(Context baseCtx, LoadImageApiParams params) {
    mixin(baseContextToSpecific!VisionContext);

    auto path_ = pathToWorkarea(ctx, params.path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    try {
        if (ctx.addVisionImage(path_, params.query)) {
            return ExecuteFuncResult(i"image loaded from '$(params.path)'".text, success: true);
        }
        return ExecuteFuncResult(i"error: failed to load image '$(params.path)'".text,
                success: false);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: failed to load image '$(params.path)': $(e.msg)".text,
                success: false);
    }
}

private:

void writeLines(AbsolutePath path, string[] lines) {
    auto f = File(path.toString, "w");
    foreach (line; lines)
        f.writeln(line);
}

string validateLineRange(long startLine, long count, long maxLines) {
    if (startLine < 1)
        return format!"error: parameter startLine %s must be > 0"(startLine);
    if (count < 1)
        return format!"error: parameter count %s must be > 0"(count);
    if (count > maxLines)
        return format!"error: tried to access %s lines but %s is max"(count, maxLines);
    return null;
}

// ============================================================================
// Integration Tests for New File Editing Tools
// ============================================================================

// Mock FileContext for integration tests
class TestFileContext : FileContext {
    import std.file;

    AbsolutePath workAreaDir;

    this(string dir) {
        workAreaDir = dir.AbsolutePath;
        mkdirRecurse(workAreaDir.toString);
    }

    void teardown() {
        import std.file : rmdirRecurse;

        try {
            rmdirRecurse(workAreaDir.toString);
        } catch (Exception) {
            // Silently ignore cleanup failures during test teardown
        }
    }

    bool isPathInsideWorkArea(AbsolutePath path) {
        auto p = path.toString;
        auto w = workAreaDir.toString;
        return p.startsWith(w) && (p.length == w.length || p[w.length] == '/');
    }

    AbsolutePath workArea() {
        return workAreaDir;
    }

    ToolLimits getToolLimits() {
        return ToolLimits();
    }
}

// Helper: create a test file with given content
void createTestFile(TestFileContext ctx, string relativePath, string content) {
    assert(!relativePath.startsWith("/"), "relativePath must not be absolute: " ~ relativePath);
    auto fullPath = (ctx.workArea ~ relativePath).AbsolutePath;
    mkdirRecurse(fullPath.dirName.toString);
    auto f = File(fullPath.toString, "w");
    f.write(content);
}

// Helper: read file content for verification
string readTestFile(TestFileContext ctx, string relativePath) {
    auto fullPath = (ctx.workArea ~ relativePath).AbsolutePath;
    return readText(fullPath.toString);
}

// Helper: create a test context with a unique directory name
TestFileContext makeTestContext(string testName) {
    import std.datetime : Clock;

    return new TestFileContext("./llmfun_test/" ~ testName ~ "_" ~ Clock.currTime.toString);
}

// ----------------------------------------------------------------------------
// editFileByMarker Integration Tests
// ----------------------------------------------------------------------------

unittest {
    // Integration test: editFileByMarker - replace mode
    auto ctx = makeTestContext("marker_replace");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nhello world\nline3\n");

    auto result = editFileByMarker(ctx, EditFileByMarkerParams("test.txt",
            "replaced", "replace", "hello world"));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "line1\nreplaced\nline3\n", content);
}

unittest {
    // Integration test: editFileByMarker - remove mode
    auto ctx = makeTestContext("marker_remove");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nhello world\nline3\n");

    auto result = editFileByMarker(ctx, EditFileByMarkerParams("test.txt", "",
            "remove", "hello world"));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "line1\nline3\n", content);
}

unittest {
    // Integration test: editFileByMarker - insert_before mode
    auto ctx = makeTestContext("marker_insertbefore");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nhello world\nline3\n");

    auto result = editFileByMarker(ctx, EditFileByMarkerParams("test.txt",
            "inserted", "insert-before", "hello world"));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "line1\ninserted\nhello world\nline3\n", content);
}

unittest {
    // Integration test: editFileByMarker - insert_after mode
    auto ctx = makeTestContext("marker_insertafter");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nhello world\nline3\n");

    auto result = editFileByMarker(ctx, EditFileByMarkerParams("test.txt",
            "inserted", "insert-after", "hello world"));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "line1\nhello world\ninserted\nline3\n", content);
}

unittest {
    // Integration test: editFileByMarker - multi-line content
    auto ctx = makeTestContext("marker_multiline");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nmarker\nline3\n");

    auto result = editFileByMarker(ctx, EditFileByMarkerParams("test.txt",
            "newA\nnewB\nnewC", "replace", "marker"));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "line1\nnewA\nnewB\nnewC\nline3\n", content);
}

unittest {
    // Integration test: editFileByMarker - error: marker not found
    auto ctx = makeTestContext("marker_notfound");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nline2\n");

    auto result = editFileByMarker(ctx, EditFileByMarkerParams("test.txt", "x",
            "replace", "notfound"));
    assert(!result.success);
    assert(result.msg.canFind("not found"), result.msg);
}

unittest {
    // Integration test: editFileByMarker - error: empty marker
    auto ctx = makeTestContext("marker_empty");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFileByMarker(ctx, EditFileByMarkerParams("test.txt", "x", "replace", ""));
    assert(!result.success);
    assert(result.msg.canFind("empty"), result.msg);
}

unittest {
    // Integration test: editFileByMarker - error: invalid path
    auto ctx = makeTestContext("marker_invalidpath");
    scope (exit)
        ctx.teardown;

    auto result = editFileByMarker(ctx,
            EditFileByMarkerParams("nonexistent.txt", "x", "replace", "marker"));
    assert(!result.success);
}

// ----------------------------------------------------------------------------
// editFileByMarkerDryRun Integration Tests
// ----------------------------------------------------------------------------

unittest {
    // Integration test: editFileByMarkerDryRun - does NOT modify file
    auto ctx = makeTestContext("markerdryrun");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nhello world\nline3\n");

    auto result = editFileByMarker(ctx, EditFileByMarkerParams("test.txt",
            "replaced", "replace", "hello world", true));
    assert(result.success, result.msg);

    // Verify file was NOT modified
    auto content = readTestFile(ctx, "test.txt");
    assert(content == "line1\nhello world\nline3\n", content);

    // Verify preview contains modified content
    auto json = parseJSON(result.msg);
    assert(json["preview"].str.canFind("replaced"));
    assert(json["matchedAt"].integer == 2);
}

unittest {
    // Integration test: editFileByMarkerDryRun - returns correct preview fields
    auto ctx = makeTestContext("markerdryrun_fields");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nhello world\nline3\n");

    auto result = editFileByMarker(ctx, EditFileByMarkerParams("test.txt",
            "new1\nnew2", "replace", "hello world", true));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["preview"].str.canFind("new1"));
    assert(json["preview"].str.canFind("new2"));
    assert(json["matchedAt"].integer == 2);
}

// ----------------------------------------------------------------------------
// searchAndReplace Integration Tests
// ----------------------------------------------------------------------------

unittest {
    // Integration test: searchAndReplace - single line replace
    auto ctx = makeTestContext("sar_single");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nfunction foo() {\n    return 1;\n}\nline5\n");

    auto result = searchAndReplace(ctx, SearchAndReplaceParams("test.txt",
            "function foo() {\n    return 1;\n}", "function bar() {\n    return 2;\n}"));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content.canFind("function bar()"));
    assert(content.canFind("return 2"));
    assert(!content.canFind("function foo()"));
}

unittest {
    // Integration test: searchAndReplace - trimmed equality (whitespace tolerance)
    auto ctx = makeTestContext("sar_trimmed");
    scope (exit)
        ctx.teardown;

    // File has 4-space indentation
    createTestFile(ctx, "test.txt", "    int x = 1;\n    int y = 2;\n");

    // Search with tab indentation - should still match via trimmed equality
    auto result = searchAndReplace(ctx, SearchAndReplaceParams("test.txt",
            "\tint x = 1;", "    int x = 100;"));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content.canFind("int x = 100"), content);
}

unittest {
    // Integration test: searchAndReplace - first match only
    auto ctx = makeTestContext("sar_firstonly");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "foo\nbar\nfoo\nbaz\n");

    auto result = searchAndReplace(ctx, SearchAndReplaceParams("test.txt", "foo", "replaced"));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "replaced\nbar\nfoo\nbaz\n", content);
}

unittest {
    // Integration test: searchAndReplace - error: search not found
    auto ctx = makeTestContext("sar_notfound");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nline2\n");

    auto result = searchAndReplace(ctx, SearchAndReplaceParams("test.txt",
            "notfound", "replacement"));
    assert(!result.success);
    assert(result.msg.canFind("not found"), result.msg);
}

unittest {
    // Integration test: searchAndReplace - error: empty search content
    auto ctx = makeTestContext("sar_empty");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = searchAndReplace(ctx, SearchAndReplaceParams("test.txt", "", "replacement"));
    assert(!result.success);
    assert(result.msg.canFind("empty"), result.msg);
}

// ----------------------------------------------------------------------------
// searchAndReplaceAll Integration Tests
// ----------------------------------------------------------------------------

unittest {
    // Integration test: searchAndReplaceAll - multiple replacements
    auto ctx = makeTestContext("sarall_multi");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "foo\nbar\nfoo\nbaz\nfoo\n");

    auto result = searchAndReplace(ctx, SearchAndReplaceParams("test.txt",
            "foo", "replaced", true, false));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["replacements"].integer == 3, json["replacements"].integer.to!string);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "replaced\nbar\nreplaced\nbaz\nreplaced\n", content);
}

unittest {
    // Integration test: searchAndReplaceAll - error: zero matches
    auto ctx = makeTestContext("sarall_zero");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nline2\n");

    auto result = searchAndReplace(ctx, SearchAndReplaceParams("test.txt",
            "notfound", "replacement", true, false));
    assert(!result.success);
    assert(result.msg.canFind("not found"), result.msg);
}

// ----------------------------------------------------------------------------
// searchAndReplaceDryRun Integration Tests
// ----------------------------------------------------------------------------

unittest {
    // Integration test: searchAndReplaceDryRun - does NOT modify file
    auto ctx = makeTestContext("sardryrun");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nfunction foo() {\n    return 1;\n}\nline5\n");

    string original = readTestFile(ctx, "test.txt");

    auto result = searchAndReplace(ctx, SearchAndReplaceParams("test.txt",
            "function foo() {\n    return 1;\n}", "function bar() {\n    return 2;\n}", false, true));
    assert(result.success, result.msg);

    // Verify file was NOT modified
    auto content = readTestFile(ctx, "test.txt");
    assert(content == original, content);

    // Verify preview contains modified content
    auto json = parseJSON(result.msg);
    assert(json["preview"].str.canFind("function bar()"));
    assert(json["matchedAt"].integer == 2);
}

// ----------------------------------------------------------------------------
// applyDiffDryRun Integration Tests
// ----------------------------------------------------------------------------

unittest {
    // Integration test: applyDiffDryRun - does NOT modify file
    auto ctx = makeTestContext("diffdryrun");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nline2\nline3\n");

    string original = readTestFile(ctx, "test.txt");

    string diff = "--- a/test.txt\n+++ b/test.txt\n@@ -1,3 +1,3 @@\n line1\n-line2\n+line2modified\n line3\n";

    auto result = applyDiff(ctx, ApplyDiffParams("test.txt", diff, dryRun: true));
    assert(result.success, result.msg);

    // Verify file was NOT modified
    auto content = readTestFile(ctx, "test.txt");
    assert(content == original, content);

    // Verify preview contains modified content
    auto json = parseJSON(result.msg);
    assert(json["preview"].str.canFind("line2modified"));
    assert(!json["preview"].str.canFind("-line2"));
}

unittest {
    // Integration test: applyDiffDryRun - error: empty diff
    auto ctx = makeTestContext("diffdryrun_empty");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = applyDiff(ctx, ApplyDiffParams("test.txt", "", dryRun: true));
    assert(!result.success);
    assert(result.msg.canFind("empty"), result.msg);
}

// ----------------------------------------------------------------------------
// Workarea Confinement Tests
// ----------------------------------------------------------------------------

unittest {
    // Integration test: workarea confinement - absolute path rejected
    auto ctx = makeTestContext("workarea");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    // Absolute path should be rejected
    auto result = editFileByMarker(ctx, EditFileByMarkerParams("/etc/passwd",
            "x", "replace", "marker"));
    assert(!result.success);
    assert(result.msg.canFind("absolute"), result.msg);
}

unittest {
    // Integration test: workarea confinement - path traversal rejected
    auto ctx = makeTestContext("workarea_traversal");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    // Path traversal should be rejected
    auto result = editFileByMarker(ctx, EditFileByMarkerParams("../test.txt",
            "x", "replace", "marker"));
    assert(!result.success);
    assert(result.msg.canFind("workarea") || result.msg.canFind("outside"), result.msg);
}

// ----------------------------------------------------------------------------
// Error Path Tests
// ----------------------------------------------------------------------------

unittest {
    // Integration test: editFileByMarker - invalid mode
    auto ctx = makeTestContext("invalidmode");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFileByMarker(ctx, EditFileByMarkerParams("test.txt", "x",
            "invalid-mode", "line1"));
    assert(!result.success);
    assert(result.msg.canFind("invalid"), result.msg);
}

unittest {
    // Integration test: searchAndReplace - whitespace-only search content
    auto ctx = makeTestContext("whitespaceonly");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = searchAndReplace(ctx, SearchAndReplaceParams("test.txt",
            "   \n\n   ", "replacement"));
    assert(!result.success);
    assert(result.msg.canFind("non-empty"), result.msg);
}

unittest {
    // Integration test: editFileByMarker - remove mode with non-empty content
    auto ctx = makeTestContext("remove_content");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFileByMarker(ctx, EditFileByMarkerParams("test.txt",
            "not empty", "remove", "line1"));
    assert(!result.success);
    assert(result.msg.canFind("remove"), result.msg);
}
