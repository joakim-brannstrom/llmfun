/// Tool broker mechanics between the model and the global tool registry:
/// pool construction, selection, activation, and compression-time pruning
/// Pure functions and
/// per-agent in-memory state; unit-testable without a live model.
module llm.tool_call.broker;

import std.algorithm : canFind, filter, map;
import std.array : array, empty;
import std.json : JSONType, JSONValue;
import std.sumtype : match;

import my.filter : ReFilter;

import llm.chat : Chat, ToolMessage;
import llm.config : GroupConfig, ToolBrokerConfig;
import llm.tool_call : RegFunction;

/// Per-Agent-instance, in-memory broker state: activated tool names
/// in activation order. alwaysOn tools are never listed here.
struct BrokerState {
    string[] activated;
    /// Activation count per group name, feeding the discovery-tool
    /// presentation cap. Bumped only when the group matched at least
    /// one pool tool; an unknown group is a full no-op.
    uint[string] tagActivationCount;
}

RegFunction[] filterRegFunctions(RegFunction[] registry, ReFilter toolFilter, string[] neverHide) @safe {
    return registry.filter!(a => toolFilter.match(a.name)
            || neverHide.canFind!(n => n == a.name)).array;
}

string[] hiddenNeverHideTools(RegFunction[] registry, ReFilter toolFilter, string[] neverHide) @safe {
    return neverHide.filter!(a => registry.canFind!(rf => rf.name == a)
            && !toolFilter.match(a)).array;
}

/// Renders one RegFunction as a tools-array entry: the existing
/// descAllFunctions() shape ({"type":"function","function":{...}}).
JSONValue toolDescription(const RegFunction f) @safe pure {
    JSONValue jfunc;
    jfunc["name"] = f.name;
    jfunc["description"] = f.desc;

    auto jparams = JSONValue.emptyObject;
    foreach (p; f.params) {
        auto j = JSONValue.emptyObject;
        j["type"] = p.type;
        if (!p.itemsType.empty) {
            auto items = JSONValue.emptyObject;
            items["type"] = p.itemsType;
            j["items"] = items;
        }
        if (!p.desc.empty) {
            j["description"] = p.desc;
        }
        jparams[p.name] = j;
    }
    jfunc["parameters"] = JSONValue.emptyObject;
    jfunc["parameters"]["type"] = "object";
    jfunc["parameters"]["properties"] = jparams;
    jfunc["parameters"]["required"] = JSONValue(f.params
            .filter!(a => a.required)
            .map!(a => a.name)
            .array);

    JSONValue jwrap;
    jwrap["type"] = "function";
    jwrap["function"] = jfunc;
    return jwrap;
}

/// Group membership (D9): the union of (a) the `tools` list of the resolved
/// config's group and (b) the tool's runtime RegFunction.tags naming the
/// group - only MCP registration writes runtime tags. A runtime tag that
/// names no defined group contributes nothing.
bool inGroup(const ToolBrokerConfig resolved, const RegFunction f, string group) @safe pure {
    if (auto g = group in resolved.groups)
        if (g.tools.canFind!(t => t == f.name))
            return true;
    return f.tags.canFind!(t => t == group);
}

/// The groups a tool belongs to (membership per inGroup): the config groups
/// listing the tool first, then the tool's runtime tags that name a defined
/// group (D9). Deduplicated; order not significant (config AA iteration order
/// is unspecified - the only consumer, isVisible, is order-independent).
string[] groupsOf(const ToolBrokerConfig resolved, const RegFunction f) @safe pure {
    string[] rval;
    foreach (name, g; resolved.groups)
        if (g.tools.canFind!(t => t == f.name))
            rval ~= name;
    foreach (t; f.tags)
        if (t in resolved.groups && !rval.canFind(t))
            rval ~= t;
    return rval;
}

