/// Model-facing tool search — the second discovery path.
///
/// With the broker enabled, registered tools carrying a tag are hidden from
/// the model's tools array until their tag is activated (listToolTags, the
/// discovery tool). toolSearch is the keyword-shaped alternative for
/// the long tail the tag presentation cap deliberately truncates: a dumb
/// case-insensitive substring match over tool names, descriptions and tags
/// (no new infrastructure — pure string matching over the visible pool
/// only, in-memory), returning tool cards for immediate use and activating
/// every match through the same explicit per-tool activation seam the tag
/// path uses (activateTool — the per-tool variant of activateTag). Activation
/// is sticky and rebuilds the model-facing tools array through the
/// context's rebuildTools hook.
///
/// The tool itself is UNtagged (⇒ alwaysOn): it must never be hidden
/// or tagged out of existence — otherwise the discovery loop dies
/// with the very tools it is meant to reveal. The test below asserts the UDA
/// carries a non-empty description (it has no tags field to assert on - the
/// compile-time tag channel is gone); the startup-warning side lives in
/// config validation (validateToolBrokerConfig, the Agent ctor's seam).
module llm.tool_call.search;

import logger = std.logger;
import std.algorithm : canFind, filter, map;
import std.array : array, empty;
import std.format : format;
import std.json : JSONValue;
import std.string : toLower;

import llm.tool_call : RegisterLlmFunctions, Context, ExecuteFuncResult,
    Function, RegFunction, RegParam, ParamDescription, baseContextToSpecific;
import llm.tool_call.broker : BrokerState, activateTool, toolDescription;
import llm.metric.monitor : MetricMonitor, brokerEvent;

mixin RegisterLlmFunctions!();

struct ToolSearchParams {
    @ParamDescription("Case-insensitive keyword matched against tool names, descriptions and tags.")
    string query;
}

/// Emits the tool_search broker events: one per search call — the
/// matched count is the hit/miss signal (matched > 0 ⇒ hit) and the activated
/// count is how many visible pool tools the search newly activated
/// (re-searches of the same keyword re-activate nothing — sticky — so
/// activated=0 there is the honest activation-rate signal). Metrics failures
/// never break the tool (local try/catch; a null monitor is skipped — bare
/// contexts emit nothing).
private void emitSearchEvents(MetricMonitor mon, string agentName, string query,
        long matched, long activated) @safe {
    if (mon is null)
        return;
    try {
        mon.recordEvent(brokerEvent("tool_search", agentName, [
            "query": JSONValue(query),
            "matched": JSONValue(matched),
            "activated": JSONValue(activated),
        ]));
    } catch (Exception e) {
        logger.tracef("broker metric failed: %s", e.msg);
    }
}

@Function("Search tools by keyword. Case-insensitive match over tool "
        ~ "names, descriptions and tags. Discovered tools are activated " ~ "for immediate use.")
