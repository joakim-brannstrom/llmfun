module llm.tool_call.think;

import logger = std.logger;
import std.algorithm : filter, map, splitter;
import std.array : appender, array, empty;
import std.file : exists, readText, dirEntries, SpanMode;
import std.format : format, formattedWrite;
import std.path : stripExtension, baseName, extension;
import std.string : replace, strip, toLower, startsWith;

import my.path : Path;

import llm.tool_call;
import llm.tool_call.utility;

mixin RegisterLlmFunctions!();

interface ThinkingContext : Context {
    /// Returns the directory where thinking templates are stored.
    Path getThinkingTemplatesDir();
    void taskDone();
}

@Function("Get a structured thinking template for a specific strategy. Use this when facing a complex problem that requires a systematic approach. Returns a formatted template with steps to follow.")
ExecuteFuncResult getThinkingTemplate(Context baseCtx, string name) {
    mixin(baseContextToSpecific!ThinkingContext);

    name = name.toLower;
    if (auto err = checkAlphaNumUnderscore(name))
        return ExecuteFuncResult(err, success: false);

    try {
        auto template_ = getTemplate(ctx, name);

        if (template_.empty) {
            return ExecuteFuncResult(
                    format!"error: template '%s' not found. Call listThinkingTemplates for available strategies."(name),
                    success: false);
        }
        return ExecuteFuncResult(template_, success: true);
    } catch (Exception e) {
        logger.tracef("error getting thinking template '%s': %s", name, e.msg);
        return ExecuteFuncResult(format!"error: retrieving template: %s"(e.msg), success: false);
    }
}

@Function("List all available thinking templates that can be used for structured reasoning.")
ExecuteFuncResult listThinkingTemplates(Context baseCtx) {
    mixin(baseContextToSpecific!ThinkingContext);

    auto dir = ctx.getThinkingTemplatesDir();
    auto buf = appender!string();
    buf.put("Available thinking templates:\n\n");

    foreach (entry; dirEntries(dir, SpanMode.shallow).filter!(a => a.extension == ".md")
            .map!(a => a.name.baseName.stripExtension)) {
        const desc = getTemplateDescription(ctx, entry);
        formattedWrite(buf, "# Template: %s\nDescription: %s\n\n", entry, desc);
    }
    return ExecuteFuncResult(buf.data, success: true);
}

@Function("Call `taskDone` **only** when you have fully completed the user’s request.")
ExecuteFuncResult taskDone(Context baseCtx) {
    mixin(baseContextToSpecific!ThinkingContext);
    ctx.taskDone;
    return ExecuteFuncResult("done", success: true);
}

private:

string getTemplate(ThinkingContext ctx, string name) {
    auto path = ctx.getThinkingTemplatesDir ~ (name ~ ".md");

    if (!path.exists) {
        logger.tracef("thinking template not found: %s", path);
        return null;
    }
    return readText(path);
}

string getTemplateDescription(ThinkingContext ctx, string name) {
    import std.stdio : File;

    auto path = ctx.getThinkingTemplatesDir ~ (name ~ ".md");
    if (!path.exists) {
        logger.tracef("thinking template not found: %s", path);
        return null;
    }
    foreach (line; File(path).byLine)
        return line.idup;
    return null;
}
