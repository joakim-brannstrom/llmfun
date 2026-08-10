/// MCP server that bridges JSON-RPC 2.0 to llmfun's @Function tool infrastructure.
///
/// The server runs as a std.concurrency actor: the main thread spawns
/// `runMcpServer` and communicates with it exclusively via messages
/// (McpServerConfig, McpShutdown, McpStarted, McpStopped, McpFailed).
/// There is no shared state between the threads.
///
/// Shutdown: the main thread sends McpShutdown and waits up to 30 s for
/// McpStopped. Since the transport uses non-blocking fd reads (see
/// transport.d), readMessage() never blocks the actor and shutdown is
/// always honoured promptly. The 30 s deadline is a safety net against
/// unforeseen edge cases.
module llm.mcp_server;

import logger = std.logger;
import std.algorithm : map;
import std.array : array;
import std.concurrency : Tid, receive, receiveTimeout, send;
import std.datetime : dur;
import std.json : JSONType, JSONValue;

import my.filter : ReFilter;
import my.path : Path;

import llm.agent.context : AgentContext;
import llm.app_config : createRag;
import llm.config : LlmConfig, readConfig, ToolFilter;
import llm.mcp_server.protocol;
import llm.mcp_server.transport : EOFException, StdioTransport, Transport;
import llm.mcp_server.types;
import llm.rag.rag : RAG;
import llm.skill : SkillManager, makeSkillManager;
import llm.tool_call : Context, descAllFunctions, executeFunc, filterToolDescriptions;

/// Carries only the scalar parameters needed to reconstruct LlmConfig
/// inside the actor thread. Avoids passing complex structs with associative
/// arrays that would violate std.concurrency aliasing requirements.
///
/// Note: userCliWorkArea is intentionally omitted because the MCP subcommand
/// has no --workarea CLI flag. The work area is read from the config file.
/// If MCP ever needs CLI work area override, extend this struct.
///
/// Note: userToLlmConfig() is not called in the actor because UserConfig.Mcp
/// has no fields that overlap with LlmConfig. Config file values are used as-is.
struct McpConfigData {
    string configPath;
    bool noCwdConfig;
    bool trustedConfig;
}

/// Sent from the main thread to the MCP server actor with the tool filter
/// configuration (include/exclude regex patterns) and LLM configuration data.
/// Uses immutable arrays so the message satisfies std.concurrency's
/// no-unshared-aliasing requirement.
struct McpServerConfig {
    ToolFilter filter;
    McpConfigData configData;
}

/// Sent from the MCP server actor to the main thread when the server is ready.
struct McpStarted {
}

/// Sent from the main thread to the MCP server actor to request shutdown.
struct McpShutdown {
}

@("Regression test for Critical #1: two pre-loaded lines -- the transport must return both without stalling even when the pipe write end stays open.")
unittest {
    import core.sys.posix.unistd : close, dup, dup2, pipe, write;
    import std.conv : text;
    import std.string : indexOf;

    // Save the original stdin fd so we can restore it after the test.
    // dup2(fds[0], 0) overwrites fd 0; without restore subsequent tests
    // that use stdin would see a closed fd.
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

    // hasData() must still see the second line even though poll on the
    // kernel buffer would return 0 (both lines were already read into
    // the C FILE buffer in the old buffered-stdin implementation).
    assert(transport.hasData(), "hasData() failed to see pre-loaded line 2 -- stall bug regression");

    // readMessage() must return the second line.
    auto msg2 = transport.readMessage();
    assert(msg2.length > 0, "readMessage() returned empty for line 2");
    assert(msg2.indexOf(`"ping"`) >= 0, text("Unexpected message: ", msg2));

    // After draining, hasData() should return false (no more data, pipe
    // still open -- no POLLHUP).
    assert(!transport.hasData(), "hasData() should be false after draining");

    // Cleanup.
    close(fds[1]);
    close(fds[0]);
}

/// Sent from the MCP server actor to the main thread when the server has stopped.
struct McpStopped {
    bool hadError;
}

/// Sent from the MCP server actor to the main thread when startup fails.
struct McpFailed {
    string msg;
}

