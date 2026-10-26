# llmfun

LLM agent harness - a CLI tool that orchestrates AI agents with tool calling, RAG, pipelines, metrics, skills, and a terminal UI. Supports remote API and local llama.cpp inference.

## Tech Stack

- **Language:** D (primary), C (vendor: sqlite3, sqlite3-vec, imtui), C++17 (cpp_tui, imgui_rich_md, llama.cpp)
- **Build:** Dub (`dub.sdl`) + Makefiles for the C/C++ components
- **Database:** SQLite3 with FTS5 and sqlite3-vec (vector search)
- **Key libraries:** argparse, requests, dyaml, colorlog, mylib, miniorm, nusilly (test runner)

## Directory Layout

```
llmfun/
|-- source/
|   |-- app.d                  # Thin command dispatcher
|   |-- utility_app.d          # Manual test utility entry (dub config llmfun_util)
|   `-- llm/
|       |-- agent/             # Agent (package.d) + AgentContext (context.d)
|       |-- app_agent/         # agent subcommand: AgentApp, TUI glue, slash commands
|       |-- app_rag.d          # rag subcommand
|       |-- app_mcp.d          # mcp subcommand
|       |-- app_tool_metrics.d # tool_metrics subcommand
|       |-- agent_pool.d       # Thread pool for agent execution
|       |-- chat.d             # Chat history, turn ids, history projections
|       |-- config.d           # Multi-layer configuration
|       |-- memory_tier.d      # Memory tier lifecycle actor (short/long/archive)
|       |-- query.d            # HTTP client with retry, streaming
|       |-- skill.d            # Skill management (Agent Skills standard)
|       |-- summary_agent.d    # Context compression / summarization
|       |-- session/           # Chat session persistence (SessionStore)
|       |-- pipeline/          # Pipeline engine and DAG
|       |-- rag/               # RAG databases, dialogue + reasoning indexes
|       |-- tool_call/         # Tool registry, broker, tools (io/ = file tools)
|       |-- mcp_server/        # MCP server (JSON-RPC 2.0 over stdio)
|       |-- metric/            # Metrics aggregation and monitoring
|       `-- tui/               # TUI D bindings
|-- common/                    # Shared embedder interface and config types
|-- local_model/               # Local inference via llama.cpp
|-- cpp_tui/                   # C++ TUI library with C API for D interop
|-- vendor/                    # Vendored C/C++ libraries and mylib
|-- config/                    # Example configs (example.yaml, execution_environments.yaml)
|-- llmfun/                    # Nested project dir: config/prompt/ (AGENT.md, nudges), skills/, data/
`-- doc/                       # Design docs (see References)
```

If your identity is **llmfun**:
- The file tools (read/write/edit/grep/list) use paths relative to `./`; the source code is under `./llmfun`.
- `execute_command` runs in `/workarea`; the absolute source path is `/workarea/llmfun`.
- Paths in this file are repo-relative: a module path such as `rag/database.d` lives under `source/llm/`.

## Commands

**Container:** use the image `llmfun/app:latest` for `execute_command`. Pass the whole command line as ONE string in a single-element array: `execute_command(environmentTag="llmfun", command=["cd llmfun && dub build --config=application"])`. `command` is an array of argument elements (no whitespace joining). The element count must be within the environment's `minArgs`/`maxArgs` (defaults 0 and 1; `-1` = unbounded). The `native` environment is a (1, 1) single-command-string environment.

```bash
dub test                                          # Unit tests (primary gate)
dub build --config=application                    # Main app (remote API only)
dub build --config=application-with-local-model   # Main app + llama.cpp
dub build --config=llmfun_util                    # Manual test utility (runs no tests)
./build/llmfun agent                              # Interactive agent
./build/llmfun rag add <path>                     # Add a file to the RAG index
./build/llmfun rag query "question"               # Query the RAG
./build/llmfun rag --dialogue                     # Read-only dialogue DB report
./build/llmfun tool_metrics                       # View tool metrics
./build/llmfun mcp --stdio                        # MCP server over stdio
./build/llmfun mcp --list-tools                   # List available MCP tools
```

## Code Conventions

### Comments

- **Ddoc, not doxygen:** `///`, `/** */`, `/+ +/`.
- **Module header:** every module starts with a 1-3 line ddoc comment before `module`.
- **Explain why, not what.** Write the code first, add comments only where genuinely needed.
- **Short:** one thought per line, 1-2 lines. No fixed-width wrapping, no mid-sentence line breaks.
- **Good vs bad:** `// reset here, as we will release the slot below` explains why; `// Get the number of items and store it in count` restates the code. Task-specific comments ("this fixes the problem you mentioned") are meaningless to a future reader.
- **Never add comments to copied code** that were not there originally.

