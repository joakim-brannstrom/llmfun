# System design - Thread rendering v5: typed turns and blocks, two modes, tool presentation

Date: 2026-01-06
Source: plan/appendix.md ("tool presentation" appendix) + code survey of llmfun (D agent/TUI)
and llmfun/cpp_tui (v4 ABI, thread store, views). Companion notes: plan/design_notes.md.
Review: plan/design_review.md - verdict APPROVE WITH FIXES; fixes W1-W14 applied this revision.

## 1. Why

The workbench thread is a terminal session: today it is fed by formatted chat sentences. Every
tool interaction is flattened into text that C++ must re-parse before it can present anything:

- D formats each message as prose ("[nr] tool call: name args", "[nr] tool result ✅/❌ name: summary",
  "[nr] role: content"), splices ordered tool calls into the streamed body as `--- Tool ---` text,
  and embeds a JSON tool dump in the stream body.
- The C++ store strips the number prefix (splitSummary), counts what survived, and pairs a call
  with its result only if the two are ADJACENT rows; the stream body carries a verbatim JSON dump
  that must be ignored by every renderer.

Costs of the text path (from the appendix):
- Streaming reports calls as body text; results arrive as sibling sentences; a step's status is a
  glyph in a summary line. Nothing can be restyled, counted, or folded honestly.
- The final answer is indistinguishable from a second user turn and streams with no live edge.
- The work group's step count, the `[nr]` ordinals, and the green/failed glyphs are all derived from
  text that the D side produced only to be human-readable.

Goal: replace the message-path ABI with typed turns and blocks, kill the v4 sentence path outright
(same iteration, no transition window), and render the thread in two modes - a default minimal mode
(user query, live work edge, answer) and a maximal mode that can be turned into the intended
"batches" view with thinking disclosures and per-step tool cards.

## 2. Requirements

### Functional
- F1 Typed thread events: the D side emits turns and blocks (prose, reasoning, tool step, answer,
  notice) through the ABI; the C++ store keeps them keyed and upserts in place. No sentence parsing.
- F2 Tool step = one unit per toolCallId carrying call and result; state machine Pending -> Running ->
  Ok/Failed/Cancelled. Replay emits already-merged blocks (D owns call+result); only live streaming is
  call-then-result.
- F3 Work group per turn with header "WORK - <N> steps"; N = number of LLM messages the group covers
  (finished call = 2, live call = 1, prose = 1), over the displayability filter, which lives C-side
  next to the weights (section 4 decision): D emits every block it has; hidden blocks are filtered,
  not unsent.
- F4 Minimal mode (default): user and answer blocks render with their bodies; work groups render
  header-only; the newest live group additionally renders one chips line - one chip per live step,
  tool name only; the Running chip is green, Pending chips dim; a step folds the moment it is
  dormant, even mid-batch. The streamed answer renders as nothing but the `[task_done]` chip until
  the result arrives, then the answer body appears.
- F5 Maximal mode: work groups render open as batches. One batch = one model response: a thinking
  disclosure (collapsed by default, meta "N tool calls"), the response's content block, and one tool
  card per callId - header "TOOL <name>" while the card streams, "TOOL <name> - <duration>" once the
  result supplies durationMs; the call (arguments) as the body; an embedded Result open by default;
  tokensIn/tokensOut render only when non-zero. Cards hang indented beneath the batch's thinking,
  sharing a left rule. Failed steps are red inside expanded groups only.
- F6 Mode selection: `minimal` | `maximum` persisted in ui.conf, edited from a settings sheet row
  (behaviour section); minimal is the factory default. No key shortcut, no palette command.
- F7 Per-group manual override, session-local: opening a group in minimal (or collapsing one in
  maximal) sets an override for that group only, keyed by the group's persisted turn id; the override
  always beats the mode default and dies on session switch. A fresh session starts at the mode default.
- F8 Ordinals: one number per turn (1-based position among displayed turns). Tool steps, thinking,
  and result bodies consume no ordinals.
- F9 One-shot CLI console keeps today's output (same lines, same emoji) although the shared sentence
  formatting dies.
- F10 Session replay renders the identical thread twice (replay parity), with expansion state preserved.

