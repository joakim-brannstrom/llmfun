/// Model-facing tool discovery for the Tool Broker.
///
/// With the broker enabled, registered tools carrying a tag are hidden from
/// the model's tools array until the model activates their tag. This module
/// closes that loop:
///
///     listToolTags called without arguments lists the visible tools' tags
///     with their configured descriptions, so the model can pick one; called
///     with a tag it activates the tag (the harness-side loadToolTag step,
///     activateTag) and names the newly visible tools in its reply (their
///     schemas land in the next request's tools array), so the model can call
///     them immediately. Activations are sticky.
///
/// The tool itself is UNtagged (⇒ alwaysOn): it must never be hidden
/// or tagged out of existence — otherwise the discovery loop dies
/// with the very tools it is meant to reveal. The test below asserts the UDA
/// has empty tags; the startup warning side lives in config validation.
///
/// composeDiscoveryDesc (used by the Agent's tools-array build) appends
/// the tag vocabulary to the discovery tool's own description.
module llm.tool_call.discovery;

import logger = std.logger;
import std.algorithm : canFind, countUntil, filter, map, min, sort;
import std.array : array, empty, join;
import std.conv : to, text;
import std.format : format;
import std.json : JSONValue;
import std.traits : EnumMembers;

import llm.tool_call : RegisterLlmFunctions, Context, ExecuteFuncResult, Function,
    RegFunction, RegParam, ParamDescription, ParamOptional, baseContextToSpecific;
import llm.tool_call.broker : BrokerState, activateTag, toolDescription;
import llm.tool_call.tags : KnownToolTag;
import llm.metric.monitor : MetricMonitor, brokerEvent;

mixin RegisterLlmFunctions!();

/// Discovery description cap: at most this many tags are
/// rendered - ordered by activation frequency then name — with an overflow
/// line pointing at the toolSearch meta-tool (the intended
/// overflow path).
enum MaxTagsInDiscoveryDesc = 20;

struct ListToolTagsParams {
    @ParamDescription("Tag to load. Omit to list all tags with descriptions.")
    @ParamOptional string tag;
}

/// Tags present on the visible pool tools, first-appearance order (registry
/// order), each listed once (tag match is element equality).
private string[] poolTagNames(RegFunction[] pool) @safe {
    bool[string] seen;
    string[] tags;
    foreach (f; pool) {
        foreach (t; f.tags) {
            if (t !in seen) {
                seen[t] = true;
                tags ~= t;
            }
        }
    }
    return tags;
}

/// Tags the discovery tool can act on: tags on visible pool tools plus the
/// config-described vocabulary (the activatable set). Deduped.
private string[] activatableTagNames(RegFunction[] pool, string[string] descriptions) @safe {
    auto tags = poolTagNames(pool);
    foreach (t; descriptions.byKey.filter!(a => !tags.canFind(a)))
        tags ~= t;
    return tags;
}

/// Tags ordered by activation frequency (descending, most-activated first),
/// then name ascending for ties. Unactivated tags (count 0) come last,
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

