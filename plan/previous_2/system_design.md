# System design - Thread rendering v6: query-first anatomy, live edge, mode rebalance, tool card shape, busy edge

Date: 2026-11-15
Source: plan/appendix_fixes.md (post-v5 feedback items 4-8 + investigation) + code survey (D agent/TUI, cpp_tui store/views).
Follow-up to "Thread rendering v5" (plan/previous/system_design.md). Companion notes: plan/design_notes.md.
Review: plan/design_review_v6.md - verdict APPROVE WITH FIXES; fix list 1-10 applied this revision (no substance of D1-D8 reopened).

## 1. Why

The v5 typed-turn rendering shipped; the user reported five problems with the rendered thread view.
The appendix investigated each one and separated real defects from rejected design decisions:

| Feedback | What it is | Verdict |
|---|---|---|
| 4 (order) | Query renders below its WORK group header | v5 anatomy as designed; **rejected by the user** - restructure (D1) |
| 4 (live edge) | Current command invisible while a turn runs | Deliberate v5 minimal-mode gap - **new live-edge element set** (D2) |
| 5 | Thinking disclosure collapsed in maximal | v5 rule as designed; **decision reversed** (D3) |
| 6a | "[N]" ordinal prefix on the query | v5 turn-ordinal feature the user never asked for - **remove display + data** (D5) |
| 6b | Send never flips to Stop | **Real wiring bug**: busy edge lands in an empty function; pipeline handlers never raise it - **fix** (D6) |
| 7 | Tool card: "+" marker, repeated name, packed args | Three verified contributors - **reshape the card** (D4) |
| 8 | ~102-card wall in a long maximal turn | Item 5's rule on a long run; **not a regression** from the pipeline query fix - fixed by D3 (+D1) |

The "got worse after the pipeline query fix" claim is explicitly resolved: that fix's regenerated
goldens touched only folded minimal scenes, the maximal-anatomy golden was never regenerated, and a
long main-view turn renders identically before and after it (the query is the turn's first block and
carries the user meta under both the old and new query rules). The fix drew attention to a
pre-existing shape. No rollback is part of this design.

Goal: apply D1-D8 to the v5 rendering in one iteration - query-first turn anatomy, a live edge that
always shows what the agent is doing, a maximal mode that opens only the newest batch, an honest
tool card shape, no ordinals, and a busy edge that actually drives the composer button.

## 2. Requirements

### Functional (MoSCoW)

- F1 (Must, D1) Query-first anatomy: the query row renders as the turn's opening row, above the work
  group header; the answer stays a turn-level row; the group carries only its work rows (thinking,
  tool steps, streamed prose). The minimal fold keeps query and answer; the WORK header sits between
  them only while the group has rows to show.
