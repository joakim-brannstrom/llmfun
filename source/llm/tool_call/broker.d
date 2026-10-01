/// Tool broker mechanics between the model and the global tool registry:
/// pool construction, selection, activation, and compression-time pruning
/// Pure functions and
/// per-agent in-memory state; unit-testable without a live model.
module llm.tool_call.broker;

import std.algorithm : canFind, filter, map;
import std.array : array, empty;
import std.json : JSONValue;
import std.sumtype : match;

import my.filter : ReFilter;

import llm.chat : Chat, ToolMessage;
import llm.tool_call : RegFunction;

/// Per-Agent-instance, in-memory broker state: activated tool names
/// in activation order. alwaysOn tools are never listed here.
struct BrokerState {
    string[] activated;
    /// Activation count per tag, feeding the discovery-tool presentation cap
    /// Bumped only when the tag matched at least
    /// one pool tool; an unknown tag is a full no-op.
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

/// Pure selection: untagged (tags.empty, alwaysOn) tools first
/// in REGISTRY order with neverHide tools forced into the head, then activated
/// tools in ACTIVATION order; deduplicated by name (first occurrence wins). A
/// tool that is both alwaysOn and activated appears once, in the head. The
/// pool is never sorted or reshuffled: identical state emits byte-identical
/// arrays (pinned by the unittest below).
JSONValue[] selectTools(RegFunction[] pool, string[] activated, string[] neverHide) @safe pure {
    JSONValue[] rval;
    string[] seen;

    foreach (f; pool.filter!(rf => rf.tags.empty || neverHide.canFind!(n => n == rf.name))) {
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

/// Explicit activation seam: appends the tag's pool tools in
/// activation order; deduplicated. An unknown tag (no pool tool matches) is a
/// full no-op: the instructive message is the loadToolTag layer's,
/// not this seam's. The activation count feeds the discovery-tool presentation
/// cap (frequency then name).
void activateTag(ref BrokerState st, RegFunction[] pool, string tag) @safe pure {
    bool anyTool;
    foreach (f; pool.filter!(rf => rf.tags.canFind(tag))) {
        anyTool = true;
        if (!st.activated.canFind!(n => n == f.name)) {
            st.activated ~= f.name;
        }
    }
    if (anyTool) {
        st.tagActivationCount[tag]++;
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

version (unittest) {
    /// Registry entry fixture; the callback is never invoked by these tests.
    private RegFunction fixtureTool(string name, string[] tags) @safe pure nothrow {
        return RegFunction(name: name, desc: "desc of " ~ name, params: [],
                callback: null, tags: tags);
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

    auto names = selectTools(pool, activated, neverHide).map!(e => e["function"]["name"].str).array;
    assert(names == ["always1", "nh1", "mid"], names.to!string);
}

@("selectTools: dedup, an activated alwaysOn tool appears once in the head")
unittest {
    import std.algorithm : map;
    import std.array : array;
    import std.conv : to;

    auto pool = [fixtureTool("t1", []), fixtureTool("t2", ["workarea"])];
    auto names = selectTools(pool, ["t1", "t2"], []).map!(e => e["function"]["name"].str).array;
    assert(names == ["t1", "t2"], names.to!string);
}

@("selectTools: byte-identical for identical inputs")
unittest {
    import std.json : JSONValue;

    auto pool = [fixtureTool("always1", []), fixtureTool("mid", ["workarea"])];
    auto activated = ["mid"];
    auto neverHide = ["always1"];

    auto s1 = JSONValue(selectTools(pool, activated, neverHide)).toString;
    auto s2 = JSONValue(selectTools(pool, activated, neverHide)).toString;
    assert(s1 == s2);
    assert(s1.length > 0);
}

@("selectTools: empty activated emits the alwaysOn head only; empty pool " ~ "emits nothing")
unittest {
    import std.algorithm : map;
    import std.array : array, empty;
    import std.conv : to;

    auto pool = [fixtureTool("always1", []), fixtureTool("t2", ["workarea"])];
    auto headOnly = selectTools(pool, [], ["taskDone"]).map!(e => e["function"]["name"].str).array;
    assert(headOnly == ["always1"], headOnly.to!string);

    assert(selectTools([], ["t1"], ["taskDone"]).empty);
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

@("activateTag: unknown tag is a full no-op; known tag appends once and " ~ "bumps the counter")
unittest {
    import std.conv : to;

    auto pool = [
        fixtureTool("t1", ["workarea"]), fixtureTool("t2", ["workarea"])
    ];

    BrokerState st;
    activateTag(st, pool, "nope");
    assert(st.activated.empty);
    assert(st.tagActivationCount.get("nope", 0) == 0);

    activateTag(st, pool, "workarea");
    activateTag(st, pool, "workarea");
    assert(st.activated == ["t1", "t2"], st.activated.to!string);
    assert(st.tagActivationCount["workarea"] == 2);
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
