module llm.utility;

import logger = std.logger;
import std.algorithm : among, sort, filter;
import std.array : array, appender, empty, join;
import std.format : format, formattedWrite;
import std.json : JSONValue, JSONType, parseJSON, JSONOptions;
import std.stdio : writef, stdout;

import my.path;

import llm.chat : Role, ToolResponse;

// Convert a 4-byte hash to a long (little-endian byte order).
private long toLong(ubyte[4] a) @safe {
    return a[0] | a[1] << 8 | a[2] << 16 | a[3] << 24;
}

// Compute a content checksum using MurmurHash3-32.
// Params:
//  content The text content to hash
// Returns: A long representing the 32-bit hash value
long computeContentHash(string content) @safe {
    import std.digest : digest;
    import std.digest.murmurhash : MurmurHash3;

    return toLong(digest!(MurmurHash3!32)(content));
}

/// Display compression result
string compressionResultToString(bool compressed, size_t originalLength,
        size_t newLength, size_t keptXCount, long keptXTokens, long ctxUsed, long newContextSize) {
    if (compressed) {
        return format("Compression finished. History %s->%s messages, kept %s msgs (%s tokens), context %s->%s\n",
                originalLength, newLength, keptXCount, keptXTokens, ctxUsed, newContextSize);
    }
    return null;
}

string summarizeToolResponse(ToolResponse msg, size_t maxLength) {
    auto content = msg.content;

    try {
        content = parseJSON(msg.content).toPrettyString(JSONOptions.doNotEscapeSlashes);
    } catch (Exception e) {
    }
    return content.length < maxLength ? content : format("%s... (%d chars)",
            content[0 .. maxLength], content.length);
}

string[] summarizeToolCalls(JSONValue calls, size_t maxLength) @trusted {
    const errorMsg = "Wants to run: <unknown>";
    if (calls.type != JSONType.array || calls.array.empty)
        return [errorMsg];

    string process(JSONValue call) {
        try {
            if ("function" !in call)
                return errorMsg;
            call = call["function"];

            auto buf = appender!string();

            formattedWrite(buf, "%-s(", call["name"].str);

            // Extract only key arguments
            auto args = parseJSON(call["arguments"].str);

            buf.put(summarizeToolCallArguments(args, maxLength));

            buf.put(")");
            return buf[];
        } catch (Exception e) {
            logger.trace("summary failed, should not happen: ", e.msg);
            return "Wants to run: <unknown>";
        }
    }

    auto rval = appender!(string[])();
    foreach (call; calls.array) {
        rval.put(process(call));
    }

    return rval[];
}

string summarizeToolCallArguments(JSONValue args, size_t maxLength) @trusted {
    try {
        auto buf = appender!string();

        // Extract only key arguments
        string[] params;
        foreach (key, value; args.object) {
            auto valueStr = value.toString;
            if (valueStr.length > maxLength) {
                valueStr = format("'%s...' (%s chars)", valueStr[0 .. maxLength], valueStr.length);
            }
            params ~= format("%s=%s", key, valueStr);
        }

        // Sort parameters by length so shortest (most visible) come first
        params = sort!("a.length < b.length")(params).array;
        bool isFirst = true;
        foreach (p; params) {
            if (!isFirst)
                buf.put(",\n");
            buf.put(p);
            isFirst = false;
        }
        return buf[];
    } catch (Exception e) {
        logger.trace("summary failed, should not happen: ", e.msg);
        return null;
    }
}

void configCatchCtrlC() {
    import core.stdc.signal;

    signal(SIGINT, &handleSIGINT);
}

void playNotification() {
    import llm.config : ProgramName;
    import my.optional;
    import my.resource;
    import std.file : exists;
    import std.process : spawnProcess, Config;
    import std.stdio : File;
    import std.sumtype : match;

    static bool hasPlayer = true;

    if (!hasPlayer)
        return;

    auto path = dataSearch(ProgramName).resolve(Path("notification.wav"));
    path.match!((Some!ResourceFile p) {
        try {
            if (p.get.exists) {
                auto f = File("/dev/null");
                spawnProcess(["aplay", p.get.toString], f, f, f, null, Config.detached);
            }
        } catch (Exception e) {
            logger.trace(e.msg);
            hasPlayer = false;
        }
    }, (None _) {});

}

private shared bool signalStopAgent;

void stopAgent() nothrow @nogc @system {
    .signalStopAgent = true;
}

bool isStopAgentTriggered() @safe nothrow @nogc {
    return .signalStopAgent;
}

void clearStopAgent() @safe nothrow @nogc {
    .signalStopAgent = false;
}

private shared bool signalInterrupt;
private extern (C) void handleSIGINT(int sig) nothrow @nogc @system {
    .signalInterrupt = true;
}

bool isInterruptTriggered() @safe nothrow @nogc {
    return .signalInterrupt;
}

void clearInterruptSignal() @safe nothrow @nogc {
    .signalInterrupt = false;
}

T getValue(T)(JSONValue v, T delegate(JSONValue v) accessor, T default_) @trusted {
    try {
        return accessor(v);
    } catch (Exception e) {
        return default_;
    }
}

void catchSignalSIGPIPE() {
    import core.sys.posix.signal;

    signal(SIGPIPE, &handleSIGPIPE);
}

private shared bool signalSIGPIPE;
private extern (C) void handleSIGPIPE(int sig) nothrow @nogc @system {
    .signalSIGPIPE = true;
}

