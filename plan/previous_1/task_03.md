## Task 3: D-side typed emitter — replay, live, compression (design P0-3, part 1)

Priority: P0. Dependencies: task 1. One additive commit; the v4 senders stay on the
live run path until the cutover (task 5) — this task's paths are exercised by
recording-sink tests only. The console formatter is task 4 (P0-3 part 2).

### Description
Grow the D-side surface (TuiSink/UiMessenger/wrapper) with the five v5 emissions
and implement the emitter that produces typed turns/blocks on the replay/session
switch path, the live path, and compression re-emission. D emits everything,
mode-independently; displayability is C-side (design §4).

### Changes Made
- llmfun/source/llm/app_agent/ui.d: TuiSink (25-49) gains `setTurns`, `turnUpsert`,
  `blockUpsert`, `turnRemove`, `threadClear` (turn/block payloads as the imported C
  types). TuiChannelSink forwards (Channel!TUICommands pattern); TuiBlockedSink stubs
  (task 4 fills them); TuiRecordingSink RECORDS field-by-field — new arrays
  `turnUpserts`, `blockUpserts`, `setTurnsSnapshots` (copy at record time, the
  LaneUpdate/ToolCallEvent pattern at ui.d:248-272), `turnRemovals`.
- llmfun/source/llm/app_agent/ui.d: UiMessenger (404-513) gains the five methods;
  StreamMessageUpdater (516-557) gains a typed sibling (content-so-far + Pending
  call upserts per tick; NO `--- Tool ---` splicing — that dies at cutover).
- llmfun/source/llm/tui/package.d: wrapper methods marshalling to the task-1 C
  entry points (pattern of addChatMessage 359-379: build the C struct, call,
  borrowed-String rules per tui_api.h 55-70).
- llmfun/source/llm/app_agent/package.d: the emitter functions (not yet called by
  the live run path):
  - `emitReplayTyped(session)`: walk the persisted history once -> ONE
    tuiSetTurns with already-merged blocks. No incremental pairing, no
    adjacency assumptions. Turn = one per user query (turnId from chat.d:89-124
    stamping). Interrupted turns replay in-flight calls as Cancelled (never a
    fabricated Ok); duration from processTime (chat.d ToolResponse:948+).
  - `emitLiveTyped(...)`: tuiTurnUpsert(Running) + query prose block on query
    (turn ordinal = D5); stream ticks upsert the current batch's content block
    and known tool calls as Pending (name + arguments-so-far); stream-done
    flips the batch's Pending -> Running; the step-completion walk (today's
    processResult 306-329 shape) emits merged tool blocks (Ok/Failed + meta +
    durationMs), the next batch's blocks, and the Answer block when the final
    task_done result arrives; turn state Done after the answer, Interrupted on
    stop (in-flight -> Cancelled, partial answer stays).
  - Compression (doCompress 252-272): affected turns re-emitted (upsert/remove),
    ordinals recomputed (D5); no stale group with orphaned block ids survives.
- Block field contract (verbatim; the console formatter and both view modes
  consume exactly these fields; bodies via truncatedForWire ui.d:564-571):
  | kind | id | title | meta | body | flags |
  |---|---|---|---|---|---|
  | Prose (query) | per-turn uid | empty | "user" | raw query text | none |
  | Prose (content) | per-block uid | empty | a.role | response content | none |
  | Reasoning | per-batch uid | empty | empty | response thinking | Collapsible |
  | Tool | tool callId | wire tool name | result summary once merged (summarizeToolResponse) | this call's summarized "name args" line (summarizeToolCalls per-call form) | Collapsible\|DefaultOpen\|Selectable |
  | Answer | "answer" | empty | empty | final answer text | none |
  | Notice | per-notice uid | empty | empty | notice text | none |
  durationMs from processTime (0 = unknown); tokensIn/tokensOut from the
  response usage (0 = not rendered). batchId = "<turnId>#<batchIndex>",
  0-based per-response index within the turn; tool-less batches carry one;
  recomputed deterministically on replay; NOT persisted (design §4).

### Verification
- [ ] Emission-order tests on TuiRecordingSink (pattern tests.d:769+, 2852-2925):
      replay snapshot vs live sequence; merged replay blocks; Pending->Running->Ok
      ordering; answer-on-done; interrupted-turn replay (in-flight call = Cancelled);
      session switch mid-run (snapshot rebuild + re-attach by turnId); compression
      re-emission (remove + recomputed ordinals); batchId format + tool-less batches;
      block field contract asserted field-by-field.
- [ ] `dub test` green (new tests + no regression; the v4 paths untouched).
- [ ] The live run path is byte-unaffected: today's console/TUI output unchanged
      (the v4 senders still own it until task 5).

### Notes
- D5: ordinals are D-assigned, 1-based position among displayed turns, recomputed
  on compression re-emission, never recomputed C-side. (Section 5's "mirroring
  today's displayableCount rule" means the emitter owns the number as today's
  code did — F8 pins the per-turn semantics; today's per-message numbering dies.)
- Statuses are emitter-driven upserts (design §4: transitions ride existing
  events; no new execution-start event). The store merges, it does not enforce.
- The pipeline/drill-down path is NOT in this task: the pipeline entry points
  keep their structured shape and are retargeted C-side at the cutover (design
  §4 decision; task 5).
- Decision letters used: D5 (overview).
