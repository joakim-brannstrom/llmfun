module llm.tool_call.date;

import std.datetime : Clock;

import llm.tool_call;

mixin RegisterLlmFunctions!();

struct CurrentDateTimeParams {
}

@Function("Get current date time as ISO 8601 string")
ExecuteFuncResult currentDateTime(Context baseCtx, CurrentDateTimeParams params) @safe {
    return ExecuteFuncResult(Clock.currTime.toISOExtString, success: true);
}
