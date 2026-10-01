/// Runtime MCP tool registration: the MCP server ("MCP tools route
/// through the broker", task 12) registers its tools into the global
/// llm.tool_call registry at runtime, when a server connects — not at program
/// start through the RegisterLlmFunctions mixin. Each configured MCP server
/// carries optional config tags (UserConfig.Mcp.tags, llm/app_config.d); every
/// tool registered from that server inherits them, so discovery, filtering and
/// activation treat them like any other broker tool.
///
/// The known-tag enum check of the @Function UDA registration path does NOT
/// apply here: this path calls addFunction directly, so a server tag that
/// is not a KnownToolTag member is accepted as-is (free-form tags are the
/// point — the enum cannot anticipate external servers' tags).
///
/// The callback is caller-supplied: RegFunction.callback is a plain function
/// pointer (no capture), so the MCP client wraps its per-server tools/call
/// dispatch (the MCP server's own executeFunc) into the callback at its call
/// site. MCP transport (connecting to servers, tool listing over the wire) is
/// out of scope — this module covers registration only.
module llm.mcp_server.registration;

import std.json : JSONValue;

import llm.tool_call : Context, ExecuteFuncResult, RegFunction, RegParam, addFunction;

/// Register one MCP server tool in the global tool registry.
///
/// Params:
///   name =      the tool name as exposed to the model (unique across the
///               registry; duplicates warn and are ignored — the registry is
///               append-only).
///   desc =      the tool description composed into the model-facing tools
///               array.
///   params =    the tool's parameter schema (see toParams for the shape).
///   callback =  dispatches a tool call for this tool: receives the tool-call
///               JSON arguments and returns the tool result.
///   serverTags = tags inherited from the MCP server's config
///               (UserConfig.Mcp.tags); free-form — the known-tag enum check
///               does not apply to this runtime path.
void registerMcpTool(string name, string desc, RegParam[] params,
        ExecuteFuncResult function(Context, JSONValue) callback, string[] serverTags) {
    addFunction(RegFunction(name: name, desc: desc, params: params, callback: callback,
            tags: serverTags));
}
