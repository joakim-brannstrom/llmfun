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

llmfun supports three subcommands:

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

## Global CLI Parameters

These parameters apply to all commands:

| Parameter | Short | Description |
|-----------|-------|-------------|
| `--config <path>` | `-c` | Path to a configuration file to read |
| `--verbose` | `-v` | Set log verbosity level (repeat for more verbosity) |

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

llmfun is configured via a JSON configuration file specified with `--config <path>` or the `LLMFUN_DEFAULT_CONFIG` environment variable. See `example.json` for a complete reference of all available options.

### Configuration Structure

```json
{
  "dataDir": "llmfun/data",
  "memoryArea": "llmfun/data/memory",
  "scratchArea": "llmfun/data/scratch",
  "thinkingTemplatesDir": "llmfun/config/thinking",
  "promptDir": "llmfun/config/prompt",
  "workArea": "llmfun/workarea",
  "containerCmd": "podman",
  "agentPrompt": "AGENT.md",
  "activeCodeModelIndex": 0,
  "toolLimits": {...},
  "rag": [...],
  "toolFilter": {...},
  "ragFilter": {...},
  "codeModels": [...],
  "summaryModel": {...},
  "embedConfig": {...}
}
```

### Top-Level Options

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `dataDir` | string | `llmfun/data` | Base directory for data files |
| `memoryArea` | string | `llmfun/data/memory` | Path to persistent memory file |
| `scratchArea` | string | `llmfun/data/scratch` | Temporary workspace and runtime data |
| `thinkingTemplatesDir` | string | `llmfun/config/thinking` | Directory with thinking strategy templates |
| `promptDir` | string | `llmfun/config/prompt` | Directory with prompt templates |
| `workArea` | string | `llmfun/workarea` | Agent working directory for file operations |
| `containerCmd` | string | `podman` | Container runtime command (podman or docker) |
| `agentPrompt` | string | `AGENT.md` | Agent system prompt file name (searched in promptDir) |
| `activeCodeModelIndex` | long | `0` | Index of the active code model in `codeModels` array |
| `toolLimits` | object | `{}` | Tool execution limits (see below) |

### Tool Limits (`toolLimits`)

Configures per-tool limits.

```json
"toolLimits": {
  "readFileMaxLines": 20
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `readFileMaxLines` | long | `20` | Maximum number of lines the `readFile` tool can read per call |

### RAG Database Configuration (`rag`)

Array of RAG database configurations. The first database is primary (read/write); additional databases are read-only.

```json
"rag": [
  {
    "path": "llmfun/data/rag.sqlite3",
    "description": "Primary RAG database (read/write)"
  },
  {
    "path": "/path/to/readonly_knowledge.sqlite3",
    "description": "Read-only knowledge base"
  }
]
```

Each entry supports:
- `path` (string, required): Path to the SQLite database file
- `description` (string, optional): Human-readable description

**Note**: For backward compatibility, plain strings are also accepted (treated as path with empty description).

### Tool Filter (`toolFilter`)

Controls which tools the agent can access via regex include/exclude patterns.

```json
"toolFilter": {
  "include": [".*"],
  "exclude": ["executeCode", "executeDCodeWithDub", "executeGit"]
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

- `include` (string[]): Regex patterns for files to include (default: `.*\.txt`, `.*\.md`)
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
  "embedUrl": "embeddings",
  "timeoutSeconds": 3600,
  "httpVerbosity": 0,
  "verifySslCert": true,
  "keepAlive": true,
  "maxRetries": 3,
  "backoffMs": 500,
  "apiKey": ""
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `url` | string | (required) | Base URL of the LLM server |
| `promptUrl` | string | `v1/completion` | Endpoint for prompt completion |
| `chatUrl` | string | `v1/chat/completions` | Endpoint for chat completion |
| `slotUrl` | string | `slots` | Endpoint for slot management |
| `embedUrl` | string | `embeddings` | Endpoint for embeddings |
| `timeoutSeconds` | long | 0 | Request timeout in seconds |
| `httpVerbosity` | long | 0 | HTTP logging verbosity level |
| `verifySslCert` | bool | true | Verify SSL/TLS certificates |
| `keepAlive` | bool | true | Keep HTTP connections alive |
| `maxRetries` | long | 3 | Maximum retries for transient failures |
| `backoffMs` | long | 500 | Initial backoff in milliseconds (exponential) |
| `apiKey` | string | "" | API key for Bearer token auth (falls back to `OPENAI_API_KEY` env var) |

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
│   ├── prompt/          # Prompt templates and system prompts
│   │   └── *.md         # Markdown prompt files
│   └── thinking/        # Thinking templates for structured reasoning
│       └── *.md         # Structured reasoning strategy templates
├── data/
│   ├── memory           # LLM-persisted memory file (shared across sessions)
│   ├── rag.sqlite3      # RAG database (SQLite with FTS5 and vector search)
│   ├── state.json       # Active model selection state (auto-saved)
│   └── scratch/         # Temporary workspace and runtime data
│       └── monitor.jsonl # Tool call metrics log (JSONL format)
└── workarea/            # Agent working directory for file operations
```

### Directory Details

| Path | Purpose |
|------|---------|
| `llmfun/config/prompt/` | System prompt templates loaded at startup |
| `llmfun/config/thinking/` | Thinking templates accessible via `getThinkingTemplate()` tool |
| `llmfun/data/memory` | Persistent memory file where the LLM stores cross-session information |
| `llmfun/data/rag.sqlite3` | SQLite database for RAG with full-text search (FTS5) and vector embeddings |
| `llmfun/data/state.json` | Auto-saved state (active model index) |
| `llmfun/data/scratch/` | Temporary runtime data, including tool call monitoring logs |
| `llmfun/workarea/` | Sandbox directory where the agent can create and modify files |

### Path Resolution Priority

llmfun resolves paths with the following priority:

1. **Local directory** (`./llmfun/`) in the current working directory
2. **System search paths** (standard configuration and data directories)
3. **Embedded resource files** (bundled with the application)

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
  ```
- Use environment variables for sensitive credentials when possible
- The `OPENAI_API_KEY` environment variable is automatically checked as a fallback if no API key is specified in the configuration

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
| `executeCode` | Execute D code in a sandbox container |
| `executeDCodeWithDub` | Execute D code with dub (build or test) in a sandbox |
| `executeGit` | Execute a git command in a sandboxed repository |

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

