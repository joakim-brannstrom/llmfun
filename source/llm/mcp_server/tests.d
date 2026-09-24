/// Unit tests for MCPServer.handleMessage and the StdioTransport buffered-stdin regression. Relocated from llm/mcp_server/package.d: unittest blocks in a package.d module compile and link but are never discovered by the `dub test` runner (dub's generated dub_test_root registers no package.d modules), so these assertions never executed there.
module llm.mcp_server.tests;

import std.array : array;
import std.json : JSONType;

import llm.mcp_server;
import llm.mcp_server.transport : StdioTransport;
import llm.mcp_server.types : JsonRpcErrorCode;
import my.filter : ReFilter;

// --- Integration tests: MCPServer.handleMessage() ---
@("Regression test for Critical #1: two pre-loaded lines -- the transport must return both without stalling even when the pipe write end stays open.")
unittest {
    import core.sys.posix.unistd : close, dup, dup2, pipe, write;
    import std.conv : text;
    import std.string : indexOf;

    // Save the original stdin fd so we can restore it after the test. dup2(fds[0], 0) overwrites fd 0; without restore subsequent tests that use stdin would see a closed fd.
    int savedStdin = dup(0);
    scope (exit) {
        dup2(savedStdin, 0);
        close(savedStdin);
    }

    // Create a pipe and redirect its read end to stdin.
    int[2] fds;
    assert(pipe(fds) == 0, "pipe failed");
    dup2(fds[0], 0); // read end -> stdin fd 0

    // Write two complete JSON lines, keeping the write end OPEN.
    auto line1 = `{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}` ~ "\n";
    auto line2 = `{"jsonrpc":"2.0","id":2,"method":"ping","params":{}}` ~ "\n";
    assert(write(fds[1], line1.ptr, line1.length) == line1.length);
    assert(write(fds[1], line2.ptr, line2.length) == line2.length);

    auto transport = new StdioTransport();

    // hasData() must see the first line.
    assert(transport.hasData(), "hasData() failed to see pre-loaded line 1");

    // readMessage() must return the first line.
    auto msg1 = transport.readMessage();
    assert(msg1.length > 0, "readMessage() returned empty for line 1");
    assert(msg1.indexOf(`"initialize"`) >= 0, text("Unexpected message: ", msg1));

    // hasData() must still see the second line even though poll on the kernel buffer would return 0 (both lines were already read into the C FILE buffer in the old buffered-stdin implementation).
    assert(transport.hasData(), "hasData() failed to see pre-loaded line 2 -- stall bug regression");

    // readMessage() must return the second line.
    auto msg2 = transport.readMessage();
    assert(msg2.length > 0, "readMessage() returned empty for line 2");
    assert(msg2.indexOf(`"ping"`) >= 0, text("Unexpected message: ", msg2));

    // After draining, hasData() should return false (no more data, pipe still open -- no POLLHUP).
    assert(!transport.hasData(), "hasData() should be false after draining");

    // Cleanup.
    close(fds[1]);
    close(fds[0]);
}

@("Integration: initialize handshake returns capabilities, protocolVersion, serverInfo")
unittest {
    import std.json : parseJSON;

    auto server = new MCPServer(ReFilter.init);

    auto resp = server.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);
    assert(resp !is null, "initialize should return a response");

    auto parsed = parseJSON(resp);
    assert(parsed["jsonrpc"].str == "2.0");
    assert(parsed["id"].integer == 1);
    assert(parsed["result"]["protocolVersion"].str == "2024-11-05");
    assert(parsed["result"]["capabilities"]["tools"].type == JSONType.object);
    assert(parsed["result"]["serverInfo"]["name"].str == "llmfun-mcp");
    assert(parsed["result"]["serverInfo"]["version"].str == "0.1.0");
    assert("error" !in parsed);
}

@("Integration: tools/list returns tools in MCP format with name, description, inputSchema")
unittest {
    import std.json : parseJSON;

    auto server = new MCPServer(ReFilter.init);
    server.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);

    auto resp = server.handleMessage(`{"jsonrpc":"2.0","id":2,"method":"tools/list"}`);
    assert(resp !is null);

    auto parsed = parseJSON(resp);
    assert(parsed["id"].integer == 2);
    assert(parsed["result"]["tools"].type == JSONType.array);

    // Verify at least one tool exists and has the required fields.
    auto tools = parsed["result"]["tools"].array;
    assert(tools.length > 0, "tools/list should return at least one tool");
    foreach (tool; tools) {
        assert("name" in tool, "tool must have name");
        assert("description" in tool, "tool must have description");
        assert("inputSchema" in tool, "tool must have inputSchema");
        assert(tool["name"].type == JSONType.string);
        assert(tool["description"].type == JSONType.string);
    }
}