### Strings and variables

- Prefer interpolated strings: `i"Found $(count) results".text`, not `format!"Found %s"`.
- Prefer backtick strings when embedding `"` or `\`.
- **Never initialize a `string` with `= ""`** - D auto-initializes.
- Prefer an immediately invoked local function over a declared variable plus branches.
- Zero-initialize floating point explicitly; D's default is NaN.

### Formatting

- K&R brace style (opening brace on the same line).
- Local imports inside functions/structs where the symbols are not pervasive.
- No magic numbers without named constants.
- No wrapper functions for stdlib symbols - import locally at the point of use.

### Error handling

- **No empty catch blocks** - log or handle: `catch (Exception e) { logger.trace(e.msg); }`.
- Use `std.logger` for diagnostics, never `stderr`.
- **Never catch `Error` or `Throwable`.**
- For `@safe nothrow` code that cannot use `.collectException`, wrap the throwing call in try/catch and nest another try/catch around the logging call; that empty inner catch is the only allowed exception to the rule above.

### Attributes

- **@safe wherever possible.** If the compiler rejects it, fix the cause instead of downgrading.
- **@trusted only to bridge** `@safe` and `@system`: verify the inputs and keep it minimal.
- **@system** only for raw pointers, assembly, and C interop.
- **pure** when the function reads or writes no global mutable state.
- **nothrow** when the function cannot throw; prefer error-return types in such APIs.

### General

- **ASCII only** in source, comments, and docs: no emdash, no unicode arrows. Use `-`, `->`, `x`, `...`.
- **Prefer reusing existing infrastructure** over introducing new components.

## Architecture & Patterns

### Entry point

`app.d` is an ultra-thin dispatcher: it parses CLI args via `argparse` and routes to `appMain` overloads in `app_agent/package.d`, `app_rag.d`, `app_mcp.d`, `app_tool_metrics.d`.

### Agent

- `Agent` (`agent/package.d`, module `llm.agent`): vision, streaming, feedback, stuck-loop detection.
- `AgentContext` (`agent/context.d`) is the tool-execution context: decoupled from `Agent` and constructible standalone with injected RAG/metrics dependencies.
- `AgentPool` (`agent_pool.d`) runs agents concurrently on a thread pool; agents communicate through a `Chat` history with role-based messages.

### Chat sessions

- One JSON file per session under `<dataDir>/chat/<id>.json`, header `{title, createdAt, updatedAt, messages}`. `SessionStore` (`session/`) speaks JSON only; `app_agent` bridges it to `agent_.chat`.
- `SessionId` is a `NamedType!string`; `state.json` stores the plain string (`activeChatSessionId`). Session id == filename, immutable, `<YYYYMMDD-HHMMSS>-<4hex>`, regex-validated at every store entry point (path traversal). Title lives in the header; rename edits the header only.
- Single writer (the agent thread), every write atomic (tmp file + rename). Unknown header keys round-trip through `SessionMeta.extra`.
- System-prompt entries are stripped before save and re-set at startup. One-shot mode (`-p`) appends to the last active session.
- Full documentation: `doc/sessions.md`.

### Turn ids and compression

- Every history entry carries a `turnId` (mixin `TurnIdMixin`); 0 = no turn. A turn is one user query plus everything produced while answering it.
- Only `Chat.add` decides boundaries: a user-query message opens a turn (allocating from `nextTurnId_`), every other insertion continues it, `setSystemPrompt` stamps 0. Vision chats open one explicitly with `beginNewTurn()`.
- Counter state is per-Chat (no static/shared/atomic). The high-water mark persists as the header key `next_turn_id`; the per-message stamp lives in `save_data["turn_id"]`.
- Invariants pinned by unittests in `llm/chat.d`: history sorted by `(turn_id, position)`, active-turn stamps > 0, counter never decreases, no id reuse within a session (also across restarts). Legacy files are reconstructed on load (`reconstructTurnIds`).
- Projections: `getDialogueHistory()` (facts) and `getReasoningTrace()` (trace). Harness control traffic (user-role messages with `userQuery == false`) appears in neither.
- Compression multicasts a `CompressionCheckpoint` once per compression that evicts verbatim content. Listeners must not block and must not throw.

### Searchable dialogue and reasoning history

- `DialogueIndex` (`rag/dialogue_index.d`) is owned by the agent thread and drives a worker actor on its own thread (`rag/dialogue_worker.d`); the worker owns its own embedder and exchanges value messages only.
- Episodes go into per-session SQLite WAL databases under `dialogueDir` (default `llmfun/data/dialogue`), one `<sessionId>.db`, as Topic sources named `d_<sess>__t<turnStart>_<turnEnd>__<epochMillis>` (`encodeTopicName`/`decodeTopicName`).
- Dedup identity is topic + content; a turn split across two checkpoints merges (existing episode text + the new piece under the same topic). FTS is rebuilt synchronously after every job that committed chunks.
- Turn metadata is always derived by decoding topic names, never from worker memory. `query_dialogue_history` takes `maxTurnAge`: 0/negative = no filtering, positive N keeps `turnEnd >= maxTurn - N`.
- Indexing is asynchronous (~2-3 s lag, stated in the tool description). When the agent should query is prompted from `llmfun/config/prompt/AGENT.md`, not from a code constant.
- `llmfun rag --dialogue` reports over the session databases read-only and dispatches before `createRag`; a session whose read-only open fails is skipped with a warning.
- Reasoning counterpart: `ReasoningIndex` (`rag/reasoning_index.d`). Docs: `doc/dialogue_search.md`, `doc/reasoning_search.md`.

### Slash commands

- Delegate registry (`app_agent/slash.d`): `SlashCommand` structs with name, aliases, help lines, `SlashArgMode`, help `order`, and an `AgentStatus delegate(ref AgentApp, string)` handler.
- Built-ins are registered by group modules: `slash_core.d` (help/quit/stop/continue/compact/debug), `slash_session.d` (sessions/switch/new/rename/delete/clear + the delete confirm state machine), `slash_model.d`, `slash_pipeline.d` (plan/code), `slash_skills.d`.
- Help text is generated from the registry (order asc, then registration index); a golden unittest in `tests.d` locks it.
- Plugin API: `addStartupSlashCommand`, `AgentApp.registerSlashCommand`, `slashCommands()`, `sendChatMessage`, plus `SlashCommand`/`SlashArgMode`/`AgentStatus`. Docs: `doc/slash_commands.md`.

### Tools and the tool broker

- A tool is a function taking a `Context` and a params struct, marked with the `@Function("description")` UDA and collected by the `RegisterLlmFunctions!()` mixin in `tool_call/package.d`.
- Group membership is configuration, not code: `toolBroker.groups` maps a group name to `{description, tools}`, `hiddenTags` lists groups that start hidden, `alwaysOn` forces tools or whole groups visible, `neverHideTools` (default: the task-completion tool; config-file values use D-axis registry names, see `config/example.yaml`) bypasses the tool filter, `enabled` is the kill switch.
- A tool in no group is always on. Hidden group members become visible when the model activates the group via `list_tool_tags`; activation is sticky and pruned at compression points. `tool_search` discovers tools by keyword.
- `tool_call/io/` is the file-tool package (`read_file`, `write_file`, `edit_file`, `apply_diff`, `grep_files`, ...). Docs: `doc/tool_broker.md`.

### Memory tiers

- Memories are Markdown files in every configured `memoryArea` dir (default resolution: `llmfun/data/memory` plus the XDG data dir), under the subdirs `short/`, `long/`, `archive/`.
- Writes always land in `short/` (rewriting an existing `long/` memory requires including its stored content). `get_memory_topics` lists `short/` only; `long/` is RAG-indexed; `archive/` is unreachable.
- `MemoryTierActor` (`memory_tier.d`) runs one pass at startup and on every `tierPassIntervalHours` tick (0 = startup only): create any missing tier dirs, flat-layout upgrade, reconcile disk vs RAG, promote aged `short/` entries (default 7 d) to `long/`, demote aged `long/` entries (default 100 d) to `archive/`.
- Every phase is diff-based, so a crash window heals on the next pass; failures are logged and skipped. Tier dirs are created by that pass, so a missing structure heals on the next agent start.
- Agent-facing rules: `llmfun/skills/llmfun-memory/SKILL.md`.

### Pipeline, RAG, skills

- DAG pipeline engine in `pipeline/package.d` (topological sort in `pipeline/graph.d`); nodes propagate outputs to downstream nodes.
- RAG: SQLite + FTS5 + sqlite3-vec, `SchemaVersion = 6` in `rag/database.d`; embedder factory with HTTP (OpenAI-compatible) and local (llama.cpp) backends.
- Skills follow the Agent Skills standard: a directory with `SKILL.md` plus optional `references/`, `scripts/`, `assets/`. Built-ins live in `llmfun/skills/`; loading copies the skill into the sandbox workarea. Docs: `doc/skills.md`.

### Configuration

- Layers: CLI args > file config > defaults. Base config from `LLMFUN_SYSTEM_CONFIG` (or the XDG system path), overlay from `--config` or `.llmfun.yaml` in the CWD.
- Security: the CWD config is skipped when the workarea equals the CWD unless `--trusted-config` is given.
- Covers `ToolLimits`, `RagConfig`, `toolBroker`, `SandboxConfig` execution environments, skill/prompt paths, and endpoint types (`llamaCpp`, `deepseek`).
- Magic words `@{llmfun}` (binary directory) and `@{llmfun_workarea}` (workarea path) are substituted in paths, env vars, and container options.

### TUI

- C++ TUI (`cpp_tui/`) renders Dear ImGui in the terminal with markdown via the vendored `imgui_rich_md`; D bindings in `tui/package.d` handle streaming and inter-thread messages; the pure C API (`tui_api.h`/`tui_api.cpp`) is the D interop seam. D can import `.c` files directly - no bindings or `extern(C)` needed.
- Session sidebar (`ChatTabSessionPanel`): rows with active marker and `[N]` count, a per-row menu (Rename/Delete), and an fzf-style fuzzy filter over title + preview. New/Delete are guard-and-skipped while busy; a pending session switch defers to a single last-click-wins slot. Mutually exclusive with the pipeline panel.
- Persistent messages render a derived header `[NR] <summary> [TIME]` (D side only): NR is the 1-based ordinal among displayed persistent messages, TIME the measured LLM/tool duration (`save_data.processMs`, formatted by mylib `my.time`). Display-only lines and the one-shot CLI are undecorated.
- Max width: `tui.maxWidth` (0 = unlimited, valid 0 or [40, 10000], enforced in `validateConfig`) is clamped in C++ every frame; `LLMFUN_TUI_MAX_WIDTH` for the standalone `cpp_tui` binary.
- Details: `doc/tui_design.md`.

### MCP server

- MCP over stdio with JSON-RPC 2.0 (`mcp_server/`: `types.d`, `protocol.d`, `transport.d`, `package.d`), bridging to the tool registry via `descAllFunctions()` and `executeFunc()`.
- Runs as a `std.concurrency` actor driven by value messages (`McpServerConfig`, `McpShutdown`, `McpStarted`, `McpStopped`, `McpFailed`); no shared state, and SIGINT/SIGTERM are blocked and consumed by the main thread.
- Methods: `initialize`, `tools/list`, `tools/call`, `ping`, `resources/list`, `prompts/list`. Tool visibility via `ReFilter` (`--include`/`--exclude`). Docs: `doc/mcp.md`.

## Testing

The runner is **nusilly** (a `silly` fork); every argument after `--` is passed to the test binary.

- `dub test` compiles and runs all inline `unittest` blocks. It is the primary gate: run it after changes to tested code.
- A failing test exits non-zero (`Error Program exited with code 1`); a green run ends with `Summary: N passed, 0 failed in X ms`.
- It takes about a minute (rebuild + run). Do not skip it to save time.
- Tests run multithreaded (one worker per CPU) - every test must pass in that default configuration. `-t 1` is only for diagnosing why a test fails; a test that requires single-threaded execution is wrong and must be rewritten.
- **Tests must not swap the global logger** (`logger.sharedLog`) or assert on log output. Any thread can log during the swap, so captured output is nondeterministic. Make the tested behavior observable through return values, out-parameters, or explicitly passed collectors instead.
- New code includes inline `unittest` blocks.

Filtering (after `dub test --`): `-i <regexp>` include, `-e <regexp>` exclude (do not combine them; both match module and test names), `-t <n>` threads, `--fail-fast` (only an `Error`/`Throwable` stops the run, not an `AssertError`), `-v` verbose, `--no-colours`, `-h`.

### Naming and experiments

A string UDA names a unittest (the first one wins); unnamed tests report as `<module> __unittest_L<line>_C<col>`.

For a temporary experiment, add `@("my_experiment") unittest { ... }` to any module, run `dub test -- -i '.*my_experiment.*'`, then remove it. Module-wide: `dub test -- -i '.*llm\.rag.*'`.

### Test descriptions

Give every unittest an `@("...")` description - it is what `dub test` prints when the test fails.

Describe the scenario (input or condition and expected outcome), not the implementation:

`@("0/0 escalation ladder is rejected")`
`@("empty file names are rejected only for enabled kinds")`
`@("a valid enum member name selects that member; an invalid one keeps the default")`

Not: `@("validateConfig calls checkNudges first")` or `@("parses the value through the Nullable branch")`.
Avoid internal jargon - the description must be readable without knowing the code's internals. Not: `@("a minArgs 0 environment forwards an empty command past the arity check for both runners")` (jargon: "runners", "arity check"). Better: `@("an environment with minArgs 0 accepts an empty command instead of rejecting it")`.

## Agent Rules

- **Always verify facts** with RAG search or memory before asserting them; internal knowledge is not enough for names, technical details, or version-specific information.
- **Read the relevant source files** before writing code; changes must blend with the existing codebase.
- **Run `dub build`** after changes to verify compilation.
- **Run `dub test`** after changes to tested code; do not replace it with partial checks.
- **Never write PR descriptions, commit messages, or reviewer responses** on behalf of the user.
- **Never commit or push** without explicit human approval. When committing for the user, use `Assisted-by:`, never `Co-authored-by:`.
- **Track known gaps** in `doc/todo.md`.

## References

- `doc/database.md` - database schema and RAG details
- `doc/rag_design.md`, `doc/rag_design2.md` - RAG design
- `doc/tool_broker.md` - tool broker and tool groups
- `doc/sessions.md` - chat session storage and agent integration
- `doc/dialogue_search.md`, `doc/reasoning_search.md` - dialogue and reasoning indexes
- `doc/compression.md` - context compression
- `doc/skills.md` - skills system
- `doc/slash_commands.md` - slash command registry and plugins
- `doc/tui_design.md` - TUI architecture
- `doc/mcp.md` - MCP protocol details
- `doc/threat_model.md` - threat model
- `doc/todo.md` - task tracking and known gaps