/// One "name (description)" list entry; a tag with no (or empty) configured
/// description renders as a bare name (the missing-description warn-once side
/// is the description composer's concern).
private string tagListEntry(string tag, string[string] descriptions) @safe {
    if (auto p = tag in descriptions) {
        return (*p).empty ? tag : format!"%s (%s)"(tag, *p);
    }
    return tag;
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

/// A tag is known (activatable in principle) if it is a KnownToolTag
/// member or has a configured description (MCP tags get config
/// entries).
private bool isKnownTag(string tag, string[string] descriptions) @safe {
    return [EnumMembers!KnownToolTag].map!(a => a.to!string)
        .canFind!(m => m == tag) || (tag in descriptions) !is null;
}

/// Instructive unknown-tag error: names the mistake, lists the
/// activatable tags with their descriptions, and says how to recover.
private string unknownTagMsg(string tag, RegFunction[] pool,
        string[string] descriptions, BrokerState st) @safe {
    auto tags = activatableTagNames(pool, descriptions);
    auto list = tags.empty
        ? "no tagged tools are registered — every visible tool is alwaysOn (untagged)" : renderTagList(
                sortedTagNames(tags, st), descriptions);
    return format!"error: unknown tool tag '%s'. Valid tags: %s. Call listToolTags with one of them (or without a tag to list them)."(
            tag, list);
}

/// Composes the discovery tool's own description (the tools-array build
/// calls this): the base text plus the configured tag vocabulary with
/// descriptions, ordered by activation frequency then name, capped at
/// MaxTagsInDiscoveryDesc with an overflow line pointing at toolSearch.
/// An empty vocabulary leaves the base text unchanged (no dangling "Tags:"
/// header).
/// A tag with no (or empty) description renders as a bare name; the
/// warn-once missing-description warning is the tools-array build's concern.
/// Side-effect-free (AA and sort reads only); not marked pure (ldc2 purity
/// inference is unreliable on AA lookups, see llmfun_const_purity_gotchas).
string composeDiscoveryDesc(string baseText, string[string] tagDescriptions, BrokerState st) @safe {
    auto tags = sortedTagNames(tagDescriptions.byKey.array, st);
    if (tags.empty)
        return baseText; // nothing configured yet — no dangling "Tags:" header
    return baseText ~ "\n\nTags: " ~ renderTagList(tags, tagDescriptions);
}

/// Emits the discovery broker events: a tag_discovery event
/// for every tagged lookup (known = the tag resolved as a known tag) and,
/// when the tag activated visible tools, a tag_activation event with the
/// activated count. A toolsActivated < 0 (the known-tag-zero-visible case)
/// emits no tag_activation event. Metrics failures never break the tool (local try/catch;
/// a null monitor is skipped — bare contexts emit nothing).
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

@Function("List available tool tags with descriptions. Call this first to "
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
        auto tags = poolTagNames(ctx.pool);
        if (tags.empty) {
            return ExecuteFuncResult("No tagged tools are visible: every visible "
                    ~ "tool is alwaysOn (untagged). Discovery is unnecessary.", true);
        }
        return ExecuteFuncResult("Available tool tags: " ~ renderTagList(sortedTagNames(tags,
                ctx.broker), ctx.toolTagDescriptions()), true);
    }

    // Activation (the harness-side loadToolTag step): a KNOWN tag — a
    // KnownToolTag member or a config-described tag — activates its visible
    // tools; an unknown tag errors instructively. No state change on error.
    auto tagged = ctx.pool.filter!(f => f.tags.canFind(params.tag)).array;

    if (tagged.empty) {
        if (isKnownTag(params.tag, ctx.toolTagDescriptions())) {
            emitDiscoveryEvents(ctx.getMetricMonitor(), ctx.agentName, params.tag, true, -1);
            return ExecuteFuncResult(i"error: tag '$(params.tag)' has no visible tools - all excluded by toolFilter"
                    .text, false);
        }
        emitDiscoveryEvents(ctx.getMetricMonitor(), ctx.agentName, params.tag, false, -1);
        return ExecuteFuncResult(unknownTagMsg(params.tag, ctx.pool,
                ctx.toolTagDescriptions(), ctx.broker), false);
    }

    activateTag(ctx.broker, ctx.pool, params.tag);
    // Metrics: tag_activation (toolsActivated = the tag's pool
    // size) + tag_discovery (known = the tag resolved as a known tag).
    emitDiscoveryEvents(ctx.getMetricMonitor(), ctx.agentName, params.tag,
            isKnownTag(params.tag, ctx.toolTagDescriptions()), cast(long) tagged.length);
    // The activation change point: the owning Agent rebuilds the
    // model-facing tools array through ctx.rebuildTools, so the activated
    // tools are in the next request's tools key.
    if (ctx.rebuildTools !is null)
        ctx.rebuildTools();

    return ExecuteFuncResult(i"Loaded tools for tag $(params.tag): $(tagged.map!(a => a.name))".text,
            true);
}

