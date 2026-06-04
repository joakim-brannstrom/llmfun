module llm.utility;

import logger = std.logger;
import std.algorithm : among, sort;
import std.array : array, appender, empty, join;
import std.format : format, formattedWrite;
import std.json : JSONValue, JSONType, parseJSON;
import std.stdio : writef, stdout;

import my.path;

import llm.chat : Role;

/// Approximate number of characters per token. Used for estimating token counts from string lengths.
immutable ApproxTokenSize = 2;

/// Display compression progress from SummaryAgent.ProgressCallback
/// Each update is printed on its own line with [PROGRESS] prefix.
void displayProgress(size_t currentChunk, size_t totalChunks, string status) {
    if (totalChunks == 0)
        return;

    // Build simple progress bar
    string bar;
    const barWidth = 10;
    const filled = (currentChunk * barWidth) / totalChunks;
    foreach (i; 0 .. barWidth) {
        bar ~= (i < filled) ? "█" : "░";
    }

    const pct = currentChunk * 100 / totalChunks;

    writef("[PROGRESS] Chunk %s/%s: %s  %s  %d%%\n", currentChunk, totalChunks, status, bar, pct);
    stdout.flush;
}

/// Display compression result
void displayCompressionResult(bool compressed, size_t originalLength, size_t newLength,
        size_t keptXCount, long keptXTokens, long ctxUsed, long newContextSize) {
    if (compressed) {
        writef("[PROGRESS] Compression finished. History %s->%s messages, kept %s msgs (%s tokens), context %s->%s\n",
                originalLength, newLength, keptXCount, keptXTokens, ctxUsed, newContextSize);
    }
    stdout.flush;
}

string summarizeToolCalls(Role role, JSONValue calls) {
    const errorMsg = format!"%s: Wants to run: <unknown>"(role);
    if (calls.type != JSONType.array || calls.array.empty)
        return errorMsg;
    auto call = calls.array[0];
    if ("function" !in call)
        return errorMsg;
    call = call["function"];

    auto buf = appender!string();

    formattedWrite(buf, "%s: run: %s(", role, call["name"]);

    // Extract only key arguments, NOT full content
    try {
        auto args = parseJSON(call["arguments"].str);
        string[] params;
        foreach (key, value; args.object) {
            if (key == "content") {
                auto contentStr = value.toString;
                if (contentStr.length > 80) {
                    params ~= format("content='%.80s...' (%d chars)", contentStr, contentStr.length);
                } else {
                    params ~= format("content='%s'", contentStr);
                }
            } else {
                params ~= format("%s=%s", key, value);
            }
        }

        // Sort parameters by length so shortest (most visible) come first
        sort!("a.length < b.length")(params);
        buf.put(params.join(", "));
    } catch (Exception e) {
        logger.trace("summary failed, should not happen: ", e.msg);
        return format!"%s: Wants to run: %s"(role, call["name"]);
    }
    buf.put(")");
    return buf[];
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

    auto path = dataSearch(ProgramName).resolve(Path("notification.mp3"));
    path.match!((Some!ResourceFile p) {
        try {
            if (p.get.exists) {
                auto f = File("/dev/null");
                spawnProcess(["cvlc", "--play-and-exit", p.get.toString], f, f,
                    f, null, Config.detached);
            }
        } catch (Exception e) {
            logger.trace(e.msg);
            hasPlayer = false;
        }
    }, (None _) {});

}

struct SystemPromptInit {
    string promptTemplate;

    this(Path systemPrompt) {
        import std.file : exists, readText;

        if (systemPrompt.exists) {
            this.promptTemplate = readText(systemPrompt);
        } else {
            logger.warningf("System prompt template file do not exist: %s", systemPrompt);
            throw new Exception("System prompt template missing");
        }
    }

    string toString() @safe const {
        return promptTemplate;
    }
}

private bool signalInterrupt;
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
