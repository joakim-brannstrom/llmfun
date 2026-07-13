module llm.tool_call.memory;

import logger = std.logger;
import std.algorithm : map, filter, startsWith, count;
import std.array : empty, appender, array;
import std.conv : to;
import std.format : format, formattedWrite;
import std.json : JSONValue;
import std.regex : Regex, regex;
import std.string : join, splitLines, indexOf, lastIndexOf, strip, split;
import std.sumtype : match;

import my.path : AbsolutePath;
import my.optional;

import llm.rag.rag;
import llm.tool_call.utility : checkAlphaNumUnderscore;
import llm.tool_call;
import llm.config : ToolLimits;

mixin RegisterLlmFunctions!();

interface MemoryContext : Context {
    string[] getMemoryFileTopics();

    bool saveMemoryFile(string topic, string content);
    Optional!string readMemory(string topic);
    bool removeMemory(string topic);

    ToolLimits getToolLimits();
}

private string checkTopic(MemoryContext ctx, string topic) {
    auto maxLen = ctx.getToolLimits().maxTopicLength;
    if (topic.empty)
        return "error: empty topic";
    if (topic.length > maxLen)
        return format!"error: topic too long. Max %s characters"(maxLen);
    if (auto err = checkAlphaNumUnderscore(topic))
        return err;
    return null;
}

private string getMemorySummary(MemoryContext ctx, string topic) {
    if (auto e = checkTopic(ctx, topic))
        return e;

    try {
        auto maxSummaryLen = ctx.getToolLimits().maxSummaryLength;
        auto content = ctx.readMemory(topic).match!((string a) => a, (_) => "");
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
        return "error: no summary available";
    } catch (Exception e) {
        return "error: reading memory";
    }
}

@Function("Store content as markdown paragraph for future retrieval about a topic")
ExecuteFuncResult writeMemory(Context baseCtx, string topic, string content) {
    mixin(baseContextToSpecific!MemoryContext);

    if (auto e = checkTopic(ctx, topic))
        return ExecuteFuncResult(e, success: false);

    try {
        if (!ctx.saveMemoryFile(topic, content)) {
            return ExecuteFuncResult(format!"error: failed saving the memory with topic '%s'"(topic),
                    success: false);
        }
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

    try {
        return ctx.readMemory(topic).match!((string a) {
            return ExecuteFuncResult(a, success: true);
        }, (_) {
            return ExecuteFuncResult(format!"error: no memory topic '%s' exist"(topic),
                success: false);
        });

    } catch (Exception e) {
        logger.tracef("error retrieving memory '%s': %s", topic, e.msg);
        return ExecuteFuncResult(format!"error: %s"(e.msg), success: false);
    }
}

@Function("Remove a memory that is no longer useful such as temporary notes about a topic")
ExecuteFuncResult removeMemory(Context baseCtx, string topic) {
    mixin(baseContextToSpecific!MemoryContext);

    if (auto e = checkTopic(ctx, topic))
        return ExecuteFuncResult(e, success: false);

    try {
        if (ctx.removeMemory(topic)) {
            return ExecuteFuncResult("OK", success: true);
        }
        return ExecuteFuncResult(
                format!"error: failed to remove memory '%s'. The memory do not exist or is write protected"(topic),
                success: false);
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
