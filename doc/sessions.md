# Chat Sessions in llmfun

llmfun persists chat history as sessions. Each session is one JSON file, and the agent always works in one active session. Multiple histories can coexist, be listed, switched between, renamed, and deleted from the agent prompt via slash commands.

The session layer is deliberately chat-free: it stores and parses JSON only and has no dependency on the `Chat` type. `app_agent.d` bridges the two worlds by feeding session JSON into `agent_.chat.load()` and writing `agent_.chat.toSaveJson()` back through the store.

---

## Table of Contents

- [Overview](#overview)
- [Storage Format](#storage-format)
  - [File Layout](#file-layout)
  - [Header Fields](#header-fields)
  - [Example Session File](#example-session-file)
- [Module Layout](#module-layout)
- [SessionStore API](#sessionstore-api)
- [Session References](#session-references)
- [Strong Typing: SessionId](#strong-typing-sessionid)
- [Agent Integration](#agent-integration)
  - [Startup](#startup)
  - [Commit Points](#commit-points)
  - [Slash Commands](#slash-commands)
  - [Delete Confirmation](#delete-confirmation)
- [Error Handling](#error-handling)
- [Concurrency and Atomicity](#concurrency-and-atomicity)
- [Tests](#tests)

---

## Overview

The core idea is simple: **one JSON file per session, no in-memory cache**.

| Aspect | Decision |
|--------|----------|
| Storage | One JSON file per session in the chat directory (`<scratchArea>/chat/`, i.e. `data/chat/` with the default scratch area) |
| Identity | Session id = filename (immutable). Renaming changes the header title only |
| State | The store is stateless; `AgentApp` holds the active session's `SessionMeta` |
| Format | JSON contract only — the session module knows nothing about `Chat` |
| Active session | Persisted in `state.json` as `activeChatSessionId` and reopened on startup |
| Writes | Atomic (tmp file + rename), single writer (the agent thread) |

---

## Storage Format

### File Layout

```
<scratchArea>/chat/
    20260618-153045-a1b2.json
    20260618-153512-3c4d.json
    ...
```

- The directory is created by the `SessionStore` constructor if missing.
- Each file is named `<id>.json`; the id is the filename without the extension.
- A stale `<id>.json.tmp` file (left by a crash between write and rename) is swept at store construction.

### Header Fields

Every session file is a JSON object:

| Field | Type | Description |
|-------|------|-------------|
| `title` | string | Human-readable title; defaults to the creation date `YYYY-MM-DD` |
| `createdAt` | integer | Unix seconds when the session was created |
| `updatedAt` | integer | Unix seconds of the last save; bumped on every save |
| `messages` | array | Chat messages as produced by `Chat.toSaveJson()` |
| *other keys* | any | Unknown header keys are preserved and round-trip on save (see below) |

`messageCount`, `userMessageCount`, and `preview` are **not stored** — they are computed from `messages` on load/list/save:

- `messageCount` — total entries in `messages`
- `userMessageCount` — entries with `role == "user"`
- `preview` — content of the first user message that has non-empty string content, truncated to 25 characters (`PreviewMaxChars`)

Unknown header keys are preserved in `SessionMeta.extra` and merged back into the file on every save. Future per-session settings (model, temperature, …) can be added without any format migration.

### Example Session File

```json
{
    "title": "2026-06-18",
    "createdAt": 1782304245,
    "updatedAt": 1782304560,
    "messages": [
        {
            "role": "user",
            "content": [
                {"type": "text", "text": "Explain D's const system"}
            ]
        },
        {
            "role": "assistant",
            "content": [
                {"type": "text", "text": "D distinguishes immutable, const, ..."}
            ]
        }
    ]
}
```

---

## Module Layout

The implementation lives in the package `source/llm/session/` (module `llm.session`):

| Module | Purpose |
|--------|---------|
| `session/package.d` | Public re-exports of `types`, `store`, `resolve` |
| `session/types.d` | `SessionMeta`, `SessionFile`, the `SessionId` strong type, id generation and validation, `PreviewMaxChars`/`MaxIdRetries` constants |
| `session/store.d` | `SessionStore`: the persistence layer (create/load/save/list/rename/remove) |
| `session/resolve.d` | `resolveSessionRef`: maps user input to a session id |
| `session/tests.d` | All unit tests (module `llm.session.tests`, following the `tool_call/io/tests.d` pattern) |

`generateId`, `generateDateTitle`, `isValidId`, and the constants are `package`-visible so the tests can reach them without widening the public API.

---

## SessionStore API

The constructor takes a `Path` and resolves it to an `AbsolutePath`, so all file operations are independent of the current working directory. It creates the directory if needed, verifies it is usable (throws otherwise), and sweeps stale `.tmp` files.

| Method | Behavior |
|--------|----------|
| `create()` | Generates a unique id, sets the title to the current date, writes an empty session (`messages: []`) with `createdAt == updatedAt == now`. Returns the new `SessionMeta` |
| `load(id)` | Returns `Optional!SessionFile` (`meta` + full `doc`). Returns none for an invalid id format, missing file, or corrupt JSON (with a warning) |
| `save(id, meta, doc)` | Rebuilds the header from `meta` (title, createdAt, preserved extra keys) plus `updatedAt = now`; copies only `doc["messages"]` (missing → `[]`). Recomputes counts/preview, writes atomically, returns the updated meta. An invalid id returns the meta unchanged with a warning |
| `list()` | Scans `*.json`, skips invalid filenames and corrupt files with warnings (never throws), sorts by `updatedAt` descending (ties broken by id descending so `/sessions` numbering is deterministic) |
| `rename(id, title)` | Rejects empty titles; loads, sets the new title, resaves **preserving** `updatedAt`. Returns `Optional!SessionMeta` (none on empty title, unknown id, or invalid id) |
| `remove(id)` | Silent no-op if the file does not exist; warns on invalid id format |

### ID Generation

Session ids use the format `<YYYYMMDD-HHMMSS>-<4hex>`, e.g. `20260618-153045-a1b2`:

- The timestamp part comes from the local clock; the 4-hex suffix is random.
- Collisions (same second, same suffix) are retried with a fresh suffix, bounded by `MaxIdRetries = 5` attempts total; if all collide the store throws.
- The D12 regex `^\d{8}-\d{6}-[0-9a-f]{4}$` validates the id at **every** store entry point (load/save/rename/remove), which also rules out path traversal through the filename.

---

## Session References

`resolveSessionRef(const SessionMeta[] sessions, string arg)` is a pure function that maps a user argument to a session id. It is shared by the CLI slash commands and any future UI. Precedence:

1. Integer string → 1-based index into the session list (out of range → none)
2. Exact id match
3. Case-insensitive exact title match

Returns `Optional!SessionId` (none when nothing matches).

---

## Strong Typing: SessionId

`SessionId` is a `NamedType!string` (mylib `my.named_type`):

```d
alias SessionId = NamedType!(string, Tag!"SessionId", null,
        Comparable, ForwardStringable, Lengthable);
```

A session id is not interchangeable with titles, previews, or other strings — the compiler rejects accidental mixing. The traits provide `==`/ordering (`Comparable`), `%s` formatting (`ForwardStringable`), and `length`/`empty` (`Lengthable`).

Practical notes:

- The empty value is `SessionId.init` (mylib `NamedType` has no default constructor; `SessionId()` does not compile).
- The string boundary is `state.json`: `LlmConfig.activeChatSessionId` stores the plain string, converted to/from `SessionId` via `.get` and `SessionId(value)` at the config boundary.

---

## Agent Integration

`AgentApp` (module `llm.app_agent`) owns the store and the active session. The store itself is stateless — `activeSession` lives in `AgentApp`, and `save()` returns the updated meta.

### Startup

`setupSession()`:

1. Constructs `SessionStore(<scratchArea>/chat)`.
2. Lists sessions.
3. Resolves the active session: saved `activeChatSessionId` if it still exists → else the most recently updated session → else `create()` a fresh one (at least one session always exists).
4. Loads the active session into `agent_.chat`, resets the response index (preventing replay of old history), and syncs the context size.
5. Persists the active id to `state.json`.

One-shot mode (`-p`) runs the same path — the prompt is appended to the last active session. Only the UI thread differs.

### Commit Points

The in-memory chat is persisted back to the active session file:

- **After every query** — `processResult()` calls `commitActiveSession()`, which converts the chat with `toSaveJson()`, strips `role: "system"` entries from `messages` (the system prompt is re-set at startup, so it is never persisted), and calls `store.save(activeSession.id, activeSession, doc)`.
- **At shutdown** — `dispose()` commits once more as a safety net for error/early-exit paths (a harmless rewrite that bumps `updatedAt` once more), then saves `state.json`.

### Slash Commands

All session commands are handled by reusable private `AgentApp` methods, so a future TUI sidebar can share the same code path:

| Command | Handler | Behavior |
|---------|---------|----------|
| `/sessions` | `doListSessions` | Lists sessions (most recent first): index, `[*]` active marker, short id, title, preview, message count, updated date |
| `/switch <n\|id\|title>` | `switchToSession` | Resolves the argument, commits the current session, activates the target |
| `/new` | `doCreateSession` | Creates a fresh session and switches to it |
| `/rename <title>` | `doRenameSession` | Renames the active session's title (empty title rejected, previous title kept) |
| `/delete <n>` | `doDeleteSession` | Deletes a session; repeats require confirmation (see below) |
| `/clear` | (inline) | Wipes the chat history **inside** the active session and saves |

`switchToSession` is commit + activate: the current session is committed exactly once, then the target is activated. Switching to the already-active session still commits, so pending changes are persisted.

`activateSession` loads the target **first** — if the load fails (missing/corrupt), it reports an error and keeps the current in-memory chat and active id unchanged. On success it clears the chat, loads the session doc, resets the response index, syncs the context, clears the UI chat/pipeline, replays the messages, resends the UI history, and persists the new active id.

Deleting the **active** session skips the pre-switch commit (the deleted file must not be resurrected by the fallback switch). The fallback is the most recently updated remaining session, or a fresh session when none remain.

### Delete Confirmation

`/delete <n>` asks twice before it acts (the index refers to the current `/sessions` order, so it may change between the two invocations):

- First `/delete <n>` → stores the resolved id in `pendingDeleteId` and asks for confirmation.
- Repeating `/delete <n>` for the **same** id → deletes it.
- `/delete <n>` with a **different** id → cancels the pending deletion and asks to start over.
- Any other command clears the pending state.

The decision logic is the pure helper `decideDeleteCommand(pendingId, resolvedId)`, returning `ignore` / `confirm` / `clear`.

---

## Error Handling

The store never throws on bad input; it logs a warning via a `@safe nothrow` `safeWarn` helper (a nested catch keeps logging failures from aborting the operation) and degrades gracefully:

| Condition | Result |
|-----------|--------|
| Invalid id format (load/save/rename/remove) | Warning, none / meta unchanged / no-op |
| Corrupt or missing file on load | Warning (corrupt only), `none` |
| Corrupt or invalid file during list | Skipped with a warning; the rest are still listed |
| Rename with empty title | Warning, `none` — caller keeps the previous title |
| Remove of a non-existent file | Silent no-op |
| All id-generation retries exhausted | Throws (genuine failure: the id space for that second is exhausted) |
| Directory not creatable at construction | Throws |

---

## Concurrency and Atomicity

- **Single writer**: only the agent thread touches the store, so no locking is needed.
- **Atomic writes**: content is written to `<id>.json.tmp` and renamed over `<id>.json` (same pattern as the legacy `Agent.saveHistory`). A crash between write and rename leaves only a `.tmp` file, which is swept on the next store construction.

---

## Tests

Unit tests live in `session/tests.d` (module `llm.session.tests`) and cover:

- create/list/load/save/rename/remove happy paths and round-trips (`save` then `load`)
- Preview extraction and truncation at 25 chars
- `extra` preservation of unknown header keys
- Corrupt JSON, invalid id formats, empty-title rename, remove of missing files
- `resolveSessionRef` precedence (index / id / case-insensitive title) and misses
- List ordering (updatedAt descending, deterministic ties)
- Collision retry during id generation

The agent-side helpers (`decideDeleteCommand`, `pickFallbackAfterDelete`) are unit-tested in `app_agent.d`.

Run them with `dub test`; the session warnings printed during the run are the expected negative-path test output.