/// Visibility predicate (design section 6) over the resolved config; the
/// pool is the registry tools surviving the global toolFilter plus
/// neverHideTools (the caller's concern):
///  - the kill switch (enabled: false) leaves everything visible - all
///    tools are treated as ungrouped (alwaysOn);
///  - a tool in no group is alwaysOn (matches today's untagged default, D4);
///  - a group name in alwaysOn un-hides the whole membership (D4); a tool
///    name in alwaysOn un-hides the tool itself;
///  - otherwise hidden iff the tool is a member of a hidden group (a group
///    in the effective hiddenTags array). Omitted or empty hiddenTags hides
///    nothing (D7 - no "unset means hide everything" materialization).
bool isVisible(const ToolBrokerConfig resolved, const RegFunction f) @safe pure {
    if (!resolved.enabled)
        return true;

    auto groups = groupsOf(resolved, f);
    if (groups.empty)
        return true; // a tool in no group is alwaysOn

    foreach (g; groups)
        if (resolved.alwaysOn.canFind!(a => a == g))
            return true; // a group name in alwaysOn un-hides the membership
    if (resolved.alwaysOn.canFind!(a => a == f.name))
        return true;

    return !groups.canFind!(g => resolved.hiddenTags.canFind!(h => h == g));
}

/// The alwaysOn set for selectTools' head: the pool tools visible under the
/// resolved config (isVisible). Callers pass the result as selectTools'
/// alwaysOn argument.
string[] alwaysOnTools(const ToolBrokerConfig resolved, RegFunction[] pool) @safe pure {
    return pool.filter!(f => isVisible(resolved, f))
        .map!(f => f.name)
        .array;
}
/// Pure selection: alwaysOn (membership-visible under the resolved config)
/// tools first in REGISTRY order with neverHide tools forced into the head,
/// then activated tools in ACTIVATION order; deduplicated by name (first
/// occurrence wins). A tool that is both alwaysOn and activated appears once,
/// in the head. The pool is never sorted or reshuffled: identical state emits
/// byte-identical arrays (pinned by the unittest below).
JSONValue[] selectTools(RegFunction[] pool, string[] activated,
        string[] neverHide, string[] alwaysOn) @safe pure {
    JSONValue[] rval;
    string[] seen;

    foreach (f; pool.filter!(rf => alwaysOn.canFind!(n => n == rf.name)
            || neverHide.canFind!(n => n == rf.name))) {
        rval ~= toolDescription(f);
        seen ~= f.name;
    }

    foreach (name; activated.filter!(n => !seen.canFind!(s => s == n))) {
        foreach (f; pool.filter!(rf => rf.name == name)) {
            rval ~= toolDescription(f);
            seen ~= f.name;
        }
    }
    return rval;
}

/// Explicit activation seam: appends the group's visible pool tools in
/// activation order; deduplicated. An unknown group (no pool tool is a
/// member - neither a config `tools` entry nor a runtime tag) is a full
/// no-op: the instructive message is the listToolTags layer's, not this
/// seam's. Inert under the kill switch (enabled: false): no state mutation,
/// no count bump - the broker is disabled and discovery errors before it
/// could reach this seam. The activation count feeds the discovery-tool
/// presentation cap (frequency then name) and is keyed by group name (D12).
void activateTag(ref BrokerState st, RegFunction[] pool, string group,
        const ToolBrokerConfig resolved) @safe pure {
    if (!resolved.enabled)
        return;
    bool anyTool;
    foreach (f; pool.filter!(rf => inGroup(resolved, rf, group))) {
        anyTool = true;
        if (!st.activated.canFind!(n => n == f.name)) {
            st.activated ~= f.name;
        }
    }
    if (anyTool) {
        st.tagActivationCount[group]++;
    }
}

/// Per-tool activation seam; loadToolTag is the per-tag
/// variant. Unknown tool names no-op; activation is sticky (dedup).
void activateTool(ref BrokerState st, RegFunction[] pool, string toolName) @safe pure {
    foreach (f; pool.filter!(rf => rf.name == toolName)
            .filter!(f => !st.activated.canFind!(n => n == f.name))) {
        st.activated ~= f.name;
    }
}

