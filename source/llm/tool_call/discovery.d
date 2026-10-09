/// Model-facing tool discovery for the Tool Broker.
///
/// With the broker enabled, tools in hidden groups are withheld from the
/// model's tools array until the model activates their group. This module
/// closes that loop:
///
///     listToolTags called without arguments lists the visible tools' groups
///     with their configured descriptions, so the model can pick one; called
///     with a group it activates the group (the harness-side loadToolTag step,
///     activateTag) and names the newly visible tools in its reply (their
///     schemas land in the next request's tools array), so the model can call
///     them immediately. Activations are sticky.
///
/// The tool itself is UNGROUPED (⇒ alwaysOn): it must never be hidden
/// or grouped out of existence - otherwise the discovery loop dies
/// with the very tools it is meant to reveal. The test below asserts the UDA
/// has empty tags; the startup warning side lives in config validation.
///
/// composeDiscoveryDesc (used by the Agent's tools-array build) appends
/// the group vocabulary to the discovery tool's own description.
module llm.tool_call.discovery;

import logger = std.logger;
import std.algorithm : canFind, countUntil, filter, map, min, sort;
import std.array : array, empty, join;
import std.conv : to, text;
import std.format : format;
import std.json : JSONValue;

import llm.config : ToolBrokerConfig;
import llm.tool_call : RegisterLlmFunctions, Context, ExecuteFuncResult, Function,
    RegFunction, RegParam, ParamDescription, ParamOptional, baseContextToSpecific;
import llm.tool_call.broker : BrokerState, activateTag, inGroup, toolDescription;
import llm.metric.monitor : MetricMonitor, brokerEvent;

mixin RegisterLlmFunctions!();

/// Discovery description cap: at most this many groups are
/// rendered - ordered by activation frequency then name — with an overflow
/// line pointing at the toolSearch meta-tool (the intended
/// overflow path).
enum MaxTagsInDiscoveryDesc = 20;

struct ListToolTagsParams {
    @ParamDescription("Group to load. Omit to list all groups with descriptions.")
    @ParamOptional string tag;
}

/// Group names of the visible pool: every group of the resolved config with
/// at least one visible pool member (membership per inGroup), in config-AA
/// order, each listed once. A runtime tag naming no defined group contributes
/// nothing. A runtime tag naming a group that is NOT in resolved.groups leaves
/// that group activatable but undiscoverable (never listed here or by
/// unknownGroupMsg) - intended MCP fallback; the MCP design (section 13) must
/// then either register config groups or define a listing rule for such groups.
private string[] visibleGroupNames(RegFunction[] pool, const ToolBrokerConfig resolved) @safe {
    string[] rval;
    foreach (name, _; resolved.groups)
        if (pool.canFind!(f => inGroup(resolved, f, name)))
            rval ~= name;
    return rval;
}

/// The config group descriptions (name - description), described groups only:
/// an undescribed (or empty-description) group is omitted - it stays
/// activatable but is not advertised (D3).
private string[string] groupDescriptions(const ToolBrokerConfig resolved) @safe {
    string[string] rval;
    foreach (name, g; resolved.groups)
        if (!g.description.empty)
            rval[name] = g.description;
    return rval;
}

/// Groups ordered by activation frequency (descending, most-activated first),
/// then name ascending for ties. Unactivated groups (count 0) come last,
/// alphabetically.
private string[] sortedTagNames(string[] tags, const BrokerState st) @safe {
    return tags.sort!((a, b) {
        auto pa = a in st.tagActivationCount;
        auto pb = b in st.tagActivationCount;
        immutable va = pa ? *pa : 0;
        immutable vb = pb ? *pb : 0;
        return va != vb ? va > vb : a < b;
    }).array;
}

/// One "name (description)" list entry; a group with no (or empty) configured
/// description renders as a bare name.
private string tagListEntry(string group, string[string] descriptions) @safe {
    if (auto p = group in descriptions) {
        return (*p).empty ? group : format!"%s (%s)"(group, *p);
    }
    return group;
}