### Non-functional
- N1 Both sides agree on the wire per commit; the v4 payload dies in the same iteration - no dual
  -source window, no config-selectable legacy path.
- N2 Bounded memory as today: capped store, eviction per whole turn (oldest first, never the live
  turn), height-hint eviction, no array-of-everything rendering.
- N3 Honesty rules unchanged: nothing renders that did not come from the run, and stop/cancel never
  invents a completed state.
- N4 1000-message sessions open and scroll without re-scan cost per frame (store lookup, not text parse).

### Constraints
- C1 The ABI is a flat C wire format; message structs are layout-pinned (D static asserts + C++ tests).
- C2 The live stream tick cadence stays C-side throttled (~20 Hz); D keeps sending chunk deltas.
- C3 The now bar's phase line, chip strip, logs, runtime status, and the agents tab lanes are separate
  surfaces and unchanged (this design covers the thread only).
- C4 No new per-tool execution-start ABI event exists; the status machine rides existing stream-done
  and result events.

## 3. Architecture

```
D agent run                     ABI (flat C)                      C++ workbench
--------------                  ----------------------             --------------------------------
TurnEmitter (new)  --emits-->   tuiSetTurns / tuiTurnUpsert         TypedThreadStore (new)
  replay: merged blocks         tuiBlockUpsert / tuiTurnRemove        turns -> blocks keyed by callId
  live: call-then-result        tuiThreadClear                      status machine, batch grouping,
  (chat path)                   (session actions, runtime           step count, bounded window
                                 status, chips: unchanged)                 |
ConsoleSink formatter  <--------+                                  ThreadStoreAdapter (seam kept)
(one-shot output)                                                 GroupView/BlockDesc vocabulary,
                                                                  extended with batch rows
                                                                        |
                                       ThreadView (two-mode projection)  Inspector (unchanged)
                                       minimal | maximal | per-group     Activity feed = log ring
                                       override                          Agents drill-down = typed turns
```

- The D side gains a TurnEmitter that produces typed events from data it already has: the history
  tail it walks per step, the stream chunks it already forwards, and the turn id it already stamps.
  No new execution hooks.
- The ABI gains the v5 turn/block types and upsert/clear entry points, and loses the v4 message
  path (add-chat, stream-chat-update, stream-clear, clear-chat) in the same iteration.
- The C++ side replaces the v4 output-line store with a typed store; ThreadStoreAdapter keeps its
  seam (GroupView + BlockDesc) so the thread view, drill-down, and settings sheet keep their shape,
  while the group/block vocabulary is extended for batches and merged tool cards.
- The one-shot console path keeps its presentation by formatting the typed events itself at the sink.

### What dies
- D-side sentence formatting for the TUI path, emoji decoration in it, the `[nr]` numbering inside
  sentences, the `--- Tool ---` stream-body splicing, and the JSON tool dump embedded in stream bodies.
- C-side: splitSummary text archaeology, adjacency pairing of call/result rows, the v4 ChatMessage
  payload and its store, the marker stripping, and the two-number (call/result) display gap.

## 4. Data model (v5 thread types)

- TuiTurn { id (persisted turnId), ordinal (1-based display position), state (Running | Done |
  Interrupted), messageCount (informational), } - the work group.
- TuiBlock { id (stable within the turn: tool callId, "answer", batch-scoped ids for thinking and
  content), batchId (see below), kind (Prose | Reasoning | Tool | Answer | Notice), status
  (None | Pending | Running | Ok | Failed | Cancelled), flags (Collapsible, DefaultOpen, Selectable),
  title, meta (one-line summary / "N tool calls" / primary argument), body (markdown/diff/output),
  durationMs, tokensIn, tokensOut }.
- TuiBlockStatus gains None (prose/answer blocks carry no outcome and paint no state mark) and
  Cancelled (in-flight calls after a stop). This extends the 8.2 sketch, which lacks a statusless
  value and would paint a green check on prose.
