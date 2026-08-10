# llmfun

LLM agent harness — a CLI tool that orchestrates AI agents with tool calling, RAG, pipelines, metrics, skills, and a terminal UI. Supports both remote API and local llama.cpp model inference.

## Tech Stack

- **Language:** D (primary), C (vendor: sqlite3, sqlite3-vec, imtui), C++17 (cpp_tui, imgui_markdown, llama.cpp)
- **Build:** Dub (`dub.sdl`) + Makefiles for C/C++ components
- **Database:** SQLite3 with FTS5 and sqlite3-vec (vector search)
- **Local Inference:** llama.cpp
- **Terminal UI:** imtui (Dear ImGui for terminals) + custom C++ TUI library
- **Key Libraries:** argparse, requests, dyaml, colorlog, mylib, miniorm

## Directory Layout

```
llmfun/
├── source/
│   ├── app.d                      # Thin command dispatcher
│   ├── utility_app.d              # Test utility entry point
│   └── llm/                       # Main package
│       ├── agent/                 # Agent package
│       │   ├── package.d          # Core Agent class (module llm.agent)
│       │   └── context.d          # Tool-execution context (AgentContext, VisionImage)
│       ├── agent_pool.d           # Thread pool for agent execution
│       ├── app_agent.d            # Agent subcommand handler + TUI
│       ├── app_rag.d              # RAG subcommand handler
│       ├── app_mcp.d              # MCP server subcommand handler
│       ├── app_tool_metrics.d     # Tool metrics subcommand handler
│       ├── config.d               # Multi-layer configuration
│       ├── chat.d                 # Chat history / message management
│       ├── query.d                # HTTP client with retry, streaming
│       ├── skill.d                # Skill management system
│       ├── summary_agent.d        # Context compression / summarization
│       ├── memory.d               # Memory consolidation orchestrator
│       ├── pipeline/              # Pipeline engine & DAG
│       ├── rag/                   # RAG database and query logic
│       ├── tool_call/             # Tool registration and implementations
│       ├── mcp_server/            # MCP server (JSON-RPC 2.0 over stdio)
│       ├── metric/                # Metrics aggregation and monitoring
│       └── tui/                   # TUI D bindings
├── common/                        # Shared embedder interface and config types
├── local_model/                   # Local model inference via llama.cpp
├── cpp_tui/                       # C++ TUI library with C API for D interop
├── vendor/                        # Vendored C/C++ libraries
├── config/                        # Runtime configuration templates
└── doc/                           # Documentation (database, skills, TUI design)
```

## Commands

**Important**: The container image to use when running the tool `executeImage` is `llmfun/app:latest`.

Example of using `executeImage`: `executeImage(imageName="llmfun/app:latest", command=["cd", "llmfun", "&&", "dub", "build", "--config=application"])`

```bash
cd llmfun

dub test                                     # Run all unit tests
dub build --config=application              # Build main app (remote API only)
dub build --config=application-with-local-model   # Build with llama.cpp support
dub build --config=llmfun_test              # Build test utility (manual testing)
./build/llmfun agent                        # Run interactive agent
./build/llmfun rag add <path>               # Add file to RAG index
./build/llmfun rag query "question"         # Query RAG knowledge base
./build/llmfun tool_metrics --data llmfun/data/scratch/monitor.jsonl   # View tool metrics
./build/llmfun mcp --stdio                                              # Run MCP server over stdio
./build/llmfun mcp --list-tools                                         # List available MCP tools
```

## Code Conventions

### Comments

- **Use ddoc comments, NOT doxygen comments.** Ddoc forms: `/** ... */`, `///`, `/+ ... +/`.
- **Module header:** Every module must start with a brief ddoc comment (1-3 lines) introducing the module and what it does, placed before the `module` declaration.
- **Comments explain why, not what.** If the code is self-documenting, the comment is redundant.
- **Concise:** 1-2 lines max. Break longer explanations into separate short comments.
- **No hard-wrapping:** Do not wrap to a fixed column width. Let lines flow naturally.
- **No mid-sentence breaks:** Each comment line should be a complete thought.
- **Simple language:** Short words, short sentences.
- **Write code first, then add comments** only where genuinely needed.
- **Never add comments to copied code** that weren't there originally.

### String Handling

- **Prefer interpolated strings** over `std.format.format` / `formattedWrite`:

  ```d
  // Good
  auto msg = i"Found $(count) results for $(query)".text;

  // Bad
  auto msg = format!"Found %s results for %s"(count, query);
  ```

