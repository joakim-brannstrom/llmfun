module llm.tool_call.io.functions;

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
import llm.tool_call.io : classifyEditError, FileContext, sanitizeLogPath,
    fileEndsWithNewline, writeLines;
import llm.tool_call.utility;
import llm.tool_call;
import llm.types : IAgent;

mixin RegisterLlmFunctions!();

immutable MaxLines = 20;

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
                buf.put(i"$(line.index + 1)→".text);
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

struct ListDirectoryParams {
    @ParamDescription("Path to the directory")
    string path;

    @ParamDescription("Set to true for recursive scan")
    @ParamOptional bool recursive;
}

@Function("List directory contents")
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
            return ExecuteFuncResult(i"error: $(results) results exceeds max allowed $(
                    grepMaxResults)".text, success: false);
        }
        if (rval.empty) {
            return ExecuteFuncResult(i"error: no matches found searching in path '$(params.path)' with pattern '$(
                    params.pattern)'".text, success: false);
        }
        return ExecuteFuncResult(rval, success: true);
    }
    return ExecuteFuncResult(i"error: failed to execute '$((cmd[0 .. $ - 1] ~ params.path).join(
            " "))': $(result.output.strip)".text, success: false);
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

private:

string validateLineRange(long startLine, long count, long maxLines) {
    if (startLine < 1)
        return i"error: parameter startLine $(startLine) must be > 0".text;
    if (count < 1)
        return i"error: parameter count $(count) must be > 0".text;
    if (count > maxLines)
        return i"error: tried to access $(count) lines but $(maxLines) is max".text;
    return null;
}
