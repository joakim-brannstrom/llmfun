module llm.tool_call.metrics;

import logger = std.logger;
import std.datetime : SysTime, DateTime, TimeZone, dur;
import std.format : format;
import std.json : JSONValue;

import llm.tool_call;
import llm.metric.calculator;
import llm.metric.monitor;
import llm.config : ToolLimits;

mixin RegisterLlmFunctions!();

interface MetricsContext : Context {
    ref MetricsCalculator getCalculator();
    ToolCallEvent[] getRecentEvents(long count);
    ToolLimits getToolLimits();
}

@Function("Get current system metrics as a markdown report")
ExecuteFuncResult getMetrics(Context baseCtx) {
    mixin(baseContextToSpecific!MetricsContext);

    try {
        auto calc = ctx.getCalculator();
        return ExecuteFuncResult(calc.generateReport(), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed getting metrics: %s"(e.msg), success: false);
    }
}

@Function("Get recent tool call history")
ExecuteFuncResult getToolHistory(Context baseCtx, long limit, long resultLen) {
    mixin(baseContextToSpecific!MetricsContext);

    auto maxArgLen = ctx.getToolLimits().maxArgLength;

    try {
        auto events = ctx.getRecentEvents(limit);
        if (events.length == 0) {
            return ExecuteFuncResult("No tool call history available", success: false);
        }

        string result;
        foreach (i, event; events) {
            auto dt = SysTime(DateTime.init) + event.timestamp.dur!"msecs";
            result ~= format!"%s. [%s] %s - Success: %s\n   Args: %s\n   Result: %s\n\n"(i + 1,
                    dt.toISOExtString(), event.toolName,
                    event.success ? "Yes" : "No", truncate(event.arguments,
                        maxArgLen), truncate(event.result, resultLen));
        }
        return ExecuteFuncResult(result, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed getting tool history: %s"(e.msg),
                success: true);
    }
}

private:

string truncate(string s, long maxLen) {
    if (s.length <= maxLen)
        return s;
    return s[0 .. maxLen] ~ "...";
}
