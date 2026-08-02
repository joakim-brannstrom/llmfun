module llm.tool_call.metrics;

import logger = std.logger;
import std.conv : text;
import std.datetime : SysTime, DateTime, TimeZone, dur;
import std.format : format;
import std.json : JSONValue, JSONOptions;

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

struct GetMetricsParams {
}

@Function("Get current system metrics as a markdown report")
ExecuteFuncResult getMetrics(Context baseCtx, GetMetricsParams params) {
    mixin(baseContextToSpecific!MetricsContext);

    try {
        auto calc = ctx.getCalculator();
        return ExecuteFuncResult(calc.generateReport(), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: failed getting metrics: $(e.msg)".text, success: false);
    }
}

struct GetToolHistoryParams {
    @ParamDescription("Maximum number of recent tool call events to return")
    @ParamOptional long limit = 10;

    @ParamDescription("Maximum length of the result string for each event")
    @ParamOptional long maxLength = 100;
}

@Function("Get recent tool call history")
ExecuteFuncResult getToolHistory(Context baseCtx, GetToolHistoryParams params) {
    mixin(baseContextToSpecific!MetricsContext);

    auto maxArgLen = ctx.getToolLimits().maxArgLength;

    try {
        auto events = ctx.getRecentEvents(params.limit);
        if (events.length == 0) {
            return ExecuteFuncResult("No tool call history available", success: false);
        }

        string result;
        foreach (i, event; events) {
            auto dt = SysTime(DateTime.init) + event.timestamp.dur!"msecs";
            result ~= i"$(i + 1). [$(dt.toISOExtString())] $(event.toolName) - Success: $(event.success
                    ? "Yes" : "No")\n   Args: $(truncate(event.arguments.toString(
                    JSONOptions.doNotEscapeSlashes), maxArgLen))\n   Result: $(
                    truncate(event.result, params.maxLength))\n\n".text;
        }
        return ExecuteFuncResult(result, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: failed getting tool history: $(e.msg)".text,
                success: false);
    }
}

private:

string truncate(string s, long maxLen) {
    if (s.length <= maxLen)
        return s;
    return s[0 .. maxLen] ~ "...";
}