- **Prefer backtick-strings** when embedding `"` or `\`:

  ```d
  // Good
  auto path = `foo "embed" bar`;

  // Bad
  auto path = "foo \"embed\" bar";
  ```

### Variable Initialization

- **NEVER initialize `string` with `= ""`.** D auto-initializes all locals:

  ```d
  // Good
  string name;

  // Bad
  string name = "";
  ```

- **Prefer local function initialization** over unnecessary variable declarations:

  ```d
  // Good
  auto status = () {
      if (isOk) return "ok";
      if (isWarning) return "warn";
      return "error";
  }();

  // Bad
  string status;
  if (isOk) status = "ok";
  else if (isWarning) status = "warn";
  else status = "error";
  ```

### Naming & Formatting

- **K&R brace style** (opening brace on the same line)
- **Local imports** inside functions/structs where symbols are not pervasive
- **No magic numbers** without named constants
- **No wrapper functions** for stdlib symbols — use local imports at point of use

### Error Handling

- **No empty catch blocks.** Always log or handle caught exceptions:

  ```d
  catch (Exception e) {
      import std.logger : trace;
      trace(e.msg);
  }
  ```

- **Logging:** Use `std.logger` (not `stderr`) for diagnostic output.
- **Silent catches for @safe:** When `.collectException` cannot be used (e.g., due to
  `@safe` violations), wrap the throwing code in try/catch and nest another
  try/catch around the logging call. The innermost catch may be empty — this is
  the only place where an empty catch block is allowed:

  ```d
  try {
      // code that may throw
  } catch (Exception e) {
      try {
          import std.logger : trace;
          trace(e.msg);
      } catch (Exception innerE) {
          // Empty inner catch allowed — keeps the parent @safe and nothrow
      }
  }
  ```

### Attributes

- **@safe:** Mark functions `@safe` whenever possible. If the compiler rejects it,
  fix the underlying issue rather than downgrading the safety level.

  ```d
  // Good — simple, obviously safe
  bool isAgentMdTopic(string topic) @safe pure nothrow { ... }

  // Bad — unmarked function that could be @safe
  bool isAgentMdTopic(string topic) { ... }
  ```

- **@trusted:** Use only when `@safe` is not feasible. `@trusted` bridges `@safe`
  and `@system` — it must verify all inputs and ensure operations are memory-safe
  before exposing a `@safe` interface. Keep `@trusted` code minimal.

- **@system:** The default safety level. Avoid unless dealing with low-level
  operations (raw pointers, assembly, C interop). Never expose `@system`
  through a `@safe` interface without `@trusted` wrapping.

- **pure:** Add `pure` to functions that do not access or modify global/mutable
  state beyond their parameters.

- **nothrow:** Mark functions `nothrow` when they do not throw exceptions. Use it
  to signal error-return patterns (returning `SumType`, error codes, etc.) and
  let the compiler enforce the contract.

### General

- **ASCII only.** Avoid emdash, unicode arrows, or any non-ASCII characters. Use `-`, `->`, `x`, `...`.
- **Do not split lines mid-sentence** or force lines to fit a fixed character width.
- **Prefer reusing existing infrastructure** over introducing new components.

## Architecture & Patterns

### Entry Point

`app.d` is an ultra-thin dispatcher. It parses CLI args via `argparse` and routes to `appMain` overloads in `app_agent.d`, `app_rag.d`, `app_mcp.d`, and `app_tool_metrics.d`.

### Agent System

- `Agent` class in `agent/package.d` (module `llm.agent`) is the core. Handles vision, streaming, feedback, and stuck-loop detection. `AgentContext` in `agent/context.d` is the tool-execution context; it is decoupled from `Agent` and can be constructed standalone with injected RAG/metrics dependencies.
- `AgentPool` manages concurrent agent execution via a thread pool.
- Agents communicate through a `Chat` history with role-based messages.

### Tool Call System

- Tools are registered via `@Function` attribute and `RegisterLlmFunctions!()` mixin in `tool_call/package.d`.
- Each tool module implements functions that take a `Context` and a params struct.
- `tool_call/io.d` is the largest module — file system operations with advanced editing (searchAndReplace, applyDiff, editFileByMarker).

### Pipeline System

- DAG-based pipeline engine in `pipeline/package.d` with topological sorting in `pipeline/graph.d`.
- Pipelines orchestrate multi-step agent workflows with node output propagation.

### RAG System

- SQLite-backed with FTS5 full-text search and sqlite3-vec for vector similarity.
- Schema version v6 in `rag/database.d`.
- Embedder factory pattern: HTTP (OpenAI-compatible) and local (llama.cpp) backends.

### Skill System

- Implements Agent Skills open standard. Skills are directories with `SKILL.md`, `references/`, `scripts/`, `assets/`.
- 13 built-in skills (create-skill, code-review, debugging, dlang, etc.).
- Skills are loaded at runtime by copying into the sandbox workarea.

### Configuration

- Multi-layer config in `config.d`: CLI args override file config, file config overrides defaults.
- Two-layer loading: base config from `LLMFUN_DEFAULT_CONFIG` / system path, overlay from `--config` / `.llmfun.json` in CWD.
- Security: CWD config skipped when workarea == CWD unless `--trusted-config` is used.
- Supports `ToolLimits`, `RagConfig`, `SandboxConfig` with image catalogs, skill paths, consolidation settings, and endpoint types (`llamaCpp`, `deepseek`).
- Magic word substitution: `@{llmfun_workarea}` and `@{llmfun}` in container options.

### TUI

- C++ TUI library (`cpp_tui/`) provides Dear ImGui-based terminal UI with markdown rendering.
- D bindings in `tui/package.d` handle streaming and inter-thread message passing.
- Exposed via pure C API (`tui_api.h` / `tui_api.cpp`) for D interop.

### MCP Server

- Implements the Model Context Protocol (MCP) over stdio using JSON-RPC 2.0.
- `mcp_server/` package: `types.d` (JSON-RPC types), `protocol.d` (parsing/serialization), `transport.d` (stdio transport), `package.d` (MCPServer class + actor).
- Bridges MCP's JSON-RPC protocol to llmfun's existing tool infrastructure via `descAllFunctions()` and `executeFunc()`.
- Uses `ReFilter` for tool visibility control (`--include`/`--exclude` CLI flags).
- Runs as a `std.concurrency` actor: the main thread spawns `runMcpServer` and communicates via messages (`McpServerConfig`, `McpShutdown`, `McpStarted`, `McpStopped`, `McpFailed`).
- No shared state between threads; termination signals (SIGINT/SIGTERM) are blocked and consumed by the main thread via `sigtimedwait`.
- Stdio transport uses unbuffered POSIX reads to avoid the poll/FILE buffering race that causes stalls.
- Supports MCP methods: `initialize`, `tools/list`, `tools/call`, `ping`, `resources/list` (empty), `prompts/list` (empty).
- See `doc/mcp.md` for protocol details and usage examples.

## Testing

- **Run all unit tests**: `dub test` (no configuration parameter). This compiles and runs all inline `unittest` blocks across all modules. This is the primary test command.
- **Build test utility**: `dub build --config=llmfun_test`. This configuration builds a separate test utility binary (`utility_app.d`) for manually testing implementation details. It does NOT run the unit test suite.
- Entry point for test utility: `source/utility_app.d`.
- Inline unit tests exist in most modules (e.g., `rag/rag.d`, `llm/tool_call/io/tests.d`).
- New code should include inline `unittest` blocks.

## Agent Rules

- **Always verify facts** using RAG search or memory before asserting them. Internal knowledge is not sufficient for specific names, technical details, or version-specific information.
- **Read relevant source files** before writing any code. Your changes must blend with the existing codebase.
- **Run `dub build`** after making changes to verify compilation.
- **Never write PR descriptions, commit messages, or reviewer responses** on behalf of the user.
- **Never commit or push** without explicit human approval. If committing on behalf of the user, use `Assisted-by:` in the commit message, never `Co-authored-by:`.
- **Track known gaps** in `doc/todo.md`.

### Code Comment Examples

```d
// GOOD (code is self-explanatory, no comment needed)

