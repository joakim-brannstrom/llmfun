module llm.tool_call.io;

import logger = std.logger;
import std.algorithm : map, filter, startsWith, count, joiner, endsWith, splitter;
import std.array : empty, appender, array;
import std.conv : to;
import std.file : readText, exists, mkdirRecurse, getSize, remove, dirEntries, SpanMode;
import std.format : format, formattedWrite;
import std.json : JSONValue;
import std.range : enumerate;
import std.regex : Regex, regex;
import std.stdio : File;
import std.string : join, splitLines, indexOf, strip, split, replace;
import std.sumtype : match;
import std.exception : enforce;
import std.path : relativePath;
import std.process : execute;

import my.path : AbsolutePath;

import llm.tool_call;
import llm.tool_call.utility;

mixin RegisterLlmFunctions!();

immutable MaxLines = 20;

interface FileContext : Context {
    bool isPathInsideWorkArea(AbsolutePath path);
    AbsolutePath workArea();
}

@Function("Remove file")
ExecuteFuncResult removeFile(Context baseCtx, string path) {
    mixin(baseContextToSpecific!FileContext);

    auto path_ = pathToWorkarea(ctx, path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    try {
        remove(path_);
        return ExecuteFuncResult("OK", success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed to remove file '%s': %s"(path,
                e.msg), success: false);
    }
}

@Function("Write content to a file, creating it (including parent directories) if it does not exist. May use with editFile for more complex edits. Returns OK or error message")
ExecuteFuncResult writeFile(Context baseCtx, string path, string content) {
    mixin(baseContextToSpecific!FileContext);

    auto path_ = pathToWorkarea(ctx, path);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    try {
        if (path_ != ctx.workArea && !path_.dirName.exists) {
            mkdirRecurse(path_.dirName.toString);
        }
        File(path_.toString, "w").write(content);
        return ExecuteFuncResult("OK", success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed writing content to file '%s': %s"(path,
                e.msg), success: false);
    }
}

@Function("Read the contents of a file. "
        ~ "If appendLoc is true, each line is prefixed with its line number (e.g. \"1→ ...\").\n"
        ~ "path: path to the file\n" ~ "startLine: first line to read, 1-based\n"
        ~ "count: number of lines to read\n"
        ~ "appendLoc: set to 1 to prefix each line with its line number")
ExecuteFuncResult readFile(Context baseCtx, string path, long startLine, long count, long appendLoc) {

    mixin(baseContextToSpecific!FileContext);

    auto json = JSONValue.emptyObject;

    auto path_ = pathToWorkarea(ctx, path, checkExist: true);
    if (!path_.valid) {
        json["error"] = path_.errorMsg;
        return ExecuteFuncResult(json.toString, success: false);
    }
    if (auto err = validateLineRange(startLine, count, MaxLines)) {
        json["error"] = err;
        return ExecuteFuncResult(json.toString, success: false);
    }
    try {
        if (getSize(path_) == 0) {
            json["content"] = "";
            return ExecuteFuncResult(json.toString, success: true);
        }

        auto buf = appender!(string)();
        const firstIdx = startLine - 1;
        const lastIndex = firstIdx + count;
        foreach (line; File(path_).byLine.enumerate.filter!(a => a.index >= firstIdx
                && a.index < lastIndex)) {
            if (appendLoc == 1) {
                formattedWrite(buf, "%s→ ", line.index + 1);
            }
            buf.put(line.value);
            buf.put('\n');
        }
        json["content"] = buf[];
        return ExecuteFuncResult(json.toString, success: true);
    } catch (Exception e) {
        json["error"] = format!"error: failed reading %s lines starting at line %s from file '%s': %s"(count,
                startLine, path, e.msg);
        return ExecuteFuncResult(json.toString, success: false);
    }
}

enum EditMode {
    replace,
    remove,
    append
}

@Function("Edit a file by applying a change. "
        ~ "The change targets a 1-based inclusive line range and has a mode: "
        ~ "\"replace\" (replace lines with content), " ~ "\"remove\" (remove lines, content must be empty string), "
        ~ "\"append\" (insert content after lineEnd). \n"
        ~ "lineStart and lineEnd is ignored when mode is \"append\"."
        ~ "content: Content to insert; must be empty string for delete mode\n"
        ~ "mode: \"replace\", \"remove\", or \"append\"\n"
        ~ "startLine: First line of the range (1-based)\n" ~ "count: number of lines to read")
