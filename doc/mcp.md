# MCP Server

llmfun implements an MCP (Model Context Protocol) server that exposes its tool system over JSON-RPC 2.0 via stdio. This allows MCP-compatible clients (Claude Desktop, Cursor, Continue, etc.) to use llmfun's tools.

## Overview

The MCP server bridges MCP's JSON-RPC protocol to llmfun's existing `@Function`/`RegisterLlmFunctions` tool infrastructure. It reuses `descAllFunctions()` for tool discovery and `executeFunc()` for tool execution, with `ReFilter` for tool visibility control.

The server constructs a full `AgentContext` inside the actor thread, providing all tools with their required context. Dependencies like RAG and SkillManager are created with graceful degradation — if unavailable, tools return clear error messages instead of crashing.

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
2. **Main thread** sends `McpServerConfig` with include/exclude filter patterns and `McpConfigData`
3. **Actor** reconstructs `LlmConfig` from `McpConfigData`
4. **Actor** creates `RAG` (may be null — graceful degradation)
5. **Actor** constructs `AgentContext` with all dependencies
6. **Actor** creates `SkillManager` (may be null — graceful degradation)
7. **Actor** sets `taskDone` handler (no-op with logging)
8. **Actor** creates `StdioTransport` and `MCPServer`, sends `McpStarted`
9. **Actor** polls for transport input (`hasData()`) and control messages (`receiveTimeout`)
10. **Main thread** sends `McpShutdown` on SIGINT/SIGTERM
11. **Actor** drains pending messages, closes transport, sends `McpStopped`

No shared state exists between threads. All communication uses typed messages.

### Message Types

| Message | Direction | Purpose |
|---------|-----------|---------|
| `McpServerConfig` | Main -> Actor | Initial config (include/exclude regex patterns, `McpConfigData`) |
| `McpStarted` | Actor -> Main | Server is ready to accept requests |
| `McpShutdown` | Main -> Actor | Request graceful shutdown |
| `McpStopped` | Actor -> Main | Server has stopped (includes error flag) |
| `McpFailed` | Actor -> Main | Startup failed (e.g., invalid regex, fatal construction error) |

### AgentContext Integration

The MCP server constructs a full `AgentContext` inside the actor thread. `AgentContext` implements all tool context interfaces:

| Context Interface | Tools | Availability in MCP |
|-------------------|-------|---------------------|
| `FileContext` | `readFile`, `writeFile`, `editFile`, `listDirectory`, `removeFile`, `countLinesInFile`, `md5HashFile`, `grepFiles` | Always available |
| `EnvironmentContext` | `executeCommand`, `listEnvironments` | Always available |
| `RAGContext` | `querySemantic`, `queryTextSearch`, `queryBestMatch`, `listRAGDatabases`, `loadFileToRAG`, `loadContentToRAG`, `removeTopicFromRAG`, `queryReadFile` | Available if RAG is configured, graceful degradation otherwise |
| `MemoryContext` | `writeMemory`, `readMemory`, `removeMemory`, `getMemoryTopics` | Always available |
| `CompletionContext` | `taskDone` | Always available (no-op with logging) |
| `MetricsContext` | `getMetrics` | Always available |
| `PipelineControlContext` | `pipelineOutput` | Always available (no-op) |
| `VisionContext` | `loadImageApi` | Always available (inline mode works without vision model) |
| `SkillContext` | `loadSkill` | Available if SkillManager is configured, graceful degradation otherwise |

### Graceful Degradation

The MCP server starts even when optional dependencies are unavailable:

**RAG not available**: RAG-dependent tools (`querySemantic`, `queryTextSearch`, `queryBestMatch`, `listRAGDatabases`, `loadFileToRAG`, `loadContentToRAG`, `removeTopicFromRAG`, `queryReadFile`) return `"error: RAG not available"` instead of crashing. The tools still appear in `tools/list` since `AgentContext` implements `RAGContext`.

**SkillManager not available**: `loadSkill` returns `"error: skill manager not available"` instead of crashing. The tool still appears in `tools/list` since `AgentContext` implements `SkillContext`.

**Vision model not configured**: `loadImageApi` operates in inline mode — it loads and base64-encodes the image, returning the data URL directly. No external vision model is called.

### taskDone Behavior

In the MCP context, `taskDone` is a no-op that logs the answer. It does not trigger framework-level task completion behavior. When called, the server logs the answer at INFO level and returns `"done"` to the MCP client. This allows MCP clients to call `taskDone` without causing the server to terminate or change state.

### pipelineOutput Behavior

In the MCP context, `pipelineOutput` returns a success message but does not propagate output to downstream nodes (there is no pipeline). The output is logged at TRACE level and discarded. This allows MCP clients to call `pipelineOutput` without side effects.

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
llmfun mcp --stdio --exclude "executeCommand" --exclude "removeFile"
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

### AgentContext with Graceful Degradation

The MCP server constructs `AgentContext` with all dependencies. RAG and SkillManager are optional — if creation fails, the server logs a warning and continues. Tools that depend on these services return clear error messages when invoked, rather than crashing. This allows the MCP server to start in environments where RAG embeddings or skill paths are not configured.

### McpConfigData for Actor Boundary

Configuration is passed across the actor boundary via `McpConfigData`, a struct containing only scalar/immutable fields. This avoids `std.concurrency` aliasing violations that would occur with complex structs containing associative arrays. The actor reconstructs `LlmConfig` from this data.

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
