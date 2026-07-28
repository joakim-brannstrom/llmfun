module llm.tool_call.completion;

import std.array : empty;
import std.string : strip;

import llm.tool_call;

mixin RegisterLlmFunctions!();

interface CompletionContext : Context {
    void taskDone(string answer);
}

@Function("Call `taskDone` **only** when you have fully completed the user's request. The 'answer' parameter must be a clear and complete summary of what was accomplished - self-contained, substantive, and directly addressing the user's request. Include code examples, key details, and explanations when relevant. Do not compress to the bare minimum.")
ExecuteFuncResult taskDone(Context baseCtx, string answer) {
    mixin(baseContextToSpecific!CompletionContext);

    if (answer.strip.empty) {
        return ExecuteFuncResult("error: 'answer' parameter is required and must not be empty",
                success: false);
    }
    ctx.taskDone(answer);
    return ExecuteFuncResult("done", success: true);
}
