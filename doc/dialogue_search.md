# Searchable Raw Dialogue History

llmfun agents run long conversations. When the context window fills up, the
summary agent compresses the history: older turns are replaced by a short
LLM-generated summary, and the raw messages are discarded. Summaries
inevitably lose exact strings — code snippets, error messages, numbers,
command lines. This feature makes that evicted raw dialogue searchable again:

- **Index at compression time.** Every time a compression evicts raw
  dialogue, the evicted turns are handed to a background indexer that embeds
  them and stores them in a per-session SQLite vector database.
- **Retrieve verbatim.** The agent can call the `queryDialogueHistory` tool
  at any time — full-text, semantic, or combined — to get the exact text of
  old turns, annotated with session id, turn range, and timestamp.
- **No context bloat.** Retrieved chunks enter the context only as tool
  results for the current turn; the active context is never inflated
  automatically. The index lives on disk, outside the context.

A core design rule: **only what is actually compressed gets indexed.** If the
context is small and no compression fires, nothing is indexed — the database
grows only when the active context actually loses content.

This is the first half of a two-part searchable-memory feature: the verbatim
dialogue (this document). The companion half — indexing summarized *reasoning
traces* (why decisions were made, what failed) — reuses the same foundation
(turn stamping, checkpoint events) but is not part of this feature.

---

## Table of Contents

