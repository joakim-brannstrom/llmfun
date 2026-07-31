module llm.tool_call.pipeline;

import std.array : empty;
import std.conv : text;
import std.string : strip;

import llm.tool_call;

mixin RegisterLlmFunctions!();

interface PipelineControlContext {
    void setPipelineOutput(string output);
}

struct PipelineOutputParams {
    @ParamDescription("The output string to store for downstream propagation. Should be structured, self-contained content that downstream nodes can use as input. Be specific and include all relevant details.")
    string output;
}

/// Tool for agents to communicate their output to downstream nodes.
/// This tool stores the output string in the pipeline's execution context
/// for edge propagation. It does NOT signal node completion (that is taskDone's role).
@Function("Stores the output of this node for downstream propagation in the pipeline. Use this to pass structured output to the next nodes.")
ExecuteFuncResult pipelineOutput(Context baseCtx, PipelineOutputParams params) @trusted {
    if (params.output.strip.empty) {
        return ExecuteFuncResult("error: 'output' parameter is required and must not be empty",
                success: false);
    }

    mixin(baseContextToSpecific!PipelineControlContext);
    ctx.setPipelineOutput(params.output);
    return ExecuteFuncResult(i"Output stored ($(params.output.length) characters) for downstream propagation".text,
            true);
}