bool isSignalSIGPIPETriggered() @safe nothrow @nogc {
    return .signalSIGPIPE;
}

void clearSignalSIGPIPE() @safe nothrow @nogc {
    .signalSIGPIPE = false;
}

bool isReadWrite(Path p) nothrow {
    import core.sys.posix.sys.stat;
    import my.file : getAttrs;

    uint attrs;
    if (getAttrs(p, attrs)) {
        return (attrs & (S_IRUSR | S_IRGRP | S_IROTH)) != 0
            && (attrs & (S_IWUSR | S_IWGRP | S_IWOTH)) != 0;
    }
    return false;
}

/// Backup memory area path to a per-area "_backup" sibling directory.
/// Returns: true on success, false on failure.
bool backupMemoryFiles(Path[] memoryArea) {
    import std.file : rmdirRecurse, exists, rename;
    import my.file : copyRecurse;

    foreach (area; memoryArea.filter!(a => a.exists)
            .filter!(a => a.isReadWrite)) {
        // Per-area backup directory to avoid filename collisions between sibling memory areas.
        auto backupDir = (area.toString ~ "_backup").Path;
        auto stagingDir = (area.toString ~ "_backup_staging").Path;

        try {
            if (stagingDir.exists) {
                rmdirRecurse(stagingDir);
            }
        } catch (Exception e) {
            logger.tracef("failed to clean old staging dir %s: %s", stagingDir, e.msg);
            return false;
        }

        try {
            copyRecurse(area, stagingDir);
            logger.tracef("staged %s -> %s", area, stagingDir);
        } catch (Exception e) {
            logger.warningf("failed to copy files %s -> %s: %s", area, stagingDir, e.msg);
            try {
                rmdirRecurse(stagingDir);
            } catch (Exception _) {
            }
            return false;
        }

        try {
            if (backupDir.exists) {
                rmdirRecurse(backupDir);
            }
            rename(stagingDir, backupDir);
            logger.tracef("backup complete for %s -> %s", area, backupDir);
        } catch (Exception e) {
            logger.warningf("failed to finalize backup for %s: %s", area, e.msg);
            try {
                rmdirRecurse(stagingDir);
            } catch (Exception _) {
            }
            return false;
        }
    }

    return true;
}

/// Restore memory from per-area backup directories back to their original memory areas.
/// Creates a rollback snapshot before restore; on failure, rolls back from snapshot.
/// Deletes backup directories after successful restore.
/// Returns: true on success, false on failure.
bool restoreMemoryFiles(Path[] memoryArea) {
    import std.file : dirEntries, SpanMode, copy, rmdirRecurse, mkdirRecurse, exists, rename;
    import my.file : copyRecurse;

    foreach (area; memoryArea.filter!(a => (a.toString ~ "_backup").exists)
            .filter!(a => a.isReadWrite)) {
        auto backupDir = (area.toString ~ "_backup").Path;

        auto rollbackDir = (area.toString ~ "_restore_rollback").Path;
        try {
            copyRecurse(backupDir, rollbackDir);
        } catch (Exception e) {
            logger.warningf("failed to create rollback snapshot for %s: %s", area, e.msg);
            try {
                rmdirRecurse(rollbackDir);
            } catch (Exception _) {
            }
            return false;
        }

        bool restoreOk;
        try {
            copyRecurse(backupDir, area);
            restoreOk = true;
        } catch (Exception e) {
            logger.warningf("restore failed for %s: %s", area, e.msg);
        }

        if (restoreOk) {
            try {
                rmdirRecurse(rollbackDir);
            } catch (Exception _) {
            }

            try {
                rmdirRecurse(backupDir);
                logger.tracef("removed backup directory %s", backupDir);
            } catch (Exception e) {
                logger.tracef("failed to remove backup directory %s: %s", backupDir, e.msg);
            }
        } else {
            try {
                copyRecurse(rollbackDir, area);
                logger.warningf("rolled back %s from snapshot", area);
            } catch (Exception e) {
                logger.warningf("rollback FAILED for %s: %s - manual recovery needed", area, e.msg);
            }
        }
    }

    return true;
}

/// Remove all memory backup directories. Logs at trace level.
/// Returns: true if all backup directories were successfully removed or none existed.
bool removeMemoryBackup(Path[] memoryArea) {
    import std.file : rmdirRecurse, exists;

    bool allOk = true;

    foreach (area; memoryArea.filter!(a => (a.toString ~ "_backup").exists)) {
        auto backupDir = (area.toString ~ "_backup").Path;

        try {
            rmdirRecurse(backupDir);
            logger.tracef("removed backup directory %s", backupDir);
        } catch (Exception e) {
            logger.tracef("failed to remove %s: %s", backupDir, e.msg);
            allOk = false;
        }
    }

    return allOk;
}

// Check if a path is safely inside the workarea.
// Uses proper directory boundary check to prevent prefix attacks
// (e.g., /workarea_evil would not match /workarea).
// Params:
//  path The path to check
//  workArea The workarea root path
// Returns: true if path is safely inside workarea
bool isPathInsideWorkarea(AbsolutePath path, AbsolutePath workArea) @safe pure nothrow @nogc {
    import std.algorithm : startsWith;

    if (!path.toString.startsWith(workArea.toString))
        return false;
    // Ensure proper directory boundary: either exact match or next char is '/'
    if (path.length == workArea.length)
        return true;
    return path[workArea.length] == '/';
}
