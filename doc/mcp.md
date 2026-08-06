# MCP Server

llmfun implements an MCP (Model Context Protocol) server that exposes its tool system over JSON-RPC 2.0 via stdio. This allows MCP-compatible clients (Claude Desktop, Cursor, Continue, etc.) to use llmfun's tools.

## Overview

The MCP server bridges MCP's JSON-RPC protocol to llmfun's existing `@Function`/`RegisterLlmFunctions` tool infrastructure. It reuses `descAllFunctions()` for tool discovery and `executeFunc()` for tool execution, with `ReFilter` for tool visibility control.

## Architecture

### Modules

| Module | Purpose |
|--------|---------|
| `mcp_server/types.d` | JSON-RPC 2.0 types (`JsonRpcRequest`, `JsonRpcResponse`, `ToolResult`) using `std.json.JSONValue` |
| `mcp_server/protocol.d` | JSON-RPC parsing (`parseRequest`) and serialization (`serializeResponse`) |
| `mcp_server/transport.d` | `Transport` interface and `StdioTransport` implementation |
| `mcp_server/package.d` | `MCPServer` class, actor entry point (`runMcpServer`), message types |
| `app_mcp.d` | CLI entry point, signal handling, actor spawning |

### Actor Model

The MCP server runs as a `std.concurrency` actor:

1. **Main thread** spawns `runMcpServer` via `std.concurrency.spawn`
2. **Main thread** sends `McpServerConfig` with include/exclude filter patterns
3. **Actor** creates `StdioTransport` and `MCPServer`, sends `McpStarted`
4. **Actor** polls for transport input (`hasData()`) and control messages (`receiveTimeout`)
5. **Main thread** sends `McpShutdown` on SIGINT/SIGTERM
6. **Actor** drains pending messages, closes transport, sends `McpStopped`

No shared state exists between threads. All communication uses typed messages.

### Message Types

| Message | Direction | Purpose |
|---------|-----------|---------|
| `McpServerConfig` | Main -> Actor | Initial config (include/exclude regex patterns) |
| `McpStarted` | Actor -> Main | Server is ready to accept requests |
| `McpShutdown` | Main -> Actor | Request graceful shutdown |
| `McpStopped` | Actor -> Main | Server has stopped (includes error flag) |
| `McpFailed` | Actor -> Main | Startup failed (e.g., invalid regex) |

### Stdio Transport

The `StdioTransport` uses unbuffered POSIX reads from the file descriptor to avoid the classic poll/FILE buffering race: when the C FILE buffer eagerly reads multiple lines, `poll(2)` sees an empty kernel buffer and returns 0, causing the actor to stall on already-available data. Bypassing FILE means `readMessage()` never blocks in the actor loop -- the actor only calls it after `hasData()` confirms a complete line is available.

## Supported Methods

| Method | Description |
|--------|-------------|
| `initialize` | MCP handshake; returns protocol version, capabilities, server info |
| `tools/list` | Return filtered tool descriptions in MCP format |
| `tools/call` | Execute a tool with the given arguments |
| `ping` | Health check; returns `{}` |
| `resources/list` | Returns empty array (no resources) |
| `prompts/list` | Returns empty array (no prompts) |

### Initialize

Request:
```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
```

