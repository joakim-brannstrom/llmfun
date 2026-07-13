module llm.tool_call.think;

import logger = std.logger;
import std.algorithm : filter, map, splitter;
import std.array : appender, array, empty;
import std.file : exists, readText, dirEntries, SpanMode;
import std.format : format, formattedWrite;
import std.string : replace, strip, toLower, startsWith;
import std.sumtype : match;

import my.path : Path;
import my.optional;

import llm.tool_call;
import llm.tool_call.utility;

mixin RegisterLlmFunctions!();

interface ThinkingContext : Context {
    string[] getThinkingTemplates();
    Optional!string readThinkingTemplate(string name);
    void taskDone(string answer);
}

@Function("Get a structured thinking template for a specific strategy. Use this when facing a complex problem that requires a systematic approach. Returns a formatted template with steps to follow.")
ExecuteFuncResult getThinkingTemplate(Context baseCtx, string name) {
    mixin(baseContextToSpecific!ThinkingContext);

    name = name.toLower;
    if (auto err = checkAlphaNumUnderscore(name))
        return ExecuteFuncResult(err, success: false);

    try {
        return ctx.readThinkingTemplate(name).match!((string a) {
            return ExecuteFuncResult(a, success: true);
        }, (_) {
            return ExecuteFuncResult(
                format!"error: template '%s' not found. Call listThinkingTemplates for available strategies."(name),
                success: false);
        });
    } catch (Exception e) {
        logger.tracef("error retrieving thinking template '%s': %s", name, e.msg);
        return ExecuteFuncResult(format!"error: retrieving template: %s"(e.msg), success: false);
    }
}

@Function("List all available thinking templates that can be used for structured reasoning.")
ExecuteFuncResult listThinkingTemplates(Context baseCtx) {
    mixin(baseContextToSpecific!ThinkingContext);

    string getTemplateDescription(string content) {
        foreach (line; content.splitter('\n'))
            return line;
        return null;
    }

    auto buf = appender!string();
    buf.put("Available thinking templates:\n\n");

    foreach (name; ctx.getThinkingTemplates) {
        const desc = ctx.readThinkingTemplate(name)
            .match!((string a) => getTemplateDescription(a), (_) => "");
        formattedWrite(buf, "# Template: %s\nDescription: %s\n\n", name, desc);
    }
    return ExecuteFuncResult(buf.data, success: true);
}

@Function("Call `taskDone` **only** when you have fully completed the user's request. The 'answer' parameter must be a clear and complete summary of what was accomplished - self-contained, substantive, and directly addressing the user's request. Include code examples, key details, and explanations when relevant. Do not compress to the bare minimum.")
ExecuteFuncResult taskDone(Context baseCtx, string answer) {
    mixin(baseContextToSpecific!ThinkingContext);

    if (answer.strip.empty) {
        return ExecuteFuncResult("error: 'answer' parameter is required and must not be empty",
                success: false);
    }
    ctx.taskDone(answer);
    return ExecuteFuncResult("done", success: true);
}