/// Actor entry point: runs the MCP server on a stdio transport in its own
/// thread. All communication with the owner thread goes through messages:
/// the owner sends McpServerConfig (initial configuration) and McpShutdown,
/// and receives McpStarted/McpFailed/McpStopped back.
void runMcpServer(Tid ownerTid) {
    // Wait for the initial configuration from the owner thread.
    McpServerConfig conf;
    receive((immutable McpServerConfig c) { conf = cast() c; });

    // Reconstruct LlmConfig from the config data passed from the main thread.
    // This is fatal if it fails - we cannot operate without configuration.
    LlmConfig llmConf;
    try {
        llmConf = readConfig(conf.configData.configPath.Path, silent: true, noCwdConfig: conf.configData.noCwdConfig,
                trustedConfig: conf.configData.trustedConfig);
    } catch (Exception e) {
        logger.errorf("Failed to read configuration: %s", e.msg);
        send(ownerTid, McpFailed("Failed to read configuration: " ~ e.msg));
        return;
    }

    // Create RAG instance - non-fatal if it fails (graceful degradation).
    RAG rag;
    try {
        rag = createRag(llmConf);
        if (rag is null) {
            logger.warningf("RAG creation returned null, MCP will operate without RAG");
        }
    } catch (Exception e) {
        logger.warningf("Failed to create RAG: %s. MCP will operate without RAG.", e.msg);
        rag = null;
    }

    // Create AgentContext - fatal if it fails.
    AgentContext agentCtx;
    try {
        agentCtx = new AgentContext(llmConf, rag);
    } catch (Exception e) {
        logger.errorf("Failed to create AgentContext: %s", e.msg);
        send(ownerTid, McpFailed("Failed to create AgentContext: " ~ e.msg));
        if (rag !is null)
            rag.destroy;
        rag = null;
        return;
    }

    // Create SkillManager - non-fatal if it fails (graceful degradation).
    SkillManager skillMgr;
    try {
        skillMgr = makeSkillManager(llmConf);
        agentCtx.setSkillManager(skillMgr);
    } catch (Exception e) {
        skillMgr = null;
        logger.warningf("Failed to create SkillManager: %s. MCP will operate without skills.",
                e.msg);
    }

    // Set taskDone handler - logs the answer rather than triggering framework behavior.
    // Fatal if it fails - cannot operate without taskDone handler.
    try {
        agentCtx.setTaskDoneHandler((string answer) {
            logger.infof("MCP taskDone called: %s", answer);
        });
    } catch (Exception e) {
        logger.errorf("Failed to set taskDone handler: %s", e.msg);
        send(ownerTid, McpFailed("Failed to set taskDone handler: " ~ e.msg));
        if (rag !is null)
            rag.destroy;
        rag = null;
        return;
    }

    // The actor owns the transport and the server instance.
    // Wrap construction in try-catch - fatal if it fails.
    Transport transport;
    MCPServer server;
    try {
        transport = new StdioTransport();
        server = new MCPServer(llmConf.toolFilter.to, agentCtx);
    } catch (Exception e) {
        logger.errorf("Failed to create MCP server: %s", e.msg);
        send(ownerTid, McpFailed("Failed to create MCP server: " ~ e.msg));
        if (rag !is null)
            rag.destroy;
        rag = null;
        return;
    }

    bool running = true;
    bool hadError = false;
    bool started = false;
    // Clean up RAG and transport on exit; send appropriate shutdown message.
    scope (exit) {
        transport.close();
        if (rag !is null)
            rag.destroy;
        if (!started) {
            send(ownerTid, McpFailed("Server failed to start"));
        } else {
            send(ownerTid, McpStopped(hadError));
        }
    }

    send(ownerTid, McpStarted());
    started = true;
    logger.info("MCP server actor started");

    // Poll interval between control-message checks; transport input is
    // drained without blocking via hasData().
    immutable PollInterval = 10.dur!"msecs";

    while (running) {
        // Handle control messages; the timeout lets us poll the transport.
        receiveTimeout(PollInterval, (McpShutdown _) {
            logger.info("Shutdown requested, stopping MCP server");
            running = false;
        });
        if (!running)
            break;

        // Drain any pending transport input without blocking.
        while (running && transport.hasData()) {
            try {
                auto message = transport.readMessage();
                if (message.length == 0)
                    continue;

                auto response = server.handleMessage(message);
                if (response !is null)
                    transport.writeMessage(response);
            } catch (EOFException e) {
                logger.info("MCP client disconnected (EOF)");
                running = false;
                break;
            } catch (Exception e) {
                hadError = true;
                logger.warning("Error in MCP loop: ", e.msg);
            }
        }
    }
}

