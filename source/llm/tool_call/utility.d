module llm.tool_call.utility;

import logger = std.logger;
import std.algorithm : filter, count;
import std.array : empty;
import std.ascii : letters, isAlphaNum;
import std.conv : text;
import std.random : randomSample;
import std.typecons : Nullable;
import std.utf : byCodeUnit;

import my.path : Path, AbsolutePath;

Path tempPath() @safe {
    import std.conv : to;

    return letters.byCodeUnit.randomSample(20).to!string.Path;
}

string checkAlphaNumUnderscore(string s) @safe pure {
    if (s.filter!(a => (!a.isAlphaNum && a != '_')).count != 0)
        return "error: topic may only contain alphanumeric characters and underscores [0-9,a-z,A-Z,_]";
    return null;
}

struct PathCheckResult {
    AbsolutePath path;
    alias path this;
    bool valid;
    string errorMsg;
}

PathCheckResult pathToWorkarea(ContextT)(ref ContextT ctx, string path, bool checkExist = false) {
    import std.file : exists, isSymlink;
    import std.path : dirName, isAbsolute;
    import std.string : startsWith;

    if (path.isAbsolute) {
        return PathCheckResult(ctx.workArea, false, i"error: path '$(path)' is an absolute path. Only relative paths are allowed"
                .text);
    }

    auto path_ = (ctx.workArea ~ path).AbsolutePath;
    if (!ctx.isPathInsideWorkArea(path_)) {
        logger.trace(path_);
        return PathCheckResult(ctx.workArea, false, i"error: path '$(path)' must be inside the allowed workarea"
                .text);
    }
    if (checkExist && !path_.exists) {
        logger.trace(path_);
        return PathCheckResult(path_, false, i"error: path '$(path)' do not exist".text);
    }
    if (path_.exists && path_.isSymlink) {
        logger.trace(path_);
        return PathCheckResult(path_, false, i"error: path '$(path)' is a symlink. Symlinks are not allowed to be read/write."
                .text);
    }
    auto checkPath = path.dirName;
    while (!checkPath.empty && checkPath.startsWith(ctx.workArea.toString)) {
        if (checkPath.exists && checkPath.isSymlink) {
            logger.trace(checkPath);
            return PathCheckResult(path_, false, i"error: path '$(checkPath)' is a symlink. Read/write is not allowed from inside a symlink."
                    .text);
        }
        checkPath = checkPath.dirName;
    }
    return PathCheckResult(path_, true, null);
}