/// Renders the capped tag list: at most MaxTagsInDiscoveryDesc
/// "name (description)" entries (ordered by the caller), then, when the cap
/// bites, an overflow line pointing at the toolSearch meta-tool
/// (the intended overflow path).
private string renderTagList(string[] tags, string[string] descriptions) @safe {
    string[] entries = tags[0 .. min(tags.length, MaxTagsInDiscoveryDesc)].map!(
            a => tagListEntry(a, descriptions)).array;
    if (tags.length > MaxTagsInDiscoveryDesc)
        entries ~= format!"…and %s more — use toolSearch"(tags.length - MaxTagsInDiscoveryDesc);
    return entries.join(", ");
}

/// A group is known (activatable in principle) iff it has a configured
/// description (D3). An undescribed group stays activatable - it is just not
/// advertised and not "known" to the metric events.
private bool isKnownGroup(string group, const ToolBrokerConfig resolved) @safe {
    auto g = group in resolved.groups;
    return g !is null && !g.description.empty;
}

/// Instructive unknown-group error: names the mistake, lists the
/// activatable groups with their descriptions, and says how to recover.
private string unknownGroupMsg(string group, RegFunction[] pool,
        const ToolBrokerConfig resolved, BrokerState st) @safe {
    auto groups = visibleGroupNames(pool, resolved);
    auto list = groups.empty ? "no groups are registered - every visible tool is alwaysOn (ungrouped)" : renderTagList(
            sortedTagNames(groups, st), groupDescriptions(resolved));
    return format!"error: unknown tool group '%s'. Valid groups: %s. Call listToolTags with one of them (or without a tag to list them)."(
            group, list);
}

/// Composes the discovery tool's own description (the tools-array build
/// calls this): the base text plus the configured group vocabulary with
/// descriptions, ordered by activation frequency then name, capped at
/// MaxTagsInDiscoveryDesc with an overflow line pointing at toolSearch.
/// Only DESCRIBED groups are advertised - an undescribed group stays
/// activatable but is not advertised here (D3). Visibility note: this static
/// description advertises every DESCRIBED group regardless of pool membership
/// (the signature has no pool), while the listToolTags list-mode shows only
/// pool-visible groups - parity with the pre-change tag-map behavior.
/// An empty vocabulary leaves the base text unchanged (no dangling "Tags:"
/// header).
/// Side-effect-free (AA and sort reads only); not marked pure (ldc2 purity
/// inference is unreliable on AA lookups, see llmfun_const_purity_gotchas).
string composeDiscoveryDesc(string baseText, ToolBrokerConfig resolved, BrokerState st) @safe {
    auto descriptions = groupDescriptions(resolved);
    auto groups = sortedTagNames(descriptions.byKey.array, st);
    if (groups.empty)
        return baseText; // nothing configured yet - no dangling "Tags:" header
    return baseText ~ "\n\nTags: " ~ renderTagList(groups, descriptions);
}

/// Emits the discovery broker events: a tag_discovery event
/// for every tagged lookup (known = the group has a configured description) and,
/// when the group activated visible tools, a tag_activation event with the
/// activated count. A toolsActivated < 0 (the known-group-zero-visible case)
/// emits no tag_activation event. Metrics failures never break the tool (local try/catch;
/// a null monitor is skipped - bare contexts emit nothing).
private void emitDiscoveryEvents(MetricMonitor mon, string agentName, string tag,
        bool known, long toolsActivated) @safe {
    if (mon is null)
        return;
    try {
        mon.recordEvent(brokerEvent("tag_discovery", agentName,
                ["tag": JSONValue(tag), "known": JSONValue(known),]));
        if (toolsActivated >= 0)
            mon.recordEvent(brokerEvent("tag_activation", agentName,
                    [
                        "tag": JSONValue(tag),
                        "toolsActivated": JSONValue(toolsActivated),
        ]));
    } catch (Exception e) {
        logger.tracef("broker metric failed: %s", e.msg);
    }
}