ExecuteFuncResult editFile(Context baseCtx, string path, string content,
        string mode, long startLine, long count) {
    immutable MaxEditLines = MaxLines * 4;
    mixin(baseContextToSpecific!FileContext);

    auto path_ = pathToWorkarea(ctx, path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }
    if (auto err = validateLineRange(startLine, count, MaxEditLines)) {
        return ExecuteFuncResult(err, success: false);
    }

    try {
        const mode_ = mode.to!EditMode;
        if (mode_ == EditMode.remove && !content.empty) {
            return ExecuteFuncResult("error: parameter mode is \"remove\" but content is not empty",
                    success: false);
        }

        auto fileLines = File(path_.toString);
        auto res = editFileMemory(fileLines.byLine, mode_, content, startLine, count);
        fileLines.close;

        writeLines(path_, res);
    } catch (Exception e) {
        return ExecuteFuncResult(
                format!"error: failed to edit %s lines starting at line %s in file '%s' with mode %s: %s"(count,
                startLine, path, mode, e.msg), success: false);
    }
    return ExecuteFuncResult("OK", success: true);
}

string[] editFileMemory(RangeT)(RangeT fileLines, EditMode mode, string content,
        long startLine, long count) @safe {
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
                lines.put(txtLine.value.idup);
                break;
            }
        } else {
            lines.put(txtLine.value.idup);
        }
    }
    if (mode == EditMode.append) {
        lines.put(content);
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

    res = editFileMemory(lines, EditMode.append, "cat", 1, 4);
    assert(res.length == 6, res.to!string);
    assert(res[4] == "beautiful", res.to!string);
    assert(res[5] == "cat", res.to!string);
}

@Function("Apply a unified diff patch to a file.\n"
        ~ "The diff must follow the standard unified format: each hunk starts with "
        ~ "`@@ -oldStart[,oldCount] +newStart[,newCount] @@`, followed by lines starting "
        ~ "with ' ' (context), '-' (remove) or '+' (add). The diff must match the *exact* "
        ~ "current content of the file; use `readFile` first to obtain it.\n"
        ~ "Returns `OK` on success, or a detailed error if the diff cannot be applied.")
ExecuteFuncResult applyDiff(Context baseCtx, string path, string diff) {
    mixin(baseContextToSpecific!FileContext);

    auto path_ = pathToWorkarea(ctx, path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }
    try {
        auto fileLines = File(path_.toString).byLineCopy.array;
        auto diffLines = diff.splitLines.filter!(a => !a.empty).array;
        auto result = applyDiffMemory(fileLines, diffLines);

        writeLines(path_, result);
        return ExecuteFuncResult("OK", success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed applying diff to file '%s': %s"(path,
                e.msg), success: false);
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
            throw new Exception(format!"Expected hunk header but got: %s"(line));

        // Parse header: "@@ -oldStart[,oldCount] +newStart[,newCount] @@"
        auto secondAt = line.indexOf("@@", 2);
        enforce(secondAt != -1, format!"Invalid hunk header (missing closing @@): %s"(line));
        auto header = line[2 .. secondAt].strip;
        auto parts = header.split;
        enforce(parts.length >= 2, format!"Invalid hunk header format: %s"(line));

        // Old range
        auto oldRange = parts[0];
        enforce(oldRange.startsWith("-"), format!"Old range must start with '-': %s"(line));
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
        enforce(newRange.startsWith("+"), format!"New range must start with '+': %s"(line));
        newRange = newRange[1 .. $];
        size_t newCount;
        if (newRange.indexOf(',') != -1) {
            newCount = newRange.split(",")[1].to!size_t;
        } else {
            newCount = 1;
        }

        // Hunks must apply sequentially to the original file
        if (oldPos < fileIdx)
            throw new Exception(format!"Hunk tries to go backward (oldStart=%d, current file position=%d)"(oldStart,
                    fileIdx + 1));

        // Copy lines from current position up to the start of this hunk
        while (fileIdx < oldPos) {
            result ~= fileLines[fileIdx];
            fileIdx++;
        }

        lineIdx++; // consume the '@@' line
        size_t processedOld = 0; // count of '-' and ' ' lines in this hunk
        size_t processedNew = 0; // count of '+' lines

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
            case ' ': // context line
                enforce(fileIdx < fileLines.length,
                        format!"Unexpected end of file at line %d (hunk context)"(fileIdx + 1));
                enforce(fileLines[fileIdx] == content,
                        format!"Context mismatch at line %d: expected '%s' but found '%s'"(fileIdx + 1,
                            content, fileLines[fileIdx]));
                result ~= fileLines[fileIdx];
                fileIdx++;
                processedOld++;
                break;
            case '-': // removal line (part of original file)
                fileIdx++;
                processedOld++;
                break;
            case '+': // addition line (goes into result, does not consume file line)
                result ~= content;
                processedNew++;
                break;
            default:
                throw new Exception(
                        format!"Invalid hunk line (must start with ' ', '-' or '+'): %s"(hunkLine));
            }
            lineIdx++;
        }

        // Validate the counts declared in the header
        enforce(processedOld == oldCount,
                format!"Hunk @@ -%d,%d +%d,%d @@ consumes %d old lines but header declares %d"(oldStart,
                    oldCount, cast(long) oldStart, newCount, processedOld, oldCount));
        enforce(processedNew == newCount,
                format!"Hunk @@ -%d,%d +%d,%d @@ produces %d new lines but header declares %d"(oldStart,
                    oldCount, cast(long) oldStart, newCount, processedNew, newCount));
    }

    // Append remaining file lines
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

