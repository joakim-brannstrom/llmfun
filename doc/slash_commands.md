# Slash Commands in llmfun

llmfun's agent mode supports slash commands (`/help`, `/quit`, `/model`, `/plan`, ...). They are implemented as a delegate-based command registry inside the `llm.app_agent` package, with per-group command modules and a public plugin API that lets code outside the package register its own commands.

---

## Table of Contents

- [Overview](#overview)
- [Module Layout](#module-layout)
- [Registry Mechanics](#registry-mechanics)
  - [Command Struct](#command-struct)
  - [ArgModes](#argmodes)
  - [Dispatch Rules](#dispatch-rules)
  - [Help Generation](#help-generation)
- [Built-in Command Groups](#built-in-command-groups)
- [Plugin API](#plugin-api)
  - [Startup Hook](#startup-hook)
  - [Instance Registration Seam](#instance-registration-seam)
  - [Example Plugin](#example-plugin)
- [Visibility Rules](#visibility-rules)
- [Cross-Cutting Behavior](#cross-cutting-behavior)
  - [Pending-Delete Rule](#pending-delete-rule)
  - [Input Strip Asymmetry](#input-strip-asymmetry)
  - [Stop Reachability](#stop-reachability)
- [Trust Model](#trust-model)
- [Testing](#testing)
  - [Golden Help Test](#golden-help-test)
  - [External-Registration Test](#external-registration-test)

---

## Overview

The pre-refactor implementation was an `if-else if` chain on the query string inside `AgentApp.runAgent`. The refactor replaces it with a registry of `SlashCommand` values; `runAgent` is now a thin dispatcher (pending-delete rule, empty check, registry dispatch, normal agent query path). Behavior is byte-identical to the old chain, locked by a golden help test.

| Aspect | Decision |
|--------|----------|
| Registry | `SlashCommandRegistry` value field of `AgentApp` (`slashCommands_`) |
| Handlers | Free functions taking `ref AgentApp` - no delegate captures, no lifetime hazards |
| Dispatch | AA lookup by command name (O(1)); aliases share the canonical entry |
| Help text | Generated from the registry, sorted by `(order asc, registration index asc)` |
| External registration | `addStartupSlashCommand` (module hook) + `registerSlashCommand` (instance seam) |

## Module Layout

```
source/llm/app_agent/
├── package.d            # AgentApp + appMain (module llm.app_agent)
├── slash.d              # Registry machinery + plugin API
├── slash_core.d         # /help /quit /q /exit /stop /compact /debug
├── slash_session.d      # /sessions /switch /new /rename /delete /clear (+ delete state machine)
├── slash_model.d        # /model
├── slash_pipeline.d     # /plan /code
├── slash_skills.d       # /skills /refresh-agent-md
├── ui.d                 # UiMessenger + stream updaters
└── tests.d              # Golden help test + built-in surface tests
```

## Registry Mechanics

### Command Struct

```d
struct SlashCommand {
    string name;                 // canonical name, no leading '/'
    string[] aliases;            // e.g. ["q", "exit"] for quit
    string[] helpLines;          // verbatim pre-formatted help lines
    SlashArgMode argMode;        // none | required | optional
    int order;                   // ascending help-listing position
    AgentStatus delegate(ref AgentApp app, string arg) handler;
}
```

### ArgModes

| Mode | Semantics |
|------|-----------|
| `none` | Exact match only. Any argument after the name takes the unknown-command path |
| `required` | An empty argument takes the unknown-command path (bare `/plan`, `/code`, `/switch`, `/rename`) |
| `optional` | Empty argument dispatches to the handler (bare `/model` lists models, bare `/delete` emits its usage error) |

### Dispatch Rules

`execute(ref AgentApp app, string input)` tokenizes exactly like the old chain: body = input without the leading `/`; name = body up to the first space; arg = raw remainder after the first space, unstripped (`""` when there is no space). The handler receives the raw arg and strips it exactly as the old chain did.

- Unknown name -> `system: Unknown command: '%s'. Type /help for available commands.` (verbatim)
- `ArgMode.none` + non-empty arg -> unknown-command path
- `ArgMode.required` + empty arg -> unknown-command path
- Duplicate registration (canonical name or alias) -> throws

### Help Generation

`helpText()` renders the header plus every command's help lines, sorted stably by `(order asc, registration index asc)`. Registration order is the tiebreak, so two commands with the same `order` keep their registration order.

`formatHelpLine(usage, description)` is the W4 padding formula:

```d
"   " ~ usage.leftJustify(19) ~ (usage.length > 19 ? "  " : "") ~ description
```

Descriptions start at column 22 for usage <= 19 chars; longer usage gets a two-space gap (column 25).

## Built-in Command Groups

| Module | Commands | Orders |
|--------|----------|--------|
| `slash_core.d` | help, quit (+q, exit), stop, compact, debug | 10, 20, 30, 40, 140 |
| `slash_session.d` | sessions, switch, new, rename, delete, clear | 50, 60, 70, 80, 90, 100 |
| `slash_model.d` | model | 110 |
| `slash_pipeline.d` | plan, code | 120, 130 |
| `slash_skills.d` | skills, refresh-agent-md | 150, 160 |

Order is a property of the command, not the file (`/debug` keeps 140 although it lives in the core file). Each group module exposes a package registrar (`registerCoreCommands`, `registerSessionCommands`, ...) invoked by `registerBuiltinCommands` in `slash.d`.

## Plugin API

The public plugin contract consists of: `SlashCommand`, `SlashCommandRegistry`, `SlashArgMode`, `AgentStatus`, `addStartupSlashCommand`, `AgentApp.registerSlashCommand`, `AgentApp.slashCommands()`, and `AgentApp.sendChatMessage`.

### Startup Hook

The bootstrap problem: only `appMain` constructs `AgentApp`, and external modules can never obtain that instance. The hook solves it:

- `slash.d` holds a module-level `private SlashCommand[] startupCommands_` and a public `addStartupSlashCommand(SlashCommand cmd)` that appends to it.
- `AgentApp`'s constructor registers built-ins first, then every startup command.
- Plugin modules append in their module `static this()` constructor. D guarantees module static constructors run before `main`, so ordering is safe.
- The list is a plain array guarded by `shared`/mutex-guarded.

### Instance Registration Seam

Code that holds an `AgentApp` instance may call `app.registerSlashCommand(cmd)` directly (e.g. between construction and `run()` in `appMain`), and may read the live registry via `app.slashCommands()` (e.g. to render help or list command names). Registration is not thread-safe: register before `run()` or via the startup hook.

### Example Plugin

```d
// Any module outside llm.app_agent, e.g. a future subsystem.
// The module must be linked in (imported somewhere or part of the build).
import llm.app_agent;
import llm.app_agent.slash;
import llmfun_tui; // TuiChatMessageType / TuiChatMessageType_Assistant (W12)

static this() { // module constructor: runs before main, before AgentApp exists
    addStartupSlashCommand(SlashCommand(
        "mcp-status", [],
        [SlashCommandRegistry.formatHelpLine("/mcp-status", "Show MCP server status")],
        SlashArgMode.none, 900,
        (ref AgentApp app, string arg) {
            app.sendChatMessage("mcp: status ok", TuiChatMessageType_Assistant);
            return AgentStatus.active;
        }));
}
```

`TuiChatMessageType_*` constants come from `llmfun_tui` (the C++ binding via
ImportC of `source/llmfun_tui.c`); plugins that build messages must
`import llmfun_tui;` (W12). `llm.tui` is the D-side TUI module and does NOT
re-export these constants — importing it alone leaves
`TuiChatMessageType` undefined.

**Test-build warning:** DUB compiles every `source/` module into the test
binary too, so a module constructor like the one above registers the
command under `dub test` as well. That is fine for a real plugin, but keep
test-only registrations inside `unittest` bodies (as `app_agent_plugin_test.d`
does) so they never leak into production or pollute other tests.

## Visibility Rules

| Scope | Members |
|-------|---------|
| Public (plugin contract) | `SlashCommand`, `SlashCommandRegistry`, `SlashArgMode`, `AgentStatus`, `addStartupSlashCommand`, `AgentApp.registerSlashCommand`, `AgentApp.slashCommands()`, `AgentApp.sendChatMessage` |
| Package (`llm.app_agent` only) | The five group registrars, `AgentApp`'s command-facing members (`llmConf`, `rag`, `monitor`, `agent_`, `sessionStore`, `activeSession`, `pendingDeleteId`, `lastServerStat`, `debugMode`, `conf_`, `uiMsg`, `skillManager_`, `slashCommands_`, `doCompress`, `doListSessions`, `doCreateSession`, `doRenameSession`, `doDeleteSession`, `switchToSession`, `commitActiveSession`, `shortSessionId`, `pickFallbackAfterDelete`, `makeStreamCallback`, `makePipelineStreamCallback`, `printHelp`), `PendingDeleteAction`, `decideDeleteCommand` |
| Private | `runAgent`, `run`, `oneShotQuery`, `uiTid`, `dispose`, `processChatMessage`, `processResult`, `activateSession`, `setStatusText`, `progressCallback`, `sendChatThinkMessage`, `updateRagMemory`, `setupSession` |

`package` protection extends to the package tree but not outside it (empirically verified); the promotion table is the single reviewed boundary.

## Cross-Cutting Behavior

### Pending-Delete Rule

The rule "any input not starting with `/delete` clears `pendingDeleteId`" lives in the dispatcher (`runAgent`), not the registry - it applies to bare queries and unknown commands too. A registered `/delete`-prefixed command is left to its handler; unregistered `/delete`-prefixed typos (`/deletefoo`) clear a stale confirmation before dispatch, matching the pre-refactor chain.

### Input Strip Asymmetry

The interactive receive loop strips queries before dispatch; one-shot mode passes the prompt unstripped. The registry applies interactive (stripped) semantics uniformly, with these documented one-shot edge deltas:

- `-p "/plan "` and `-p "/code "`: today run empty pipelines; after the refactor they become "Unknown command" (an improvement).
- `-p "/switch "` / `-p "/rename "`: today produce a usage error message; after the refactor they become "Unknown command" (cosmetic regression, accepted).

### Stop Reachability

The TUI intercepts only the exact raw string `/stop` (no strip before the comparison). Anything else reaches the agent: `/stop ` with a trailing space (interactive) and one-shot `-p "/stop"` both dispatch to the registered `/stop` handler, which prints the neutral message "assistant: Stop requested.".

## Trust Model

Slash-command plugins are trusted in-process code; the API surface, not the registry, is the security boundary. There is no runtime loading and no sandboxing; duplicate registration throws to surface shadowing.

## Testing

### Golden Help Test

`tests.d` embeds the pre-refactor `/help` output as a literal block and asserts `registry.helpText()` is byte-for-byte equal. **Maintenance rule (W11): any change to a command's help line, order, name, or aliases must update the golden block in the same change.** The golden test uses a bare registry (built-ins only) so global startup-hook commands cannot pollute it.

### External-Registration Test

`app_agent_plugin_test.d` lives deliberately outside the `llm.app_agent` package (module `llm.app_agent_plugin_test`) and proves the startup hook works from outside: it registers a uniquely named command via `addStartupSlashCommand`, constructs an `AgentApp`, and asserts the command appears in `commandNames()`, `helpText()`, and dispatches through `execute`. Registration happens inside the unittest body, not a module constructor, so production binaries carry no test command.