@Function("List available tool groups with descriptions. Call this first to "
        ~ "discover tools, then the tools become visible for immediate use.")
ExecuteFuncResult listToolTags(Context baseCtx, ListToolTagsParams params) {
    import llm.agent.context : AgentContext;

    mixin(baseContextToSpecific!AgentContext);

    // Kill switch: with the broker disabled every registered tool
    // is always visible, so discovery would only add confusion.
    if (!ctx.brokerEnabled) {
        return ExecuteFuncResult("error: the tool broker is disabled "
                ~ "(toolBroker.enabled: false) - every registered tool is " ~ "always visible (alwaysOn); discovery is inert",
                false);
    }

    if (params.tag.empty) {
        auto groups = visibleGroupNames(ctx.pool, ctx.brokerConf);
        if (groups.empty) {
            return ExecuteFuncResult("No tool groups are visible: every visible "
                    ~ "tool is alwaysOn (ungrouped). Discovery is unnecessary.", true);
        }
        return ExecuteFuncResult("Available tool groups: " ~ renderTagList(sortedTagNames(groups,
                ctx.broker), groupDescriptions(ctx.brokerConf)), true);
    }

    // Activation (the harness-side loadToolTag step): a KNOWN group - a
    // config-described group - activates its visible tools; an unknown group
    // errors instructively. No state change on error.
    auto members = ctx.pool.filter!(f => inGroup(ctx.brokerConf, f, params.tag)).array;

    if (members.empty) {
        if (isKnownGroup(params.tag, ctx.brokerConf)) {
            emitDiscoveryEvents(ctx.getMetricMonitor(), ctx.agentName, params.tag, true, -1);
            return ExecuteFuncResult(i"error: group '$(params.tag)' has no visible tools - all excluded by toolFilter"
                    .text, false);
        }
        emitDiscoveryEvents(ctx.getMetricMonitor(), ctx.agentName, params.tag, false, -1);
        return ExecuteFuncResult(unknownGroupMsg(params.tag, ctx.pool,
                ctx.brokerConf, ctx.broker), false);
    }

    activateTag(ctx.broker, ctx.pool, params.tag, ctx.brokerConf);
    // Metrics: tag_activation (toolsActivated = the group's visible pool
    // size) + tag_discovery (known = the group has a description).
    emitDiscoveryEvents(ctx.getMetricMonitor(), ctx.agentName, params.tag,
            isKnownGroup(params.tag, ctx.brokerConf), cast(long) members.length);
    // The activation change point: the owning Agent rebuilds the
    // model-facing tools array through ctx.rebuildTools, so the activated
    // tools are in the next request's tools key.
    if (ctx.rebuildTools !is null)
        ctx.rebuildTools();

    return ExecuteFuncResult(i"Loaded tools for group $(params.tag): $(members.map!(a => a.name))".text,
            true);
}

version (unittest) {
    import llm.agent.context : AgentContext;
    import llm.config : GroupConfig, LlmConfig;

    /// Minimal AgentContext for discovery tests: a real LlmConfig (for
    /// ctor defaults), no RAG; the pool, broker state, and resolved broker
    /// config are then injected directly (public fields).
    private AgentContext discoveryContext() {
        auto conf = LlmConfig();
        auto ctx = new AgentContext(conf, null, null);
        ctx.brokerConf.groups["workarea"] = GroupConfig("workarea file tools", [
            "wf_read"
        ]);
        ctx.brokerConf.groups["rag"] = GroupConfig("RAG knowledge base tools", [
        ]);
        return ctx;
    }

    /// A visible pool tool with the given name and tags.
    private RegFunction discoveryTool(string name, string[] tags) @safe pure nothrow {
        return RegFunction(name: name, desc: "desc of " ~ name, params: [
            RegParam("path", "string", "File to read", true, "")
        ], callback: null, tags: tags);
    }

}

