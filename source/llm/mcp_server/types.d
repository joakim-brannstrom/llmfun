module llm.mcp_server.types;

import std.json : JSONType, JSONValue;

/// Standard JSON-RPC 2.0 error codes.
enum JsonRpcErrorCode : int {
    ParseError = -32700, /// Invalid JSON was received by the server.
    InvalidRequest = -32600, /// The JSON sent is not a valid Request object.
    MethodNotFound = -32601, /// The method does not exist / is not available.
    InvalidParams = -32602, /// Invalid method parameter(s).
    InternalError = -32603, /// Internal JSON-RPC error.
}

/// A JSON-RPC 2.0 request object.
struct JsonRpcRequest {
    string jsonrpc = "2.0";
    /// The request id, echoed back in the response; null when the request
    /// has no `id` field.
    JSONValue id;
    /// Whether an `id` field was present in the request. Distinguishes a
    /// missing id from an explicit `"id": null`.
    bool hasId;
    string method;
    /// The request parameters; defaults to `{}` when absent.
    JSONValue params;

    /// Whether this request is a notification (no `id` field present).
    bool isNotification() const pure @safe {
        return !hasId;
    }

    /// Build a request from a parsed JSON object. The caller must have
    /// validated that `jsonrpc` and `method` are present and of the right
    /// type before calling this.
    static JsonRpcRequest fromJson(JSONValue json) @safe {
        JsonRpcRequest req;
        req.jsonrpc = json["jsonrpc"].str;
        if (auto idPtr = "id" in json) {
            req.id = *idPtr;
            req.hasId = true;
        } else {
            req.id = JSONValue.init;
            req.hasId = false;
        }
        req.method = json["method"].str;
        if (auto p = "params" in json)
            req.params = *p;
        else
            req.params = JSONValue.emptyObject;
        return req;
    }
}

/// A JSON-RPC 2.0 response object.
struct JsonRpcResponse {
    string jsonrpc = "2.0";
    JSONValue id;
    JSONValue result;
    /// Trailing underscore to avoid clashing with the `error` identifier.
    JSONValue error_;

    /// Serialize to a JSON object. Exactly one of `error`/`result` is
    /// emitted: `error` when `error_` is non-null, otherwise `result`.
    JSONValue toJson() const pure @safe {
        assert(error_.type != JSONType.null_ || result.type != JSONType.null_,
                "JsonRpcResponse: neither error_ nor result set");
        JSONValue j = JSONValue.emptyObject;
        j["jsonrpc"] = jsonrpc;
        j["id"] = id;
        if (error_.type != JSONType.null_)
            j["error"] = error_;
        else
            j["result"] = result;
        return j;
    }
}

/// A JSON-RPC 2.0 error object (the `error` member of a response).
struct JsonRpcError {
    /// Standard JSON-RPC error code (see `JsonRpcErrorCode`).
    int code;
    /// Short, human-readable error message.
    string message;
    /// Additional error details; omitted from the output when null.
    JSONValue data;

    JSONValue toJson() const pure @safe {
        JSONValue j = JSONValue.emptyObject;
        j["code"] = code;
        j["message"] = message;
        if (data.type != JSONType.null_)
            j["data"] = data;
        return j;
    }
}

/// An MCP tool definition as returned by `tools/list`.
struct ToolDefinition {
    /// Tool name as exposed to the MCP client.
    string name;
    /// Human-readable tool description.
    string description;
    /// JSON Schema describing the tool's input parameters.
    JSONValue inputSchema;

    JSONValue toJson() const pure @safe {
        JSONValue j = JSONValue.emptyObject;
        j["name"] = name;
        j["description"] = description;
        j["inputSchema"] = inputSchema;
        return j;
    }
}

/// The result of a tool execution, wrapped in MCP content format.
struct ToolResult {
    /// A single content block inside a tool result.
    struct Content {
        /// Content block type; always `"text"` for this server.
        string type_ = "text";
        /// The text payload.
        string text;

        JSONValue toJson() const pure @safe {
            JSONValue j = JSONValue.emptyObject;
            j["type"] = type_;
            j["text"] = text;
            return j;
        }
    }

    /// Ordered list of content blocks in the result.
    Content[] content;
    /// Whether the tool execution failed (MCP `isError` flag).
    bool isError;