/// Compression-time prune scan: returns the activated (non-alwaysOn)
/// tool names with no tool CALL in the intact pre-compression chat. Usage is
/// structured ToolMessage.toolCalls names only -- summary text and assistant
/// prose never count -- and the ENTIRE chat is scanned (kept messages included,
/// conservative). Deviations from the task's verbatim contract, both recorded:
/// "in Chat" became "scope Chat" (getMessages() is not const-qualified,
/// chat.d:150, and the const getReasoningTrace() projection is filtered --
/// the design mandates the whole-chat corpus), and "pure" is dropped
/// (ToolMessage.hasTool transitively calls the delegate-taking getValue,
/// utility.d:264, blocking purity inference; the scan is side-effect free).
/// @system solely because ToolMessage.hasTool is @system (chat.d:879).
string[] scanInactiveTools(const(BrokerState) st, scope Chat chat) @system {
    bool usedByChat(string name) {
        foreach (msg; chat.getMessages) {
            if (msg.match!((ToolMessage m) => m.hasTool(name), (_) => false)) {
                return true;
            }
        }
        return false;
    }

    return st.activated
        .filter!(n => !usedByChat(n))
        .map!(a => a.idup)
        .array;
}

/// Prune apply: removes the inactive names from the activation
/// list. The caller (Agent.compress) owns the apply gate: apply only
/// when the compression actually rewrote history.
void applyPrune(ref BrokerState st, const(string[]) inactive) @safe pure {
    st.activated = st.activated.filter!(n => !inactive.canFind(n)).array;
}

/// Seed-side usage scan: the inverse corpus of scanInactiveTools -- the
/// structured tool-call names the chat contains, in chronological message
/// order, first-occurrence deduplicated. Same corpus rule as
/// scanInactiveTools: structured ToolMessage calls only -- the toolCalls
/// extraction mirrors ToolMessage.hasTool (chat.d:880-890); summary text and
/// assistant prose never count. Refused calls count as uses (the refusal
/// travels as the tool response to a recorded structured call, so the
/// request-side ToolMessage exists either way -- the scan must not try to
/// tell them apart). The applier (Agent.seedBrokerFromChat, task 2) owns
/// all filtering and state mutation.
string[] scanUsedTools(scope Chat chat) @system {
    import llm.utility : getValue;

    string[] used;
    bool unseen(string name) {
        return !name.empty && !used.canFind!(n => n == name);
    }

    foreach (msg; chat.getMessages) {
        msg.match!((ToolMessage m) {
            foreach (tool; m.getFunctions) {
                if (unseen(tool.name))
                    used ~= tool.name;
            }
        }, (_) {});
    }
    return used;
}

version (unittest) {
    /// Registry entry fixture; the callback is never invoked by these tests.
    private RegFunction fixtureTool(string name, string[] tags) @safe pure nothrow {
        return RegFunction(name: name, desc: "desc of " ~ name, params: [],
                callback: null, tags: tags);
    }

    /// Resolved-config fixture for the selectTools tests: two groups whose
    /// members start hidden, nothing alwaysOn - the alwaysOn set the tests
    /// pass comes from alwaysOnTools over this config.
    private ToolBrokerConfig hiddenGroupsConfig() @safe pure {
        ToolBrokerConfig resolved;
        resolved.enabled = true;
        resolved.groups["workarea"] = GroupConfig("workarea file tools", ["mid"]);
        resolved.groups["rag"] = GroupConfig("RAG knowledge base tools", ["nh1"]);
        resolved.hiddenTags = ["workarea", "rag"];
        return resolved;
    }

    /// Builds the toolCalls JSONValue a ToolMessage carries:
    /// [{"function": {"name": n}}, ...].
    private JSONValue toolCallsJson(in string[] names) @safe pure {
        JSONValue[] calls;
        foreach (n; names) {
            JSONValue f;
            f["name"] = n;
            JSONValue call;
            call["function"] = f;
            calls ~= call;
        }
        return JSONValue(calls);
    }

    /// Chat fixture: one ToolMessage per entry, carrying that message's calls.
    /// Not marked pure: ToolMessage.s ctor does not get purity inferred (ldc2),
    /// so constructing one blocks it.
    private Chat chatWith(in string[][] callsPerMessage) @safe {
        Chat chat;
        foreach (calls; callsPerMessage) {
            chat.add(ToolMessage("", toolCallsJson(calls)));
        }
        return chat;
    }
}