@("Integration: tools/call executes a registered tool and returns ToolResult content")
unittest {
    import std.json : parseJSON;

    auto server = new MCPServer(ReFilter.init);
    server.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);

    // Get tools list and find one with no required parameters.
    auto listResp = server.handleMessage(`{"jsonrpc":"2.0","id":2,"method":"tools/list"}`);
    auto listParsed = parseJSON(listResp);
    auto tools = listParsed["result"]["tools"].array;

    string toolName;
    foreach (tool; tools) {
        auto name = tool["name"].str;
        auto inputSchema = tool["inputSchema"];
        // Check if tool has no required parameters.
        bool hasRequired = false;
        if ("required" in inputSchema.object) {
            auto required = inputSchema.object["required"];
            hasRequired = (required.type == JSONType.array) && (required.array.length > 0);
        }
        if (!hasRequired) {
            toolName = name;
            break;
        }
    }
    assert(toolName != "", "found a tool with no required parameters");

    // Call the tool with empty arguments.
    auto callResp = server.handleMessage(
            `{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"`
            ~ toolName ~ `","arguments":{}}}`);
    assert(callResp !is null);

    auto parsed = parseJSON(callResp);
    assert(parsed["id"].integer == 3);
    assert(parsed["result"]["content"].type == JSONType.array);
    assert(parsed["result"]["content"].array.length > 0);
    assert(parsed["result"]["content"].array[0]["type"].str == "text");
    // Note: With null Context, tools that require a specific context type will fail with isError: true. This test verifies the response format is correct regardless of tool success.
    auto isErrorType = parsed["result"]["isError"].type;
    assert(isErrorType == JSONType.true_ || isErrorType == JSONType.false_,
            "isError should be a boolean");
}

@("Integration: tools/call with ReFilter-rejected tool returns MethodNotFound")
unittest {
    import std.algorithm : startsWith;
    import std.json : parseJSON;

    // Filter that excludes all tools.
    auto server = new MCPServer(ReFilter([], ["^.*$"]));
    server.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);

    auto resp = server.handleMessage(
            `{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"anytool","arguments":{}}}`);
    assert(resp !is null);

    auto parsed = parseJSON(resp);
    assert(parsed["error"]["code"].integer == JsonRpcErrorCode.MethodNotFound);
    assert(parsed["error"]["message"].str.startsWith("Method not found:"));
}

@("Integration: ping returns empty object result")
unittest {
    import std.json : parseJSON;

    auto server = new MCPServer(ReFilter.init);
    server.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);

    auto resp = server.handleMessage(`{"jsonrpc":"2.0","id":2,"method":"ping"}`);
    assert(resp !is null);

    auto parsed = parseJSON(resp);
    assert(parsed["id"].integer == 2);
    assert(parsed["result"].type == JSONType.object);
    assert("error" !in parsed);
}

@("Integration: unknown method returns MethodNotFound error")
unittest {
    import std.json : parseJSON;

    auto server = new MCPServer(ReFilter.init);
    server.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);

    auto resp = server.handleMessage(`{"jsonrpc":"2.0","id":2,"method":"unknown/method"}`);
    assert(resp !is null);

    auto parsed = parseJSON(resp);
    assert(parsed["error"]["code"].integer == JsonRpcErrorCode.MethodNotFound);
    assert(parsed["error"]["message"].str == "Method not found: unknown/method");
}

