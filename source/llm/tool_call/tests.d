/// Unit tests for initParams parameter decoding.
module llm.tool_call.tests;

import std.algorithm : canFind;
import std.array : empty;
import std.json : JSONValue, parseJSON;

import llm.tool_call : Context, ExecuteFuncResult, Function, ParamOptional, initParams, toParams;

/// Sample params struct used by the initParams unit tests (was private in llm/tool_call/package.d; moved here since only these tests use it).
private struct TestCmdParams {
    string[] command;
    @ParamOptional string cwd;
}

unittest {
    // Double-encoded array: a string containing JSON array integers must produce a hint that names the fix instead of a bare type-mismatch error.
    auto json = parseJSON(`{"command": "[1, 2]"}`);
    auto params = initParams!TestCmdParams(json, toParams!TestCmdParams);
    assert(params.errorMsg.length > 0);
    assert(params.errorMsg.canFind("array of strings"), params.errorMsg);
    assert(params.errorMsg.canFind("command"), params.errorMsg);
}

unittest {
    // Double-encoded array: a string containing JSON array text should decode the json array to a D string array
    auto json = parseJSON(`{"command": "[\"cd\", \"llmfun\"]"}`);
    auto params = initParams!TestCmdParams(json, toParams!TestCmdParams);
    assert(params.errorMsg.empty);
    assert(params.value.command[0] == "cd");
    assert(params.value.command[1] == "llmfun");
}

unittest {
    // A real array converts without error.
    auto okJson = parseJSON(`{"command": ["cd", "llmfun"]}`);
    auto okParams = initParams!TestCmdParams(okJson, toParams!TestCmdParams);
    assert(okParams.errorMsg.length == 0, okParams.errorMsg);
    assert(okParams.value.command == ["cd", "llmfun"]);
}

unittest {
    // A plain string (not JSON) for an array parameter gets the generic received/expected message.
    auto strJson = parseJSON(`{"command": "cd llmfun"}`);
    auto strParams = initParams!TestCmdParams(strJson, toParams!TestCmdParams);
    assert(strParams.errorMsg.canFind("received string"), strParams.errorMsg);
    assert(strParams.errorMsg.canFind("string[]"), strParams.errorMsg);
}

// --- @Function UDA tags mirrored into RegFunction ---

/// Empty params struct for the tags fixtures (nothing to decode).
private struct TagsFixtureParams {
}

/// Fixture whose @Function UDA carries tags; registered by the unittests below via addFunction.
/// The resulting module-global registry entry is a deliberate, benign test leak (addFunction dedupes by name).
@Function(desc : "tags fixture tool", tags:
        ["workarea"]) private void tagsUdaFixture(Context ctx, TagsFixtureParams params) {
}

/// (Context, JSONValue)-shaped callback matching the RegFunction.callback type.
private ExecuteFuncResult tagsFixtureRawCallback(Context ctx, JSONValue args) {
    return ExecuteFuncResult("ok", true);
}

unittest {
    // Registration mirrors the UDA: a registered RegFunction carries the tags from its @Function UDA.
    import std.algorithm : filter;
    import std.array : array;
    import std.conv : text;
    import std.traits : getUDAs;

    import llm.tool_call : RegFunction, addFunction, getFunctions, toParams;

    enum uda = getUDAs!(tagsUdaFixture, Function)[0];
    addFunction(RegFunction(name: "tags_uda_fixture_tool", desc: uda.desc, params: toParams!TagsFixtureParams,
            callback: &tagsFixtureRawCallback, tags: uda.tags));
    auto hits = getFunctions.filter!(f => f.name == "tags_uda_fixture_tool").array;
    assert(hits.length == 1);
    assert(hits[0].tags == ["workarea"], text(hits[0].tags));
}

unittest {
    // Duplicate-name registration warns and ignores: first registration wins (order-proof).
    import std.algorithm : filter;
    import std.array : array;
    import std.conv : text;
    import std.traits : getUDAs;

    import llm.tool_call : RegFunction, addFunction, getFunctions, toParams;

    enum uda = getUDAs!(tagsUdaFixture, Function)[0];
    addFunction(RegFunction(name: "tags_dup_probe", desc: uda.desc, params: toParams!TagsFixtureParams,
            callback: &tagsFixtureRawCallback, tags: ["first"]));
    addFunction(RegFunction(name: "tags_dup_probe", desc: "other", params: toParams!TagsFixtureParams,
            callback: &tagsFixtureRawCallback, tags: ["second"]));
    auto hits = getFunctions.filter!(f => f.name == "tags_dup_probe").array;
    assert(hits.length == 1, "duplicate addFunction must be ignored");
    assert(hits[0].tags == ["first"], text(hits[0].tags));
}

// --- KnownToolTag enum check (unknownToolTags / knownToolTagNames) ---

unittest {
    // Every known enum member passes the registration-time check silently.
    import llm.tool_call.tags : KnownToolTag, unknownToolTags;
    import std.conv : to;
    import std.traits : EnumMembers;

    string[] all;
    static foreach (e; EnumMembers!KnownToolTag)
        all ~= e.to!string;
    assert(unknownToolTags(all).empty);
}

unittest {
    // A typoed tag is reported; unknown in => unknown out, order kept.
    import llm.tool_call.tags : unknownToolTags;

    assert(unknownToolTags(["workareaa"]) == ["workareaa"]);
    assert(unknownToolTags(["workarea", "ragg", "workareaa"]) == [
        "ragg", "workareaa"
    ]);
    assert(unknownToolTags([]).empty);
}

unittest {
    // knownToolTagNames lists the enum members comma-joined, for warning text.
    import std.algorithm : canFind;

    import llm.tool_call.tags : knownToolTagNames;

    const names = knownToolTagNames();
    assert(names.canFind("workarea"), names);
    assert(names.canFind("mcp"), names);
    assert(!names.canFind("workareaa"), names);
}
