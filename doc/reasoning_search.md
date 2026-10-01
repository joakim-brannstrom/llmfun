# Searchable Reasoning History

llmfun agents think in long conversations. When the context window fills up,
the summary agent compresses the transcript, and the reasoning that produced
a decision — the thinking blocks, the tool calls, and the tool responses
behind it — is discarded. What survives is a dialogue summary that records
*what* happened, not *why*: which approaches were tried and abandoned, which
choices were binding, what was still open.

This feature makes that evicted reasoning searchable again:

- **Index at compression time.** Every time a compression actually evicts
  reasoning, the evicted trace is projected, budget-capped, summarized by the
  summary model into a structured four-section record, embedded, and stored
  in the per-session SQLite database under the `r_` topic kind.
- **Retrieve on demand.** The agent can call the `queryReasoningHistory` tool
  — full-text, semantic, or combined — for "why did we do X?" questions and
  when it suspects it is stuck repeating an abandoned approach.
- **Advisory, never authoritative.** A record is a summary of the agent's own
  past thinking at a compression point; every match is returned with an
  anti-anchoring annotation, and the prompt rules state plainly that a past
  thought is not ground truth.
- **No context bloat.** Retrieved records enter the context only as tool
  result for the current turn; the active context is never inflated
  automatically. The index lives on disk, outside the context.

The same core design rule as the dialogue half applies: **only reasoning that
actually leaves the context is indexed.** If nothing is evicted, no record is
written.

This is the second half of the two-part searchable-memory feature; the first
half — verbatim raw dialogue — is documented in `doc/dialogue_search.md`.
Both kinds share one database per session and one indexing worker; they are
kept apart by a topic-name codec and per-kind post-filters.

---

## Table of Contents