version (unittest) {
    import llm.agent.context : AgentContext;
    import llm.config : LlmConfig;

    /// Minimal AgentContext for discovery tests: a real LlmConfig (for
    /// toolTagDescriptions + ctor defaults), no RAG; the pool and broker state are
    /// then injected directly (public fields).
    private AgentContext discoveryContext() {
        auto conf = LlmConfig();
        conf.toolBroker.toolTagDescriptions = [
            "workarea": "workarea file tools",
            "rag": "RAG knowledge base tools",
        ];
        return new AgentContext(conf, null, null);
    }

    /// A visible pool tool with the given name and tags.
    private RegFunction discoveryTool(string name, string[] tags) @safe pure nothrow {
        return RegFunction(name: name, desc: "desc of " ~ name, params: [
            RegParam("path", "string", "File to read", true, "")
        ], callback: null, tags: tags);
    }

}

unittest {
    // List mode: pool tags with config descriptions; a tag in the enum but
    // with no config description renders as a bare name (the missing-desc
    // warn-once side is the composer's).
    auto ctx = discoveryContext();
    ctx.pool = [
        discoveryTool("wf_read", ["workarea"]),
        discoveryTool("rag_search", ["rag"]),
        discoveryTool("mem_store", ["memory"])
    ];
    ctx.brokerEnabled = true;

    auto rval = listToolTags(ctx, ListToolTagsParams());
    assert(rval.success);
    assert(rval.msg.canFind("workarea (workarea file tools)"), rval.msg);
    assert(rval.msg.canFind("rag (RAG knowledge base tools)"));
    assert(rval.msg.canFind("memory")); // enum member, no config desc
    assert(!rval.msg.canFind("memory ()"));
}

unittest {
    // Ordering: activation frequency desc then name asc; cap at
    // MaxTagsInDiscoveryDesc with an overflow line pointing at toolSearch.
    auto ctx = discoveryContext();
    string[] tagNames;
    foreach (i; 0 .. MaxTagsInDiscoveryDesc + 2)
        tagNames ~= format!"tag%02d"(i);
    ctx.pool = [discoveryTool("only", tagNames)];
    ctx.brokerEnabled = true;
    ctx.broker.tagActivationCount["tag03"] = 1;

    auto rval = listToolTags(ctx, ListToolTagsParams());
    assert(rval.success);
    assert(rval.msg.canFind("tag00"));
    assert(!rval.msg.canFind("tag21")); // capped
    assert(rval.msg.canFind("…and 2 more — use toolSearch"));
    // the once-activated tag comes before its count-0 siblings
    assert(rval.msg.countUntil("tag03") < rval.msg.countUntil("tag00"));
    assert(rval.msg.countUntil("tag00") < rval.msg.countUntil("tag01"));
}

unittest {
    // Activation: a known tag activates its visible tools (sticky) and
    // names them in the reply (no card JSON: the schemas are already in the
    // next request's tools array, so re-sending them would double the parse).
    auto ctx = discoveryContext();
    ctx.pool = [
        discoveryTool("wf_read", ["workarea"]),
        discoveryTool("other", ["unrelated"])
    ];
    ctx.brokerEnabled = true;

    auto rval = listToolTags(ctx, ListToolTagsParams("workarea"));
    assert(rval.success);
    assert(rval.msg.canFind("Loaded tools for tag workarea:"), rval.msg);
    assert(rval.msg.canFind("wf_read"), rval.msg);
    // sticky: the activation is recorded and re-listing ranks the tag first
    assert(ctx.broker.activated.canFind("wf_read"));
    assert(ctx.broker.tagActivationCount["workarea"] == 1);
    auto again = listToolTags(ctx, ListToolTagsParams());
    assert(again.msg.canFind("workarea (workarea file tools)"));
    assert(again.msg.countUntil("workarea") < again.msg.countUntil("unrelated"));
}

unittest {
    // A known tag (enum member) whose every visible tool is excluded by the
    // agent toolFilter ⇒ instructive text, not a bare empty success; no state
    // change.
    auto ctx = discoveryContext();
    ctx.pool = [discoveryTool("other", ["unrelated"])];
    ctx.brokerEnabled = true;

    auto rval = listToolTags(ctx, ListToolTagsParams("workarea"));
    assert(!rval.success);
    assert(rval.msg == "error: tag 'workarea' has no visible tools - all "
            ~ "excluded by toolFilter");
    assert(ctx.broker.activated.empty);
    assert(ctx.broker.tagActivationCount.empty);
}