Response:
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "protocolVersion": "2024-11-05",
    "capabilities": {
      "tools": { "listChanged": false }
    },
    "serverInfo": {
      "name": "llmfun-mcp",
      "version": "0.1.0"
    }
  }
}
```

### Tools/List

Request:
```json
{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}
```

Response:
```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "tools": [
      {
        "name": "readFile",
        "description": "Read file contents with optional line numbering and range selection",
        "inputSchema": {
          "type": "object",
          "properties": {
            "path": { "type": "string" },
            "startLine": { "type": "integer" },
            "count": { "type": "integer" }
          },
          "required": ["path", "startLine", "count"]
        }
      }
    ]
  }
}
```

### Tools/Call

Request:
```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "tools/call",
  "params": {
    "name": "readFile",
    "arguments": {
      "path": "README.md",
      "startLine": 1,
      "count": 10
    }
  }
}
```

Response (success):
```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "content": [
      { "type": "text", "text": "file content here..." }
    ],
    "isError": false
  }
}
```

Response (error):
```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "content": [
      { "type": "text", "text": "error message" }
    ],
    "isError": true
  }
}
```

## Error Handling

The server follows JSON-RPC 2.0 error codes:

| Code | Meaning | When |
|------|---------|------|
| `-32700` | Parse error | Malformed JSON |
| `-32600` | Invalid request | Missing `jsonrpc` or `method` field |
| `-32601` | Method not found | Unknown method or filtered-out tool |
| `-32602` | Invalid params | Missing or wrong-type parameters |
| `-32603` | Internal error | Unexpected exception during tool execution |

### Notification Suppression

Requests without an `id` field are treated as notifications. Per JSON-RPC 2.0 spec, the server sends no response for notifications.

### Pre-Initialize Rejection

Methods other than `initialize`, `notifications/initialized`, and `notifications/cancelled` are rejected with `-32600` (InvalidRequest) if sent before the `initialize` handshake.

### Tool Filtering

When `tools/call` requests a tool that is hidden by `ReFilter` (via `--include`/`--exclude`), the server returns `-32601` (MethodNotFound) to avoid leaking which tools exist but are hidden.

## CLI Usage

The `--stdio` flag is required to start the server (HTTP transport is not yet implemented). `--stdio` and `--list-tools` are mutually exclusive.

### Start MCP Server

```bash
llmfun mcp --stdio
```

### List Available Tools

```bash
llmfun mcp --list-tools
```

### Filter Tools

```bash
# Only file-related tools
llmfun mcp --stdio --include "^read.*" --include "^write.*" --include "^list.*"

# Exclude dangerous tools
llmfun mcp --stdio --exclude "executeImage" --exclude "removeFile"
```

### Pipe Requests

```bash
# Single request
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' | llmfun mcp --stdio

# Multiple requests (use a here-doc or process substitution)
llmfun mcp --stdio <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}
EOF
```

## Integration with MCP Clients

### Claude Desktop

Add to `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "llmfun": {
      "command": "path/to/build/llmfun",
      "args": ["mcp", "--stdio"]
    }
  }
}
```

### Cursor

Add to `.cursor/mcp.json`:

```json
{
  "mcpServers": {
    "llmfun": {
      "command": "path/to/build/llmfun",
      "args": ["mcp", "--stdio"]
    }
  }
}
```

## Design Decisions

### std.json over mir

The implementation uses `std.json.JSONValue` instead of `mir.algebraic_alias.json.JsonAlgebraic`. This avoids the `mir` dependency and keeps the MCP server self-contained within Phobos.

### No Context Object

The MCP server passes `null` as the `Context` to `executeFunc()`. Most tools don't dereference Context (stateless tools like file I/O, encoding, memory). Tools that require Context (RAG queries, agent interactions) may not work correctly via MCP.

### Arguments Pass-Through

The `params["arguments"]` JSONValue object is passed directly to `executeFunc()` without stringify/re-parse. This preserves the original JSON structure and avoids serialization artifacts.

### Array Unwrapping

Both `descAllFunctions()` and `filterToolDescriptions()` return guaranteed `JSONValue(JSONValue[])` (array type). The code safely accesses `.array` on these values.

## Future Work

- **HTTP transport**: `--host`/`--port` flags exist but are not yet implemented; the server requires `--stdio`
- **SSE transport**: Server-sent events for bidirectional communication
- **Resource support**: Expose files and directories as MCP resources
- **Prompt support**: Expose prompt templates as MCP prompts
- **Streaming**: Support streaming tool results for long-running operations