unittest {
    // List mode: groups of the visible pool with config descriptions; an
    // undescribed group renders as a bare name.
    auto ctx = discoveryContext();
    ctx.brokerConf.groups["misc"] = GroupConfig(null, ["mem_store"]);
    ctx.brokerConf.groups["ghost"] = GroupConfig("ghost tools", [
        "not_registered"
    ]);
    ctx.pool = [
        discoveryTool("wf_read", ["workarea"]),
        discoveryTool("rag_search", ["rag"]), discoveryTool("mem_store", [
            "misc"
        ])
    ];
    ctx.brokerEnabled = true;

    auto rval = listToolTags(ctx, ListToolTagsParams());
    assert(rval.success);
    assert(rval.msg.canFind("workarea (workarea file tools)"), rval.msg);
    assert(rval.msg.canFind("rag (RAG knowledge base tools)"));
    assert(rval.msg.canFind("misc")); // undescribed group, no config desc
    assert(!rval.msg.canFind("misc ()"));
    assert(!rval.msg.canFind("ghost"), "zero-visible member groups are omitted");
}

unittest {
    // Ordering: activation frequency desc then name asc; cap at
    // MaxTagsInDiscoveryDesc with an overflow line pointing at toolSearch.
    auto ctx = discoveryContext();
    string[] groupNames;
    foreach (i; 0 .. MaxTagsInDiscoveryDesc + 2)
        groupNames ~= format!"tag%02d"(i);
    ctx.pool = [discoveryTool("only", groupNames)];
    ctx.brokerEnabled = true;
    foreach (g; groupNames)
        ctx.brokerConf.groups[g] = GroupConfig(null, ["only"]);
    ctx.broker.tagActivationCount["tag03"] = 1;

    auto rval = listToolTags(ctx, ListToolTagsParams());
    assert(rval.success);
    assert(rval.msg.canFind("tag00"));
    assert(!rval.msg.canFind("tag21")); // capped
    assert(rval.msg.canFind("…and 2 more — use toolSearch"));
    // the once-activated group comes before its count-0 siblings
    assert(rval.msg.countUntil("tag03") < rval.msg.countUntil("tag00"));
    assert(rval.msg.countUntil("tag00") < rval.msg.countUntil("tag01"));
}

unittest {
    // Activation: a known group activates its visible tools (sticky) and
    // names them in the reply (no card JSON: the schemas are already in the
    // next request's tools array, so re-sending them would double the parse).
    auto ctx = discoveryContext();
    ctx.pool = [
        discoveryTool("wf_read", ["workarea"]),
        discoveryTool("other", ["unrelated"])
    ];
    ctx.brokerEnabled = true;
    // Activation requires an enabled resolved config: the seam is inert
    // under the kill switch (brokerConf defaults to disabled here).
    ctx.brokerConf.enabled = true;
    ctx.brokerConf.groups["unrelated"] = GroupConfig(null, ["other"]);

    auto rval = listToolTags(ctx, ListToolTagsParams("workarea"));
    assert(rval.success);
    assert(rval.msg.canFind("Loaded tools for group workarea:"), rval.msg);
    assert(rval.msg.canFind("wf_read"), rval.msg);
    // sticky: the activation is recorded and re-listing ranks the group first
    assert(ctx.broker.activated.canFind("wf_read"));
    assert(ctx.broker.tagActivationCount["workarea"] == 1);
    auto again = listToolTags(ctx, ListToolTagsParams());
    assert(again.msg.canFind("workarea (workarea file tools)"));
    assert(again.msg.countUntil("workarea") < again.msg.countUntil("unrelated"));
}