- F2 (Must, D2) Live edge (both modes - minimal draws it directly under the header, where no other rows
  render; maximal at the open group's bottom): while any tool runs, one chips line draws at the group's
  live edge - running tools' names only, orange, not clickable; their details live inside WORK once they
  finish. Pending calls draw no chip (v5's dim Pending chip dies - a stated change). While the model
  streams thinking, a folded, count-only Thinking row renders instead. While the model streams content
  with no tool running, nothing extra draws. At most one live element. While tools run the group's step
  count updates live. When the turn finishes the live element folds away and the group keeps its count.
  Finished tools keep today's colors; live tools are orange. The live edge exists in both modes; the
  modes differ only in fold defaults (F3).
- F3 (Must, D3) Maximal rebalance: each response that carries thinking renders one Thinking row
  grouping that response's thinking with its calls (label = the tool-call count only; the trailing
  summary today's disclosure renders is removed). Thinking renders open by default. Tool cards
  auto-open only for the most recent batch; older tool cards render collapsed into their headers - no
  second count row inside the group, the header's step count is the only count. Auto-open is
  live-only: when the turn finishes the group folds to its header with the counts (the same finished
  shape minimal shows) - the finish fold applies only to groups at their mode default; an explicit
  mid-stream manual open (a user override) survives it and keeps the finished shape visible.
  Re-expanding a finished group renders every Thinking row open with tool rows as
  folded headers - nothing re-opens, the newest-batch rule has no bearing on a finished turn. Opening a
  group mid-stream re-applies that mode's fold defaults and renders the newest batch's full details
  (arguments and result) open in BOTH modes - the one live exception. Minimal and maximal render the
  identical shape with identical click behavior; the modes differ only in fold defaults. Per-group
  manual overrides (v5 F7, keyed by the persisted turn id, session-local) still beat the mode defaults.
- F4 (Must, D4) Tool card shape: the card is a foldable - header carries a disclosure arrow (">" folded,
  "v" open) and no status glyph; a failure renders a textual FAIL (red) on the header, folded or open; no
  token fields. Folded, the card is the header only; open, the body is arguments only, one `key=value`
  pair per line, no repeated name prefix, no "[N]" prefix. The embedded RESULT row renders a JSON object
  result unpacked one level: one dim `RESULT <key> = <value>` line per pair, sorted by key; nested
  values render as compact JSON text; arrays and non-JSON results render as-is, one dim line; a
  zero-pair object renders compact as-is, one dim `RESULT {}` line. Pairs cap at 100 and every value at
  200 characters - past either, the first 100 keys / first 200 characters render and one dim "... K more"
  tail closes the line or the card.
- F5 (Must - rides the cutover commit, D5) No ordinals: the "[N]" prefix disappears from the query row and the ordinal leaves the
  data model with it; ordering is turnId's job and session restore reads the history sequence; nothing
  else renders or stores them.
- F6 (Must, D6) Real busy edge: the ready flag is the single source of truth for "a run is in flight"; the
  messenger's busy half clears the flag (mirroring its ready half); every turn entry point raises the edge
  - main query and compression (already bracketed, start working once the body exists), the /plan and
  /code handlers (add the bracket around the pipeline run), and every additional path the task-1
  enumeration verifies (one-shot mode, drag&drop-triggered runs, session resume). The app runs at most one
  run at a time (the composer refuses a second query while one is in flight), so the plain-bool bracket is
  sound; if the enumeration finds a path that can overlap a live run, the bracket becomes a nesting
  counter instead. Stop targets the run in flight regardless of the session shown. Run-in-flight
  predicates elsewhere (stop affordances) read the same flag. The runtime-status phase edge stays
  display-only.
- F7 (Must, D7) Scope: D1-D6 are stated for the normal thread view; the pipeline drill-down shares the
  adapter and rendering and inherits all of them. No pipeline special-casing in the generic chat-message
  view; pipeline-specific shaping hides inside the adapter; wrong data in the structure is a population
  bug - fix the population, not the view. The drill-down keeps having no query row (an agent run has none).
- F8 (Must, D8) Count units: the WORK count is steps - every message between the harness and the model
  inference server is +1 (a tool call and its tool answer are +2 together, a plain message +1). The
  Thinking row's counter is tool calls, not steps - one model response can carry thinking plus N calls and
  the row groups that response's thinking with its N calls. Steps and tool calls never mix in one count.
- F9 (Must) Console parity: the one-shot CLI console output stays byte-identical (v6 touches the TUI
  projection and two D-side payloads; the console formatter's lines do not change).

### Non-functional

- N1 Both sides agree on the wire per commit; no dual-source window, no config-selectable legacy path
  (v5 rule, unchanged). The ordinal removal is a breaking layout change and rides the cutover commit.
- N2 Bounded memory as today: capped store, eviction per whole turn (oldest first, never the live turn),
  height-hint eviction, no array-of-everything rendering. The RESULT pair set is bounded by the caps
  (max 100 pairs x 200 chars + one moreCount).
- N3 Honesty rules unchanged: nothing renders that did not come from the run; stop/cancel never invents a
  completed state; a failed tool renders FAIL, never a silent card.
- N4 1000-message sessions open and scroll without per-frame re-scan cost (store lookup, not text parse);
  the RESULT unpack adds no per-frame work (D unpacks once at emission; C renders strings).

### Constraints

- C1 The ABI is a flat C wire format; message structs are layout-pinned (D static asserts + C++ offset
  tests). Every layout-changing commit bumps its own TUI_API_VERSION and re-pins the asserts: task 2
  (additive pair fields) bumps to v6, task 4 (ordinal removal) bumps to v7 - the version always tells the
  truth about the layout it labels, which is what N1's per-commit agreement requires.
- C2 The ~20 Hz C-side stream tick throttle stays; D keeps sending chunk deltas.
- C3 The now bar's phase line, chip strip, logs, runtime status, and the agents tab lanes are separate
  surfaces and unchanged; this design covers the thread view, the drill-down (via inheritance), and the
  composer button.
- C4 No new per-tool execution-start ABI event exists; status transitions keep riding stream-done, result,
  and stop events (v5 decision, unchanged).
- C5 The v5 emission architecture stands: typed turns/blocks, upsert keyed by (turnId, block id),
  batchId = "<turnId>#<batchIndex>", group id = persisted turnId, D emits everything mode-independently,
  displayability filter C-side. v6 only changes what the appendix changes.

## 3. Architecture

```
D agent run                          ABI (flat C)                      C++ workbench
--------------                        ----------------------             --------------------------------
TurnEmitter (v5, amended)             tuiSetTurns / tuiTurnUpsert        TypedThreadStore (v5, amended)
  - turn upsert WITHOUT ordinal       tuiBlockUpsert / tuiTurnRemove       - TuiBlock gains result pair fields
  - tool blocks carry RESULT pairs      tuiThreadClear                     - TuiTurn loses ordinal
    (sorted, capped, moreCount)                                          |
  - uiAgentBusy clears readyStatus    (busy edge: NEW body)              ThreadStoreAdapter (seam kept)
TuiChannelSink.busy() -> uiAgentBusy                                       - query/answer project as turn-level rows
                                                                           - group row list = work rows
                                                                           - no "[N]" prefix
                                     Composer button                    ThreadView (v6 projection)
                                       reads readyStatus (unchanged)      - query-first anatomy, two modes
                                     /plan /code handlers                 - D3 fold defaults, D2 live edge
                                       bracket pipeline runs (NEW)        - D4 card shape
                                     D-side pair unpacker (NEW)         Inspector (unchanged); console formatter
                                       JSON object -> sorted pair set     output unchanged (F9)
```

- The D side keeps the v5 TurnEmitter; v6 amends two payloads (turn upserts lose the ordinal; tool
  blocks gain the RESULT pair set) and fills the empty busy half. A small D-side unpacker converts a
  JSON-object tool result into the sorted, capped pair set at emission time (live and replay).
- The ABI gains the pair fields on the tool block (additive, task 2 -> version v6) and loses the turn
  ordinal (breaking, same-iteration flip, task 4 -> version v7); each layout-changing commit bumps and
  re-pins per C1.
- The C++ store keeps its topology and upsert path; it stores the pair set on the tool block.
- The adapter keeps its seam but changes the projection: the query and answer blocks surface as
  turn-level rows outside the group, the group's row list carries only work rows, and the ordinal
  prefix logic is deleted.
- The thread view re-derives fold defaults (D3), the live edge (D2), and the card shape (D4) from the
  same store state v5 already emits - D2/D3/D4 need no new D-side events.
- The composer button logic is untouched (flip logic is golden-green); what changes is that a real
  edge finally drives it, from every entry point (D6).

### What dies
- D-side: the turn ordinal assignment and its UiTurnData field.
- C-side: the "[N]" prefix on the query row and the dead GroupView.ordinal field, the
  `defaultOpen = !groupMinimal && kind != Thinking` maximal rule, the maximal batch anatomy's collapsed
  thinking + all-cards-open shape, the tool card's status glyph and "N in / N out" token header counts,
  the repeated "name(" body prefix, the JSON-object RESULT blob path (the packed raw fallback stays - D4),
  and v5's dim Pending chip (Pending calls draw no chip - F2).

### What stays
- Typed store, upsert merge, batchId, turnId-keyed groups and overrides, eviction, the
  displayability filter, stop/cancel semantics, compression notice handling, the inspector, the
  agents tab, the composer flip logic, the console formatter's output.

## 4. Data model (v6 deltas over v5)

- TuiTurn loses `ordinal` (and the adapter's `GroupView.ordinal` dies with it). State and messageCount
  keep their v5 meanings; the group id remains the persisted turnId.
- Turn ordering (the question the ordinal answered): live turns order by the store's turn-array insertion
  order - a turn upsert appends on first sight and a re-emitted turn's upsert keeps its slot by turnId,
  so compression re-emission and an emitter re-attach after a session switch cannot reorder; replay
  order comes from the history sequence. A store test pins both (section 8).
- TuiBlock (tool kind) gains the RESULT pair transport, per D4's raw-data rule:
  - `resultKeys: String[]`, `resultValues: String[]` - parallel vectors, same length, sorted by key
    (byte-wise, ascending); duplicate keys collapse last-wins; values truncated at 200 characters
    (codepoints - never splitting a UTF-8 sequence) by the sender.
  - `resultMore: u32` - the number of pairs past the 100-pair cap (0 when under), so the renderer
    draws one dim "... K more" tail without counting.
  - `resultRaw: String` - the packed fallback: when the result is not an unpackable JSON object
    (array, non-JSON, zero-pair object), the pairs stay empty and `resultRaw` carries the one-line
    string the renderer draws as-is (today's dim RESULT line). This keeps every shape across the seam
    as data - a packed string that the C++ side would have to parse is exactly what produced the blob.
- Field placement rides the C1 pattern: append at the end of the layout-pinned struct, bump
  TUI_API_VERSION per layout-changing commit (task 2 -> v6, task 4 -> v7), re-pin offsets on both
  sides in the same commit.
- Token fields (`tokensIn`, `tokensOut`) stay layout-pinned on the wire - production never sets them
  and the renderer stops drawing them (I4); removing them would cost a second breaking change for no
  rendering gain.
- Everything else in the v5 model (kinds, statuses, flags, batchId, upsert semantics) is unchanged.

Design decision: the pair set is computed once on the D side at block emission (live step completion and
replay alike), not per render. The unpacker walks the JSON object one level, sorts keys, applies the caps,
and fills the vectors; C++ renders strings verbatim and never re-parses (D4 transport rule, N4).

Design decision: the caps live with the unpacker (I5). D sends at most 100 pairs and truncated values plus
the moreCount; C++ cannot render past what it was given, so a value can never un-cap on a later frame, and
the store's memory per tool block is bounded (N2).

Design decision: the pair set replaces atomically on merge - an upsert carrying a pair set replaces all four
result fields together, never field-by-field (a store test pins it, section 8). The caps are RESULT-only:
the card body's arguments keep the existing D-side summarizer and its 1000-char per-value cap.

## 5. Emission paths (D side)

- Live turn: unchanged v5 shape, minus the ordinal on the turn upsert. Stream ticks upsert content and
  Pending tool calls as today; stream-done flips to Running; the step-completion walk emits merged tool
  blocks - v6 adds: the result's pair set (unpacked, sorted, capped, moreCount) or the packed fallback,
  per section 4. The Answer block and final turn upsert unchanged (no ordinal).
- Replay / session switch: unchanged v5 walk, minus the ordinal, plus the same pair computation for each
  stored tool result (deterministic from the stored payload; replay parity holds - F9/testing).
- Interrupted-turn replay: in-flight calls replay as Cancelled (v5, unchanged); a Cancelled tool block
  carries no result pair set (there is no result) - its card renders header-only with no FAIL and no
  RESULT row, exactly as today renders it.
- Busy edge: `uiAgentBusy()` gains its body - clear the ready flag (mirroring what `uiAgentReady()` already
  does in the other direction). The brackets become exception-safe (scope exit) so a run that unwinds still
  restores ready. Pre-step before any of this: enumerate every busy()/runInFlight-raising path,
  search-verified - no truncated greps (the lesson that once hid package.d:1434) - covering one-shot
  mode, drag&drop-triggered runs, and session resume. At-most-one-run is the stated assumption (F6); if
  the enumeration finds a path that can overlap a live run, the bracket becomes a nesting counter. A Stop
  click targets the run in flight (the global stop-agent path) regardless of the session shown. Entry
  points:
  - Main query (userQuery) and compression (doCompress): already call busy()/ready() around their runs -
    they start working the moment the body exists (D6).
  - /plan and /code pipeline slash handlers: raise busy() before the pipeline run and ready() after, in all
    paths (I6). The handlers own the UI lifetime of the run; the pipeline engine stays UI-agnostic (appendix -
    rejected alternative).
- Run-in-flight predicates (stop affordances) switch to reading the ready flag (they revive with it per D6);
  `runInFlight` bookkeeping that duplicates the flag collapses onto it where the appendix's "single source of
  truth" requires.
- The runtime-status phase edge (sendRuntimeStatus) is untouched and stays display data (appendix - rejected
  alternative: deriving the button state from it).
- Ordinals: the emitter stops assigning them; no displayability or counting logic changes (D8 restates the v5
  weights - nothing to re-implement, only to pin in tests).

## 6. Presentation (C++ thread view)

- Turn anatomy (D1): the adapter's projection changes - the query block renders as the turn's opening row,
  above the group header; the answer renders as a turn-level row after the group; the group's row list carries
  the work rows (thinking, tool steps, streamed prose; notices keep rendering as today, I1). The store data is
  untouched - the query remains the turn's first block; this is projection + render order only. The minimal fold
  keeps query and answer visible always; the WORK header renders between them only while the group has rows.
- Live edge (D2, both modes): in minimal the folded group stays header-only (unchanged) and the live turn
  draws exactly one live element at the group's live edge - directly under the header (where no other rows
  render); in maximal at the open group's bottom (the appendix's maximal sketch shows the same line):
  - the chips line while at least one call is Running: one chip per running call, tool name only, orange,
    not clickable, folding the moment the call goes dormant (result lands, mid-batch included); Pending
    calls draw no chip (v5's dim Pending chip dies - stated change);
  - a folded Thinking row while the model streams thinking: count only, no summary - "Thinking" bare when the
    response carries no calls yet, "Thinking - N tool calls" once calls exist (I3; the pending calls of the
    streaming response count, per the v5 store state);
  - nothing while the model streams prose with no tool running;
  - the group's step count updates live while tools run; when the turn finishes the live element folds away and
    the group keeps its count. The v5 `task_done` chip rule is unchanged (the streamed answer shows as the
    chip until its result arrives).
- Maximal mode (D3): the group renders open for the whole live turn (the group's own disclosure state is
  unchanged - v6 moves the thinking and card defaults, not the group's):
  - one Thinking row per response batch, grouping that response's thinking with its N calls; thinking renders
    open by default; the label is the count only;
  - tool cards auto-open only in the newest batch; older batches' cards render as folded headers; no second
    count row inside the group - the header's step count is the only count;
  - when the turn finishes the group folds to its header with the counts - the same finished shape minimal
    shows; the fold applies only to groups at their mode default, so a passive maximal watcher sees the open
    live tail fold at completion (recorded accepted consequence, not a bug) while an explicit mid-stream
    manual open (a user override) survives it; re-expanding a finished group renders every Thinking row open
    with tool rows as folded headers - nothing re-opens, the newest-batch rule has no bearing on a finished
    turn;
  - opening the group mid-stream re-applies the mode's fold defaults (maximal opens every Thinking row, minimal
    keeps them folded) and renders the newest batch's full details open in both modes - the one live exception.
- Tool card (D4): header = disclosure arrow + "TOOL <name>" while streaming / "TOOL <name> - <duration>" once
  the result lands, no status glyph; a failure renders textual FAIL (red) on the header, folded or open; no
  token fields. Open body = arguments only, one `key=value` pair per line (no "name(" prefix, no "[N]"). The
  embedded RESULT row = one dim `RESULT <key> = <value>` line per pair (values stringified, nested values as
  compact JSON text), the packed fallback line for arrays/non-JSON/zero-pair objects, and the "... K more" dim
  tail when moreCount > 0. Card click behavior matches the Thinking rows (same disclosure shape, F3). A
  Cancelled card renders header-only, no marker - recorded decision (N3 honesty; golden-visible via the
  interrupted-turn replay frame).
- Counts (D8): the WORK header keeps "WORK - N steps" (v5 weights); the Thinking row's N is tool calls; the
  two never mix - pinned by tests, no rendering change expected.
- Overrides (v5 F7, unchanged mechanics): opening a group in minimal / collapsing one in maximal writes the
  session-local override keyed by the turn id; it beats the mode defaults. What "open" renders is the D3 shape
  above (mid-stream: mode defaults + newest details open; finished: thinking open, cards folded).
- Drill-down (D7): renders a per-agent typed store through the same adapter; inherits every rule above. It keeps
  having no query row - its turns are seeded by the pipeline stream path with no query block (correct, unrelated).
  Pipeline-specific shaping stays inside the adapter; wrong data is fixed at population, never in the view.
- Composer: the flip logic (`busy = not ready`) is untouched and stays golden-covered; v6 only supplies the real
  edge (section 5). With the flag finally moving, Stop shows during main queries, compression, and /plan//code
  runs, and Send returns when the run ends - including after a failure or a stop (exception-safe brackets, I6).

## 7. Failure modes and edge cases

- Result shapes: JSON object with nested objects -> pairs with compact-JSON values; array or non-JSON ->
  packed raw line; zero-pair object -> `RESULT {}`; oversized -> capped pairs + "... K more". No shape
  reaches C++ as a string it must parse (D4).
- Result for an unknown callId (live): unchanged v5 behavior - recorded under its callId, merged when the call
  lands; the pair set rides the same upsert.
- Hidden/filtered tools: filtered C-side at render/weight time as today; their pair set costs nothing at render.
- Stop mid-run: in-flight calls -> Cancelled; the partial answer stays; the composer returns to Send because the
  run's unwinding still fires ready() (exception-safe bracket).
- Compression: affected turns re-emitted (upsert/remove) as today; notices keep rendering; no ordinals to
  recompute (F5 removes the recomputation v5 required).
- Upsert storms: idempotent merges, throttled as today; the pair set is part of the merged tool block and
  never grows per frame.
- Session switch mid-run: store rebuilt from snapshot, emitter re-attaches by turnId (v5 shape); the ready
  flag belongs to the run, not the session - a switch during a run keeps Stop showing until the run ends,
  and a Stop click targets the run in flight (the global stop-agent path) regardless of the session shown -
  unambiguous under the at-most-one-run assumption (F6).
- Empty group (a turn with no work rows): no WORK header renders (F1) - query and answer stand alone.
- Replay of a turn that ended without a final answer (open question 6.4): renders as today; the caveat is
  recorded, not assumed - if it ever reproduces as a fold bug it is filed separately.
- Busy edge re-entrancy: busy() while already busy is an idempotent clear; ready() sets the flag; the flag is
  a bool and the predicates that read it are frame-safe. The brackets are non-refcounted by design under the
  at-most-one-run assumption (F6); the enumeration's fallback is a nesting counter if that assumption ever breaks.

## 8. Testing strategy

- Wire (C1): re-pinned static asserts D-side and offset tests C-side for the v6 struct deltas (pairs added,
  ordinal removed); TUI_API_VERSION bump covered by the existing contract test pattern.
- D-side emission tests (recording sink): turn upserts carry no ordinal; tool blocks carry the sorted pair
  set (byte-wise key order, duplicate-key last-wins, codepoint-safe 200-char value truncation, 100-pair cap,
  moreCount), packed fallback for array/non-JSON/zero-pair, nested values as compact JSON; replay emits identical
  pair sets (parity); Cancelled calls carry no pairs. Cap boundaries pinned: exactly 100 pairs -> moreCount = 0,
  101 -> 1; a value at exactly 200 characters truncates nothing; a multi-byte UTF-8 character at the cut stays
  valid.
- Busy-edge tests (D-side unittests): uiAgentBusy clears the ready flag (today nothing asserts the busy half -
  this is the regression test for the bug); userQuery/doCompress brackets; /plan and /code handlers raise the
  edge; an exception unwinding out of a run still restores ready (I6); stop affordances read the flag; the
  task-1 enumeration's verified entry-point set is listed in the acceptance and each named path has a test.
- Store unit tests: pair fields stored and merged on the tool block, the pair set replaced atomically (all four
  result fields together); ordinal gone from the turn view and GroupView; turn order pinned across a compression
  re-emission and an emitter re-attach (insertion order held, no reorder); step weights unchanged (D8 pinned:
  call+result = 2, prose = 1; a thinking+2-calls response counts 0 steps by itself and its calls 2/1 while live -
  the units never mix).
- Adapter/view tests: query-first ordering (query above header, answer after the group); header suppressed for an
  empty group; minimal fold shape; live-edge element selection (chips / thinking row / nothing) and folding on
  dormancy incl. mid-batch; maximal newest-batch auto-open; finished-group fold + re-expand shape; mid-stream
  open in both modes; override interplay (ForceOpen/ForceClosed still beat defaults); the W4 rules pinned (no
  Pending chip; an explicit mid-stream manual open survives the finish fold; the live edge draws in both modes);
  no "[N]" anywhere.
- Golden frames (appendix-mandated pattern, regolden + eyeball the diff): minimal idle; minimal live turn with
  mixed states (chips + live count + streamed-thinking row); post-completion fold; maximal live long turn (the
  wall scenario, now thinking + newest batch open); maximal finished group re-expanded; maximal mid-stream opened
  in minimal and maximal; failed tool header (FAIL red) folded and open; RESULT unpack shapes; composer Stop
  during each entry point's run, Send after - driven by the real D-side edge in the harness (not by poking the
  C++ flag directly, which is how today's composer-busy golden masks the bug).
- Console parity: scripted session renders byte-identical lines to today's output (F9).
- Cross-cutting battery: replay parity x2 with expansion preserved; upsert storm; stop mid-batch -> Cancelled +
  Send restored; compression re-emission; 1000-message session open/scroll; unknown-callId result; session switch
  mid-run (flag stays busy until the run ends).

## 9. Rollout and commit shape

The task list (section 10) lands in five commit groups; each is independently green:

1. P0 busy edge (task 1, D-side only, no wire dependency) - the one true bug, shippable alone.
2. P0 additive (tasks 2-3): pair fields + version bump to v6 + asserts + bindings, then the D-side pair
   emission behind the recording sink; both sides build, nothing consumes the pairs yet, existing goldens
   pass unchanged.
3. P1 cutover (task 4, one commit, both sides): ordinal removal + version bump to v7 + the v6 projection.
   Existing minimal goldens regolden with only the expected label/order deltas; the maximal-anatomy golden is
   regenerated here or at task 6 (whichever lands its shape).
4. P1 rendering (tasks 5-6): live edge + card shape; maximal rebalance.
5. P2 hardening (task 7), then P3 doc sweep (task 8).

## 10. Task list

## Critical (P0)
1. **Task**: Repair the busy edge (D6). Pre-step: enumerate every busy()/runInFlight-raising path,
   search-verified - no truncated greps - covering one-shot mode, drag&drop-triggered runs, and session
   resume; then give `uiAgentBusy()` its body (clear the ready flag), bracket the /plan and /code pipeline
   handlers with busy()/ready() (exception-safe), point run-in-flight predicates (stop affordances) at the
   ready flag, and raise the edge at any additional path the enumeration names (a nesting counter instead if
   any path can overlap a live run).
   - **Acceptance**: the verified entry-point set is listed and each named path raises the edge (D-side
     unittests, including an exception unwinding); the composer flips to Stop during a main query, compression,
     and a /plan or /code run, and back to Send after - driven by the real edge; phase edge untouched; the
     at-most-one-run assumption stated or the nesting counter landed.
   - **Dependencies**: None

2. **Task**: ABI v6 additive plumbing - append the RESULT pair transport to the tool block (resultKeys,
   resultValues, resultMore, resultRaw per D4/I5), bump TUI_API_VERSION to v6, update D bindings + static
   asserts and the C++ store/struct mirrors (fields unwired).
   - **Acceptance**: contract tests pin the new offsets both sides; both sides build; existing goldens pass unchanged.
   - **Dependencies**: None

3. **Task**: D-side RESULT pair emission (D4) - unpack a JSON-object tool result one level, sort by key, apply
   the 100-pair / 200-character caps + moreCount, packed fallback for array/non-JSON/zero-pair; wire it into
   the live step-completion walk and the replay walk; console formatter passes lines through unchanged.
   - **Acceptance**: recording-sink tests cover every result shape + caps + replay parity; console parity golden
     unchanged; Cancelled calls carry no pairs.
   - **Dependencies**: Task 2

## High (P1)
4. **Task**: Cutover commit (one commit, both sides) - remove the ordinal from TuiTurn/UiTurnData, the D
   emitter, and GroupView.ordinal, delete the "[N]" prefix on the C++ query row, bump TUI_API_VERSION to
   v7, and flip the adapter/thread view to the query-first anatomy (D1): query and answer project as
   turn-level rows, the group row list carries only work rows, the WORK header renders only while the
   group has rows (I1).
   - **Acceptance**: no ordinal references remain either side (search-verified, GroupView.ordinal included);
     offsets re-pinned for v7; build green; regoldened minimal goldens show only the expected label/order
     deltas; empty group renders headerless.
   - **Dependencies**: Task 2, Task 3

5. **Task**: Minimal-mode live edge + tool card shape (D2 + D4): the live-edge element selection in both
   modes (orange name-only chips while a tool runs, folded count-only Thinking row while thinking streams,
   nothing while prose streams, live step count, fold on finish - and no Pending chip for queued calls); the
   card rebuild (disclosure arrow, no status glyph, FAIL red on failure, Cancelled header-only, no tokens, args
   one pair per line at the summarizer's 1000-character cap, dim RESULT key = value lines + "... K more" tail).
   - **Acceptance**: golden frames for minimal idle, live mixed states, post-completion fold, and the card
     shapes (success, failure folded/open, Cancelled header-only, every RESULT shape incl. caps); click
     behavior matches Thinking rows.
   - **Dependencies**: Task 4

6. **Task**: Maximal-mode rebalance (D3): per-response Thinking rows (count-only label, trailing summary removed),
   thinking open by default, newest-batch-only card auto-open, no second count row, live fold-on-finish,
   re-expand shape, mid-stream open rule (mode defaults + newest details open in both modes), override interplay.
   - **Acceptance**: golden frames for the long-turn wall (thinking + newest batch open, older cards folded),
     finished group folded, finished re-expand, mid-stream opened in minimal AND maximal; overrides still beat
     defaults and an explicit mid-stream manual open survives the finish fold (W4); the maximal-anatomy golden
     regenerated and eyeballed; the watcher-tail fold-at-completion behavior asserted as the recorded accepted
     consequence.
   - **Dependencies**: Task 4

## Medium (P2)
7. **Task**: Cross-cutting hardening battery: replay parity x2 (expansion preserved), upsert storm, stop
   mid-batch (Cancelled + Send restored), compression re-emission, 1000-message open/scroll, unknown-callId
   result, session switch mid-run (flag stays busy until the run ends), and the busy-edge battery across all
   entry points.
   - **Acceptance**: all listed scenarios green in the headless harness; D-side suite green (dub test);
     C++ suite green (incl. xvfb GUI tests).
   - **Dependencies**: Task 1, Task 5, Task 6

## Low (P3)
8. **Task**: Doc sweep - doc/tui_design.md (C API boundary: ordinal gone, RESULT pair transport added, busy
   edge real), doc/todo_modern_ui.md composer notes (button flips via the real edge), and any todo/doc lines
   still describing the v5 shapes this design replaces.
   - **Acceptance**: no doc reference to the "[N]" ordinals, the empty busy half, or the v5 maximal default
     remains; goldens dir README/comments (if any) match the v6 shapes.
   - **Dependencies**: Task 7

## Notes
- Execute tasks in order; tasks 1 and 2 are independent and can land in either order.
- Each task produces code + tests; tasks are design-level here - the implementation-plan step breaks them down.
- N1 forbids a dual-source window: the ordinal removal cannot split across commits (task 4 is one commit).
- The v5 deferral notes (todo_modern_ui.md thread section, doc/todo.md live-tool/stats lines) stay delivered;
  task 8 only updates what v6 changes.
- Open question 6.4 stays a recorded caveat: no fold-bug work is planned on an unconfirmed report.
- Appendix open questions 6.1 (status glyph) and 6.2 (ordinal as feature) are closed by this design: 6.1 by the
  D4 no-glyph card, 6.2 by the D5 ordinal removal.

## 11. Out of scope

Pipeline engine internals (the engine stays UI-agnostic), agents-tab lanes and inspector, the now bar, session
list, chip strip, logs, runtime-status models, cross-turn search, tuiMoveCall, any config-selectable legacy path,
removing the token fields from the wire (I4), and any fix for the unconfirmed 6.4 scenario.