@Function("Replace all occurrences of from with to in text. Returns the modified text or `error`.")
ExecuteFuncResult replaceAll(Context baseCtx, string text, string from, string to) {
    auto res = text.replace(from, to);
    if (res == text)
        return ExecuteFuncResult("error: no `from` from in `text`", success: false);
    return ExecuteFuncResult(res, success: true);
}

immutable MaxDirEntries = 50;
@Function("List files in directory as JSON array of paths, types and sizes. recursive=1 for recursive scan. Max "
        ~ MaxDirEntries.to!string ~ " entries are returned for recursive scan or error.")
ExecuteFuncResult listFilesInDirectory(Context baseCtx, string path, long recursive) {
    mixin(baseContextToSpecific!FileContext);

    auto path_ = pathToWorkarea(ctx, path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    try {
        JSONValue[] rval;
        foreach (a; dirEntries(path_.toString, recursive != 0 ? SpanMode.depth : SpanMode.shallow)) {
            if (recursive != 0 && rval.length > MaxDirEntries) {
                return ExecuteFuncResult(
                        format!"error: failed listing directory recursive: more than %s entries in the result"(
                        MaxDirEntries), success: false);
            }

            auto e = JSONValue.emptyObject;
            e["path"] = a.name.relativePath(ctx.workArea.toString).JSONValue;
            e["type"] = (a.isDir ? "dir" : "file").JSONValue;
            if (a.isFile)
                e["size"] = a.size.JSONValue;
            rval ~= e;
        }
        return ExecuteFuncResult(JSONValue(rval).toString, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed listing directory '%s': %s"(path,
                e.msg), success: false);
    }
}

immutable GrepMaxResults = 1000;
@Function(
        "Search for a pattern in files at path. Returns up to maxResults matching lines with file and line number")
ExecuteFuncResult grepFiles(Context baseCtx, string path, string pattern, long maxResults) {
    mixin(baseContextToSpecific!FileContext);

    auto path_ = pathToWorkarea(ctx, path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    auto cmd = [
        "grep", "-rn", "-m", maxResults.to!string, "-E", pattern, path_.toString
    ];
    auto result = execute(cmd);
    if (result.status == 0) {
        string rval = result.output.strip.replace(path_.toString, path);
        const results = rval.splitter('\n').count;
        if (results > GrepMaxResults) {
            return ExecuteFuncResult(format!"error: %s results exceeds max allowed %s"(results,
                    GrepMaxResults), success: false);
        }
        if (rval.empty) {
            return ExecuteFuncResult(format!"error: no matches found searching in path '%s' with pattern '%s'"(path,
                    pattern), success: false);
        }
        return ExecuteFuncResult(rval, success: true);
    }
    return ExecuteFuncResult(format!"error: failed to execute '%(%-s %)': %s"(
            cmd[0 .. $ - 1] ~ path, result.output.strip), success: false);
}

@Function("Count number of lines in file. Return number or error message")
ExecuteFuncResult countLinesInFile(Context baseCtx, string path) {
    mixin(baseContextToSpecific!FileContext);

    auto path_ = pathToWorkarea(ctx, path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    try {
        return ExecuteFuncResult(File(path_).byLineCopy.count.to!string, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: %s"(e.msg), success: false);
    }
}

@Function("Calculate the MD5 hash of a file. Returns a hexadecimal string.")
ExecuteFuncResult md5HashFile(Context baseCtx, string path) {
    import std.base64 : Base64;
    import std.digest : toHexString;
    import std.digest.md : md5Of;
    import std.format : format;
    import std.string : representation;

    mixin(baseContextToSpecific!FileContext);

    auto path_ = pathToWorkarea(ctx, path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    try {
        auto content = readText(path_.toString);
        return ExecuteFuncResult(content.representation.md5Of.toHexString.idup, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: %s"(e.msg), success: false);
    }
}

interface VisionContext : Context {
    bool isPathInsideWorkArea(AbsolutePath path);
    AbsolutePath workArea();
    bool addVisionImage(AbsolutePath path, string query) nothrow;
}

// TODO: update supported formats by checking what stb_image supports.
@Function("Load an image from path into the API vision context for sending to the OpenAI API. Supported formats are jpg, png, bmp, gif. The image will be attached to the next user message. The query will be part of the image message.")
ExecuteFuncResult loadImageApi(Context baseCtx, string path, string query) {
    mixin(baseContextToSpecific!VisionContext);

    auto path_ = pathToWorkarea(ctx, path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    try {
        if (ctx.addVisionImage(path_, query)) {
            return ExecuteFuncResult(format!"image loaded from '%s'"(path), success: true);
        }
        return ExecuteFuncResult(format!"error: failed to load image '%s'"(path), success: false);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed to load image '%s': %s"(path,
                e.msg), success: false);
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
        return format!"error: tried to read %s lines but %s is max"(count, maxLines);
    return null;
}