- [Problem and Goals](#problem-and-goals)
- [Architecture](#architecture)
  - [Components](#components)
  - [Threading Model](#threading-model)
- [Data Model](#data-model)
  - [TurnID: the Time Coordinate](#turnid-the-time-coordinate)
  - [Episodes: the Indexing Unit](#episodes-the-indexing-unit)
  - [Per-Session Databases](#per-session-databases)
  - [Embeddings](#embeddings)
- [Control Flow](#control-flow)
  - [Startup Wiring](#startup-wiring)
  - [Indexing at a Compression Checkpoint](#indexing-at-a-compression-checkpoint)
  - [Query Path](#query-path)
  - [Retrieved Content Lifecycle](#retrieved-content-lifecycle)
  - [Shutdown](#shutdown)
- [Tool and Prompt Contract](#tool-and-prompt-contract)
  - [Tool Definition](#tool-definition)
  - [Age Window](#age-window)
  - [Prompt Trigger Rules](#prompt-trigger-rules)
- [Concurrency and Consistency](#concurrency-and-consistency)
- [Failure Modes and Degradation](#failure-modes-and-degradation)
- [Configuration](#configuration)
- [Known Limitations](#known-limitations)

---

## Problem and Goals

Without this feature, a compressed conversation is a lossy one: once a turn
is summarized, the only record of its exact content is the summary. The
agent then has two bad options when the user asks "what was the exact error
message?" — guess, or make the user repeat themselves.

The goals, in priority order:

1. **Verbatim fidelity.** Retrieval returns the original text, not a
   paraphrase.
2. **Temporal precision.** Every result carries the turn range and
   timestamp it came from, so the agent (and the user) can place a quote in
   the conversation.
3. **Zero context cost.** The index must never bloat the active context;
   retrieved content is transient (current turn only).
4. **Asynchrony.** Embedding a compressed range must not stall the agent's
   next turn.
5. **No feedback loops.** The index must never re-index its own output.

## Architecture

```
            agent thread (AgentApp receive loop)
 ┌──────────────────────────────────────────────────────────────────────┐
 │ AgentApp ──owns──► Agent ──owns──► Chat (turn-stamped history)       │
 │   │                      │                              ▲            │
 │   │                      └─owns─► SummaryAgent ─────────┘            │
 │   │                               compress() fires a                │
 │   │                               CompressionCheckpoint             │
 │   │                               (synchronous multicast)           │
 │   │                                      │                         │
 │   │                                      ▼                         │
 │   │                     AgentContext ◄── DialogueIndex.onCheckpoint │
 │   │                     (tool context:      │  [DiJob]  (values)    │
 │   │                      index, session id, ▼                      │
 │   │                      tool limits) ───────────────────────┐      │
 │   └── receive loop: queryDialogueHistory tool dispatch       │      │
 └───────────────────────────────────────────────────────────────┼──────┘
                         query() read-only opens                 │
                         ▼                                       ▼
      <dataDir>/dialogue/<sessionId>.db   ┌──────────────────────────────┐
      (SQLite: chunks + vec0 + FTS5)      │ worker thread (actor)        │
                                          │  own Embedder (HTTP)          │
                                          │  per-session WAL write conns  │
                                          │  mailbox: DiJob / DiDrain     │
                                          │  replies: DiDrained /        │
                                          │          DiDegraded           │
                                          └──────────────────────────────┘
```

### Components

| Component | Module | Responsibility |
|-----------|--------|----------------|
| `Chat` | `llm.chat` | Turn-stamped message history. Every entry carries a typed `turnId` |
| `SummaryAgent` | `llm.summary_agent` | Compression. Emits one `CompressionCheckpoint` per compression that actually evicts content |
| `DialogueIndex` | `llm.rag.dialogue_index` | Agent-thread coordinator: checkpoint listener (filter, group, dispatch), read-only query dispatch with post-filtering, cold-start queries, dispose drain |
| dialogue worker | `llm.rag.dialogue_worker` | Actor thread: owns the indexing embedder and per-session WAL write connections; chunks, embeds, commits, rebuilds FTS |
| `queryDialogueHistory` | `llm.tool_call.dialogue` | Tool surface: parameter validation, session resolution, result rendering |
| `AgentContext` | `llm.agent.context` | Tool context implementing the `DialogueContext` interface (index access, active session id, tool limits) |
| `AgentApp` | `llm.app_agent` | Startup wiring, session-id stamping before compression, degradation handling in the receive loop, shutdown ordering |
| prompt data | `llmfun/config/prompt/AGENT.md` | Trigger rules telling the model when to call the tool |

The per-session databases reuse llmfun's RAG engine (see `doc/database.md`):
the same schema, the same `addToDatabase` chunk-and-embed seam, and the same
triple search modes (semantic, FTS5 full-text, combined) as the knowledge
RAG. A dialogue DB is a regular RAG database whose "topics" are dialogue
episodes.

### Threading Model

Two threads participate: the **agent thread** (the `AgentApp` receive loop —
it owns the chat, the summary agent, and the tool dispatch) and the
**dialogue worker** (spawned at startup, runs for the process lifetime).

- All cross-thread communication is `std.concurrency` **value messages**:
  `DiJob` (a batch of episodes to index), `DiDrain`/`DiDrained` (shutdown
  handshake), `DiDegraded` (one-shot failure signal). There is no shared
  mutable state and no lock.
- The two threads **never share an embedder**. The worker builds and owns its
  own embedder on its thread; queries are embedded by the agent's RAG
  embedder. The agent's embedder is also what supplies the model name and
  vector dimensions when a read-only DB is opened, guaranteeing the reader
  and the writer agree on the vector layout.
- Database handles are thread-owned: the worker keeps per-session write
  connections (WAL mode); the agent opens short-lived read-only connections
  per query and closes them when done.

## Data Model

### TurnID: the Time Coordinate

A **turn** is one user query plus everything produced while answering it
(assistant messages, tool calls, tool results, the final answer).

- Every history entry carries a typed `turnId` (`0` = no turn; the system
  prompt is always `0`). The stamping policy lives in `Chat.add`: a user
  query opens a new turn from a per-chat monotonic counter; every later
  insertion continues the current turn.
- The counter is per-session and never decreases. Its high-water mark is
  persisted in the session file header (`next_turn_id`) and restored on
  load, so turn ids never collide or get reused across process restarts.
  The identity of an indexed record is the composite `(session_id, turn_id)`.
- Turn ids round-trip through the session JSON in `save_data["turn_id"]`, so
  a reloaded session keeps its temporal identity.
- When a compression merges a range of turns into one summary message, that
  message is stamped with the range's end turn and carries
  `summary_turn_start`/`summary_turn_end` markers in its save data. The
  marker is what lets the indexer exclude summaries from indexing (a summary
  is derived from already-indexed content; indexing it would let the
  database amplify itself).

### Episodes: the Indexing Unit

The indexer does not chunk by fixed token lengths. It chunks by **dialogue
episodes**, where one episode is one turn:

- Evicted messages are grouped by turn id; each turn becomes one document
  containing the turn's dialogue text in order: the user query, the
  assistant's final text, and the `taskDone` final answer (if the turn ended
  with one). Thinking text and tool traffic are excluded.
- **What counts as dialogue** (the "facts projection"): user queries,
  assistant messages with non-empty final text, and tool messages carrying a
  `taskDone` final answer. Harness control traffic (system-injected nudges
  that are not user queries) and tool calls/results do not qualify. This
  predicate is shared by the live-history projection and the checkpoint
  filter, so both views of "dialogue" agree.
- Two exclusions are applied before grouping: entries with `turnId == 0`
  (legacy or session-less chats) and merged summary markers.
- A single turn can be split across two consecutive compressions (part of it
  summarized in one, the rest in the next). Both halves arrive under the
  same topic name, and the worker **merges** the new text with the existing
  episode instead of replacing it — a turn never ends up as two episodes.

### Per-Session Databases

- Location: `<dataDir>/dialogue/<sessionId>.db`, one SQLite file per chat
  session (the directory is configurable; see Configuration). The worker
  opens the connection lazily on first use, in WAL journal mode.
- An episode is stored as a RAG *topic source*. The episode's **topic name
  is its metadata carrier**:

  ```
  d_<sessionId with '-'→'_'>__t<turnStart>_<turnEnd>__<epochMillis>
  ```

  Session id, turn range, and creation time are all recoverable by parsing
  the name — no separate metadata column is needed. Queries parse every
  hit's topic name and drop anything unparseable.
- **Chunking.** Oversized episodes are split by the shared `addToDatabase`
  seam into sliding windows (window size taken from the embedder's batch
  size), with a **10% overlap** between adjacent windows so a match at a
  boundary does not split a phrase. The episode's topic name is passed as a
  *dedup salt*: identical text arriving under two different episodes is not
  deduplicated away, and re-adding an unchanged episode is a no-op.
- **FTS5.** The full-text index is an external-content FTS5 table over the
  chunk table, which SQLite does not auto-sync. After every indexing job
  that committed at least one chunk, the worker performs a synchronous FTS5
  rebuild for that session, so committed chunks are never left
  text-invisible (except in the crash window noted in Known Limitations).
- **No TTL.** Old episodes are never purged from storage; staleness is
  controlled at retrieval time with the `maxTurnAge` window (see Age
  Window), which keeps search quality high without data loss.

### Embeddings

Dense embeddings come from a configurable OpenAI-compatible HTTP endpoint
(`embedConfig` in the YAML config). This is a dedicated dense retriever —
the local summary/compression model is deliberately **not** used for
embeddings, because it is optimized for compression, not semantic search.
Two independent embedder instances exist (worker's, for indexing; agent
RAG's, for queries), both built from the same configuration.

## Control Flow

### Startup Wiring

`AgentApp.run` wires the feature before any session is loaded:

1. Create the knowledge RAG (which owns the agent-side embedder).
2. Create the `Agent` (which owns the chat and the summary agent).
3. Create the `DialogueIndex`: it creates the dialogue directory (if needed)
   and **spawns the worker thread**. The worker immediately tries to create
   its own embedder; if that fails it sends a single `DiDegraded` to the
   owner and indexing is disabled for the process lifetime (the worker still
   answers drain messages, so shutdown can never hang).
4. Register the checkpoint listener:
   `agent.addCompressionCheckpointListener(&dialogueIndex.onCheckpoint)` —
   the seam on the summary agent is multicast, so future consumers (e.g.
   the reasoning-trace indexer) can subscribe independently.
5. Expose the index to tools: `agent.toolContext().setDialogueIndex(...)`.
6. Register `scope(exit) dispose()` **before** session setup, so a failure
   in setup still runs the ordered teardown.

One-shot mode (`-p "query"`) runs the identical wiring — indexing works
without a UI thread; only the UI sends are skipped.

### Indexing at a Compression Checkpoint

**Triggers.** Compression fires when context usage passes 90% of the model
window (checked before each LLM request and inside the agent loop), when the
agent proactively calls `requestCompression` with a self-written summary
(which is re-injected after compression so the agent keeps continuity), or
when the user forces it with `/compact`. An 80% usage threshold sends a nudge
encouraging the agent to compress on its own terms first.

**Checkpoint emission** (`SummaryAgent.compress`):

1. Split the history: **Y** = the 5 most recent messages (each oversized
   Y-message above a 4096-token budget is individually summarized; the
   pre-replacement originals are captured as *evicted in place*). **X** =
   older messages, taken newest-first until a 4096-token budget is reached.
   Everything else is *remaining*.
2. Summarize *remaining* (chunked, via the configured summary model) and
   merge the chunk summaries into one replacement message.
3. New history = `[system prompt, merged summary, X..., Y...]`.
4. **Fire exactly one checkpoint if and only if verbatim content was
   actually evicted** (remaining, purged tool traffic, or in-place
   replacements non-empty). The event carries **copied** slices of the
   evicted messages, so consumers may process them asynchronously after the
   live history has moved on. Events include the owning session id, the
   evicted turn range, and size statistics.
5. Listeners are invoked **synchronously** on the thread that is compressing.
   A listener that throws is caught, logged, and skipped — it never breaks
   compression or prevents the remaining listeners from running.

**Checkpoint handling** (`DialogueIndex.onCheckpoint` — fast, no I/O, never
throws):

1. Validate the event's session id. Empty or malformed ids are refused with
   a log line — this is how session-less compressions (e.g. background
   model-pool work) stay out of the index.
2. Apply the facts projection to the evicted slices.
3. Drop `turnId == 0` entries and summary markers.
4. Group the rest by turn id into episodes (first-occurrence order) and
   encode each topic name.
5. Send one `DiJob` to the worker — fire-and-forget. **The agent continues
   its next turn immediately**; the index finishes populating in the
   background seconds later.

**Session stamping.** Before each compression the app stamps the currently
active session id onto the summary agent, so the checkpoint that follows is
attributed to the session whose history was compressed. Callers that do not
know a session leave the id empty, and the indexer refuses it.

**Worker** (per `DiJob`):

1. Validate the session id (defense in depth), open or reuse the session's
   WAL write connection.
2. For each episode: if an episode with the same topic already exists (a
   turn split across two compressions), read its current text and append the
   new pieces — the topic name and turn range stay unchanged.
3. Chunk, embed, and commit through `addToDatabase`. The seam adapts the
   embedding batch size per thread in fixed steps based on success/failure,
   and recursively splits any single chunk that still fails to embed. A
   failed episode is warned and counted; the job continues.
4. If at least one chunk was committed, rebuild the session's FTS5 index.
5. Emit exactly one trace line for the job (episode/chunk/failure counts and
   turn range). Episode text is never logged.

**Durability.** Episodes are durable on disk as soon as committed. The DBs
survive restarts, and cold-start queries work: "newest indexed turn" and
"has any history" are computed by reading the DB, not from memory.

### Query Path

The agent calls `queryDialogueHistory` (see Tool Definition). The flow:

1. **Validate parameters before touching state:** at least one of
   `textQuery`/`vectorQuery`; `topK` within `[1, maxTopK]`; the resolved
   session id (explicit `sessionId`, else the active session) must match the
   session-id grammar — invalid ids are rejected before any file path is
   constructed.
2. **Dispatch** (`DialogueIndex.query`, read-only):
   - Open `<dialogueDir>/<sessionId>.db` read-only, using the agent embedder's
     model name and dimensions (they must match what the worker wrote; a DB
     created with a different model simply cannot be opened and surfaces as
     a graceful "no history" result).
   - Embed `vectorQuery` with the agent's embedder. If embedding fails,
     fall back to text-only when `textQuery` is present; otherwise return an
     error result.
   - Run the appropriate search — combined semantic + FTS5, FTS5-only, or
     semantic-only — with a **candidate headroom** (10x topK, minimum 100):
     the DB over-fetches because post-filtering can drop candidates, and the
     final `topK` cap is applied *after* filtering, not by the SQL LIMIT
     alone.
3. **Post-filter:** parse each hit's topic name into episode metadata (drop
   unparseable names); apply the `maxTurnAge` window (drop episodes older
   than N turns behind the newest indexed turn); cap at `topK`.
4. **Render:** each match is rendered as

   ```
   --- Match 1 (session: 20260618-153045-a1b2, turns 42-42, 2026-06-18 15:34:02, rank: 0.812) ---
   <verbatim chunk text>
   ```

   and returned as the tool result.

**Result contract.** Messages prefixed with `error:` map to a failed tool
call (`success: false`). The "no history yet" and "no matches found"
notices are *successful* results (`success: true`) — they are authoritative
answers, and the prompt rules forbid the agent from retrying them with
paraphrases or guessing.

### Retrieved Content Lifecycle

Retrieved chunks enter the context as an ordinary tool response for the
current turn, and that is their whole lifecycle. Two mechanisms prevent a
feedback loop in which the index grows by re-indexing its own output:

- **Tool responses are not dialogue.** The facts projection excludes tool
  traffic, so if the turn carrying the retrieval is later compressed, the
  retrieved text is *not* re-indexed.
- **Summaries are markers, not content.** The merged compression summary of
  a range is an LLM distillation, it carries the summary-marker save data,
  and the indexer explicitly excludes summary markers — retrieved strings do
  not accumulate verbatim in the long-term summary either.

### Shutdown

`AgentApp.dispose()` runs (via `scope(exit)`) on every exit path:

1. **Drain the dialogue worker before anything else that matters:** send
   `DiDrain`, wait up to 5 seconds for `DiDrained`. The worker first
   processes its whole queue, then checkpoints (TRUNCATE) and closes all WAL
   connections, leaving clean, sidecar-free DBs for the next process's
   read-only opens. The drain is **idempotent** and never throws, and it
   happens **before `rag.destroy`** — so no in-flight indexing job can
   embed against weights that are being destroyed.
2. Terminate the UI thread and destroy the RAG.
3. Commit the active session file (dirty-gated), sweep empty sessions, save
   `state.json`.

An abnormal kill (crash, `SIGKILL`) skips all of this: committed episodes
survive (SQLite WAL recovers on the next open), but queued jobs are lost —
a documented limitation.

## Tool and Prompt Contract

### Tool Definition

`queryDialogueHistory` — *"Retrieve verbatim historical dialogue from
compressed turns. Use this only when the user asks for exact quotes,
specific numbers, error messages, code, or command-line inputs that are
missing from the compressed summary. Pass the user's EXACT nouns and
entities in the query; do not paraphrase. … Returns matching episodes with
the session id, turn ranges, timestamp, and the matched verbatim text."*

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `textQuery` | `""` | Bare keywords for full-text search, as written in the conversation |
| `vectorQuery` | `""` | Natural-language description, matched semantically (for paraphrases/concepts) |
| `topK` | `5` | Maximum matches; hard-capped by `toolLimits.maxTopK` (default 20) |
| `maxTurnAge` | `0` | `0`/negative = no age filtering; positive N keeps only matches within N turns of the newest indexed turn |
| `sessionId` | active session | Search another session's history (format `YYYYMMDD-HHMMSS-4hex`) |

### Age Window

`maxTurnAge` is the staleness control: matches are kept only when
`turnEnd >= maxTurn - N`, where `maxTurn` is the highest turn end indexed in
that session's DB (computed from the DB, so it is correct after restarts).
The default (0) searches the whole indexed history — the query's own
keywords/semantics are expected to do the narrowing, and the window exists
for "the last 20 turns or so" style questions, where searching 500 turns
back would mostly waste tokens.

### Prompt Trigger Rules

The main agent's prompt (`llmfun/config/prompt/AGENT.md`) carries a
*Dialogue History Retrieval* section:

- When the user refers to an exact string that is missing from the
  compressed summary (a quote, number, error message, code, or command-line
  input), call `queryDialogueHistory` with the user's EXACT nouns:
  space-separated exact terms in `textQuery`, a natural-language description
  in `vectorQuery` for paraphrases, or both — at least one non-empty.
- `sessionId` defaults to the active session; `maxTurnAge` 0/negative means
  no age filtering.
- "No matches found" and "No dialogue history indexed" are authoritative:
  do not retry with paraphrases, and never guess or invent exact strings.

The prompt is data, not code, so a guard test in `llm.agent` loads the real
prompt through the production composition path and asserts both the section
heading and the tool name are present — removing the rule from the prompt
file is caught by the unit tests instead of silently changing agent
behavior.

## Concurrency and Consistency

- **No shared state, no locks.** Agent ↔ worker talk only via value
  messages. The worker's mailbox is unbounded (no backpressure — see Known
  Limitations).
- **Single writer per DB.** Only the worker writes a session DB (WAL). The
  agent opens read-only connections per query and closes them after use.
  WAL mode lets the reader see committed episodes while the writer works —
  no reader/writer lock and no hand-off of DB handles between threads.
- **Listener contract.** Checkpoint listeners run synchronously on the
  compressing thread and must be quick and non-blocking (the dialogue
  listener does filtering, grouping, and one message send — no I/O).
  Throwing listeners are isolated by the multicast.
- **Embedder isolation + teardown order.** Each thread owns its embedder;
  the dispose order (drain worker → destroy RAG) guarantees an in-flight
  indexing job never races destruction of the agent-side embedder.
- **Session switching is safe.** Queries resolve a session id and open the
  DB per call, so switching sessions mid-conversation cannot leave a stale
  handle. Indexing always targets the session stamped on the checkpoint.
- **Turn ordering stays consistent.** Compression preserves the
  (turn id, position) ordering of history and stamps the merged summary
  with the summarized range's end turn, so the DB's turn metadata and the
  live history stay consistent across repeated compressions and restarts.

## Failure Modes and Degradation

| Failure | Behavior |
|---------|----------|
| Worker cannot create its embedder (bad config, no network) | One `DiDegraded` → warning on the agent side. Indexing disabled for the process lifetime (documented limitation: no recovery). Existing history stays queryable; shutdown cannot hang. |
| One episode fails to index | Warning; job continues; counted in the job's trace line. |
| FTS5 rebuild fails | Warning; chunks are still committed — vector search works, text search misses the new chunks until the next job's rebuild. |
| Vector-query embedding fails at query time | Fall back to text-only when `textQuery` is present; otherwise an `error:` result (failed tool call). |
| Session DB missing, empty, or corrupt | Graceful "No dialogue history indexed for this session yet." (successful result). |
| Invalid session id (tool param or checkpoint) | Rejected before any file path is built (grammar-validated id type); logged, error result / skip. Defense in depth: validated at the tool level *and* the index level. |
| Checkpoint with empty session id (session-less compression) | Logged and refused; compression itself is unaffected. |
| A checkpoint listener throws | Caught by the multicast; other listeners and the compression proceed. |
| Crash between chunk commit and FTS rebuild | Text search misses those chunks until the next rebuild; vector search is unaffected. |
| Abnormal termination with queued jobs | Queued/in-flight episodes are lost (no job journaling); committed episodes are durable. |

The overall posture: **indexing failures degrade to "nothing indexed",
query failures degrade to "no history" or a clean tool error — the agent
loop never crashes or blocks because of this feature.**

## Configuration

| Key | Default | Role |
|-----|---------|------|
| `dialogueDir` | `<dataDir>/dialogue` | Directory holding the per-session dialogue DBs |
| `embedConfig` (server URL, model, dimensions, …) | — | Embedding endpoint; used by both the worker (indexing) and the agent (queries) |
| `toolLimits.maxTopK` | `20` | Hard cap for the tool's `topK` parameter |

The dialogue databases run with a fixed RAG config: **10% chunk-window
overlap** (knowledge RAG defaults to 50%) — episodes should overlap just
enough to keep boundary phrases intact, not so much that they duplicate.

The feature requires a configured summary model (to compress in the first
place) and a working embedder (to index). With either missing it simply
stays dormant or degrades, per the table above.

## Known Limitations

- **In-flight loss.** Episodes queued or being embedded when the process
  dies abnormally are lost; there is no job journaling.
- **Unbounded mailbox.** The worker has no backpressure against the agent;
  a long embedding outage lets jobs pile up in memory.
- **No TTL in storage.** Old episodes are never deleted; the `maxTurnAge`
  retrieval window bounds staleness at query time instead.
- **No embedder recovery.** If the worker's embedder fails at startup,
  indexing stays off for the whole process (a restart recovers it).
- **Timestamp precision.** The epoch millis in a topic name is the
  checkpoint-handling time, not the original eviction time; the difference
  is microseconds and was intentionally left as-is.