/// MCP server that exposes @Function-registered tools over JSON-RPC 2.0.
/// Takes a ReFilter at construction to control which tools are visible to
/// MCP clients. Uses descAllFunctions() and executeFunc() from llm.tool_call.
/// This class is a pure request/response handler -- the event loop lives in
/// the runMcpServer actor, which owns the transport.
class MCPServer {
private:
    ReFilter filter_;
    Context ctx;
    bool initialized_;

public:
    /// Construct server with tool filter and optional execution context.
    /// A null context works for stateless tools (most tools don't dereference Context).
    this(ReFilter filter, Context ctx = null) {
        this.filter_ = filter;
        this.ctx = ctx;
    }

    /// Parse a JSON-RPC message and return the serialized response string,
    /// or null for notifications (no response required).
    string handleMessage(string message) {
        if (message is null || message.length == 0)
            return null;

        JsonRpcRequest request;
        try {
            request = parseRequest(message);
        } catch (ProtocolException e) {
            if (e.isParseError)
                return serializeResponse(createParseErrorResponse());
            else
                return serializeResponse(createInvalidRequestResponse(JSONValue.init, e.msg));
        }

        // C4: Notifications (no id field) return null -- no response written.
        if (request.isNotification()) {
            return null;
        }

        trace("Received: " ~ message);
        auto response = handleRequest(request);

        auto json = serializeResponse(response);
        trace("Sent: " ~ json);
        return json;
    }

private:
    /// Route a parsed request to the appropriate handler.
    JsonRpcResponse handleRequest(JsonRpcRequest request) {
        // Reject methods other than initialize before the server is initialized.
        if (!initialized_ && request.method != "initialize"
                && request.method != "notifications/initialized"
                && request.method != "notifications/cancelled") {
            return createInvalidRequestResponse(request.id,
                    "Server not initialized. Send 'initialize' first.");
        }

        switch (request.method) {
        case "initialize":
            return handleInitialize(request);
            // NOTE: "notifications/initialized" and "notifications/cancelled" are
            // intentionally NOT handled in the switch below. They are notifications
            // (no id field), so handleMessage() returns null before reaching
            // handleRequest(). They are only whitelisted in the pre-init check
            // above to allow pre-init notifications without triggering the
            // "not initialized" error.
        case "tools/list":
            return handleToolsList(request);
        case "tools/call":
            return handleToolCall(request);
        case "resources/list":
            return handleResourcesList(request);
        case "prompts/list":
            return handlePromptsList(request);
        case "ping":
            return handlePing(request);
        default:
            return createMethodNotFoundResponse(request.id, request.method);
        }
    }

    /// Handle the MCP initialize handshake.
    JsonRpcResponse handleInitialize(JsonRpcRequest request) {
        initialized_ = true;

        JSONValue caps = JSONValue.emptyObject;
        JSONValue toolsCap = JSONValue.emptyObject;
        toolsCap["listChanged"] = false;
        caps["tools"] = toolsCap;

        JSONValue serverInfo = JSONValue.emptyObject;
        serverInfo["name"] = "llmfun-mcp";
        serverInfo["version"] = "0.1.0";

        JSONValue result = JSONValue.emptyObject;
        result["protocolVersion"] = "2024-11-05";
        result["capabilities"] = caps;
        result["serverInfo"] = serverInfo;

        logger.info("MCP Server initialized by client");
        return createSuccessResponse(request.id, result);
    }

    /// Handle tools/list -- return filtered tool descriptions in MCP format.
    JsonRpcResponse handleToolsList(JsonRpcRequest request) {
        // H1: descAllFunctions() returns JSONValue(JSONValue[]) -- guaranteed array.
        auto allTools = descAllFunctions();
        auto filtered = filterToolDescriptions(allTools, filter_);

        // Convert OpenAI-format tool descriptions to MCP ToolDefinition format.
        JSONValue[] mcpTools;
        foreach (toolEntry; filtered.array) {
            // Each entry has {"type": "function", "function": {"name": ..., "description": ..., "parameters": ...}}
            auto func = toolEntry["function"];
            mcpTools ~= convertToMcpTool(func);
        }

        JSONValue result = JSONValue.emptyObject;
        result["tools"] = JSONValue(mcpTools);
        return createSuccessResponse(request.id, result);
    }