@("selectTools: untagged first in registry order, neverHide forced into "
        ~ "the head, activated in activation order, overlap emitted once")
unittest {
    import std.algorithm : map;
    import std.array : array;
    import std.conv : to;

    auto pool = [
        fixtureTool("always1", []), fixtureTool("mid", ["workarea"]),
        fixtureTool("nh1", ["rag"])
    ];
    auto activated = ["mid", "always1", "ghost"];
    auto neverHide = ["nh1"];

    auto resolved = hiddenGroupsConfig();
    auto alwaysOn = alwaysOnTools(resolved, pool);

    auto names = selectTools(pool, activated, neverHide, alwaysOn).map!(
            e => e["function"]["name"].str).array;
    assert(names == ["always1", "nh1", "mid"], names.to!string);
}

@("selectTools: dedup, an activated alwaysOn tool appears once in the head")
unittest {
    import std.algorithm : map;
    import std.array : array;
    import std.conv : to;

    auto pool = [fixtureTool("t1", []), fixtureTool("t2", ["workarea"])];
    auto resolved = hiddenGroupsConfig();
    auto names = selectTools(pool, ["t1", "t2"], [], alwaysOnTools(resolved, pool)).map!(
            e => e["function"]["name"].str).array;
    assert(names == ["t1", "t2"], names.to!string);
}

@("selectTools: byte-identical for identical inputs")
unittest {
    import std.json : JSONValue;

    auto pool = [fixtureTool("always1", []), fixtureTool("mid", ["workarea"])];
    auto activated = ["mid"];
    auto neverHide = ["always1"];
    auto alwaysOn = alwaysOnTools(hiddenGroupsConfig(), pool);

    auto s1 = JSONValue(selectTools(pool, activated, neverHide, alwaysOn)).toString;
    auto s2 = JSONValue(selectTools(pool, activated, neverHide, alwaysOn)).toString;
    assert(s1 == s2);
    assert(s1.length > 0);
}

@("selectTools: empty activated emits the alwaysOn head only; empty pool " ~ "emits nothing")
unittest {
    import std.algorithm : map;
    import std.array : array, empty;
    import std.conv : to;

    auto pool = [fixtureTool("always1", []), fixtureTool("t2", ["workarea"])];
    auto headOnly = selectTools(pool, [], ["taskDone"], ["always1"]).map!(
            e => e["function"]["name"].str).array;
    assert(headOnly == ["always1"], headOnly.to!string);

    assert(selectTools([], ["t1"], ["taskDone"], []).empty);
}

@("filterRegFunctions: excluded names are gone except neverHide, registry " ~ "order kept")
unittest {
    import std.algorithm : map;
    import std.array : array;
    import std.conv : to;

    auto registry = [
        fixtureTool("a", []), fixtureTool("b", ["workarea"]), fixtureTool("c", [
        ])
    ];
    auto toolFilter = ReFilter(["a", "c"], []);

    auto pool = filterRegFunctions(registry, toolFilter, ["taskDone", "b"]);
    auto names = pool.map!(f => f.name).array;
    assert(names == ["a", "b", "c"], names.to!string);

    auto hidden = hiddenNeverHideTools(registry, toolFilter, ["taskDone", "b"]);
    assert(hidden == ["b"], hidden.to!string);
}

@("scanInactiveTools: used calls keep activation, unused and non-array "
        ~ "toolCalls do not, later messages count")
unittest {
    import std.array : empty;
    import std.conv : to;

    auto st = BrokerState();
    st.activated = ["t1", "t2"];
    auto chat = chatWith([["t1"], [], ["t3"]]);
    chat.add(ToolMessage("summary mentions t2", JSONValue("not an array")));

    auto inactive = scanInactiveTools(st, chat);
    assert(inactive == ["t2"], inactive.to!string);
}