ExecuteFuncResult toolSearch(Context baseCtx, ToolSearchParams params) {
    import llm.agent.context : AgentContext;

    mixin(baseContextToSpecific!AgentContext);

    // Kill switch: with the broker disabled every registered tool
    // is always visible, so search would only add confusion.
    if (!ctx.brokerEnabled) {
        return ExecuteFuncResult(
                "error: the tool broker is disabled " ~ "(toolBroker.enabled: false) — every registered tool is "
                ~ "always visible (alwaysOn); discovery is inert", false);
    }

    // An empty query is a substring of everything: it would match — and
    // activate — the whole visible pool. Require a keyword instead.
    if (params.query.empty) {
        return ExecuteFuncResult("error: empty search query — provide a "
                ~ "keyword to match against tool names, descriptions and tags", false);
    }

    // Dumb case-insensitive substring match over the VISIBLE pool:
    // toolFilter-excluded tools are not discoverable — and stay
    // refused at the dispatch site (not in the pool). The tag arm matches
    // SUBSTRING per tag (tags feed the search: tag = coarse search) —
    // canFind on a string[] with a string needle would be element equality,
    // hence the per-tag predicate.
    auto needle = params.query.toLower;
    auto matched = ctx.pool.filter!(f => f.name.toLower.canFind(needle)
            || f.desc.toLower.canFind(needle) || f.tags
                .map!(a => a.toLower)
                .canFind!(t => t.canFind(needle))).array;

    if (matched.empty) {
        // No state change on a miss; the instructive text names the
        // recovery path: tags are the coarse vocabulary.
        emitSearchEvents(ctx.getMetricMonitor(), ctx.agentName, params.query, 0, 0);
        return ExecuteFuncResult(format("No visible tools match '%s'. Tags are "
                ~ "the coarse search: call listToolTags (without a tag) to list " ~ "the available tags with descriptions.",
                params.query), true);
    }

    // Activation (the loadToolTag step's per-tool variant): activate
    // every match through the per-tool seam, sticky — dedup is the
    // seam's (an already-activated tool is not re-appended).
    immutable before = ctx.broker.activated.length;
    foreach (f; matched)
        activateTool(ctx.broker, ctx.pool, f.name);
    immutable activated = cast(long)(ctx.broker.activated.length - before);

    // The activation change point: the owning Agent rebuilds the
    // model-facing tools array through this hook, so the activated tools are
    // in the next request's tools key.
    if (ctx.rebuildTools !is null)
        ctx.rebuildTools();

    // Metrics: one tool_search event per search with the matched
    // and activated counts.
    emitSearchEvents(ctx.getMetricMonitor(), ctx.agentName, params.query,
            cast(long) matched.length, activated);

    // Tool cards (the standard shape) for immediate use; no cap on search
    // results (few expected) — the search is the overflow path for the
    // discovery description cap.
    JSONValue[] cards;
    foreach (f; matched)
        cards ~= toolDescription(f);
    return ExecuteFuncResult(JSONValue(cards).toPrettyString, true);
}

version (unittest) {
    import llm.agent.context : AgentContext;
    import llm.config : LlmConfig;

    /// Minimal AgentContext for search tests (mirrors discovery.d's
    /// discoveryContext): a real LlmConfig, no RAG; the pool and broker state
    /// are injected directly (public fields).
    private AgentContext searchContext(MetricMonitor mon = null) {
        auto conf = LlmConfig();
        return new AgentContext(conf, null, mon);
    }

    /// A visible pool tool with the given name, description and tags.
    private RegFunction searchTool(string name, string desc, string[] tags) @safe pure nothrow {
        return RegFunction(name: name, desc: desc, params: [
            RegParam("path", "string", "File to read", true, "")
        ], callback: null, tags: tags);
    }
}

unittest {
    // Match by name (case-insensitive); cards in the standard tools-array
    // shape; every match activated (sticky) through activateTool; the
    // rebuildTools hook fires so the owning Agent rebuilds the tools array.
    auto ctx = searchContext();
    ctx.pool = [
        searchTool("wf_read", "Reads a workarea file", ["workarea"]),
        searchTool("mem_store", "Stores a memory", ["memory"])
    ];
    ctx.brokerEnabled = true;

    size_t rebuilds;
    ctx.rebuildTools = delegate() @safe { rebuilds++; };

    auto rval = toolSearch(ctx, ToolSearchParams("WF_READ"));
    assert(rval.success);
    import std.json : parseJSON;

    auto cards = parseJSON(rval.msg).array;
    assert(cards.length == 1);
    assert(cards[0]["type"].str == "function");
    assert(cards[0]["function"]["name"].str == "wf_read");
    assert(cards[0]["function"]["description"].str == "Reads a workarea file");
    assert(("path" in cards[0]["function"]["parameters"]["properties"]) !is null);
    assert(ctx.broker.activated.canFind("wf_read"));
    assert(rebuilds == 1);

    // Sticky: a re-search re-activates nothing (dedup in the seam) and the
    // cards come back.
    auto again = toolSearch(ctx, ToolSearchParams("wf_read"));
    assert(again.success);
    assert(parseJSON(again.msg).array.length == 1);
    assert(ctx.broker.activated.length == 1);
    assert(rebuilds == 2); // the hook still fires (a harmless no-op rebuild)
}

unittest {
    // Match by description and by tag (both case-insensitive); the tag match
    // is a SUBSTRING per tag (tags feed the search: tag = coarse search).
    auto ctx = searchContext();
    ctx.pool = [
        searchTool("alpha", "compiles the report", ["build"]),
        searchTool("beta", "stores things", ["workarea_file"]),
        searchTool("gamma", "unrelated", ["misc"])
    ];
    ctx.brokerEnabled = true;

    auto byDesc = toolSearch(ctx, ToolSearchParams("COMPILES THE"));
    assert(byDesc.success);
    assert(byDesc.msg.canFind("alpha"));
    assert(!byDesc.msg.canFind("beta"));

    auto byTag = toolSearch(ctx, ToolSearchParams("area_fil"));
    assert(byTag.success);
    assert(byTag.msg.canFind("beta"));
    assert(!byTag.msg.canFind("gamma"));
}