- [Problem and Goals](#problem-and-goals)
- [Architecture](#architecture)
  - [Components](#components)
  - [Threading Model](#threading-model)
- [Data Model](#data-model)
  - [Records: the Indexing Unit](#records-the-indexing-unit)
  - [Per-Session Databases](#per-session-databases)
  - [Kind Codec](#kind-codec)
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

Without this feature, a compressed conversation loses its reasoning: once a
trace is evicted, the only record of why an approach was chosen or abandoned
is the dialogue summary, which is written for continuity, not for decision
archaeology. The agent then re-tries dead ends, forgets which alternatives
were rejected, and cannot answer "why did we do X?" after a compression.

The goals, in priority order:

1. **Recall abandoned rationale.** Reconstruct why a decision was made, which
   alternatives were rejected and why, and what was still uncertain, from
   compact records.
2. **Stop re-trying abandoned approaches.** A stuck loop is often the model
   repeating its own failed path; the records make that path visible again,
   and the anti-anchoring rules forbid repeating it without new evidence.
3. **Cross-compression continuity.** "Why did we do X?" stays answerable
   after the trace that contained X has left the context.
4. **Cost control.** One extra summarizer round-trip per evicting checkpoint,
   bounded by a budget derived from the summary model's own context; a
   degraded embedder skips the job entirely (no LLM spend); no new tool-limit
   entries and no new environment variables.
5. **Advisory-only stance.** Past thought is not ground truth. Records steer
   the agent; they never override the user's latest instruction or verbatim
   facts, and they are never presented as a new justification.

## Architecture

```
            agent thread (AgentApp receive loop)
 ┌──────────────────────────────────────────────────────────────────────┐
 │ AgentApp ──owns──► Agent ──owns──► Chat (turn-stamped history)       │
 │   │                      │                              ▲            │
 │   │                      └─owns─► SummaryAgent ─────────┘            │
 │   │                               compress() fires a                 │
 │   │                               CompressionCheckpoint              │
 │   │                               (synchronous multicast)            │
 │   │                                      │                           │
 │   │                                      ▼                           │
 │   │                    AgentContext ◄── ReasoningIndex.onCheckpoint  │
 │   │                     (tool context:      │  [RiJob] (trace text)  │
 │   │                      index, session id, ▼                        │
 │   │                      tool limits) ───────────────────────┐       │
 │   └── receive loop: queryReasoningHistory tool dispatch      │       │
 └───────────────────────────────────────────────────────────────┼──────┘
                         query() read-only opens                 │
                         ▼                                       ▼
     <dataDir>/dialogue/<sessionId>.db   ┌──────────────────────────────┐
     (SQLite: chunks + vec0 + FTS5)      │ worker thread (actor)        │
                                         │  own Embedder (HTTP)         │
                                         │  per-session WAL write conns │
                                         │  mailbox: DiJob / RiJob /    │
                                         │    RiRecord / RiDone /       │
                                         │    DiDrain                   │
                                         │  per RiJob: a short-lived    │
                                         │  summarizer thread makes     │
                                         │  the LLM call OFF the        │
                                         │  mailbox (dedicated timeout) │
                                         └──────────────────────────────┘
```

### Components

| Component | Module | Responsibility |
|-----------|--------|----------------|
| `Chat` | `llm.chat` | Turn-stamped message history. The reasoning projection `traceOf` is a free `@safe nothrow`, Chat-free function here (mirrors `dialogueOf`; extracted from `getReasoningTrace`) |
| `SummaryAgent` | `llm.summary_agent` | Compression. Emits one `CompressionCheckpoint` per compression that evicts content — the dialogue-index seam, unchanged except that `stripFences` was made public for the worker |
| `ReasoningIndex` | `llm.rag.reasoning_index` | Agent-thread coordinator: checkpoint listener (project, filter, cap, budget, dispatch), read-only `r_`-only query dispatch with post-filtering, effective context cached at startup. Stateless — no dispose |
| dialogue worker | `llm.rag.dialogue_worker` | Shared actor thread: `RiJob` handler, per-job spawned summarizer thread (LLM call off the mailbox), `RiRecord`/`RiDone` completion handling, bounded-join drain |
| `queryReasoningHistory` | `llm.tool_call.reasoning` | Tool surface: parameter validation, session resolution, result rendering with the anti-anchoring annotation |
| `AgentContext` | `llm.agent.context` | Tool context implementing the `ReasoningContext` interface (index access, active session id, tool limits) |
| `AgentApp` | `llm.app_agent` | Startup wiring (prompt load, extended `DialogueIndex` ctor, second checkpoint listener, tool context), shutdown drain |
| prompt data | `llmfun/config/prompt/AGENT.md`, `llmfun/config/prompt/REASONING_SUMMARY.md` | Trigger + anti-anchoring rules for the main agent; the structured-record prompt for the summary model |

Reasoning records reuse llmfun's RAG engine (see `doc/database.md`) and the
same per-session databases as the dialogue episodes (see
`doc/dialogue_search.md`): a record is a RAG topic source, with the same
chunk-and-embed seam and the same triple search modes (semantic, FTS5
full-text, combined).

### Threading Model

Three kinds of threads participate: the **agent thread** (the `AgentApp`
receive loop — it owns the chat, the summary agent, and tool dispatch), the
**shared dialogue worker** (spawned once at startup, lives for the process
lifetime), and **per-job summarizer threads** (short-lived, one per `RiJob`,
spawned by the worker).

- All cross-thread communication is `std.concurrency` **value messages**:
  the dialogue-index set (`DiJob`, `DiDrain`/`DiDrained`, `DiDegraded`) plus
  `RiJob` (a pre-formatted, pre-budgeted trace), and the `RiRecord`/`RiDone`
  completions. There is no shared mutable state and no lock.
- The **checkpoint listener runs synchronously on the thread that is
  compressing** (the agent thread), per the dialogue-index listener contract. It
  does pure CPU work only — projection, filters, per-entry caps, budget — and
  ends with one `send` of an `RiJob`. No I/O, no LLM call; O(n) string work
  with hard per-entry caps.
- Each `RiJob` **spawns a short-lived thread OFF the worker's mailbox**; the
  mailbox thread only validates, encodes the topic name, and spawns
  (microseconds). The worker posts **exactly one completion** — `RiRecord`
  or `RiDone` — per spawned thread, and tracks in-flight threads with a
  worker-local counter.
- **Drain = bounded join.** On `DiDrain` the worker flushes its mailbox,
  consumes completions until no summarization thread is outstanding (or the
  join deadline expires), then checkpoints and closes the DBs and answers
  `DiDrained`. The worker **survives the drain**: the actor loop keeps
  receiving afterwards, and closed DB handles are reopened lazily by
  `ensureDb` on the next job (`rag/dialogue_worker.d:371-398`, `:198-222`).
- The two threads never share an embedder. The worker owns its indexing
  embedder; queries are embedded by the agent's RAG embedder. Database
  handles are thread-owned: the worker keeps per-session WAL write
  connections, the agent opens short-lived read-only connections per query.

## Data Model

### Records: the Indexing Unit

The indexer does not store raw traces. It stores **records**, and the unit is
the compression checkpoint, not the turn: one structured summary per
checkpoint that evicted reasoning.

- **Four sections.** The summarizer prompt demands exactly: *Abandoned
  Hypotheses* (`<approach tried> — <why it failed>`), *Binding Decisions*
  (`<irreversible choice> — <why>`), *Current Uncertainties* (`<open
  question>`), and *Justification* (one sentence per turn, covering only the
  three most recent turns present in the input). No narrative, no restating
  user queries, `None` for an empty section, ~150 words. The section
  structure is a prompt-level contract for the model — the database stores
  the response text **verbatim** (fence-stripped), with no parsing or
  re-rendering.
- **Identity and turn range.** Each record's topic name carries its
  metadata:

  ```
  r_<sessionId with '-'→'_'>__t<turnStart>_<turnEnd>__<epochMillis>
  ```

  `turnStart`/`turnEnd` are the min/max turn id over the trace lines that
  survived the budget trim. Session id and creation time are recoverable by
  parsing the name; queries parse every hit's topic name and drop anything
  unparseable.
- **One call, one record.** The trace is summarized in a single summarizer
  call (no chunk chaining); the expected output is 100–300 tokens, hard-capped
  at 512 by the request's `max_tokens`. An empty or failed response produces
  no record at all.
- **Records are per checkpoint, not per turn.** Unlike dialogue episodes
  (which merge a turn split across two compressions), a second checkpoint
  that touches the same turn simply produces a second record with a fresh
  topic name — consecutive records can overlap.

### Per-Session Databases

- **Location:** `<dataDir>/dialogue/<sessionId>.db` — one SQLite file per
  chat session, **shared by both kinds** (the directory is configurable; see
  Configuration). The worker opens the connection lazily on first use, in WAL
  journal mode.
- **Chunking:** oversized record text is split by the shared `addToDatabase`
  seam into sliding windows (window size taken from the embedder's batch
  size) with the same **10% overlap** as dialogue episodes. The topic name is
  passed as a *dedup salt*: identical text arriving under two different
  checkpoints is not deduplicated away, and re-adding an unchanged record is
  a no-op.
- **FTS5:** the full-text index is an external-content FTS5 table over the
  chunk table, which SQLite does not auto-sync. After every record that
  committed at least one chunk, the worker performs a synchronous FTS5
  rebuild for that session, so committed chunks are never left
  text-invisible (except in the crash window noted in Known Limitations).
- **Durability:** records are durable on disk as soon as committed. The DBs
  survive restarts, and cold-start queries work — "has any reasoning
  history" and the `maxTurnAge` window are computed by reading the DB, not
  from memory.
- **No TTL:** records are never purged from storage; staleness is controlled
  at retrieval time with the `maxTurnAge` window (see Age Window).

### Kind Codec

Two topic kinds share the topic namespace:

- `d_` — a dialogue episode (verbatim evicted dialogue).
- `r_` — a reasoning record (this feature).

`encodeTopicName(sessionId, turnStart, turnEnd, epochMillis, kind)` emits
`d_` by default (byte-identical to the dialogue encoder) and `r_` for
`Kind.reasoning`; `decodeTopicName` recovers session id, turn range, epoch,
and kind, and returns "none" for anything with an unknown prefix (such
topics are dropped by every post-filter) — see `rag/dialogue_index.d:110-169`.

Two mechanisms keep the kinds apart at query time:

- **Kind-drop post-filter in both indexes.** A dialogue query drops `r_`
  candidates (`rag/dialogue_index.d:464-465`); a reasoning query drops `d_`
  candidates (`rag/reasoning_index.d:374-375`). This is applied on every
  search path — full-text, semantic, and combined.
- **Kind-scoped history detection.** The "no history" decision counts only
  decodable topics of the queried kind, so a session whose DB holds only
  dialogue episodes reports "no reasoning history" (and vice versa) even
  though the file is shared.

The single-database choice was an open question from the dialogue feature ("is one DB per
session enough for two kinds?"). It is **closed as adequate for the reasoning
feature**: the cross-kind drop is exercised in both query paths (full-text and
semantic) by the integration suite, and sharing the DB also shares the WAL
discipline, the single writer, and the drain. A separate reasoning DB would
add a second writer/lifecycle for no measured benefit; escalate only if
scale data ever shows otherwise.

### Embeddings

Dense embeddings for records use the same stack as dialogue, and
deliberately **not** the summary model:

- At index time, the worker embeds each chunk with **its own embedder**,
  built from the `embedConfig` endpoint (the same embedder that indexes
  dialogue episodes). The model name and vector dimensions are stored in the
  DB header.
- At query time, the agent's RAG embedder embeds `vectorQuery` and supplies
  the model name/dimensions for the read-only DB open, so reader and writer
  always agree on the vector layout. A DB written with a different model
  simply cannot be opened and surfaces as a graceful "no history" result.
- The summarization model (the local summary model configured in
  `summaryModel`) writes the record *text*; it is never used for embeddings.

## Control Flow

### Startup Wiring

`AgentApp.run` wires the feature before any session is loaded, in a fixed
order (`app_agent/package.d:818-850`):

1. **Load the reasoning prompt:**
   `reasoningPrompt = loadReasoningPrompt(llmConf)`. This resolves the
   configured file against `promptDir` and falls back to the built-in
   default with a warning when the file is missing or unreadable. It must
   happen **before** step 2: the `DialogueIndex` constructor spawns the
   worker, and the worker needs the prompt as a spawn argument.
2. **Create the `DialogueIndex`** with the extended constructor
   (dialogue dir, `embedConfig`, the dialogue RAG config, a null
   embedder factory, `llmConf.summaryModel`, the reasoning prompt, and a
   null summarizer). The constructor spawns the **shared worker** — now
   reasoning-aware — and exposes `workerTid` for the reasoning index. Register
   the dialogue checkpoint listener and expose the index to tools
   (`agent.toolContext().setDialogueIndex(...)`).
3. **Create the `ReasoningIndex`** with the dialogue dir, the summary model
   config, and `dialogueIndex.workerTid`. The constructor resolves and
   caches the effective context (see below) — once, at startup.
4. **Register the reasoning checkpoint listener:**
   `agent.addCompressionCheckpointListener(&reasoningIndex.onCheckpoint)` —
   the second listener on the same synchronous multicast, so both consumers
   see every checkpoint independently. (The dialogue listener was registered
   first, which keeps dialogue jobs first in the worker mailbox — mailbox
   priority for user-facing dialogue indexing.)
5. **Expose it to tools:** `agent.toolContext().setReasoningIndex(...)`.
6. **Shutdown:** `AgentApp.dispose` drains the shared worker through
   `dialogueIndex.dispose()` before `rag.destroy()`; `ReasoningIndex` is
   stateless and has nothing to dispose. (The design draft listed a
   `setReasoningIndex(null)` teardown step; the build needs none — there is
   no per-index lifecycle to unwind, and the process tears down immediately
   after the drain. The dispose comment in `app_agent/package.d:109-118`
   records this.)

One-shot mode (`-p "query"`) runs the identical wiring — indexing works
without a UI thread; only the UI sends are skipped.

### Indexing at a Compression Checkpoint

**Triggers.** Compression triggers are unchanged from the dialogue index — context
usage over 90% (checked before each LLM request and inside the agent loop),
an agent-initiated `requestCompression`, or the user's `/compact` (with the
80% nudge encouraging proactive compression). The reasoning listener rides
the same multicast checkpoint as the dialogue listener; a checkpoint is
fired only when content was actually evicted. Events carry **copied** slices
of the evicted messages, so the listener may process them asynchronously
after the live history has moved on.

**Checkpoint handling** (`ReasoningIndex.onCheckpoint` — fast, no I/O,
never throws; `rag/reasoning_index.d:134-205`):

1. Validate the event's session id. Empty or malformed ids are refused with
   a log line — session-less compressions (e.g. background model-pool work)
   stay out of the index.
2. If the effective context is not larger than the answer reserve
   (`≤ 8192`), skip loudly: no record can fit one summarizer call, so no
   job is sent.
3. **Project** the evicted slices: `traceOf(evictedSummarized ~
   evictedInPlace)` plus the `evictedPurged` slice rendered the same way
   (always empty today; correct by construction if a tool filter ever
   populates it). `traceOf` keeps thinking-bearing messages, non-final tool
   call messages, and every tool response; final messages and harness
   control traffic are excluded.
4. **Filter:** drop entries with `turnId == 0` (legacy or session-less
   chats), merged summary markers, and — the **retrieval-artifact guard** —
   `ToolResponse`s whose tool name is `queryDialogueHistory` or
   `queryReasoningHistory`, by exact name. If nothing remains, no record is
   created.
5. **Render** one line per entry with hard per-entry caps: thinking ≤ 2000
   characters, tool-call summary ≤ 500, tool response pretty-print ≤ 1000.
   This is a dedicated renderer for the trace shapes (the dialogue
   summary's `formatMessagesToText` is deliberately not reused).
6. **Budget:** `(effectiveContext − 8192) × ApproxTokenSize` characters,
   where 8192 is the summary agent's answer reserve. When the trace is over
   budget, lines are dropped **oldest-first** until it fits, and a marker
   line `[N older trace entries omitted for size]` is prepended; a single
   surviving line is hard-capped to the budget. The record's turn range is
   the min/max turn id over the included lines.
7. **Dispatch:** send exactly one `RiJob(sessionId, traceText, turnStart,
   turnEnd)` — fire-and-forget. The agent continues its next turn
   immediately; the record finishes in the background seconds later.
   Observability: counts, lengths, turn range, and topic names only — never
   trace or record content.

**Budget and its effective-context source.** The effective context is
`min(getContextSize(summaryCfg), summaryCfg.contextChunkSize)`, computed
**once** in the `ReasoningIndex` constructor and cached for the process
lifetime — never re-resolved per checkpoint (`rag/reasoning_index.d:127-129`).
`getContextSize` performs a **live llama.cpp slot query** when the summary
endpoint is a `llamaCpp` server, returning the model's actual context; for
other endpoint types it returns the configured `contextSize`, and a failed
slot query falls back to that configured value (`endpoint.d:7-18`). This is
**exactly the value `SummaryAgent` computes for its own chunking**
(`summary_agent.d:71`), so the trace can never be bigger than what the
summarizer model itself accepts. A change to the served slot size therefore
takes effect on the next process start, not mid-run.

**Worker** (per `RiJob`; `rag/dialogue_worker.d:305-332`):

1. If the worker is degraded (no embedder) or the session DB cannot be
   opened/created, the job is dropped here — **no LLM spend, no spawned
   thread**. The guard is shared with the dialogue path.
2. Encode the `r_` topic name (turn range from the job, epoch millis from
   the worker's clock at receipt), increment the outstanding-thread counter,
   **spawn** a summarizer thread, and return. A failed spawn rolls the
   counter back and logs, so a later drain never waits for a completion that
   cannot arrive.
3. **Why the call is off the mailbox.** The summarization LLM call must not
   run on the mailbox thread: a mailbox-synchronous call would couple every
   dialogue-indexing job queued behind it to the *worst case* of the
   summarizer call — and the compression summary's own timeout chain is
   exactly that worst case: `server.timeoutSeconds × (1+maxRetries) +
   backoff` (minutes to hours, and **unbounded if `timeoutSeconds` is
   unset**). The spawned thread uses a dedicated budget instead:
   `timeoutS = 300` per attempt with `maxRetries = 1` (the design's
   "300 s × 2" worst case — at most two attempts, 300 s × 2 + backoff
   ≈ 10 minutes) and `max_tokens = 512` (`rag/dialogue_worker.d:139-140`).
   Nothing from the compression summary's timeout or token settings is
   inherited. A slow or hung local model therefore delays only its own
   record; the mailbox (and dialogue indexing) keeps flowing.

   The summarizer thread (`rag/dialogue_worker.d:459-502`) makes exactly one
   chat call: system prompt = the loaded reasoning prompt, user message =
   the trace text, no tools, non-streaming. Tests inject a `SummarizerFn`
   fake in place of the real `LlmRequester` call; the thread is spawned
   either way.

4. On success the response is passed through `stripFences` (the same helper
   the dialogue summaries use) and sent back as `RiRecord {topicName,
   recordText}` — the record text is the fence-stripped model response,
   **verbatim**. On failure or an empty response, `RiDone {topicName,
   reason}` carries a short reason code (`llm_error` / `empty`).

**Completion handling** (worker mailbox; `rag/dialogue_worker.d:338-365`):

- `RiRecord` → index the record text under its topic (chunk, embed, commit
  through `addToDatabase`, dedup salt = topic name), then rebuild FTS5 when
  at least one chunk was committed. Fast — no LLM work.
- `RiDone` → log the skip (reason truncated) and index nothing.
- Both decrement the outstanding-thread counter (first thing, so a
  concurrent drain join observes it even if the indexing below fails).

### Query Path

The agent calls `queryReasoningHistory` (see Tool Definition). The flow
mirrors `DialogueIndex.query` with `r_`-only semantics
(`rag/reasoning_index.d:271-391`):

1. **Validate parameters before touching state:** at least one of
   `textQuery`/`vectorQuery`; `topK` within `[1, maxTopK]`; the resolved
   session id (explicit `sessionId`, else the active session) must match the
   session-id grammar — invalid ids are rejected before any file path is
   constructed.
2. **Dispatch** (`ReasoningIndex.query`, read-only):
   - Open `<dialogueDir>/<sessionId>.db` read-only, using the agent
     embedder's model name and dimensions.
   - Embed `vectorQuery` with the agent's embedder. If embedding fails,
     fall back to text-only when `textQuery` is present; otherwise return an
     error result.
   - Run the appropriate search — combined semantic + FTS5, FTS5-only, or
     semantic-only — with a **candidate headroom** (10x topK, minimum 100):
     the DB over-fetches because post-filtering can drop candidates, and the
     final `topK` cap is applied *after* filtering, not by the SQL LIMIT
     alone.
3. **Post-filter:** parse each hit's topic name into record metadata (drop
   unparseable names); drop every non-reasoning candidate (kind drop); apply
   the `maxTurnAge` window; cap at `topK`.
4. **No-history detection:** the query counts decodable `r_` topics in the
   session DB first; zero → the graceful notice `No reasoning history
   indexed for this session yet.` (a successful result). This is
   kind-scoped: a session with only dialogue history reports no reasoning
   history.
5. **Render:** each match is rendered as a header, the record body, and the
   anti-anchoring annotation (see Tool and Prompt Contract), and returned as
   the tool result.

**Result contract.** Messages prefixed with `error:` map to a failed tool
call (`success: false`). The "no history yet" and "no matches found" notices
are *successful* results (`success: true`) and are authoritative answers, not
errors. (The explicit "do not retry with paraphrases, never guess" wording
lives in the dialogue prompt section — the reasoning section carries no such
clause.)

### Retrieved Content Lifecycle

A retrieved record enters the context as an ordinary tool response for the
current turn, and that is its whole lifecycle — the database is never
auto-injected into the conversation. Two mechanisms close the feedback loop
in which the index could grow by re-summarizing its own output:

- **Retrieval responses are not trace material.** The trace filter drops
  `ToolResponse`s from `queryDialogueHistory` and `queryReasoningHistory` by
  exact tool name, so a record that was retrieved (and the dialogue quotes
  retrieved beside it) can never become input to a future reasoning record.
  This is the reasoning analogue of the dialogue index's no-feedback-loop
  exclusion.
- **Summaries are markers, not content.** A merged compression summary
  carries the summary-marker save data and is excluded by the trace filter,
  so text distilled into a dialogue summary cannot re-enter the reasoning
  pipeline either.

Together, a retrieved string can appear in the live context, be compressed
away, and be found again by a later query — but it never *accumulates*: each
record is generated only from reasoning that has never been through the
index.

### Shutdown

`AgentApp.dispose()` runs (via `scope(exit)`) on every exit path:

1. **Drain the shared worker before anything else that matters:** send
   `DiDrain` (with the explicit join budget), wait up to
   `ReasoningDrainBudget + 10 s` for `DiDrained`. The worker first processes
   its whole queue, then performs the **bounded join** of in-flight
   summarization threads: it consumes `RiRecord`/`RiDone` completions until
   none are outstanding or the join deadline (`ReasoningTimeoutS + 30 s =
   330 s`) expires — indexing every record that arrives in time — and logs
   a warning (`record(s) lost (advisory)`) if the deadline hits. It then
   checkpoints (TRUNCATE) and closes all WAL connections, leaving clean,
   sidecar-free DBs for the next process's read-only opens
   (`rag/dialogue_worker.d:371-398`).
2. The caller waits `ReasoningDrainBudget + 10 s = 340 s` for `DiDrained`
   and consumes a possible late reply (`rag/dialogue_index.d:534-551`). The
   drain is **idempotent** and never throws. **The worst-case shutdown cost
   is ≈ 340 s, and only when a summarization is actually in flight at
   dispose time.**
3. Terminate the UI thread and destroy the RAG.
4. Commit the active session file (dirty-gated), sweep empty sessions, save
   `state.json`.

**The record-loss window (advisory records only).** A completion that
arrives **after the join deadline** is inside the accepted loss window: the
worker itself does not terminate — a late completion is still consumed and
reopens the session DB via `ensureDb` — but shutdown proceeds and the
process exits, so such a record is not counted on (the deadline warning says
so explicitly). A completion racing worker termination hits the
`OwnerTerminated` guard: warning, record lost. Committed records are
durable; they are on disk before the drain closes the DB.

**No `setReasoningIndex(null)` teardown.** `ReasoningIndex` is stateless and
owns no resources: it rides the shared worker's drain, and there is no
per-index unsubscribe or close step at shutdown (the design draft listed
one; the build does not need it).

An abnormal kill (crash, `SIGKILL`) skips all of this: committed records
survive (SQLite WAL recovers on the next open), but queued/in-flight jobs
are lost — a documented limitation.

## Tool and Prompt Contract

### Tool Definition

`queryReasoningHistory` — *"Retrieve past strategic thinking, failed
attempts, and binding decisions from compressed context. Use ONLY when the
user asks why a past decision was made, or when you have tried multiple
approaches and suspect you are stuck repeating one. Not for verbatim quotes
— use queryDialogueHistory."*

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `textQuery` | `""` | Exact terms (FTS5-style) that appear in a reasoning record — a decision, an abandoned approach, or a tool name |
| `vectorQuery` | `""` | Natural-language description of the past decision, alternative, or roadblock |
| `topK` | `5` | Maximum matches; hard-capped by `toolLimits.maxTopK` (default 20) |
| `maxTurnAge` | `0` | `0`/negative = no age filtering; positive N keeps only matches within N turns of the newest indexed turn |
| `sessionId` | active session | Search another session's history (format `YYYYMMDD-HHMMSS-4hex`) |

The description is deliberately self-contained so that a future decision
router can adopt the tool unchanged. The rendering is one block per match:

```
--- Match 1 (session: 20260618-153045-a1b2, turns 42-44, 2026-06-18 15:34:02, rank: 0.812) ---
Abandoned Hypotheses:
- brute-force solver — O(n!) timed out on the 12-item case
Binding Decisions:
- memoized solver — O(1) per query after the table build
Current Uncertainties:
- none
Justification:
- turn 42: the timing failure ruled out the brute-force path
- turn 43: memoization was the only approach that met the budget
- turn 44: the table size stayed within memory
(Past thought, not ground truth — formed during turns 42-44. If it contradicts the user's latest instruction or verbatim facts, ignore it. Do not repeat an abandoned approach without new evidence.)
```

The final parenthesized line is the **per-match anti-anchoring annotation**,
appended verbatim to every match with that match's turn range
(`tool_call/reasoning.d:110-113`). It is the local, always-present reminder;
the standing rule lives in the main agent's prompt (see Prompt Trigger
Rules).

### Age Window

`maxTurnAge` is the staleness control: matches are kept only when
`turnEnd >= maxTurn - N`, where `maxTurn` is the highest turn end indexed in
that session's DB (computed from the DB, so it is correct after restarts).
The default (0) searches the whole indexed history — the query's own
keywords/semantics do the narrowing, and the window exists for "the last 20
turns or so" style questions, where searching hundreds of turns back would
mostly waste tokens.

### Prompt Trigger Rules

The main agent's prompt (`llmfun/config/prompt/AGENT.md`) carries a
`# Reasoning History Retrieval` section directly after the dialogue-history
section:

- Use `queryReasoningHistory` **only** when (a) the user asks why a past
  decision was made or what alternatives were considered, or (b) several
  approaches have been tried and the model suspects it is repeating one that
  already failed. Describe the decision or roadblock in `vectorQuery`; use
  `textQuery` for exact terms.
- **Anti-anchoring:** treat results as PAST THOUGHTS, NOT ground truth. If a
  retrieved thought contradicts the user's latest instruction or verbatim
  facts, ignore the thought; use it only to understand context and to skip
  approaches already abandoned for a stated reason.
- Never present a retrieved thought as a new justification; when relying on
  one, cite the turn range from the match header.

The prompt is data, not code, so a guard test in `llm.agent` loads the real
prompt through the production composition path and asserts the section
heading, the tool name, and the anti-anchoring sentence are present
(`agent/package.d:1119-1131`) — removing the rule from the prompt file is
caught by the unit tests instead of silently changing agent behavior.

## Concurrency and Consistency

- **No shared state, no locks.** Agent ↔ worker talk only via value
  messages. The worker's mailbox is unbounded (no backpressure — see Known
  Limitations). The worker-local in-flight-thread counter is the only
  bookkeeping involved — only the worker's mailbox thread ever reads or
  writes it (spawned threads signal through their one completion message).
- **Off-mailbox summarization.** The LLM call runs on a per-job spawned
  thread, so a slow model never delays the mailbox: a dialogue job queued
  behind an in-flight `RiJob` is processed immediately (asserted by the
  worker's slow-fake test).
- **One completion per job.** Each summarizer thread sends exactly one of
  `RiRecord`/`RiDone`; a failed spawn rolls the counter back instead of
  awaiting a completion.
- **Single writer per DB.** Only the worker writes a session DB (WAL). The
  agent opens read-only connections per query and closes them after use.
  WAL mode lets the reader see committed records while the writer works —
  no reader/writer lock, no hand-off of DB handles between threads.
- **Listener contract.** Checkpoint listeners run synchronously on the
  compressing thread and must be quick and non-blocking (the reasoning
  listener does filtering, formatting, budgeting, and one message send — no
  I/O). A throwing listener is isolated by the multicast; the listener's own
  body is additionally wrapped, so a reasoning bug can never break
  compression or the dialogue listener.
- **Per-job isolation.** Every worker message pattern (`RiJob`, `RiRecord`,
  `RiDone`) has its own try/catch; a reasoning failure never touches the
  dialogue path, and vice versa.
- **Teardown order.** The drain (worker → in-flight completions → WAL
  checkpoint → DB close) runs before `rag.destroy`, so no in-flight job can
  embed against weights that are being destroyed.
- **Turn ordering.** Records carry the min/max turn id over the included
  lines; topic names are the identity. Records are checkpoint-ordered:
  consecutive checkpoints can cover overlapping turns, exactly as they
  happened in the live history.

## Failure Modes and Degradation

| Failure | Behavior |
|---------|----------|
| Worker cannot create its embedder (bad config, no network) | One `DiDegraded` → warning on the agent side. Indexing is disabled for the process lifetime (documented limitation: no recovery). **An `RiJob` is dropped before any LLM call — no LLM spend while degraded.** The summarizer thread is never spawned; existing records stay queryable; the worker still answers the drain, so shutdown cannot hang. |
| Summarizer LLM error, timeout, or empty response | `RiDone` with a short reason code → the worker logs the skip; no record indexed. Dialogue indexing is unaffected, and the next checkpoint retries naturally (no permanent degraded state). |
| Record indexing fails (embed/commit) | Logged; the record is skipped; the job's counter and the dialogue path are unaffected. |
| FTS5 rebuild fails | Logged; chunks are still committed — vector search works, text search misses the new chunks until the next job's rebuild. |
| Retrieval query: vector-query embedding fails | Fall back to text-only when `textQuery` is present; otherwise an `error:` result (failed tool call). |
| Session DB missing, empty, or held only by the other kind | Graceful `No reasoning history indexed for this session yet.` (successful result). |
| Invalid session id (tool param or checkpoint) | Rejected before any file path is built (grammar-validated id type); logged; error result / skip. Defense in depth: validated at the tool level, in the index, and again in the worker's `ensureDb`. |
| Degenerate budget (`effectiveContext ≤ 8192`) | The listener skips the checkpoint with a warning — no job is sent; nothing can fit. |
| Budget overflow | Drop-oldest + omission marker; never a crash; a single oversized line is hard-capped. |
| A checkpoint listener throws | Caught by the multicast (and locally); other listeners and the compression proceed. |
| Shutdown join deadline exceeded | Deadline warning `record(s) lost (advisory)`; the worker checkpoints/closes and still answers `DiDrained`; late completions are inside the accepted record-loss window (the worker is still running, but the process exits — see Shutdown). |
| Crash between chunk commit and FTS rebuild | Text search misses those chunks until the next rebuild; vector search is unaffected. |
| Abnormal termination with queued jobs | Queued/in-flight records are lost (no job journaling); committed records are durable. |

The overall posture: **record production degrades to "nothing indexed" —
at worst one LLM call is skipped or its result dropped — and query failures
degrade to "no history" or a clean tool error. The agent loop never crashes
or blocks because of this feature**, and the dialogue guarantee (a slow
local model never blocks indexing) holds structurally because the call is
off the mailbox.

## Configuration

| Key | Default | Role |
|-----|---------|------|
| `dialogueDir` | `<dataDir>/dialogue` | Directory holding the per-session DBs — shared with the dialogue index |
| `reasoningSummaryPrompt` | `REASONING_SUMMARY.md` | The reasoning-summary prompt filename, resolved against `promptDir` |
| `summaryModel` (server URL/api-key, `modelName`, temp, `contextSize`, `contextChunkSize`) | — | The summarizer call and the budget's effective context (cached at startup). Shared with the dialogue summary agent |
| `embedConfig` (server URL, model, dimensions, …) | — | Embedding endpoint for records (same embedder stack as dialogue) |
| `toolLimits.maxTopK` | `20` | Hard cap for the tool's `topK` parameter (no new limits were added) |

The summarizer prompt is loaded once at startup: the configured file is read
raw (`readPromptFile`; no skills/context composition), and a missing or
unreadable file falls back to the built-in default constant with a warning —
startup never hard-fails on this file. The shipped
`llmfun/config/prompt/REASONING_SUMMARY.md` is kept **byte-identical** to the
built-in default (`rag/reasoning_index.d:66-94`; enforced by a unit test),
so the file-present and the fallback experiences are the same prompt; the
prompt is strictly different from the dialogue `SUMMARY.md` (asserted too).
The prompt's rules are the four-section contract, no narrative, no
restating user queries, ~150 words, `None` for empty sections, and
justification only for the three most recent turns present.

No new environment variables are involved; the feature activates with a
configured summary model (to compress in the first place) and a working
embedder (to index). With either missing it stays dormant or degrades per
the table above.

## Known Limitations

- **Advisory only.** The annotation and the prompt rules are strong
  guidance, not enforcement; a model can still over-trust a record. The
  records are deliberately framed as past thought so that verbatim facts
  and the user's latest instruction always win.
- **No verbatim trace recall.** Records are summaries (~150 words) produced
  by the summary model; the raw trace text is never written to disk — it
  exists only as the summarizer's input. For exact strings, use
  `queryDialogueHistory` (dialogue retrieval).
- **Quality bound by the summary model.** A small local model can produce
  thin or noisy sections; the prompt is a hard format contract, not a
  guarantee of insight.
- **No sub-agent records.** Checkpoints without a session stamp are refused;
  sub-agent compressions carry none, so their reasoning is not indexed
  (same as the dialogue index's side).
- **No TTL in storage.** Records are never deleted; the `maxTurnAge`
  retrieval window bounds staleness at query time instead.
- **Cross-kind combined queries stay out.** The two tools and the two
  post-filters are separate by design; a tool that queries both kinds at
  once is future work (a decision router).
- **Cost and concurrency note.** One extra LLM call per evicting
  checkpoint, on the same local model as the dialogue summary; the
  compression summary and a reasoning record can be in flight at the same
  time (different threads). The mailbox can also buffer large `RiJob`
  payloads during a compression burst. Both are bounded in practice by the
  compression cadence — revisit only if a burst is ever observed.
- **Timestamp precision.** The epoch millis in an `r_` topic name is the
  worker's clock at job receipt (the dialogue codec convention — the worker
  clock, deliberately not the eviction timestamp); for records the
  difference is the mailbox latency.
- **Unbounded mailbox.** The worker has no backpressure against the agent;
  a long outage lets jobs pile up in memory (dialogue-index property, unchanged).

**Evaluation.** Recall quality is covered by two layers. A scripted
tool-level recall suite asserts that (a) a "why did you…" query retrieves
the record containing the stated reason, cites the turn range in the header,
and carries the annotation verbatim; (b) a stuck-loop query surfaces the
abandoned approach and the anti-retry clause; (c) a cross-session query
cannot leak a sibling session's record (with an active-session positive
control). On top of that, a manual evaluation protocol is run at phase
evaluation: a contradiction case (after a contradicting post-compression
instruction, a retrieved past thought must not override it — the annotation
plus the prompt rule) and a turns-to-solve delta on decision-heavy tasks
versus the dialogue-only baseline (~30% drop target — reported, never asserted).
A "violated" contradiction verdict is a quality regression to escalate, not
a test failure.
