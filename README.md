# llmfun

An interactive AI agent with tool calling, RAG (Retrieval-Augmented Generation), and pipeline support.

## Table of Contents

- [Installation](#installation)
- [CLI Commands](#cli-commands)
- [Examples](#examples)
- [CLI Parameters](#cli-parameters)
- [Slash Commands](#slash-commands)
- [Configuration](#configuration)
- [Configuration Directory Structure](#configuration-directory-structure)
- [Security & Configuration](#security--configuration)
- [Tools](#tools)

## Installation

### Prerequisites

- D compiler (DMD or LDC)
- Dub package manager

### Build

```bash
cd llmfun
dub build
```

## CLI Commands

llmfun supports four subcommands:

### `agent` (default)

Starts the interactive agent chat mode. The agent can process queries, call tools, and maintain conversation history.

```bash
llmfun agent [options]
```

#### Parameters

| Parameter | Short | Description |
|-----------|-------|-------------|
| `--workarea <path>` | `-w` | Restrict agent file read/write operations to the specified workarea directory |
| `--local-setup` | *none* | Create the `llmfun/...` directory structure in the current working directory |
| `--db <path>` | *none* | RAG database path(s). The first DB is primary (read/write); additional DBs are read-only |
| `--prompt <text>` | `-p` | One-shot prompt for the agent (non-interactive mode) |
| `--no-memory` | *none* | Deactivate the persistent read/write memory |

### `rag`

Manage the RAG (Retrieval-Augmented Generation) database. Add, remove, or list indexed sources.

```bash
llmfun rag [options]
```

#### Parameters

| Parameter | Short | Description |
|-----------|-------|-------------|
| `--add` | *none* | Add files to the RAG index |
| `--rm` | *none* | Remove files from the RAG index |
| `--list` | *none* | List all indexed sources |
| `--sync` | *none* | Synchronize files with the RAG index (add or remove as needed) |
| `--dry-run` | *none* | Preview changes without modifying the database |
| `--path <path>` | *none* | Recursively add all text files from the specified path |
| `--db <path>` | *none* | RAG database path(s) |
| `--include <pattern>` | `-i` | Include regex pattern for RAG files (repeatable). Overrides config file |
| `--exclude <pattern>` | `-e` | Exclude regex pattern for RAG files (repeatable). Overrides config file |
| `--local-setup` | *none* | Create the `llmfun/...` directory structure in the current working directory |

**Note**: `--add`, `--rm`, `--list` and `--sync` are mutually exclusive.

### `tool_metrics`

Print metrics about tool call performance from a monitoring data file.

```bash
llmfun tool_metrics [options]
```

#### Parameters

| Parameter | Short | Description |
|-----------|-------|-------------|
| `--data <path>` | *none* | **(Required)** Path to the metric data file (JSONL format) |
| `--number <n>` | `-n` | Number of tools to print in the report |
| `--follow` | `-f` | Live monitor the tool calls (tail mode) |

### `mcp`

Run llmfun as an MCP (Model Context Protocol) server over stdio. Exposes all registered tools to MCP-compatible clients via JSON-RPC 2.0.

```bash
llmfun mcp [options]
```

#### Parameters

| Parameter | Short | Description |
|-----------|-------|-------------|
| `--stdio` | *none* | Use stdio transport (required; mutually exclusive with `--list-tools`) |
| `--list-tools` | *none* | List available tools matching the filter and exit (diagnostic mode) |
| `--include <pattern>` | *none* | Include tool name regex pattern (repeatable) |
| `--exclude <pattern>` | *none* | Exclude tool name regex pattern (repeatable) |
| `--host <addr>` | *none* | Host for HTTP transport (not yet implemented) |
| `--port <num>` | *none* | Port for HTTP transport (not yet implemented) |

**Note**: `--stdio` and `--list-tools` are mutually exclusive.

#### Usage

Pipe JSON-RPC 2.0 requests to stdin and read responses from stdout:

```bash
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' | llmfun mcp --stdio
```

List available tools without starting the server:

```bash
llmfun mcp --list-tools
llmfun mcp --list-tools --include "^read.*"
llmfun mcp --list-tools --exclude "executeCommand"
```

## Examples

### Start interactive agent with local setup

```bash
llmfun agent --local-setup
```

### Add files to RAG index

```bash
llmfun rag --add --path ./docs
```

### Run a one-shot prompt

```bash
llmfun agent -p "Summarize the codebase"
```

### List RAG sources

```bash
llmfun rag --list
```

### Remove files from RAG by pattern

```bash
llmfun rag --rm --include ".*deprecated.*"
```

### Print tool metrics

```bash
llmfun tool_metrics --data llmfun/data/scratch/monitor.jsonl --number 10
```

### Live monitor tool calls

```bash
llmfun tool_metrics --data llmfun/data/scratch/monitor.jsonl --follow
```

### Start MCP server over stdio

```bash
llmfun mcp --stdio
```

### List available MCP tools

```bash
llmfun mcp --list-tools
```

### List tools matching a filter

```bash
llmfun mcp --list-tools --include "^read.*" --exclude "readMemory"
```

## Global CLI Parameters

These parameters apply to all commands:

| Parameter | Short | Description |
|-----------|-------|-------------|
| `--config <path>` | `-c` | Path to a configuration file to read |
| `--verbose` | `-v` | Set log verbosity level (repeat for more verbosity) |
| `--no-cwd-config` | *none* | Do not read `.llmfun.json` from current directory |
| `--trusted-config` | *none* | Allow loading `.llmfun.json` from CWD when workarea equals CWD |

## Slash Commands

When in interactive agent mode (`agent` command), the following slash commands are available:

| Command | Description |
|---------|-------------|
| *(bare query)* | Send a message to the agent |
| `/help` | Show the help message with all slash commands |
| `/quit`, `/q`, `/exit` | Exit the agent |
| `/compact` | Force compress the chat history (summarize older messages to save context space) |
| `/new` | Clear history and start a new conversation |
| `/plan <query>` | Run the plan pipeline (System Designer → Implementation Planner) |
| `/code <query>` | Run the coder pipeline (Coder → Code Reviewer loop) |

### Pipeline Commands

- **`/plan <query>`**: Executes a two-stage pipeline where a System Designer agent produces a design document, and an Implementation Planner agent converts it into actionable tasks. Results are saved to `plan/`.

- **`/code <query>`**: Executes a coder-reviewer loop pipeline. The Coder agent implements code, saves it to `code/implementation.md`, and a Code Reviewer agent provides feedback. The loop runs up to 3 iterations.

## Configuration

llmfun is configured via a JSON configuration file specified with `--config <path>` or the `LLMFUN_DEFAULT_CONFIG` environment variable. See `config/example.json` for a complete reference of all available options.

### Multi-Layer Configuration

llmfun uses a two-layer configuration system:

1. **Layer 1 (Base)**: Loaded from `LLMFUN_DEFAULT_CONFIG` environment variable or `$XDG_CONFIG_HOME/llmfun/config.json`.
2. **Layer 2 (Overlay)**: Loaded from `--config` CLI argument or `.llmfun.json` in the current working directory.

Layer 2 values override Layer 1 values. This allows global defaults with project-specific overrides.

**Security**: By default, if the workarea equals the current working directory, `.llmfun.json` in the CWD is NOT loaded (to prevent malicious projects from injecting config). Use `--trusted-config` to allow this, or `--no-cwd-config` to suppress the warning.

### Configuration Structure

```json
{
  "dataDir": "llmfun/data",
  "memoryArea": "llmfun/data/memory",
  "scratchArea": "llmfun/data/scratch",
  "promptDir": "llmfun/config/prompt",
  "workArea": "llmfun/workarea",
  "sandboxConfig": {"systemExecutionEnvironmentsFile": "execution_environments.json", ...},
  "agentPrompt": "AGENT.md",
  "activeCodeModelIndex": 0,
  "noMemory": false,
  "warnIfNoApiKey": true,
  "skillPathsUser": [],
  "skillPathsSystem": [],
  "maxManifestSkills": 200,
  "maxAlwaysApplyTokens": 4000,
  "disableSkills": false,
  "consolidationInterval": 10,
  "toolLimits": {...},
  "ragPrimary": {...},
  "ragSecondary": {...},
  "ragConfig": {...},
  "toolFilter": {...},
  "ragFilter": {...},
  "codeModels": [...],
  "summaryModel": {...},
  "visionModel": {...},
  "embedConfig": {...}
}
```

### Top-Level Options

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `dataDir` | string | `llmfun/data` | Base directory for data files |
| `memoryArea` | string[] | `llmfun/data/memory` | Path(s) to persistent memory area(s) |
| `scratchArea` | string | `llmfun/data/scratch` | Temporary workspace and runtime data |
| `promptDir` | string/[] | `llmfun/config/prompt` | Directory with prompt templates |
| `workArea` | string | `llmfun/workarea` | Agent working directory for file operations |
| `sandboxConfig` | object | defaults | Container runtime config (see below) |
| `agentPrompt` | string | `AGENT.md` | Agent system prompt file name (searched in promptDir) |
| `activeCodeModelIndex` | long | `0` | Index of the active code model in `codeModels` array |
| `noMemory` | bool | `false` | Deactivate persistent read/write memory |
| `warnIfNoApiKey` | bool | `true` | Emit warnings when no API key is configured for model servers |
| `skillPathsUser` | string/[] | `[]` | User skill directories |
| `skillPathsSystem` | string/[] | system defaults | System skill directories |
| `maxManifestSkills` | long | `200` | Maximum skills shown in the manifest |
| `maxAlwaysApplyTokens` | long | `4000` | Max tokens for always-apply skill blocks (0 = unlimited) |
| `disableSkills` | bool | `false` | Disable all skills |
| `consolidationInterval` | uint | `10` | Memory consolidation trigger interval (sessions). 0 = disabled |
| `ragPrimary` | object | `llmfun/data/rag.sqlite3` | Primary RAG database (read/write) |
| `ragSecondary` | object | `{}` | Additional read-only RAG databases |
| `toolLimits` | object | defaults | Tool execution limits (see below) |

### Sandbox Configuration (`sandboxConfig`)

Configures the execution environments for command execution. Supports both container-based (Docker, Podman) and host-based execution.

```json
"sandboxConfig": {
  "maxOutputBytes": 1048576,
  "defaultOptions": {
    "00_subcommand": ["run"],
    "01_cleanup": ["--rm"],
    "02_user": ["--user", "1000:1000"],
    "03_resources": ["--memory", "256m", "--cpus", "0.5"],
    "04_tmpfs": ["--tmpfs", "/tmp:rw,noexec,nosuid,size=64m"],
    "05_timeout": ["--stop-timeout", "60"],
    "06_network": ["--network", "none"],
    "entrypoint_cmd": ["sh", "-c"]
  },
  "systemExecutionEnvironmentsFile": "execution_environments.json",
  "userExecutionEnvironmentsFile": ""
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `maxOutputBytes` | long | `1048576` | Maximum output bytes per stream (stdout/stderr) |
| `defaultOptions` | object | defaults | Default container options merged into container environments |
| `systemExecutionEnvironmentsFile` | string | *(empty)* | Path to system execution environments JSON file |
| `userExecutionEnvironmentsFile` | string | *(empty)* | Path to user execution environments JSON file |

#### Default Options

The `defaultOptions` object uses numbered keys to control the order of CLI arguments. Keys must be prefixed with `<NN>_` (e.g. `00_subcommand`, `01_cleanup`). Values are arrays of CLI arguments. These options are merged into container-type environments at load time.

#### Execution Environments

Execution environments define how commands are executed. They are loaded from JSON files configured via `systemExecutionEnvironmentsFile` and `userExecutionEnvironmentsFile`. User entries override system entries with the same tag.

The environment config format is a JSON file with `version`, `defaultEnvironment`, and `environments` array:

```json
{
  "version": 1,
  "defaultEnvironment": "alpine",
  "environments": [
    {
      "tag": "alpine",
      "description": "Minimal Linux environment. **shell: sh**",
      "capabilities": ["linux", "minimal"],
      "isIsolated": true,
      "timeout": 120,
      "commandJoinMode": "whitespace",
      "config": {
        "type": "container",
        "runtimeCli": "docker",
        "image": "alpine:latest",
        "options": {
          "02_security": ["--read-only"],
          "04_tmpfs": ["--tmpfs", "/tmp:rw,noexec,nosuid,size=64m"],
          "05_mounts": ["-v", "@{llmfun_workarea}:/workarea"],
          "06_network": ["--network", "none"]
        }
      }
    }
  ]
}
```

**Environment entry fields:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `tag` | string | yes | Unique identifier used in `executeCommand` calls |
| `description` | string | no | Human-readable description shown by `listEnvironments` |
| `capabilities` | string[] | no | Tags for categorization (e.g. "dev", "python", "isolated") |
| `isIsolated` | bool | no | Whether the environment provides isolation (default: false) |
| `timeout` | int | yes | Maximum execution time in seconds |
| `commandJoinMode` | string | no | How command array elements are joined: `"whitespace"` (join with spaces, default) or `"append"` (each element as separate argument) |
| `config` | object | yes | Environment-specific configuration (see below) |

**Container config** (`type: "container"`):

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | string | yes | Must be `"container"` |
| `runtimeCli` | string | yes | Container runtime (e.g. "docker", "podman") |
| `image` | string | yes | Container image reference (e.g. "alpine:latest") |
| `options` | object | no | Container CLI options (merged with `sandboxConfig.defaultOptions`) |

**Host config** (`type: "host"`):

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | string | yes | Must be `"host"` |
| `options` | object | no | Shell command prefix options (e.g. `{"00_shell": ["sh", "-c"]}`) |
| `workingDir` | string | no | Working directory for the subprocess (supports magic words) |
| `envVars` | object or array | no | Environment variables (map: `{"KEY": "VALUE"}` or list: `["KEY=VALUE"]`) |
| `allowedCommandPrefixes` | string[] | no | Restrict commands to these prefixes (placeholder, not enforced yet) |

**Magic words** in environment config values:
- `@{llmfun_workarea}`: Replaced with the configured workarea path
- `@{llmfun}`: Replaced with the directory of the llmfun executable

See `config/execution_environments.json` for a complete example with multiple environments.

### Tool Limits (`toolLimits`)

Configures per-tool limits.

```json
"toolLimits": {
  "readFileMaxLines": 20
  "editFileMaxLines": 80,
  "maxDirEntries": 50,
  "grepMaxResults": 1000,
  "maxSummaryLength": 200,
  "maxTopicLength": 100,
  "maxTopK": 20,
  "maxArgLength": 200
}
```

| Field                 | Type | Default | Description |
|-----------------------|------|---------|-------------|
| `readFileMaxLines`    | long | 20      | Max lines readFile can read in one call |
| `editFileMaxLines`    | long | 80      | Max lines editFile can operate on in one call |
| `maxDirEntries`       | long | 50      | Max entries returned by listDirectory recursive scan |
| `grepMaxResults`      | long | 1000    | Max results returned by grepFiles |
| `maxSummaryLength`    | long | 200     | Max chars for memory topic summary |
| `maxTopicLength`      | long | 100     | Max chars for topic names (memory and RAG) |
| `maxTopK`             | long | 20      | Max topK value for RAG search queries |
| `maxArgLength`        | long | 200     | Max chars for argument truncation in tool history |

### RAG Database Configuration (`ragPrimary`, `ragSecondary`)

Configures the primary (read/write) and secondary (read-only) RAG databases.

#### Primary Database (`ragPrimary`)
The primary database is used for indexing and retrieval.

```json
"ragPrimary": {
  "path": "llmfun/data/rag.sqlite3",
  "description": "Recent project source code, documentation and files added with tools loadFileToRAG, loadContentToRAG"
}
```

#### Secondary Databases (`ragSecondary`)
A collection of read-only databases organized by category.

```json
"ragSecondary": {
  "user_knowledge": [
    {
      "path": "/path/to/knowledge.sqlite3",
      "description": "Read-only user collected knowledge base"
    }
  ]
}
```

Each database entry supports:
- `path` (string, required): Path to the SQLite database file
- `description` (string, optional): Human-readable description

### RAG Configuration (`ragConfig`)

Controls RAG indexing behavior.

```json
"ragConfig": {
  "windowOverlapPercent": 50
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `windowOverlapPercent` | long | 50 | Sliding window overlap as percentage (0-99). 50 means each chunk overlaps 50% with the previous one. Must be in range [0, 99]. |

### Tool Filter (`toolFilter`)

Controls which tools the agent can access via regex include/exclude patterns.

```json
"toolFilter": {
  "include": [".*"],
  "exclude": ["executeCommand"]
}
```

- `include` (string[]): Regex patterns for tools to allow (default: all tools)
- `exclude` (string[]): Regex patterns for tools to block

### RAG Filter (`ragFilter`)

Controls which files are indexed into the RAG database.

```json
"ragFilter": {
  "include": [".*\\.txt", ".*\\.md"],
  "exclude": []
}
```

- `include` (string[]): Regex patterns for files to include (default: `.*\\.txt`, `.*\\.md`)
- `exclude` (string[]): Regex patterns for files to exclude

### Code Models (`codeModels`)

Array of LLM model configurations for the agent. At least one model is required.

```json
"codeModels": [
  {
    "server": { ... },
    "name": "local-model",
    "temp": 0.6,
    "contextSize": 128000,
    "maxTokens": -1,
    "reasoningBudget": 4096,
    "preserveThinking": true
  }
]
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `server` | object | (required) | Server configuration (see below) |
| `name` | string | (required) | Model name (e.g. "gpt-4o", "local-model") |
| `temp` | double | 0 | Temperature for generation |
| `contextSize` | long | 0 | Context window size in tokens |
| `maxTokens` | long | -1 | Maximum tokens to generate (-1 = unlimited) |
| `reasoningBudget` | long | 0 | Token budget for reasoning/thinking |
| `preserveThinking` | bool | false | Preserve thinking tags in output |

### Server Configuration (`server`)

Used by `codeModels`, `summaryModel`, and `embedConfig`.

```json
"server": {
  "url": "http://127.0.0.1:1234",
  "promptUrl": "v1/completion",
  "chatUrl": "v1/chat/completions",
  "slotUrl": "slots",
  "embedUrl": "v1/embeddings",
  "timeoutSeconds": 3600,
  "httpVerbosity": 0,
  "verifySslCert": true,
  "maxRetries": 3,
  "backoffMs": 500,
  "apiKey": "",
  "type": "llamaCpp"
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `url` | string | (required) | Base URL of the LLM server |
| `promptUrl` | string | `v1/completion` | Endpoint for prompt completion |
| `chatUrl` | string | `v1/chat/completions` | Endpoint for chat completion |
| `slotUrl` | string | `slots` | Endpoint for slot management |
| `embedUrl` | string | `v1/embeddings` | Endpoint for embeddings |
| `timeoutSeconds` | long | 0 | Request timeout in seconds |
| `httpVerbosity` | long | 0 | HTTP logging verbosity level |
| `verifySslCert` | bool | true | Verify SSL/TLS certificates |
| `maxRetries` | long | 3 | Maximum retries for transient failures |
| `backoffMs` | long | 500 | Initial backoff in milliseconds (exponential) |
| `apiKey` | string | "" | API key for Bearer token auth (falls back to `OPENAI_API_KEY` env var) |
| `type` | string | "" | Endpoint type: `llamaCpp`, `deepseek`, or empty for generic |

**Endpoint Types:**
- `llamaCpp`: Uses `chat_template_kwargs` with `reasoning_budget` and `preserve_thinking` for reasoning models
- `deepseek`: Uses `thinking` block with `reasoning_effort` levels (high/max) for reasoning

### Summary Model (`summaryModel`)

Configuration for the model used to compress chat history.

```json
"summaryModel": {
  "server": { ... },
  "name": "summary-model",
  "prompt": "SUMMARY.md",
  "temp": 0.3,
  "contextSize": 32768,
  "contextChunkSize": 32768,
  "maxTokens": 4096,
  "reasoningBudget": 0,
  "preserveThinking": false
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `server` | object | (required) | Server configuration |
| `name` | string | (required) | Model name |
| `prompt` | string | `SUMMARY.md` | Summary prompt template file |
| `temp` | double | 0 | Temperature |
| `contextSize` | long | 0 | Context window size |
| `contextChunkSize` | long | 32768 | Chunk size for summarization |
| `maxTokens` | long | -1 | Maximum generation tokens |
| `reasoningBudget` | long | 0 | Reasoning token budget |
| `preserveThinking` | bool | false | Preserve thinking tags |

### Vision Model (`visionModel`)

Optional dedicated vision model for image processing. When configured, `loadImageApi` delegates image analysis to a separate vision-specialized model instead of sending images to the main agent model. This enables hardware separation (e.g., vision model on CPU, main model on GPU) and uses a model optimized for image understanding.

```json
"visionModel": {
  "server": { ... },
  "name": "vision-model",
  "systemPrompt": "",
  "temp": 0.3,
  "contextSize": 32768,
  "maxTokens": 4096,
  "reasoningBudget": 0,
  "preserveThinking": false,
  "timeoutSecs": 60
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `server` | object | (required) | Server configuration (see Server Configuration) |
| `name` | string | (required) | Vision model name (e.g. "llava", "qwen2.5-vl") |
| `systemPrompt` | string | "" | Custom system prompt for image description. Empty uses built-in prompt |
| `temp` | double | 0 | Temperature for generation |
| `contextSize` | long | 0 | Context window size in tokens |
| `maxTokens` | long | -1 | Maximum tokens to generate (-1 = unlimited) |
| `reasoningBudget` | long | 0 | Token budget for reasoning/thinking |
| `preserveThinking` | bool | false | Preserve thinking tags in output |
| `timeoutSecs` | long | 60 | Maximum time for a single vision request in seconds |

**Behavior:**
- When `visionModel` is configured: `loadImageApi` sends the image to the vision model and returns the text description as the tool result
- When `visionModel` is not configured: `loadImageApi` loads the image into the main agent's vision context (existing inline behavior)
- The vision model operates in isolation with no access to tools, memory, RAG, or skills

### Embedding Configuration (`embedConfig`)

Configuration for the embedding backend. Supports both local (llama.cpp) and remote (HTTP API) backends.

#### Remote Embedding (HTTP API)

```json
"embedConfig": {
  "type": "remote",
  "server": { ... },
  "name": "nomic-embed-text",
  "nBatch": 512,
  "dimensions": 768
}
```

#### Local Embedding (llama.cpp)

```json
"embedConfig": {
  "type": "local",
  "modelPath": "/path/to/embedding-model.gguf",
  "context": 8192,
  "nBatch": 512,
  "dimensions": 768
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `type` | string | (required) | Either `"remote"` or `"local"` |
| `server` | object | - | Server config (remote only) |
| `name` | string | - | Model name (remote only) |
| `modelPath` | string | - | Path to GGUF model file (local only) |
| `context` | long | - | Context size for local embedding |
| `nBatch` | long | 512 | Batch size for embedding |
| `dimensions` | long | 768 | Embedding vector dimensions |

## Configuration Directory Structure

llmfun uses a local directory structure for data and configuration. The structure can be created with the `--local-setup` flag.

```
llmfun/
├── config/
│   ├── example.json         # Complete configuration reference
│   ├── execution_environments.json   # Curated execution environments
│   ├── prompt/              # Prompt templates and system prompts
│   │   └── *.md             # Markdown prompt files
│   └── thinking/            # Thinking templates for structured reasoning
│       └── *.md             # Structured reasoning strategy templates
├── data/
│   ├── memory               # LLM-persisted memory file (shared across sessions)
│   ├── rag.sqlite3          # RAG database (SQLite with FTS5 and vector search)
│   ├── state.json           # Active model selection state (auto-saved)
│   └── scratch/             # Temporary workspace and runtime data
│       └── monitor.jsonl    # Tool call metrics log (JSONL format)
└── workarea/                # Agent working directory for file operations
```

### Directory Details

| Path | Purpose |
|------|---------|
| `llmfun/config/` | Configuration files and templates |
| `llmfun/config/prompt/` | System prompt templates loaded at startup |
| `llmfun/config/thinking/` | Thinking templates accessible via `getThinkingTemplate()` tool |
| `llmfun/data/memory` | Persistent memory file where the LLM stores cross-session information |
| `llmfun/data/rag.sqlite3` | SQLite database for RAG with full-text search (FTS5) and vector embeddings |
| `llmfun/data/state.json` | Auto-saved state (active model index, session count, consolidation lock) |
| `llmfun/data/scratch/` | Temporary runtime data, including tool call monitoring logs |
| `llmfun/workarea/` | Sandbox directory where the agent can create and modify files |

### Path Resolution Priority

llmfun resolves paths with the following priority:

1. **Local directory** (`./llmfun/`) in the current working directory
2. **System search paths** (standard configuration and data directories)
3. **Embedded resource files** (bundled with the application)

### Magic Words

Configuration values support magic word substitution:

- `@{llmfun_workarea}`: Replaced with the configured workarea path
- `@{llmfun}`: Replaced with the directory of the llmfun executable

These are useful in `sandboxConfig.defaultOptions` for mount paths.

## Security & Configuration

### API Keys

llmfun requires API keys for LLM providers. These should be configured via:

- **Environment variables**: `OPENAI_API_KEY` (checked as fallback when no API key is configured in the config file)
- **Configuration file**: Server configuration with `apiKey` field

### Best Practices

- Never commit API keys to version control
- Add the following to your `.gitignore`:
  ```
  llmfun/config/
  llmfun/data/
  .llmfun.json
  ```
- Use environment variables for sensitive credentials when possible
- The `OPENAI_API_KEY` environment variable is automatically checked as a fallback if no API key is specified in the configuration

### CWD Config Security

When the workarea is set to the current working directory (or `.`), llmfun will NOT load `.llmfun.json` from the CWD by default. This prevents malicious projects from injecting configuration. To allow CWD config loading:

- Use `--trusted-config` flag
- Set workarea to a different directory than CWD

## Tools

The agent has access to the following tools:

### File I/O

| Tool | Description |
|------|-------------|
| `removeFile` | Remove a file by path |
| `writeFile` | Write content to a file, creating parent directories if needed |
| `readFile` | Read file contents with optional line numbering and range selection |
| `editFile` | Edit a file by replacing, removing, or appending lines |
| `applyDiff` | Apply a unified diff patch to a file |
| `replaceAll` | Replace all occurrences of a string in text |
| `listDirectory` | List files in a directory as JSON array |
| `grepFiles` | Search for a pattern in files |
| `countLinesInFile` | Count lines in a file |
| `md5HashFile` | Calculate the MD5 hash of a file |
| `loadImageApi` | Load an image for OpenAI API vision context |

### Encoding

| Tool | Description |
|------|-------------|
| `base64Encode` | Encode text as Base64 |
| `base64Decode` | Decode Base64 to text |
| `md5Hash` | Calculate MD5 hash of data |

### Memory

| Tool | Description |
|------|-------------|
| `writeMemory` | Store content as markdown for future retrieval about a topic |
| `readMemory` | Retrieve stored memory about a topic |
| `removeMemory` | Remove a stored memory entry |
| `getMemoryTopics` | List all memory topics with summaries |

### RAG (Retrieval-Augmented Generation)

| Tool | Description |
|------|-------------|
| `querySemantic` | Semantic vector search for relevant results (supports `database` parameter for scoping) |
| `queryTextSearch` | Full-text search (FTS5) for keyword matching (supports `database` parameter for scoping) |
| `queryBestMatch` | Combined semantic and full-text search (supports `database` parameter for scoping) |
| `listRAGDatabases` | List all available RAG databases with names and file paths |
| `loadFileToRAG` | Index a file into the RAG database |
| `loadContentToRAG` | Index raw content into the RAG database |
| `removeTopicFromRAG` | Remove a topic from the RAG index |
| `queryReadFile` | Read a specific line from a file in the RAG index |

### Thinking & Reasoning

| Tool | Description |
|------|-------------|
| `getThinkingTemplate` | Get a structured thinking template for a specific strategy |
| `listThinkingTemplates` | List all available thinking templates |

### Code Execution

| Tool | Description |
|------|-------------|
| `executeCommand` | Execute a command in an execution environment (container or host). Returns JSON with exitCode, stdout, and stderr. Use `environmentTag` to select the environment |
| `listEnvironments` | List available execution environments with tag, description, capabilities, and configuration details |

### Pipeline

| Tool | Description |
|------|-------------|
| `pipelineOutput` | Store output for downstream propagation in a pipeline |
| `taskDone` | Signal that the agent's task is fully completed |

### Metrics

| Tool | Description |
|------|-------------|
| `getMetrics` | Get current system metrics as a markdown report |
| `getToolHistory` | Get recent tool call history |

### Date/Time

| Tool | Description |
|------|-------------|
| `currentDateTime` | Get current date/time as ISO 8601 string |