unittest {
    // Zero matches ⇒ instructive empty text naming the recovery path;
    // no state change: nothing activated, no rebuild.
    auto ctx = searchContext();
    ctx.pool = [searchTool("alpha", "unrelated", ["misc"])];
    ctx.brokerEnabled = true;

    size_t rebuilds;
    ctx.rebuildTools = delegate() @safe { rebuilds++; };

    auto rval = toolSearch(ctx, ToolSearchParams("no_such_keyword"));
    assert(rval.success);
    assert(rval.msg.canFind("No visible tools match 'no_such_keyword'"));
    assert(rval.msg.canFind("listToolTags"));
    assert(ctx.broker.activated.empty);
    assert(rebuilds == 0);
}

unittest {
    // Kill switch: with the broker disabled every tool is
    // alwaysOn — search is inert, instructive error, no state change.
    // An empty query would match everything (it is a substring of it all)
    // and is refused instructively, also with no state change.
    auto ctx = searchContext();
    ctx.pool = [searchTool("alpha", "unrelated", ["misc"])];
    ctx.brokerEnabled = false;

    auto off = toolSearch(ctx, ToolSearchParams("alpha"));
    assert(!off.success);
    assert(off.msg.canFind("broker is disabled"));
    assert(ctx.broker.activated.empty);

    ctx.brokerEnabled = true;
    auto emptyQuery = toolSearch(ctx, ToolSearchParams(""));
    assert(!emptyQuery.success);
    assert(emptyQuery.msg.canFind("empty search query"));
    assert(ctx.broker.activated.empty);
}

unittest {
    // Metrics: one tool_search event per search — hit (matched > 0)
    // and miss both land in the JSONL sink with kind+ts+agent.
    import std.datetime : Clock;
    import std.file : mkdirRecurse, rmdirRecurse, readText;
    import std.json : parseJSON;
    import std.path : buildPath;
    import std.string : splitLines;

    import my.path : Path;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/search_t14_events_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto dataFile = buildPath(tmpDir, "monitor.jsonl").Path;

    auto mon = new MetricMonitor(dataFile);
    auto ctx = searchContext(mon);
    ctx.pool = [
        searchTool("wf_read", "reads a file", ["workarea"]),
        searchTool("mem_store", "stores memory", ["memory"])
    ];
    ctx.brokerEnabled = true;

    // Hit: wf_read matches by name and is activated.
    toolSearch(ctx, ToolSearchParams("wf_read"));
    // Miss: nothing matches.
    toolSearch(ctx, ToolSearchParams("zzz_no_match"));

    const lines = readText(dataFile).splitLines.filter!(a => !a.empty).array;
    assert(lines.length == 2);
    auto hit = parseJSON(lines[0]);
    assert(hit["kind"].str == "tool_search");
    assert(hit["agent"].str.empty); // bare context: no agent name set
    assert(hit["query"].str == "wf_read");
    assert(hit["matched"].integer == 1);
    assert(hit["activated"].integer == 1);
    auto miss = parseJSON(lines[1]);
    assert(miss["kind"].str == "tool_search");
    assert(miss["matched"].integer == 0);
    assert(miss["activated"].integer == 0);
}

unittest {
    // Guard: the discovery meta-tools (listToolTags + toolSearch)
    // must stay described - the description is what the registry lists, and
    // their untagged/alwaysOn status is what keeps them permanently visible
    // (the startup-warning side lives in validateToolBrokerConfig).
    import std.traits : getUDAs;

    import llm.tool_call.discovery : listToolTags;

    enum listUda = getUDAs!(listToolTags, Function)[0];
    assert(listUda.desc.length > 0);
    enum searchUda = getUDAs!(toolSearch, Function)[0];
    assert(searchUda.desc.length > 0);
}

unittest {
    // Registration (mixin RegisterLlmFunctions!() after the imports): the
    // search tool is in the global registry.
    import llm.tool_call : getFunctions;

    assert(getFunctions().canFind!(f => f.name == "toolSearch"));
}