@("scanInactiveTools: empty activation list yields empty output")
unittest {
    import std.array : empty;

    auto chat = chatWith([["t1"]]);
    assert(scanInactiveTools(BrokerState(), chat).empty);
}

@("scanUsedTools: calls count, prose mentions do not, refused calls "
        ~ "count, duplicates collapse in first-occurrence order")
unittest {
    import std.array : empty;
    import std.conv : to;

    auto chat = chatWith([["t1"], [], ["t3", "t1"]]);
    // Refused calls count as uses: the refusal travels as the tool response
    // to a recorded structured call, so the request-side ToolMessage exists
    // either way - the scan reads request-side toolCalls only and never
    // tries to tell them apart.
    chat.add(ToolMessage("summary mentions t2", JSONValue("not an array")));

    auto used = scanUsedTools(chat);
    assert(used == ["t1", "t3"], used.to!string); // t1 dup collapses; prose t2 ignored
    assert(scanUsedTools(Chat()).empty);
}

@("activateTag: unknown tag is a full no-op; known tag appends once and " ~ "bumps the counter")
unittest {
    import std.conv : to;

    auto pool = [
        fixtureTool("t1", ["workarea"]), fixtureTool("t2", ["workarea"])
    ];
    auto resolved = ToolBrokerConfig.init;
    resolved.enabled = true;
    resolved.groups["workarea"] = GroupConfig("workarea tools", ["t1", "t2"]);

    BrokerState st;
    activateTag(st, pool, "nope", resolved);
    assert(st.activated.empty);
    assert(st.tagActivationCount.get("nope", 0) == 0);

    activateTag(st, pool, "workarea", resolved);
    activateTag(st, pool, "workarea", resolved);
    assert(st.activated == ["t1", "t2"], st.activated.to!string);
    assert(st.tagActivationCount["workarea"] == 2);
}

@("activateTag: a group with no visible pool tools is a no-op that does " ~ "not bump the count")
unittest {
    auto pool = [fixtureTool("t1", [])];
    auto resolved = ToolBrokerConfig.init;
    resolved.enabled = true;
    resolved.groups["empty_grp"] = GroupConfig("nothing registered", [
        "ghost_tool"
    ]);

    BrokerState st;
    activateTag(st, pool, "empty_grp", resolved);
    assert(st.activated.empty);
    assert(st.tagActivationCount.empty);
}

@("inGroup/groupsOf: the config tools list UNION runtime tags matching a "
        ~ "group name; runtime tags naming no group contribute nothing")
unittest {
    import std.array : empty;

    auto resolved = ToolBrokerConfig.init;
    resolved.enabled = true;
    resolved.groups["workarea"] = GroupConfig("workarea file tools", ["wf_read"]);

    auto confTool = fixtureTool("wf_read", []); // config-listed member
    auto runtimeTool = fixtureTool("mcp_read", ["workarea"]); // runtime tag names the group
    auto strayTool = fixtureTool("odd", ["not_a_group"]); // no defined group

    assert(inGroup(resolved, confTool, "workarea"));
    assert(!inGroup(resolved, confTool, "rag"));
    assert(inGroup(resolved, runtimeTool, "workarea"));
    assert(groupsOf(resolved, confTool) == ["workarea"]);
    assert(groupsOf(resolved, runtimeTool) == ["workarea"]);
    assert(groupsOf(resolved, strayTool).empty);
}

@("isVisible: ungrouped tool visible with and without config; hidden group "
        ~ "hides members; alwaysOn tool or group name un-hides")
