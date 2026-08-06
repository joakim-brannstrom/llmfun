module llm.mcp_server.protocol;

import std.json : JSONType, JSONValue, parseJSON;
import llm.mcp_server.types;

/// Exception thrown during JSON-RPC protocol parsing or validation.
class ProtocolException : Exception {
    /// True if the exception was caused by malformed JSON (parse error).
    bool isParseError;

    this(string msg, bool parseError = false) {
        super(msg);
        isParseError = parseError;
    }
}

/// Parse a JSON-RPC 2.0 request from a JSON string.
/// Throws `ProtocolException` with `isParseError=true` for malformed JSON,
/// or `isParseError=false` for structurally invalid requests.
JsonRpcRequest parseRequest(string jsonText) {
    JSONValue json;
    try {
        json = parseJSON(jsonText);
    } catch (Exception e) {
        throw new ProtocolException("Parse error: " ~ e.msg, true);
    }

    if (json.type != JSONType.object)
        throw new ProtocolException("Invalid request: expected JSON object", false);

    if ("jsonrpc" !in json)
        throw new ProtocolException("Invalid request: missing jsonrpc field", false);
    if (json["jsonrpc"].type != JSONType.string || json["jsonrpc"].str != "2.0")
        throw new ProtocolException("Invalid request: missing or invalid jsonrpc field", false);

    if ("method" !in json)
        throw new ProtocolException("Invalid request: missing method field", false);
    if (json["method"].type != JSONType.string)
        throw new ProtocolException("Invalid request: missing or invalid method field", false);

    return JsonRpcRequest.fromJson(json);
}

/// Serialize a JSON-RPC 2.0 response to a single-line JSON string.
string serializeResponse(JsonRpcResponse response) {
    return response.toJson().toString();
}

/// Create a JSON-RPC error response.
JsonRpcResponse createErrorResponse(JSONValue id, int code, string message,
        JSONValue data = JSONValue.init) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.error_ = JsonRpcError(code, message, data).toJson();
    return resp;
}

/// Create a parse error response (id is null per spec).
JsonRpcResponse createParseErrorResponse() {
    return createErrorResponse(JSONValue.init, JsonRpcErrorCode.ParseError, "Parse error");
}

/// Create an invalid request response.
JsonRpcResponse createInvalidRequestResponse(JSONValue id, string message) {
    return createErrorResponse(id, JsonRpcErrorCode.InvalidRequest, message);
}

/// Create a method not found response.
JsonRpcResponse createMethodNotFoundResponse(JSONValue id, string method) {
    return createErrorResponse(id, JsonRpcErrorCode.MethodNotFound, "Method not found: " ~ method);
}

/// Create an invalid params response.
JsonRpcResponse createInvalidParamsResponse(JSONValue id, string message) {
    return createErrorResponse(id, JsonRpcErrorCode.InvalidParams, message);
}

/// Create an internal error response.
JsonRpcResponse createInternalErrorResponse(JSONValue id, string message) {
    return createErrorResponse(id, JsonRpcErrorCode.InternalError, message);
}

/// Create a success response.
JsonRpcResponse createSuccessResponse(JSONValue id, JSONValue result) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.result = result;
    return resp;
}