unittest {
    // A described group whose every visible tool is excluded by the agent
    // toolFilter ⇒ instructive text, not a bare empty success; no state
    // change.
    auto ctx = discoveryContext();
    ctx.pool = [discoveryTool("other", ["unrelated"])];
    ctx.brokerEnabled = true;

    auto rval = listToolTags(ctx, ListToolTagsParams("workarea"));
    assert(!rval.success);
    assert(
            rval.msg == "error: group 'workarea' has no visible tools - all "
            ~ "excluded by toolFilter");
    assert(ctx.broker.activated.empty);
    assert(ctx.broker.tagActivationCount.empty);
}

unittest {
    // An unknown group ⇒ instructive error listing the activatable groups with
    // descriptions; no state change.
    auto ctx = discoveryContext();
    ctx.pool = [discoveryTool("other", ["unrelated"])];
    ctx.brokerConf.groups["unrelated"] = GroupConfig(null, ["other"]);
    ctx.brokerEnabled = true;

    auto rval = listToolTags(ctx, ListToolTagsParams("typo_group"));
    assert(!rval.success);
    assert(rval.msg.canFind("unknown tool group 'typo_group'"));
    assert(rval.msg.canFind("unrelated")); // the pool's valid group is listed
    assert(rval.msg.canFind("Call listToolTags with one of them"));
    assert(ctx.broker.activated.empty);
    assert(ctx.broker.tagActivationCount.empty);
}

unittest {
    // Every visible tool ungrouped ⇒ nothing to discover; the list mode says
    // so instead of returning an empty string.
    auto ctx = discoveryContext();
    ctx.pool = [discoveryTool("plain", [])];
    ctx.brokerEnabled = true;

    auto rval = listToolTags(ctx, ListToolTagsParams());
    assert(rval.success);
    assert(rval.msg.canFind("alwaysOn (ungrouped)"));
}

unittest {
    // Kill switch: with the broker disabled, discovery is inert —
    // instructive error, no state change, in both modes.
    auto ctx = discoveryContext();
    ctx.pool = [discoveryTool("wf_read", ["workarea"])];
    ctx.brokerEnabled = false;

    auto listed = listToolTags(ctx, ListToolTagsParams());
    assert(!listed.success);
    assert(listed.msg.canFind("broker is disabled"));
    assert(ctx.broker.activated.empty);

    auto loaded = listToolTags(ctx, ListToolTagsParams("workarea"));
    assert(!loaded.success);
    assert(loaded.msg.canFind("broker is disabled"));
    assert(ctx.broker.activated.empty);
}

unittest {
    // The composed description contains every described group (up to the
    // cap); an undescribed group is not advertised.
    ToolBrokerConfig resolved;
    resolved.groups["workarea"] = GroupConfig("workarea file tools", ["wf_read"]);
    resolved.groups["rag"] = GroupConfig("", ["rag_search"]); // undescribed
    resolved.groups["memory"] = GroupConfig(null, ["mem_store"]); // undescribed
    auto st = BrokerState();
    auto composed = composeDiscoveryDesc("Discover tools.", resolved, st);
    assert(composed.canFind("Discover tools."));
    assert(composed.canFind("workarea (workarea file tools)"));
    assert(composed.canFind("\n\nTags: "));
    assert(!composed.canFind("rag")); // undescribed - not advertised

    // Cap + overflow with MaxTagsInDiscoveryDesc + 2 described groups.
    ToolBrokerConfig big;
    foreach (i; 0 .. MaxTagsInDiscoveryDesc + 2)
        big.groups[format!"tag%02d"(i)] = GroupConfig(format!"desc %02d"(i), [
        "only"
    ]);
    auto st2 = BrokerState();
    st2.tagActivationCount["tag03"] = 1;
    auto bigDesc = composeDiscoveryDesc("base", big, st2);
    assert(bigDesc.canFind("tag00"));
    assert(!bigDesc.canFind("tag21")); // capped
    assert(bigDesc.canFind("…and 2 more — use toolSearch"));
    assert(bigDesc.countUntil("tag03") < bigDesc.countUntil("tag00"));
}

