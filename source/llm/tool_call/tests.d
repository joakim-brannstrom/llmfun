/// Unit tests for initParams parameter decoding.
module llm.tool_call.tests;

import std.algorithm : canFind;
import std.array : empty;
import std.json : parseJSON;

import llm.tool_call : ParamOptional, initParams, toParams;

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