    /// Handle tools/call -- execute a tool with the given arguments.
    JsonRpcResponse handleToolCall(JsonRpcRequest request) {
        try {
            if (request.params.type != JSONType.object)
                return createInvalidParamsResponse(request.id, "params must be an object");

            if ("name" !in request.params)
                return createInvalidParamsResponse(request.id, "Missing 'name' in params");
            if (request.params["name"].type != JSONType.string)
                return createInvalidParamsResponse(request.id, "'name' must be a string");
            auto name = request.params["name"].str;

            // ReFilter rejection: return MethodNotFound to avoid leaking tool existence.
            if (!filter_.match(name))
                return createMethodNotFoundResponse(request.id, name);

            // C1: params["arguments"] is already a parsed JSONValue object.
            // Pass it directly to executeFunc() -- do NOT stringify and re-parse.
            JSONValue args;
            if ("arguments" in request.params) {
                args = request.params["arguments"];
                if (args.type != JSONType.object)
                    return createInvalidParamsResponse(request.id,
                            "params.arguments must be an object");
            } else {
                args = JSONValue.emptyObject;
            }

            // Execute the tool with null Context (stateless tools don't dereference it).
            auto result = executeFunc(ctx, name, args);

            // Wrap result in MCP ToolResult content format.
            ToolResult toolResult;
            toolResult.isError = !result.success;
            toolResult.content = [ToolResult.Content("text", result.msg)];

            return createSuccessResponse(request.id, toolResult.toJson());
        } catch (Exception e) {
            logger.warning("Error in handleToolCall: ", e.toString());
            return createInternalErrorResponse(request.id, e.msg);
        }
    }

    /// Handle ping -- return empty object as result.
    JsonRpcResponse handlePing(JsonRpcRequest request) {
        JSONValue result = JSONValue.emptyObject;
        return createSuccessResponse(request.id, result);
    }

    /// Handle resources/list -- return empty array (no resources).
    JsonRpcResponse handleResourcesList(JsonRpcRequest request) {
        JSONValue result = JSONValue.emptyObject;
        JSONValue[] emptyArr;
        result["resources"] = JSONValue(emptyArr);
        return createSuccessResponse(request.id, result);
    }

    /// Handle prompts/list -- return empty array (no prompts).
    JsonRpcResponse handlePromptsList(JsonRpcRequest request) {
        JSONValue result = JSONValue.emptyObject;
        JSONValue[] emptyArr;
        result["prompts"] = JSONValue(emptyArr);
        return createSuccessResponse(request.id, result);
    }

    /// Convert an OpenAI-format function description to an MCP ToolDefinition.
    /// Input: {"name": ..., "description": ..., "parameters": {"type": "object", "properties": ..., "required": ...}}
    /// Output: {"name": ..., "description": ..., "inputSchema": {"type": "object", "properties": ..., "required": ...}}
    JSONValue convertToMcpTool(JSONValue func) pure @safe {
        ToolDefinition def;
        def.name = func["name"].str;
        def.description = func["description"].str;
        if (auto p = "parameters" in func)
            def.inputSchema = *p;
        else
            def.inputSchema = JSONValue.emptyObject;
        return def.toJson();
    }
}

/// Optional trace logging, enabled with version (TraceMcp).
private void trace(string msg) {
    version (TraceMcp) {
        logger.info("[MCP] " ~ msg);
    }
}

// --- Integration tests: MCPServer.handleMessage() ---

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
    // Note: With null Context, tools that require a specific context type will
    // fail with isError: true. This test verifies the response format is correct
    // regardless of tool success.
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

    // notifications/initialized is whitelisted before init (line 238).
    auto resp = server.handleMessage(`{"jsonrpc":"2.0","method":"notifications/initialized"}`);
    assert(resp is null, "notifications/initialized before init should return null (no response)");
}

@("Integration: notifications/cancelled before initialize returns null (no error)")
unittest {
    auto server = new MCPServer(ReFilter.init);
    // Do NOT initialize first.

    // notifications/cancelled is whitelisted before init (line 239).
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

@("Integration: full lifecycle -- initialize, list, call, ping, shutdown")
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
    // Note: With null Context, tools that require a specific context type will
    // fail with isError: true. This verifies the response format is correct.
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