- Entry points: tuiSetTurns (bulk snapshot: replay, session switch), tuiTurnUpsert, tuiBlockUpsert
  (the only mutating path: upsert keyed by (turnId, block id); a first upsert creates the slot,
  later upserts merge in place), tuiTurnRemove (compression may drop whole turns; the remove
  cascades to the turn's blocks - there is no per-block remove), tuiThreadClear (replaces clear-chat).
  Session actions, runtime status, chips, logs, and query history entry points are unchanged from v4.
- Store topology: one typed store exists per v4 ChatTab equivalent - the main thread plus one per
  pipeline agent; the pipeline entry points are scoped by agentId and feed the drill-down's store.
- The pipeline (agents) entry points keep their current structured shape but their payloads are
  retargeted onto the typed store, scoped by agentId, for the drill-down rendering.

Design decision: batch identity rides a batchId string on TuiBlock, not a marker event. D numbers
batches per turn (one per model response); thinking, content, and tool blocks of one response share
it. It is recomputed deterministically on replay (batch index within the turn) and is stable within
the turn's lifetime in one process run; it is not persisted. Exact format: "<turnId>#<batchIndex>";
tool-less batches (thinking and content only) carry a batchId too.

Design decision: the v5 group id is the persisted turnId, not an adapter-assigned sequence number.
Today's work-group ids renumber on full rebuild; with turnId as the key, overrides and expansion
survive upserts, re-emission, and rebuilds within a session.

Design decision: eviction is per whole turn, oldest first, and never touches the live turn. Evicting
blocks mid-turn would corrupt "N steps" headers and leave chips referencing evicted steps; evicting
a live turn would fight the upsert merge.

Design decision: the displayability filter lives C-side, next to the step-count weights. D emits every
block it has, including hidden tools; hidden blocks are filtered at render/weight time. The store
never assumes a stored block is displayable, and the console formatter applies the same C-side filter.

Design decision: live status transitions ride existing events - stream-done flips the batch's
Pending calls to Running; the result upsert flips to Ok/Failed; a stop flips in-flight calls to
Cancelled. No new execution-start event is added (appendix constraint).

Design decision: D emits everything, mode-independently. The store keeps the full feed; minimal and
maximal are projections. The inspector's Activity view keeps the full live feed regardless of the
thread's mode.

## 5. Emission paths (D side)

- Replay / session switch: walk the persisted history once and emit tuiSetTurns with already-merged
  blocks: prose blocks from Messages (and compression notices), reasoning+content from each response,
  one merged Tool block per callId (status Ok/Failed from the stored result, duration from processTime,
  Cancelled for in-flight calls - an interrupted turn replays honestly, not as a fabricated Ok), the
  Answer block from the final task_done payload. No incremental pairing, no adjacency assumptions.
- Live: the turn upsert opens the turn (user query prose block, ordinal assigned); stream ticks upsert
  the current batch's content block and its known tool calls as Pending blocks (name + arguments-so-far);
  stream-done flips them to Running; the step completion walk emits merged tool blocks (Ok/Failed),
  the next batch's blocks, and - when the result carries the final answer - the Answer block. Upsert
  is the only mutating path: a first upsert creates the block's slot, later upserts merge in place.
- Ordinals and step counts: the emitter assigns the turn's ordinal (display position among displayed
  turns, mirroring today's displayableCount rule per turn). The group's "N steps" is computed C-side
  from store weights (finished tool = 2, live call = 1, prose = 1) so the D side does not duplicate
  the displayability logic.
- Visibility is unchanged for the user: hidden tools are emitted by D and filtered C-side at render/
  weight time (they stay hidden and uncounted); compression notices render as Notice blocks; the run's
  user query is the turn's opening prose block.

Design decision: the task_done call is not special-cased. While it streams it is a tool step like any
other (chip only, or card in maximal); the answer it carries renders when the result arrives. The
`[task_done]` chip label is block meta set by the presentation layer, not a D-side special case.

## 6. Presentation (C++ thread view)

- Minimal (default): user and Answer blocks render with bodies (they are the turn's content, not its
  work); work groups render header-only ("WORK - N steps"); the newest live group adds one chips line
  under its header - tool names only, one chip per live step, Running green, Pending dim, folding the
  moment a step is dormant (mid-batch folding included). The streamed answer shows as the `[task_done]`
  chip on the live edge until its result arrives, then the answer body appears.
- Maximal: work groups render open as batches - thinking disclosure (collapsed, meta "N tool calls"),
  content block, tool cards ("TOOL <name>" while streaming, "TOOL <name> - <duration>" once the
  result lands; arguments as body; embedded Result open by default; token fields render only when
  non-zero), cards indented under the batch's thinking behind a shared left rule, failed red inside
  expanded groups.
- Overrides: opening a group in minimal renders that one group maximal-style; collapsing a group in
  maximal pins it closed; either writes a session-local override keyed by group id that beats the mode.
- The inspector (Agents tab lanes, Activity feed) is untouched. The drill-down transcript renders a
  per-agent typed store through the same adapter seam (see decision below).
- The one-shot console path (blocked sink) formats the typed events into today's lines locally - same
  text, same emoji - because the shared D-side formatter dies with the v4 path. The appendix's
  "console path is unaffected" refers to the output, not the code: the sink's formatter is rebuilt
  over typed events.

Design decision: the agent drill-down migrates to typed turns in the same iteration. The v4 payload
(ChatMessage/ChatTab) dies entirely, and the drill-down renders through it today; pipeline emitters
emit typed events scoped by agentId. The lane mirror and the Agents tab consume the mirror, not the
store, and are untouched.

## 7. Failure modes and edge cases

- Result for an unknown callId (live path): the store records the result under its callId and renders
  the merged step when the call lands - never silently dropped. Hidden tools are no longer this case:
  D emits their blocks and C++ filters them at render time (section 4 decision).
- Stop/cancel: in-flight calls flip to Cancelled; the partial answer stays (today's behavior, kept).
- Compression rewrites history: the affected turns are re-emitted (upsert/remove); the ordinal
  sequence is recomputed. No stale group survives with orphaned block ids.
- Upsert storms (long streams): upserts are idempotent merges; the ~20 Hz C-side throttle bounds cost;
  the store never re-parses or re-scans on upsert.
- Session switch mid-run: the in-flight turn belongs to its session; the typed store is rebuilt from
  the snapshot on switch, and the live emitter re-attaches by turnId (existing behavior shape).

## 8. Testing strategy

- Wire/packing: static asserts D-side and offset-pinning tests C-side for every new struct (pattern of
  the existing v4 contract test, case 1f).
- Store unit tests: upsert merge by callId (first upsert creates, later upserts merge); status machine
  transitions; batch grouping; step count weights; eviction granularity (whole turns, oldest first,
  never the live turn); turn remove cascades to blocks; group id = persisted turnId, stable across
  re-emission and rebuilds; unknown-callId result recorded and merged when the call lands; clear.
- Emission-order tests on the recording sink: replay snapshot vs live sequence; merged replay blocks;
  Pending->Running->Ok ordering; answer-on-done; interrupted-turn replay (in-flight call replays as
  Cancelled); session switch mid-run (store rebuilt from snapshot, emitter re-attaches by turnId).
- Console output parity: scripted session renders byte-identical lines to today's output (golden text).
- Golden frame scenarios (appendix-mandated): both modes; live folding mid-batch; a failed step inside
  an expanded group; a turn whose ordinal and step count show together. Plus replay parity (same
  session renders identically twice, expansion survives) and a 1000-message session open/scroll check.
- Stop mid-batch -> Cancelled states render honestly; no fabricated Ok.

## 9. Rollout and commit shape

1. Additive commit: v5 types, entry points, version bump, static asserts, store skeleton behind the
   adapter (unwired). Both sides build and agree (nothing carries v5 on the wire yet).
2. Cutover commit (one commit, both sides): flip the D emitter to typed events, retarget the C++ store
   and thread view, migrate the drill-down, delete the v4 message path on both sides (formatter, stream
   splicing, output-line store, adjacency pairing, marker stripping). The console formatter is wired
   here (built in P0-3 behind the recording sink).
3. Rendering commits: minimal mode + live folding; maximal mode; settings row + per-group override.
4. Hardening commit: the cross-cutting battery (replay parity, upsert storm, stop, compression, perf).
5. Doc sweep: mark the section 8 deferral note (todo_modern_ui.md) and the live-tool-tab/stats-window
   lines (doc/todo.md) as delivered by this design.

## 10. Task list

P0 - foundations (each independently testable, both sides green per commit):

- P0-1 v5 thread types + ABI surface (additive). TuiTurn/TuiBlock/kinds/statuses/flags/batchId;
  tuiSetTurns/tuiTurnUpsert/tuiBlockUpsert/tuiTurnRemove/tuiThreadClear; TUI_API_VERSION bump; D
  bindings + static asserts. Acceptance: contract tests pin layout/offsets; both sides build.
  Depends: nothing.
- P0-2 C++ typed thread store + adapter rewrite. Turns/blocks keyed by (turnId, block id); upsert
  merge; status machine riding stream-done and result events; batch grouping; step-count weights;
  bounded window; whole-turn FIFO eviction (oldest first, never the live turn) + height-hint eviction;
  turn remove cascades; group id = persisted turnId. Acceptance: store unit tests listed in section 8,
  including the eviction, remove-cascade, and group-id invariants; adapter still feeds the existing
  thread view (v4 store still live until P1-4). Depends: P0-1.
- P0-3 D-side typed emitter + console formatter. Replay snapshot (merged blocks; interrupted turn
  replays in-flight calls as Cancelled); live call-then-result; turn upserts with ordinals; answer-on-
  done; Notice blocks; D emits every block, filter C-side; console sink renders typed events to today's
  lines (built here, wired at cutover). Acceptance: recording-sink emission-order tests; console parity
  golden text. Depends: P0-1.

P1 - cutover and rendering:

- P1-4 Cutover commit: flip emitter+store, migrate drill-down, delete v4 path both sides (D sentences
  to TUI, stream-body splicing, ChatTab output-line store, adjacency pairing, marker stripping; C++
  add-chat/stream-update/stream-clear/clear-chat). Acceptance: no reference to removed symbols either
  side; build green; existing goldens pass on the typed store; console formatter wired, output unchanged.
  Depends: P0-2, P0-3 (same iteration as P0-1, per appendix N1).
- P1-5 Minimal mode + live folding. Header-only groups, chips line (names only; green/dim; fold on
  dormancy, mid-batch included), answer-chip streaming, answer body on done. Acceptance: golden frames
  - idle thread, live batch with mixed statuses, post-completion fold. Depends: P1-4.
- P1-6 Maximal mode. Batch anatomy (thinking disclosure + "N tool calls" meta, content block, tool
  cards with duration/args/Result-open), left-rule indent, failed-red in expanded groups. Acceptance:
  golden frames - batch anatomy, failed step in expanded group, card + duration. Depends: P1-4.
- P1-7 Settings row + per-group override. ShellPrefs field + ui.conf key + behaviour-section row;
  session-local override set keyed by the group's persisted turn id, beating the mode; reset on
  session switch. Acceptance:
  sheet row cycles both words; override survives mode flips within a session and dies on switch;
  persistence round-trip with new key defaulting on old files. Depends: P1-5, P1-6.

P2 - hardening:

- P2-8 Cross-cutting battery. Replay parity (identical render twice, expansion survives), upsert
  storm, stop mid-batch -> Cancelled, compression re-emission, 1000-message session open/scroll,
  result-for-unknown-callId (live), session switch mid-run, interrupted-turn replay.
  Acceptance: all listed scenarios green in the headless harness. Depends: P1-4 (extends with P1-5..7).

P3 - docs:

- P3-9 Doc sweep. doc/todo_modern_ui.md: mark the thread v5 / tool-card deferral note delivered, keep
  the non-thread remainder; doc/todo.md: mark the live-tool-tab and stats-window lines delivered;
  doc/tui_design.md: update the C API boundary section (ChatMessageParam and the v4 chat path are gone,
  the v5 turn/block surface replaces them). Depends: P2-8.

## 11. Out of scope (appendix)

Session list/now bar/chip strip/log/runtime-status v5 models; cross-turn search index; tuiMoveCall;
agents-tab lane internals and inspector internals; key shortcut or palette command for the mode;
any config-selectable legacy path.
