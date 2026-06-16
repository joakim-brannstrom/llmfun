module llm.tool_call.memory;

import logger = std.logger;
import std.algorithm : map, filter, startsWith, count;
import std.array : empty, appender, array;
import std.conv : to;
import std.file : readText, exists;
import std.format : format, formattedWrite;
import std.json : JSONValue;
import std.regex : Regex, regex;
import std.stdio : File;
import std.string : join, splitLines, indexOf, lastIndexOf, strip, split;
import std.sumtype : match;

import my.path : AbsolutePath;

import llm.rag.rag;
import llm.tool_call.utility : checkAlphaNumUnderscore;
import llm.tool_call;
import llm.config : ToolLimits;

mixin RegisterLlmFunctions!();

interface MemoryContext : Context {
    Path getMemoryFile(string topic);
    string[] getMemoryFileTopics();
    ToolLimits getToolLimits();
}

private string checkTopic(MemoryContext ctx, string topic) {
    auto maxLen = ctx.getToolLimits().maxTopicLength;
    if (topic.length > maxLen)
        return format!"error: topic too long. Max %s characters"(maxLen);
    if (auto err = checkAlphaNumUnderscore(topic))
        return err;
    return null;
}

private string getMemorySummary(MemoryContext ctx, string topic) {
    auto maxSummaryLen = ctx.getToolLimits().maxSummaryLength;
    if (auto e = checkTopic(ctx, topic))
        return "Error reading memory";
    auto path_ = ctx.getMemoryFile(topic);
    if (!path_.exists)
        return "No summary available";

    try {
        auto content = readText(path_);
        foreach (line; content.splitLines) {
            auto trimmed = line.strip;
            if (!trimmed.empty) {
                if (trimmed.length > maxSummaryLen) {
                    auto cutoff = trimmed[0 .. maxSummaryLen];
                    auto spacePos = cutoff.lastIndexOf(" ");
                    if (spacePos > maxSummaryLen / 2 && spacePos != size_t.max) {
                        return trimmed[0 .. spacePos].strip ~ "...";
                    }
                    // Fallback: hard-truncate at maxSummaryLen
                    return trimmed[0 .. maxSummaryLen] ~ "...";
                }
                return trimmed;
            }
        }
        return "No summary available";
    } catch (Exception e) {
        return "Error reading memory";
    }
}

@Function("Store content as markdown paragraph for future retrieval about a topic")
ExecuteFuncResult writeMemory(Context baseCtx, string topic, string content) {
    mixin(baseContextToSpecific!MemoryContext);

    if (auto e = checkTopic(ctx, topic))
        return ExecuteFuncResult(e, success: false);
    auto path_ = ctx.getMemoryFile(topic);

    try {
        File(path_, "w").writeln(content);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: %s"(e.msg), success: false);
    }
    return ExecuteFuncResult("OK", success: true);
}

@Function("Retrieve stored memory from past self about a topic")
ExecuteFuncResult readMemory(Context baseCtx, string topic) {
    mixin(baseContextToSpecific!MemoryContext);

    if (auto e = checkTopic(ctx, topic))
        return ExecuteFuncResult(e, success: false);

    auto path_ = ctx.getMemoryFile(topic);
    if (!path_.exists)
        return ExecuteFuncResult(format!"error: no memory for topic '%s' written"(topic),
                success: false);

    try {
        return ExecuteFuncResult(readText(path_), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: %s"(e.msg), success: false);
    }
}

@Function("Remove a memory that is no longer useful such as temporary notes about a topic")
ExecuteFuncResult removeMemory(Context baseCtx, string topic) {
    import std.file : remove;

    mixin(baseContextToSpecific!MemoryContext);

    if (auto e = checkTopic(ctx, topic))
        return ExecuteFuncResult(e, success: false);

    auto path_ = ctx.getMemoryFile(topic);
    if (!path_.exists)
        return ExecuteFuncResult(format!"error: no memory for topic '%s' written"(topic),
                success: false);

    try {
        remove(path_);
        return ExecuteFuncResult("OK", success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: %s"(e.msg), success: false);
    }
}

@Function("Retrieve all memory topics with summaries for each topic")
ExecuteFuncResult getMemoryTopics(Context baseCtx) {
    mixin(baseContextToSpecific!MemoryContext);
    auto topics = ctx.getMemoryFileTopics;
    if (topics.empty)
        return ExecuteFuncResult("No memory topics available.", success: true);

    auto buf = appender!string();
    formattedWrite(buf, "Available memory topics:\n");
    foreach (topic; topics) {
        auto summary = getMemorySummary(ctx, topic);
        formattedWrite(buf, "\n# Memory: %s\nSummary: %s\n", topic, summary);
    }
    return ExecuteFuncResult(buf.data, success: true);
}