@("Integration: request before initialize returns InvalidRequest error")
unittest {
    import std.algorithm : startsWith;
    import std.json : parseJSON;

    auto server = new MCPServer(ReFilter.init);

    auto resp = server.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"tools/list"}`);
    assert(resp !is null);

    auto parsed = parseJSON(resp);
    assert(parsed["error"]["code"].integer == JsonRpcErrorCode.InvalidRequest);
    assert(parsed["error"]["message"].str.startsWith("Server not initialized"));
}

@("Integration: notification (no id) returns null from handleMessage")
unittest {
    auto server = new MCPServer(ReFilter.init);
    server.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);

    auto resp = server.handleMessage(`{"jsonrpc":"2.0","method":"notifications/initialized"}`);
    assert(resp is null, "notification should return null (no response)");
}

@("Integration: resources/list returns empty array")
unittest {
    import std.json : parseJSON;

    auto server = new MCPServer(ReFilter.init);
    server.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);

    auto resp = server.handleMessage(`{"jsonrpc":"2.0","id":2,"method":"resources/list"}`);
    assert(resp !is null);

    auto parsed = parseJSON(resp);
    assert(parsed["id"].integer == 2);
    assert(parsed["result"]["resources"].type == JSONType.array);
    assert(parsed["result"]["resources"].array.length == 0);
}

@("Integration: prompts/list returns empty array")
unittest {
    import std.json : parseJSON;

    auto server = new MCPServer(ReFilter.init);
    server.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);

    auto resp = server.handleMessage(`{"jsonrpc":"2.0","id":2,"method":"prompts/list"}`);
    assert(resp !is null);

    auto parsed = parseJSON(resp);
    assert(parsed["id"].integer == 2);
    assert(parsed["result"]["prompts"].type == JSONType.array);
    assert(parsed["result"]["prompts"].array.length == 0);
}

@("Integration: malformed JSON returns ParseError with null id")
unittest {
    import std.json : parseJSON;

    auto server = new MCPServer(ReFilter.init);

    auto resp = server.handleMessage(`{"jsonrpc":"2.0","id":`);
    assert(resp !is null);

    auto parsed = parseJSON(resp);
    assert(parsed["id"].type == JSONType.null_, "parse error response must have null id");
    assert(parsed["error"]["code"].integer == JsonRpcErrorCode.ParseError);
}

@("Integration: invalid JSON-RPC (missing method) returns InvalidRequest")
unittest {
    import std.json : parseJSON;

    auto server = new MCPServer(ReFilter.init);

    auto resp = server.handleMessage(`{"jsonrpc":"2.0","id":1}`);
    assert(resp !is null);

    auto parsed = parseJSON(resp);
    assert(parsed["error"]["code"].integer == JsonRpcErrorCode.InvalidRequest);
}

@("Integration: tools/call with missing name returns InvalidParams")
unittest {
    import std.json : parseJSON;
    import std.string : indexOf;

    auto server = new MCPServer(ReFilter.init);
    server.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);

    auto resp = server.handleMessage(
            `{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"arguments":{}}}`);
    assert(resp !is null);

    auto parsed = parseJSON(resp);
    assert(parsed["error"]["code"].integer == JsonRpcErrorCode.InvalidParams);
    assert(indexOf(parsed["error"]["message"].str, "name") >= 0);
}

@("Integration: tools/call with non-object arguments returns InvalidParams")
unittest {
    import std.json : parseJSON;

    auto server = new MCPServer(ReFilter.init);
    server.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);

    auto resp = server.handleMessage(
            `{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"anytool","arguments":"notanobject"}}`);
    assert(resp !is null);

    auto parsed = parseJSON(resp);
    assert(parsed["error"]["code"].integer == JsonRpcErrorCode.InvalidParams);
}

@("Integration: tools/list with restrictive ReFilter reduces tool count")
unittest {
    import std.json : parseJSON;

    // Get the full tool list with no filter.
    auto fullServer = new MCPServer(ReFilter.init);
    fullServer.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);
    auto fullResp = fullServer.handleMessage(`{"jsonrpc":"2.0","id":2,"method":"tools/list"}`);
    auto fullTools = parseJSON(fullResp)["result"]["tools"].array.length;

    // Get the filtered tool list with an exclude-all filter.
    auto filtServer = new MCPServer(ReFilter([], ["^.*$"]));
    filtServer.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);
    auto filtResp = filtServer.handleMessage(`{"jsonrpc":"2.0","id":2,"method":"tools/list"}`);
    auto filtTools = parseJSON(filtResp)["result"]["tools"].array.length;
    // The filtered list should have fewer tools than the full list.
    assert(fullTools > 0, "Full tool list should not be empty for this test");
    assert(filtTools < fullTools,
            "Filtered tools (" ~ filtTools.stringof
            ~ ") should be < full tools (" ~ fullTools.stringof ~ ")");
    assert(filtTools == 0, "Exclude-all filter should return zero tools");
}

@("Integration: tools/call with non-existent tool returns isError true")
unittest {
    import std.json : parseJSON;

    auto server = new MCPServer(ReFilter.init);
    server.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);

    auto resp = server.handleMessage(
            `{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"__nonexistent_tool__","arguments":{}}}`);
    assert(resp !is null);

    auto parsed = parseJSON(resp);
    // Tool not found should return isError: true in the content.
    assert(parsed["result"]["isError"].boolean == true, "non-existent tool should fail");
    assert(parsed["result"]["content"].type == JSONType.array);
}

@("Integration: notifications/initialized before initialize returns null (no error)")
unittest {
    auto server = new MCPServer(ReFilter.init);
    // Do NOT initialize first.

    // notifications/initialized is whitelisted before init.
    auto resp = server.handleMessage(`{"jsonrpc":"2.0","method":"notifications/initialized"}`);
    assert(resp is null, "notifications/initialized before init should return null (no response)");
}

@("Integration: notifications/cancelled before initialize returns null (no error)")
unittest {
    auto server = new MCPServer(ReFilter.init);
    // Do NOT initialize first.

    // notifications/cancelled is whitelisted before init.
    auto resp = server.handleMessage(`{"jsonrpc":"2.0","method":"notifications/cancelled"}`);
    assert(resp is null, "notifications/cancelled before init should return null (no response)");
}

@("Integration: duplicate initialize calls are idempotent")
unittest {
    import std.json : parseJSON;

    auto server = new MCPServer(ReFilter.init);

    auto resp1 = server.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);
    assert(resp1 !is null);
    assert(parseJSON(resp1)["result"]["protocolVersion"].str == "2024-11-05");

    auto resp2 = server.handleMessage(`{"jsonrpc":"2.0","id":2,"method":"initialize","params":{}}`);
    assert(resp2 !is null);
    assert(parseJSON(resp2)["result"]["protocolVersion"].str == "2024-11-05");
}

@("Integration: error response format matches JSON-RPC 2.0 spec")
unittest {
    import std.json : parseJSON;

    auto server = new MCPServer(ReFilter.init);
    server.handleMessage(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);

    // Trigger a MethodNotFound error.
    auto resp = server.handleMessage(`{"jsonrpc":"2.0","id":42,"method":"bogus"}`);
    assert(resp !is null);

    auto parsed = parseJSON(resp);
    // Must have jsonrpc, id, and error fields.
    assert(parsed["jsonrpc"].str == "2.0");
    assert(parsed["id"].integer == 42);
    assert("error" in parsed);
    assert("code" in parsed["error"]);
    assert("message" in parsed["error"]);
    // Must NOT have result field.
    assert("result" !in parsed);
}

@("Integration: full lifecycle -- initialize, tools/list, tools/call, ping, notification")
unittest {
    import std.json : parseJSON;

    auto server = new MCPServer(ReFilter.init);

    // Step 1: Initialize.
    auto initResp = server.handleMessage(
            `{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`);
    assert(initResp !is null);
    assert(parseJSON(initResp)["result"]["protocolVersion"].str == "2024-11-05");

    // Step 2: List tools.
    auto listResp = server.handleMessage(`{"jsonrpc":"2.0","id":2,"method":"tools/list"}`);
    assert(listResp !is null);
    auto tools = parseJSON(listResp)["result"]["tools"].array;
    assert(tools.length > 0);

    // Step 3: Call a tool.
    string toolName = tools[0]["name"].str;
    auto callResp = server.handleMessage(
            `{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"`
            ~ toolName ~ `","arguments":{}}}`);
    // Note: With null Context, tools that require a specific context type will fail with isError: true. This verifies the response format is correct.
    auto callParsed = parseJSON(callResp);
    assert(callParsed["result"]["content"].type == JSONType.array);
    auto isErrorType = callParsed["result"]["isError"].type;
    assert(isErrorType == JSONType.true_ || isErrorType == JSONType.false_,
            "isError should be a boolean");

    // Step 4: Ping.
    auto pingResp = server.handleMessage(`{"jsonrpc":"2.0","id":4,"method":"ping"}`);
    assert(pingResp !is null);
    assert(parseJSON(pingResp)["result"].type == JSONType.object);

    // Step 5: Notification (no response expected).
    auto notifResp = server.handleMessage(`{"jsonrpc":"2.0","method":"notifications/initialized"}`);
    assert(notifResp is null);
}