unittest {
    // An unknown tag ⇒ instructive error listing the activatable tags with
    // descriptions; no state change.
    auto ctx = discoveryContext();
    ctx.pool = [discoveryTool("other", ["unrelated"])];
    ctx.brokerEnabled = true;

    auto rval = listToolTags(ctx, ListToolTagsParams("typo_tag"));
    assert(!rval.success);
    assert(rval.msg.canFind("unknown tool tag 'typo_tag'"));
    assert(rval.msg.canFind("unrelated")); // the pool's valid tag is listed
    assert(rval.msg.canFind("Call listToolTags with one of them"));
    assert(ctx.broker.activated.empty);
    assert(ctx.broker.tagActivationCount.empty);
}

unittest {
    // Every visible tool alwaysOn ⇒ nothing to discover; the list mode says
    // so instead of returning an empty string.
    auto ctx = discoveryContext();
    ctx.pool = [discoveryTool("plain", [])];
    ctx.brokerEnabled = true;

    auto rval = listToolTags(ctx, ListToolTagsParams());
    assert(rval.success);
    assert(rval.msg.canFind("alwaysOn (untagged)"));
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
    // The composed description contains every tag+description pair (up to the
    // cap); a tag with no (or empty) description renders as a bare name.
    string[string] desc = [
        "workarea": "workarea file tools", "rag": "", "memory": null,
    ];
    auto st = BrokerState();
    auto composed = composeDiscoveryDesc("Discover tools.", desc, st);
    assert(composed.canFind("Discover tools."));
    assert(composed.canFind("workarea (workarea file tools)"));
    assert(composed.canFind("\n\nTags: "));
    assert(!composed.canFind("rag ()")); // empty desc ⇒ bare name

    // Cap + overflow with MaxTagsInDiscoveryDesc + 2 undescribed tags.
    string[string] big;
    foreach (i; 0 .. MaxTagsInDiscoveryDesc + 2)
        big[format!"tag%02d"(i)] = null;
    auto st2 = BrokerState();
    st2.tagActivationCount["tag03"] = 1;
    auto bigDesc = composeDiscoveryDesc("base", big, st2);
    assert(bigDesc.canFind("tag00"));
    assert(!bigDesc.canFind("tag21")); // capped
    assert(bigDesc.canFind("…and 2 more — use toolSearch"));
    assert(bigDesc.countUntil("tag03") < bigDesc.countUntil("tag00"));
}

unittest {
    // Zero-visible subcases: a CONFIG-DESCRIBED tag (not just an enum member)
    // with no pool tool ⇒ the pinned instructive text; and, with an empty
    // config vocabulary, an unknown tag ⇒ the "no tagged tools are registered"
    // fallback instead of an empty valid-tags list.
    auto ctx = discoveryContext();
    ctx.pool = [discoveryTool("plain", [])]; // no pool tool carries "rag"
    ctx.brokerEnabled = true;

    auto rval = listToolTags(ctx, ListToolTagsParams("rag"));
    assert(!rval.success);
    assert(rval.msg == "error: tag 'rag' has no visible tools - all " ~ "excluded by toolFilter");

    // composeDiscoveryDesc with an empty vocabulary: no dangling "Tags:" header.
    assert(composeDiscoveryDesc("Discover tools.", null, BrokerState()) == "Discover tools.");

    auto bare = new AgentContext(LlmConfig(), null, null);
    bare.pool = [discoveryTool("plain", [])];
    bare.brokerEnabled = true;
    auto miss = listToolTags(bare, ListToolTagsParams("nope"));
    assert(!miss.success);
    assert(miss.msg.canFind("unknown tool tag 'nope'"));
    assert(miss.msg.canFind("no tagged tools are registered"));
}

unittest {
    // Guard: the discovery tool itself must stay untagged (⇒
    // alwaysOn) — it must never be tagged out of existence.
    import std.traits : getUDAs;

    enum uda = getUDAs!(listToolTags, Function)[0];
    assert(uda.tags.empty);
}

unittest {
    // Registration (mixin RegisterLlmFunctions!() after the imports): the
    // discovery tool is in the global registry.
    import llm.tool_call : getFunctions;

    assert(getFunctions().canFind!(f => f.name == "listToolTags"));
}
