module llm.tool_call.context;

import std.array : empty;

import llm.tool_call;

mixin RegisterLlmFunctions!();

interface ContextManagement : Context {
    void agentRequestCompression(string messageToSelf) @safe nothrow;
}

struct RequestCompressionParams {
    @ParamDescription("A detailed note written by the agent to its future self, to be used after context compression. It should be self-contained and include:
- Current goal and task status.
- Key decisions made and why.
- Important facts, constraints, or user preferences.
- Pending actions or next steps.
- Any recent events or context that would otherwise be lost.

Write it as if you are briefing a new instance of yourself that has no memory of the previous conversation.")
    string messageToSelf;
}

@Function("Request a compression of the context")
ExecuteFuncResult requestCompression(Context baseCtx, RequestCompressionParams params) nothrow {
    mixin(baseContextToSpecific!ContextManagement);

    if (params.messageToSelf.empty) {
        return ExecuteFuncResult("error: messageToSelf must not be empty", success: false);
    }
    ctx.agentRequestCompression(params.messageToSelf);
    return ExecuteFuncResult("Compression initiated", success: true);
}
