/// Runtime MCP tool registration: the MCP server ("MCP tools route
/// through the broker", task 12) registers its tools into the global
/// llm.tool_call registry at runtime, when a server connects - not at program
/// start through the RegisterLlmFunctions mixin. Each configured MCP server
/// carries optional config tags (UserConfig.Mcp.tags, llm/app_config.d); every
/// tool registered from that server inherits them as RUNTIME tags on its
/// RegFunction.
///
/// Runtime tags are a FALLBACK membership source (D9): a runtime tag matching
/// a configured group name puts the tool in that group (llm.tool_call.broker
/// membership - inGroup/groupsOf/isVisible), alongside the group's config
/// `tools` list. A runtime tag naming no defined group contributes nothing.
/// Configuring where MCP tools are placed per server (per-server group
/// assignment) is a later design task (design section 13) - out of scope here.
///
/// The compile-time tag channel is gone: the @Function UDA no longer carries
/// tags (task 2 removed the UDA's tags field), so this runtime path is the
/// only tags writer. The UDA-side registration path does NOT constrain this
/// one: both call addFunction directly, so a server tag is accepted as-is
/// (free-form - external servers' tags cannot be anticipated ahead of time,
/// and the known-tag enum check does not apply to this runtime path).
///
/// The callback is caller-supplied: RegFunction.callback is a plain function
/// pointer (no capture), so the MCP client wraps its per-server tools/call
/// dispatch (the MCP server's own executeFunc) into the callback at its call
/// site. MCP transport (connecting to servers, tool listing over the wire) is
/// out of scope - this module covers registration only.
module llm.mcp_server.registration;

import std.json : JSONValue;

import llm.tool_call : Context, ExecuteFuncResult, RegFunction, RegParam, addFunction;

/// Register one MCP server tool in the global tool registry.
///
/// Params:
///   name =      the tool name as exposed to the model (unique across the
///               registry; duplicates warn and are ignored - the registry is
///               append-only).
///   desc =      the tool description composed into the model-facing tools
///               array.
///   params =    the tool's parameter schema (see toParams for the shape).
///   callback =  dispatches a tool call for this tool: receives the tool-call
///               JSON arguments and returns the tool result.
///   serverTags = runtime tags inherited from the MCP server's config
///               (UserConfig.Mcp.tags); free-form - the known-tag enum check
///               does not apply to this runtime path. FALLBACK membership
///               source (D9): a tag matching a configured group name puts the
///               tool in that group; an unmatched tag leaves the tool alwaysOn
///               (in no group).
void registerMcpTool(string name, string desc, RegParam[] params,
        ExecuteFuncResult function(Context, JSONValue) callback, string[] serverTags) {
    addFunction(RegFunction(name: name, desc: desc, params: params, callback: callback,
            tags: serverTags));
}

version (unittest) {
    import llm.config : GroupConfig, ToolBrokerConfig;
    import llm.tool_call.broker : groupsOf, isVisible;

    /// Callback fixture for the unittest below - never invoked.
    private ExecuteFuncResult t10FixtureCallback(Context ctx, JSONValue args) {
        return ExecuteFuncResult("ok", true);
    }
}

@("An MCP runtime tag matching a configured group name puts the tool in "
        ~ "that group; an unmatched tag leaves it alwaysOn")
unittest {
    import std.algorithm : canFind, filter;
    import std.array : array, empty;
    import std.conv : text;

    import llm.tool_call : getFunctions;

    enum grouped = "mcp_ext_t10_grouped";
    enum loose = "mcp_ext_t10_loose";
    registerMcpTool(grouped, "ext tool tagged into a configured group", [],
            &t10FixtureCallback, ["mcp_ext_t10_grp"]);
    registerMcpTool(loose, "ext tool with an unmatched tag", [],
            &t10FixtureCallback, ["no_such_group_t10"]);

    ToolBrokerConfig resolved;
    resolved.enabled = true;
    resolved.groups["mcp_ext_t10_grp"] = GroupConfig("MCP group", []);
    resolved.hiddenTags = ["mcp_ext_t10_grp"];

    auto reg = getFunctions.filter!(f => f.name == grouped || f.name == loose).array;
    assert(reg.length == 2, text(reg.length));
    auto g = reg.filter!(f => f.name == grouped).array[0];
    auto l = reg.filter!(f => f.name == loose).array[0];

    // D9 fallback membership: the runtime tag names the configured group, so
    // the tool is a member even though the group's config `tools` list is empty.
    assert(groupsOf(resolved, g).canFind("mcp_ext_t10_grp"), text(g.tags));
    assert(!isVisible(resolved, g), "a hidden-group member stays hidden");

    // An unmatched runtime tag names no defined group: in no group = alwaysOn.
    assert(groupsOf(resolved, l).empty, text(l.tags));
    assert(isVisible(resolved, l), "an unmatched tag leaves the tool alwaysOn");
}