unittest {

    // --- parseRequest: valid requests ---

    // Valid request with integer id and params.
    {
        auto req = parseRequest(`{"jsonrpc":"2.0","id":1,"method":"ping","params":{}}`);
        assert(req.jsonrpc == "2.0");
        assert(req.hasId);
        assert(req.id.type == JSONType.integer);
        assert(req.id.integer == 1);
        assert(req.method == "ping");
        assert(!req.isNotification());
    }

    // Valid request with string id.
    {
        auto req = parseRequest(
                `{"jsonrpc":"2.0","id":"abc","method":"tools/call","params":{"name":"x"}}`);
        assert(req.hasId);
        assert(req.id.type == JSONType.string);
        assert(req.id.str == "abc");
        assert(req.method == "tools/call");
        assert(req.params["name"].str == "x");
    }

    // Valid notification (no id field) — isNotification returns true.
    {
        auto req = parseRequest(`{"jsonrpc":"2.0","method":"notifications/initialized"}`);
        assert(!req.hasId);
        assert(req.isNotification());
        assert(req.id.type == JSONType.null_);
    }

    // Valid request with explicit "id": null — NOT a notification (hasId is true).
    {
        auto req = parseRequest(`{"jsonrpc":"2.0","id":null,"method":"ping"}`);
        assert(req.hasId);
        assert(!req.isNotification());
        assert(req.id.type == JSONType.null_);
    }

    // Valid request with complex params object.
    {
        auto req = parseRequest(`{"jsonrpc":"2.0","id":42,"method":"tools/call",
            "params":{"name":"read_file","arguments":{"path":"test.txt"}}}`);
        assert(req.id.integer == 42);
        assert(req.method == "tools/call");
        assert(req.params["name"].str == "read_file");
        assert(req.params["arguments"]["path"].str == "test.txt");
    }

    // Valid request with array params (positional parameters).
    {
        auto req = parseRequest(
                `{"jsonrpc":"2.0","id":1,"method":"ping","params":[1,"hello",true]}`);
        assert(req.params.type == JSONType.array);
        assert(req.params.array[0].integer == 1);
        assert(req.params.array[1].str == "hello");
        assert(req.params.array[2].type == JSONType.true_);
    }

    // Valid request with boolean id.
    {
        auto req = parseRequest(`{"jsonrpc":"2.0","id":true,"method":"ping"}`);
        assert(req.hasId);
        assert(req.id.type == JSONType.true_);
    }

    // Request with extra unknown fields — ignored per spec.
    {
        auto req = parseRequest(
                `{"jsonrpc":"2.0","id":1,"method":"ping","extra":"ignored","foo":42}`);
        assert(req.id.integer == 1);
        assert(req.method == "ping");
    }

    // --- parseRequest: malformed JSON (isParseError=true) ---

    // Empty string — parseJSON returns null, then fails the object check.
    {
        bool caught = false;
        try {
            parseRequest("");
        } catch (ProtocolException e) {
            caught = true;
            assert(!e.isParseError);
        }
        assert(caught);
    }

    // Truncated JSON.
    {
        bool caught = false;
        try {
            parseRequest(`{"jsonrpc":"2.0","id":`);
        } catch (ProtocolException e) {
            caught = true;
            assert(e.isParseError);
        }
        assert(caught);
    }

    // Invalid JSON characters.
    {
        bool caught = false;
        try {
            parseRequest("not json at all");
        } catch (ProtocolException e) {
            caught = true;
            assert(e.isParseError);
        }
        assert(caught);
    }

    // --- parseRequest: non-object JSON (isParseError=false) ---

    // JSON array instead of object.
    {
        bool caught = false;
        try {
            parseRequest("[1,2,3]");
        } catch (ProtocolException e) {
            caught = true;
            assert(!e.isParseError);
        }
        assert(caught);
    }

    // JSON string instead of object.
    {
        bool caught = false;
        try {
            parseRequest(`"hello"`);
        } catch (ProtocolException e) {
            caught = true;
            assert(!e.isParseError);
        }
        assert(caught);
    }

    // JSON number instead of object.
    {
        bool caught = false;
        try {
            parseRequest("42");
        } catch (ProtocolException e) {
            caught = true;
            assert(!e.isParseError);
        }
        assert(caught);
    }

    // --- parseRequest: missing/invalid required fields (isParseError=false) ---

    // Missing jsonrpc field.
    {
        bool caught = false;
        try {
            parseRequest(`{"id":1,"method":"ping"}`);
        } catch (ProtocolException e) {
            caught = true;
            assert(!e.isParseError);
        }
        assert(caught);
    }

    // Missing method field.
    {
        bool caught = false;
        try {
            parseRequest(`{"jsonrpc":"2.0","id":1}`);
        } catch (ProtocolException e) {
            caught = true;
            assert(!e.isParseError);
        }
        assert(caught);
    }

    // jsonrpc with wrong value.
    {
        bool caught = false;
        try {
            parseRequest(`{"jsonrpc":"1.0","id":1,"method":"ping"}`);
        } catch (ProtocolException e) {
            caught = true;
            assert(!e.isParseError);
        }
        assert(caught);
    }

    // jsonrpc as non-string.
    {
        bool caught = false;
        try {
            parseRequest(`{"jsonrpc":2.0,"id":1,"method":"ping"}`);
        } catch (ProtocolException e) {
            caught = true;
            assert(!e.isParseError);
        }
        assert(caught);
    }

    // method as non-string.
    {
        bool caught = false;
        try {
            parseRequest(`{"jsonrpc":"2.0","id":1,"method":123}`);
        } catch (ProtocolException e) {
            caught = true;
            assert(!e.isParseError);
        }
        assert(caught);
    }

    // --- serializeResponse: success response ---

    // Success with integer id and empty object result.
    {
        JsonRpcResponse resp;
        resp.id = JSONValue(1);
        resp.result = JSONValue.emptyObject;
        string jsonout = serializeResponse(resp);
        auto parsed = parseJSON(jsonout);
        assert(parsed["jsonrpc"].str == "2.0");
        assert(parsed["id"].integer == 1);
        assert(parsed["result"].type == JSONType.object);
        assert("error" !in parsed);
    }

    // Success with string id and text result.
    {
        JsonRpcResponse resp;
        resp.id = JSONValue("xyz");
        resp.result = JSONValue("hello");
        string jsonout = serializeResponse(resp);
        auto parsed = parseJSON(jsonout);
        assert(parsed["id"].str == "xyz");
        assert(parsed["result"].str == "hello");
    }

    // Success with null id (notification echo).
    {
        JsonRpcResponse resp;
        resp.id = JSONValue.init;
        resp.result = JSONValue.emptyArray;
        string jsonout = serializeResponse(resp);
        auto parsed = parseJSON(jsonout);
        assert(parsed["id"].type == JSONType.null_);
        assert(parsed["result"].type == JSONType.array);
    }

    // --- serializeResponse: error response ---

    // Error response with MethodNotFound.
    {
        auto resp = createErrorResponse(JSONValue(5),
                JsonRpcErrorCode.MethodNotFound, "Method not found: bogus");
        string jsonout = serializeResponse(resp);
        auto parsed = parseJSON(jsonout);
        assert(parsed["jsonrpc"].str == "2.0");
        assert(parsed["id"].integer == 5);
        assert(parsed["error"]["code"].integer == JsonRpcErrorCode.MethodNotFound);
        assert(parsed["error"]["message"].str == "Method not found: bogus");
        assert("result" !in parsed);
    }

    // Error response with data payload.
    {
        auto resp = createErrorResponse(JSONValue("a"),
                JsonRpcErrorCode.InternalError, "internal", JSONValue("detail"));
        string jsonout = serializeResponse(resp);
        auto parsed = parseJSON(jsonout);
        assert(parsed["error"]["data"].str == "detail");
    }

    // Error response without data (data omitted from output).
    {
        auto resp = createErrorResponse(JSONValue(1), JsonRpcErrorCode.ParseError, "bad json");
        string jsonout = serializeResponse(resp);
        auto parsed = parseJSON(jsonout);
        assert("data" !in parsed["error"]);
        assert(parsed["error"]["code"].integer == JsonRpcErrorCode.ParseError);
        assert(parsed["error"]["message"].str == "bad json");
    }

    // Response with both error_ and result — error_ takes precedence.
    {
        JsonRpcResponse resp;
        resp.id = JSONValue(1);
        resp.result = JSONValue("should be hidden");
        resp.error_ = JsonRpcError(JsonRpcErrorCode.InternalError, "oops").toJson();
        string jsonout = serializeResponse(resp);
        auto parsed = parseJSON(jsonout);
        assert(parsed["error"]["code"].integer == JsonRpcErrorCode.InternalError);
        assert("result" !in parsed);
    }

    // --- Factory functions ---

    // createParseErrorResponse has null id.
    {
        auto resp = createParseErrorResponse();
        string jsonout = serializeResponse(resp);
        auto parsed = parseJSON(jsonout);
        assert(parsed["id"].type == JSONType.null_);
        assert(parsed["error"]["code"].integer == JsonRpcErrorCode.ParseError);
    }

    // createInvalidRequestResponse.
    {
        auto resp = createInvalidRequestResponse(JSONValue(2), "missing field");
        string jsonout = serializeResponse(resp);
        auto parsed = parseJSON(jsonout);
        assert(parsed["error"]["code"].integer == JsonRpcErrorCode.InvalidRequest);
        assert(parsed["error"]["message"].str == "missing field");
    }

    // createMethodNotFoundResponse includes method name in message.
    {
        auto resp = createMethodNotFoundResponse(JSONValue(3), "unknown");
        string jsonout = serializeResponse(resp);
        auto parsed = parseJSON(jsonout);
        assert(parsed["error"]["message"].str == "Method not found: unknown");
    }

    // createInvalidParamsResponse.
    {
        auto resp = createInvalidParamsResponse(JSONValue(4), "bad arg");
        string jsonout = serializeResponse(resp);
        auto parsed = parseJSON(jsonout);
        assert(parsed["error"]["code"].integer == JsonRpcErrorCode.InvalidParams);
    }

    // createInternalErrorResponse.
    {
        auto resp = createInternalErrorResponse(JSONValue(5), "oops");
        string jsonout = serializeResponse(resp);
        auto parsed = parseJSON(jsonout);
        assert(parsed["error"]["code"].integer == JsonRpcErrorCode.InternalError);
    }

    // createSuccessResponse.
    {
        auto resp = createSuccessResponse(JSONValue(6), JSONValue.emptyObject);
        string jsonout = serializeResponse(resp);
        auto parsed = parseJSON(jsonout);
        assert(parsed["result"].type == JSONType.object);
        assert("error" !in parsed);
    }

    // --- Round-trip: parseRequest -> serializeResponse -> parseJSON ---

    // Round-trip a valid request through a success response.
    {
        auto req = parseRequest(`{"jsonrpc":"2.0","id":"rt1","method":"ping"}`);
        assert(req.method == "ping");
        assert(req.id.str == "rt1");

        auto resp = createSuccessResponse(req.id, JSONValue.emptyObject);
        string jsonout = serializeResponse(resp);
        auto parsed = parseJSON(jsonout);
        assert(parsed["jsonrpc"].str == "2.0");
        assert(parsed["id"].str == "rt1");
        assert(parsed["result"].type == JSONType.object);
    }

    // Round-trip a notification through an error response.
    {
        auto req = parseRequest(`{"jsonrpc":"2.0","method":"notifications/initialized"}`);
        assert(req.isNotification());

        auto resp = createErrorResponse(req.id, JsonRpcErrorCode.InvalidRequest, "not init");
        string jsonout = serializeResponse(resp);
        auto parsed = parseJSON(jsonout);
        assert(parsed["id"].type == JSONType.null_);
        assert(parsed["error"]["code"].integer == JsonRpcErrorCode.InvalidRequest);
    }

    // --- Edge cases ---

    // Request with no params defaults to empty object.
    {
        auto req = parseRequest(`{"jsonrpc":"2.0","id":1,"method":"ping"}`);
        assert(req.params.type == JSONType.object);
    }

    // serializeResponse produces a single-line string (no embedded newlines).
    {
        JsonRpcResponse resp;
        resp.id = JSONValue(1);
        resp.result = JSONValue.emptyObject;
        string jsonout = serializeResponse(resp);
        import std.algorithm : count;

        assert(jsonout.count('\n') == 0, "serializeResponse should produce single-line output");
    }
}
