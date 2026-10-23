module llm.tool_call.memory;

import logger = std.logger;
import std.algorithm : map, filter, startsWith, count, sort;
import std.array : empty, appender, array;
import std.conv : text;
import std.datetime : SysTime;
import std.json : JSONValue;
import std.range : take;
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

struct MemoryTopic {
    string name;
    SysTime lastModified;
}

/// Result of a memory write: on a tier-fail (writing over an existing
/// long-term memory) the message carries the stored content plus the
/// replace/merge guidance instead of a bare error string.
struct SaveMemoryResult {
    bool success;
    string message; // empty on success
}

interface MemoryContext : Context {
    MemoryTopic[] getMemoryFileTopics();

    SaveMemoryResult saveMemoryFile(string topic, string content);
    Optional!string readMemory(string topic);
    bool removeMemory(string topic);

    ToolLimits getToolLimits();
}

private string checkTopic(MemoryContext ctx, string topic) {
    auto maxLen = ctx.getToolLimits().maxTopicLength;
    if (topic.empty)
        return "error: empty topic";
    if (topic.length > maxLen)
        return i"error: topic too long. Max $(maxLen) characters".text;
    if (auto err = checkAlphaNumUnderscore(topic))
        return err;
    return null;
}

private string getMemorySummary(MemoryContext ctx, string topicName) {
    if (auto e = checkTopic(ctx, topicName))
        return e;

    try {
        const maxSummaryLen = ctx.getToolLimits().maxSummaryLength;
        auto content = ctx.readMemory(topicName).match!((string a) => a, (_) => "");

        string summary = "error: no summary available";
        foreach (trimmed; content.splitLines
                .map!(a => a.strip)
                .filter!(a => !a.empty)
                .take(1)) {
            if (trimmed.length > maxSummaryLen) {
                auto cutoff = trimmed[0 .. maxSummaryLen];
                auto spacePos = cutoff.lastIndexOf(" ");
                if (spacePos > maxSummaryLen / 2 && spacePos != size_t.max) {
                    return trimmed[0 .. spacePos].strip ~ "...";
                }
                // Fallback: hard-truncate at maxSummaryLen
                summary = trimmed[0 .. maxSummaryLen] ~ "...";
            } else {
                summary = trimmed;
            }
        }
        return summary;
    } catch (Exception e) {
        return "error: reading memory";
    }
}

struct WriteMemoryParams {
    @ParamDescription("Topic name for the memory (alphanumeric + underscore, limited length)")
    string topic;

    @ParamDescription("Content to store as markdown paragraph for future retrieval")
    string content;
}

@Function(`Store content as markdown paragraph for future retrieval about a topic.
New memories start in the short-term tier and are promoted to the long-term tier
automatically in the background. Writing over an existing long-term memory FAILS
and returns its stored content plus guidance, and nothing is saved - UNLESS the
new content already includes the stored content (that write IS the replace or
merge and lands in the short-term tier); the second write (full content again =
replace, stored content plus additions = merge) decides.
# What to Remember (concrete criteria)
STORE in memory when:
- You made a mistake that cost time to debug
- You discovered non-obvious behavior (API, tool, language)
- A pattern repeats across 2+ different tasks
- User reveals a preference, convention, or project-specific detail
- You found a workaround for a tool limitation
- You solved a problem in a way you'd want to remember

DO NOT store:
- Common knowledge that doesn't require lookup
- Temporary session-specific state
- Information already in the RAG index
- Speculative ideas that haven't been verified`)
ExecuteFuncResult writeMemory(Context baseCtx, WriteMemoryParams params) {
    mixin(baseContextToSpecific!MemoryContext);

    if (auto e = checkTopic(ctx, params.topic))
        return ExecuteFuncResult(e, success: false);

    try {
        auto r = ctx.saveMemoryFile(params.topic, params.content);
        if (!r.success)
            return ExecuteFuncResult(r.message, success: false);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: $(e.msg)".text, success: false);
    }
    return ExecuteFuncResult("OK", success: true);
}

struct ReadMemoryParams {
    @ParamDescription("Topic name to retrieve memory from")
    string topic;
}

@Function(
        "Retrieve stored memory from past self about a topic (both short- and long-term memories)")
ExecuteFuncResult readMemory(Context baseCtx, ReadMemoryParams params) {
    mixin(baseContextToSpecific!MemoryContext);

    if (auto e = checkTopic(ctx, params.topic))
        return ExecuteFuncResult(e, success: false);

    try {
        return ctx.readMemory(params.topic).match!((string a) {
            return ExecuteFuncResult(a, success: true);
        }, (_) { return ExecuteFuncResult(i"error: no memory topic '$(params.topic)' exists".text,
                success: false); });
    } catch (Exception e) {
        logger.tracef("error retrieving memory '%s': %s", params.topic, e.msg);
        return ExecuteFuncResult(i"error: $(e.msg)".text, success: false);
    }
}

struct RemoveMemoryParams {
    @ParamDescription("Topic name of the memory to remove")
    string topic;
}

@Function("Remove a memory that is no longer useful (both short- and long-term memories)") ExecuteFuncResult removeMemory(
        Context baseCtx, RemoveMemoryParams params) {
    mixin(baseContextToSpecific!MemoryContext);

    if (auto e = checkTopic(ctx, params.topic))
        return ExecuteFuncResult(e, success: false);

    try {
        if (ctx.removeMemory(params.topic)) {
            return ExecuteFuncResult("OK", success: true);
        }
        return ExecuteFuncResult(i"error: failed to remove memory '$(params.topic)'. The memory does not exist".text,
                success: false);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: $(e.msg)".text, success: false);
    }
}

struct GetMemoryTopicsParams {
}

@Function("Retrieve all short-term memory topics with summaries for each topic. Use the RAG to retrieve long-term memories (queryBestMatch/querySemantic)")
ExecuteFuncResult getMemoryTopics(Context baseCtx, GetMemoryTopicsParams params) {
    mixin(baseContextToSpecific!MemoryContext);
    auto topics = ctx.getMemoryFileTopics;
    if (topics.empty)
        return ExecuteFuncResult("No memory topics available.", success: true);

    auto buf = appender!string();
    buf.put("Available memory topics:\n");
    foreach (topic; topics.sort!((a, b) => a.lastModified > b.lastModified)) {
        auto summary = getMemorySummary(ctx, topic.name);
        buf.put(i"\n# Memory: $(topic.name)\nSummary: $(summary)\nLast modified: $(
                topic.lastModified.toISOExtString(0))\n\n".text);
    }
    return ExecuteFuncResult(buf.data, success: true);
}