unittest {
    // Zero-visible subcases: a DESCRIBED group (not just a runtime tag name)
    // with no pool tool ⇒ the pinned instructive text; and, with no described
    // groups, an unknown group ⇒ the "no groups are registered" fallback
    // instead of an empty valid-groups list.
    auto ctx = discoveryContext();
    ctx.pool = [discoveryTool("plain", [])]; // no pool tool is a "rag" member
    ctx.brokerEnabled = true;

    auto rval = listToolTags(ctx, ListToolTagsParams("rag"));
    assert(!rval.success);
    assert(rval.msg == "error: group 'rag' has no visible tools - all " ~ "excluded by toolFilter");

    // composeDiscoveryDesc with an empty vocabulary: no dangling "Tags:" header.
    ToolBrokerConfig resolved;
    resolved.groups["rag"] = GroupConfig("", ["x"]); // undescribed only
    assert(composeDiscoveryDesc("Discover tools.", resolved, BrokerState()) == "Discover tools.");

    auto bare = new AgentContext(LlmConfig(), null, null);
    bare.pool = [discoveryTool("plain", [])];
    bare.brokerEnabled = true;
    auto miss = listToolTags(bare, ListToolTagsParams("nope"));
    assert(!miss.success);
    assert(miss.msg.canFind("unknown tool group 'nope'"));
    assert(miss.msg.canFind("no groups are registered"));
}

unittest {
    // M1: the discovery metric events carry the group semantics - known =
    // "the group has a description", toolsActivated = the group's visible
    // pool size; a miss records known=false and no activation event.
    import std.algorithm : filter, map;
    import std.array : array;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, rmdirRecurse, readText;
    import std.format : format;
    import std.json : parseJSON;
    import std.path : buildPath;
    import std.string : splitLines;

    import my.path : Path;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/discovery_m1_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto mon = new MetricMonitor(buildPath(tmpDir, "monitor.jsonl").Path);

    auto conf = LlmConfig();
    auto ctx = new AgentContext(conf, null, mon);
    ctx.brokerConf.groups["workarea"] = GroupConfig("workarea file tools", [
        "wf_read"
    ]);
    ctx.pool = [
        discoveryTool("wf_read", ["workarea"]),
        discoveryTool("other", ["unrelated"])
    ];
    ctx.brokerEnabled = true;

    // Hit: a described group activates its visible members.
    auto hit = listToolTags(ctx, ListToolTagsParams("workarea"));
    assert(hit.success);

    // Miss: an unknown group errors before any activation.
    auto miss = listToolTags(ctx, ListToolTagsParams("typo_group"));
    assert(!miss.success);

    auto events = readText(buildPath(tmpDir, "monitor.jsonl")).splitLines
        .filter!(a => !a.empty)
        .map!(a => parseJSON(a))
        .array;

    auto discoveries = events.filter!(e => e["kind"].str == "tag_discovery").array;
    assert(discoveries.length == 2, "the miss and the hit both record");
    assert(discoveries[0]["tag"].str == "workarea");
    assert(discoveries[0]["known"].boolean == true);
    assert(discoveries[1]["tag"].str == "typo_group");
    assert(discoveries[1]["known"].boolean == false);

    auto activations = events.filter!(e => e["kind"].str == "tag_activation").array;
    assert(activations.length == 1, "only the hit activates");
    assert(activations[0]["tag"].str == "workarea");
    assert(activations[0]["toolsActivated"].integer == 1, "the group's visible pool size");
}

unittest {
    // Guard: the discovery tool itself must stay described - the
    // description is what the registry lists. It must never be silently
    // emptied.
    import std.traits : getUDAs;

    enum uda = getUDAs!(listToolTags, Function)[0];
    assert(uda.desc.length > 0);
}

unittest {
    // Registration (mixin RegisterLlmFunctions!() after the imports): the
    // discovery tool is in the global registry.
    import llm.tool_call : getFunctions;

    assert(getFunctions().canFind!(f => f.name == "listToolTags"));
}