    JSONValue toJson() const pure @safe {
        JSONValue j = JSONValue.emptyObject;
        JSONValue[] arr;
        foreach (ref c; content)
            arr ~= c.toJson();
        j["content"] = JSONValue(arr);
        j["isError"] = isError;
        return j;
    }
}

unittest {
    import std.json : parseJSON;

    // fromJson with an id field.
    auto req = JsonRpcRequest.fromJson(
            parseJSON(`{"jsonrpc":"2.0","id":1,"method":"ping","params":{}}`));
    assert(req.jsonrpc == "2.0");
    assert(req.hasId);
    assert(req.id.type == JSONType.integer);
    assert(req.id.integer == 1);
    assert(req.method == "ping");
    assert(!req.isNotification());

    // fromJson without an id (notification).
    auto notif = JsonRpcRequest.fromJson(
            parseJSON(`{"jsonrpc":"2.0","method":"notifications/initialized"}`));
    assert(!notif.hasId);
    assert(notif.isNotification());
    assert(notif.id.type == JSONType.null_);
    assert(notif.params.type == JSONType.object);

    // fromJson with string id and explicit params.
    auto req2 = JsonRpcRequest.fromJson(parseJSON(
            `{"jsonrpc":"2.0","id":"abc","method":"tools/call","params":{"name":"x"}}`));
    assert(req2.hasId);
    assert(req2.id.type == JSONType.string);
    assert(req2.id.str == "abc");
    assert(req2.params["name"].str == "x");

    // Explicit "id": null is NOT a notification: hasId stays true.
    auto req3 = JsonRpcRequest.fromJson(parseJSON(`{"jsonrpc":"2.0","id":null,"method":"ping"}`));
    assert(req3.hasId);
    assert(!req3.isNotification());
    assert(req3.id.type == JSONType.null_);

    // Success response serialization.
    JsonRpcResponse resp;
    resp.id = JSONValue(1);
    resp.result = JSONValue.emptyObject;
    auto json = resp.toJson();
    assert(json.type == JSONType.object);
    assert(json["jsonrpc"].str == "2.0");
    assert(json["id"].integer == 1);
    assert("result" in json);
    assert("error" !in json);

    // Error response serialization.
    JsonRpcResponse errResp;
    errResp.id = JSONValue("abc");
    errResp.error_ = JsonRpcError(JsonRpcErrorCode.MethodNotFound, "method not found").toJson();
    auto errJson = errResp.toJson();
    assert("error" in errJson);
    assert(errJson["id"].str == "abc");
    assert(errJson["error"]["code"].integer == JsonRpcErrorCode.MethodNotFound);
    assert(errJson["error"]["message"].str == "method not found");
    assert("result" !in errJson);

    // JsonRpcError with data payload.
    auto err = JsonRpcError(JsonRpcErrorCode.InternalError, "boom").toJson();
    assert(err["code"].integer == JsonRpcErrorCode.InternalError);
    assert(err["message"].str == "boom");
    assert("data" !in err);

    // ToolResult serialization.
    ToolResult tr;
    tr.content ~= ToolResult.Content("text", "done");
    tr.isError = false;
    auto trJson = tr.toJson();
    assert(trJson["content"].type == JSONType.array);
    assert(trJson["content"][0]["type"].str == "text");
    assert(trJson["content"][0]["text"].str == "done");
    assert(trJson["isError"].type == JSONType.false_);

    // ToolDefinition serialization.
    ToolDefinition td;
    td.name = "read_file";
    td.description = "Reads a file";
    td.inputSchema = parseJSON(`{"type":"object","properties":{}}`);
    auto tdJson = td.toJson();
    assert(tdJson["name"].str == "read_file");
    assert(tdJson["description"].str == "Reads a file");
    assert(tdJson["inputSchema"]["type"].str == "object");

    // Error code enum values.
    assert(JsonRpcErrorCode.ParseError == -32700);
    assert(JsonRpcErrorCode.InvalidRequest == -32600);
    assert(JsonRpcErrorCode.MethodNotFound == -32601);
    assert(JsonRpcErrorCode.InvalidParams == -32602);
    assert(JsonRpcErrorCode.InternalError == -32603);
}
