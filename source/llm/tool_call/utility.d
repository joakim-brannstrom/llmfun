module llm.tool_call.utility;

import logger = std.logger;
import std.algorithm : filter, count;
import std.ascii : letters, isAlphaNum;
import std.conv : to;
import std.format : format;
import std.random : randomSample;
import std.typecons : Nullable;
import std.utf : byCodeUnit;

import my.path : Path, AbsolutePath;

Path tempPath() @safe {
    return letters.byCodeUnit.randomSample(20).to!string.Path;
}

string checkAlphaNumUnderscore(string s) {
    if (s.filter!(a => (!a.isAlphaNum && a != '_')).count != 0)
        return format!"error: topic may only contain alphanumeric characters and underscores [0-9,a-z,A-Z,_]";
    return null;
}

struct PathCheckResult {
    AbsolutePath path;
    alias path this;
    bool valid;
    string errorMsg;
}

PathCheckResult pathToWorkarea(ContextT)(ref ContextT ctx, string path, bool checkExist = false) {
    import std.path : isAbsolute;
    import std.file : exists;

    if (path.isAbsolute) {
        return PathCheckResult(ctx.workArea, false,
                format!"error: path '%s' is an absolute path. Only relative paths are allowed"(path));
    }

    auto path_ = (ctx.workArea ~ path).AbsolutePath;
    if (!ctx.isPathInsideWorkArea(path_)) {
        logger.trace(path_);
        return PathCheckResult(ctx.workArea, false,
                format!"error: path '%s' must be inside the allowed workarea"(path));
    }
    if (checkExist && !path_.exists) {
        logger.trace(path_);
        return PathCheckResult(path_, false, format!"error: path '%s' do not exist"(path));
    }
    return PathCheckResult(path_, true, null);
}
