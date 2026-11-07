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

import llm.config : PathCategory;

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
    bool deniedByFilter; // true only when ALL containment checks passed
    // but the configured filter rejected the path
}

PathCheckResult pathToWorkarea(ContextT)(ref ContextT ctx, string path,
        PathCategory category, bool checkExist = false) {
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
    // a symlinked directory component silently redirects the operation outside
    // the workarea and lexical checks cannot see it, so walk the real components
    auto checkPath = path_.dirName.toString;
    while (checkPath.startsWith(ctx.workArea.toString)
            && checkPath.length > ctx.workArea.toString.length) {
        // exists first: isSymlink throws for a component that does not exist
        // (new parent dirs are legal - e.g. writeFile mkdirRecurse later), and
        // a nonexistent component cannot be a symlink anyway. exists is true
        // for dangling symlinks here (lstat), so those are still rejected.
        if (checkPath.exists && checkPath.isSymlink) {
            logger.trace(checkPath);
            return PathCheckResult(path_, false, i"error: path '$(checkPath)' is a symlink. Read/write is not allowed from inside a symlink."
                    .text);
        }
        checkPath = checkPath.dirName;
    }
    auto flt = ctx.getToolLimits().filesystemFilterRuntime;
    if (!flt.allows(category, path_)) {
        logger.trace(path_);
        auto r = PathCheckResult(path_, false, i"error: path '$(path_)' is rejected by the configured filesystem filter ($(
                category))".text);
        r.deniedByFilter = true;
        return r;
    }
    return PathCheckResult(path_, true, null);
}