auto count = items.length;


// BAD (too verbose, restates what the code already says)

// Get the number of items in the items array and store it in count
auto count = items.length;
```

```d
// GOOD (explains a non-obvious invariant)

accept();
bool hasClient = listen(idleInterval);
if (hasClient) {
    taskQueue.onIdle(); // also signal child disconnection
}


// BAD (too verbose, restates what the code already says)

// Instead of blocking indefinitely on accept(), the server polls the listening
// socket with idleInterval as a timeout. If no new client connects within that
// interval, it fires taskQueue.onIdle() and loops back
```

```d
// GOOD (generic, useful to any future reader)

// reset here, as we will release the slot below
nTokens = 0;
// ... (a lot of code)
release();


// BAD (addresses the user's task, meaningless out of context)

// Reset nTokens to 0 before releasing the slot. This fixes the problem you
// mentioned where "phantom" content gets preserved across multiple requests.
nTokens = 0;
```

```d
// GOOD (comment is kept concise and useful)

// one decode step of codePredictor
// at stepIdx g:
// - read code from outCodeCache[g], then embed it with codebook table g-1
// - write new kv at cache row g+1, sample with lmHead[g]
// - write result to outCodeCache[g+1]


// BAD (long, hard-wrapped to fixed column, annoying to read)

// one autoregressive decode step of the 5-layer codePredictor. See the
// comment in models.h for the cache/tensor conventions this relies on.
//
// index mapping (derived from the reference pipeline-tts.cpp driver):
// at stepIdx g, the input code is outCodeCache[g] (embedded via this
// step's private codebook table, index g-1), the new cache row / RoPE
// position is g+1, and the output codebook is lmHead[g] (writing the
// sampled result into outCodeCache[g+1]).
```

## References

- `doc/database.md` — Database schema and RAG details
- `doc/skills.md` — Skills system documentation
- `doc/tui_design.md` — TUI architecture
- `doc/todo.md` — Task tracking and known gaps