unittest {
    auto resolved = ToolBrokerConfig.init;
    resolved.enabled = true;
    resolved.groups["workarea"] = GroupConfig("workarea file tools", [
        "wf_read", "wf_write"
    ]);
    resolved.groups["rag"] = GroupConfig("RAG tools", ["rag_search"]);
    resolved.hiddenTags = ["workarea"];

    auto ungrouped = fixtureTool("taskDone", []);
    auto grouped = fixtureTool("wf_read", []);

    // A tool in no group is alwaysOn - with a configured config and with the
    // struct defaults.
    assert(isVisible(resolved, ungrouped));
    assert(isVisible(ToolBrokerConfig.init, ungrouped));

    // A member of a hidden group is hidden; the group name in alwaysOn
    // un-hides the whole membership; the tool name un-hides the tool itself
    // (tool names OR group names in one array, D4).
    assert(!isVisible(resolved, grouped), "member of a hidden group");
    resolved.alwaysOn ~= "workarea";
    assert(isVisible(resolved, grouped), "group name in alwaysOn un-hides");
    resolved.alwaysOn = ["wf_read"];
    assert(isVisible(resolved, grouped), "tool name in alwaysOn un-hides");
}

@("isVisible: the kill switch (enabled: false) leaves everything visible")
unittest {
    auto resolved = ToolBrokerConfig.init;
    resolved.groups["workarea"] = GroupConfig("workarea file tools", ["wf_read"]);
    resolved.hiddenTags = ["workarea"];

    assert(isVisible(resolved, fixtureTool("wf_read", [])));

    // The activation seam is inert under the kill switch too: no state
    // mutation, no count bump.
    BrokerState st;
    activateTag(st, [fixtureTool("wf_read", [])], "workarea", resolved);
    assert(st.activated.empty);
    assert(st.tagActivationCount.empty);
}

@("selectTools: a neverHide tool in a hidden group stays in the head "
        ~ "(isVisible says hidden; the neverHide union-back wins)")
unittest {
    import std.algorithm : map;
    import std.array : array;
    import std.conv : to;

    auto resolved = ToolBrokerConfig.init;
    resolved.enabled = true;
    resolved.groups["rag"] = GroupConfig("RAG tools", ["nh_hidden"]);
    resolved.hiddenTags = ["rag"];

    auto pool = [fixtureTool("nh_hidden", [])];
    auto names = selectTools(pool, [], ["nh_hidden"], alwaysOnTools(resolved, pool)).map!(
            e => e["function"]["name"].str).array;
    assert(names == ["nh_hidden"], names.to!string);
}

@("activateTool: activates a single tool by name; unknown name no-ops")
unittest {
    auto pool = [fixtureTool("t1", ["workarea"]), fixtureTool("t2", ["rag"])];

    BrokerState st;
    activateTool(st, pool, "t2");
    assert(st.activated == ["t2"]);
    activateTool(st, pool, "t2"); // sticky: no duplicate
    assert(st.activated == ["t2"]);
    activateTool(st, pool, "ghost");
    assert(st.activated == ["t2"]);
    activateTool(st, pool, "t1");
    assert(st.activated == ["t2", "t1"]);
}

@("applyPrune removes inactive names from the activation list")
unittest {
    BrokerState st;
    st.activated = ["t1", "t2", "t3"];
    applyPrune(st, ["t2", "ghost"]);
    assert(st.activated == ["t1", "t3"]);
}

@("an undescribed group (empty description) still governs visibility and "
        ~ "activation - group descriptions are a discovery concern only")
unittest {
    import std.conv : to;

    auto resolved = ToolBrokerConfig.init;
    resolved.enabled = true;
    resolved.groups["misc"] = GroupConfig(null, ["mem_store"]); // undescribed
    resolved.hiddenTags = ["misc"];

    auto member = fixtureTool("mem_store", []);
    assert(!isVisible(resolved, member), "member of a hidden (undescribed) group");
    resolved.alwaysOn ~= "misc";
    assert(isVisible(resolved, member), "group name in alwaysOn un-hides");

    // Activation: the undescribed group activates its visible members and
    // bumps the count; a group that is not defined at all stays a no-op.
    auto pool = [fixtureTool("mem_store", []), fixtureTool("odd", [])];
    BrokerState st;
    activateTag(st, pool, "misc", resolved);
    assert(st.activated == ["mem_store"], st.activated.to!string);
    assert(st.tagActivationCount["misc"] == 1);

    BrokerState unknownSt;
    activateTag(unknownSt, pool, "not_a_group", resolved);
    assert(unknownSt.activated.empty);
    assert(unknownSt.tagActivationCount.empty);
}
