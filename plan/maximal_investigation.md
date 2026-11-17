# Maximal mode: diagnosis, agreed design, implementation guidance

Investigation of HEAD `a1e7743` (branch `update-ui`). This file absorbs the
`appendix_more_tui_fixes.md` chapters (D11's turn-lifecycle bug record, 4.12;
D12's maximal-mode violations, 3.1-3.4) and supersedes their fix decisions.
Source references are exact; the grids quoted are probe output from this tree,
not sketches.

Design settled over four rounds: the fold owner is the response (round 2), the
`AGENT` row with chips, the content as a first-class part of the response,
displayability as a wire flag, the duration on the `AGENT` row (round 3), and
the log form for settled responses plus the group folding when the turn ends
(round 4, 9.1). Section 7 is the implementation guidance; section 9 records the
assessment and the shape the user judged.

## 1. How this was verified

- Read the plan trail (`appendix_more_tui_fixes.md` before absorbing it,
  `previous_1`, `previous_2`).
- Read the whole render path: `cpp_tui/store/thread_store.cpp` (the fold
  derivation), `cpp_tui/views/thread.cpp` (the draw), `cpp_tui/views/blocks.cpp`
  (the header text and the row chrome), `cpp_tui/model/block.h` (the
  vocabulary), `cpp_tui/store/typed_thread_store.cpp` (the store).
- Read the D emitter: `source/llm/app_agent/ui.d` (stream ticks),
  `source/llm/app_agent/package.d` (live step walk, stop, replay),
  `source/llm/agent/package.d` (the run loop).
- Rebuilt the C++ goldens (`build/tui`, `make test_blocks`): 424 checks pass,
  so the goldens describe the shipped behaviour, not a stale build.
- Ran four temporary probe scenes in maximal mode (a live batch with reasoning
  and one call, `Running` and `Pending`; a live batch with content + reasoning +
  one call; a finished turn whose response called `task_done`), dumped the
  grids, then reverted the scenes and the goldens. Every grid quoted below is
  that run's output.
- Not verified at runtime: the D-side claims in 4.6/4.7 are read off the code
  and the existing unit tests, not reproduced against a live model.

## 2. What the pipeline actually does

D emits, per turn: one `Prose` query block (`meta = "user"`), then per LLM
response a batch of blocks sharing a `batchId = "<turnId>#<batchIndex>"` - the
response's `Reasoning` block (only when the model emitted reasoning), its
`Prose` content block, and one `Tool` block per call - then, when the response
called `task_done`, the `Answer` block and the turn's close. Batches are per
response, not per call: every call of one response carries the same `batchId`
(`source/llm/app_agent/ui.d:787`).

C++ projects that store in three steps:

1. `ThreadStoreAdapter::syncTyped()` - one group per turn, one flat row per
   block, in insertion order (`store/thread_store.cpp:175-383`).
2. The fold derivation - `blockExpanded()` / `expanded()` / `groupMinimal()`
   (`store/thread_store.cpp:460-705`).
3. The draw - `renderThreadBody()`, a row loop over the group's rows
   (`views/thread.cpp:446-719`), with `blockHeaderText()` composing each
   header (`views/blocks.cpp:189-239`).

Every symptom below comes from steps 1 and 2; the renderer only draws what the
derivation says.

## 3. The reported symptoms: all confirmed

### 3.1 "Thinking is always folded"

`store/thread_store.cpp:659-660`:

```cpp
const bool defaultOpen =
    !groupMinimal(entry.view.id) && block->kind != model::BlockKind::Thinking;
```

`Thinking` is the one kind excluded from maximal's open default, i.e. the v5
rule that D3 (`previous_2/design_notes.md`) reversed and the next round restated
(3.1-3.4 below are that chapter's four violations, re-verified against HEAD). The
same expression is repeated for the write path at `:695-696`.

The probe, maximal, one live call, with reasoning:

```
 YOU
 sweep the tree
v WORK - 2 steps
 AGENT
> Thinking - 1 tool call
  v TOOL  bash
   ls -R
  [bash]
```

The thinking text ("working out the tree") is nowhere on screen while it is
being written.

The goldens pin this shape, so they are also the ones that must move:
`blocks-maxanatomy-100x30.txt` (a finished maximal turn: folded `Thinking` row,
open cards, open group) and `blocks-maxlive-100x30.txt` (a live maximal batch:
cards open, chips line). `streaming-thinking-100x30.txt` is the minimal live
shape (folded group, folded `Thinking` row, no body).

### 3.2 "The tools never fold"

There is no batch-liveness logic anywhere in the fold derivation: the only
inputs are the mode and the row's kind (`:659-661`). A card therefore draws open
in maximal from the first frame to the end of the session. "Settled" exists
only for the chips line (4.7), never for a card's or a batch's fold.

### 3.3 "The TOOL rows are not part of the Thinking fold"

Cards are independent rows that merely hang under the Thinking row: the loop
indents a `ToolStep` row by `m.pad2` and draws a rule beside it
(`views/thread.cpp:568-575, 654-664`). Nothing connects a card's visibility to
the row above it - `blockExpanded()` is asked per row, so a folded Thinking row
hides only itself.

### 3.4 The group draws open always

`store/thread_store.cpp:645-646`:

```cpp
return minimalMode() ? chat_->openWorkGroups.count(entry.view.id) != 0
                     : true; // F5: a maximal batch draws open, always
```

No turn state, no liveness, no "fold when finished". A long maximal turn is
every card of every batch, open - the wall the earlier trail complained about.

## 4. Defects the plan does not list

### 4.1 The live "Thinking" line is drawn twice in maximal

`views/thread.cpp:503-556` selects a `liveThinking` row and draws a folded
`Thinking - N tool calls` line as the group's live edge. In minimal that line is
the only place the streaming reasoning shows (the rows are behind the group
fold), so it is right there. In maximal the row itself is already on screen, so
the same word is drawn twice. The probe, one *Pending* call (the normal state
while the response is still streaming):

```
 YOU
 sweep the tree
v WORK - 2 steps
 AGENT
> Thinking - 1 tool call      <- the row
  v TOOL  bash
   ls -R
> Thinking - 1 tool call      <- the live edge, same batch
```

That is the "even more stupid now" reading: two folded Thinking lines and not
one character of the thinking.

### 4.2 An empty content block draws a bare `AGENT` row

The live stream upserts `content#N` on every tick, and on the first tick its
body is empty (`source/llm/app_agent/ui.d:777-779`). The row is built anyway
(`blockDisplayable()` filters hidden tool names only,
`store/typed_thread_store.cpp:11-18`), so maximal draws an `AGENT` row with no
body - line 4 of the probe grid above. In minimal the row is invisible only
because the group is folded; open the group and it is there.

`cpp_tui/test_blocks.cpp:1611` asserts "an empty stream draws no agent body row"
and its comment claims the store drops it - it does not, the test passes on the
fold.

### 4.3 The response's content is invisible in minimal mode while it streams

This is the regression against master the user described, and it is bigger than
the fold discussion. The `Assistant` (content) row is a work row, so minimal
mode hides it behind the group header; the live edge only ever draws a chips
line or the bare `Thinking` shadow. The golden that pins today's shape,
`streaming-live-100x30.txt`, is a scene streaming
"The answer is **still** arriving..." with reasoning "Read the request." -
and the screen is:

```
 YOU
 stream this
> WORK - 1 step
> Thinking
```

No answer text at all, and the word on screen says "Thinking" while the model is
writing its answer. `streamHasText()` exists but only feeds the now bar's phase
clock (`views/shell.cpp:622-624`), never the thread.

Master showed the assistant's text as a message body; the redesign shows it
nowhere until the `Answer` block lands. The fix is part of this design (5.3).

### 4.4 A `task_done` response repeats its text three times in maximal

Probe, maximal, a finished turn: the streamed content is the answer's text, the
card carries the same text as its arguments and as its result, and the `Answer`
row repeats it.

```
 YOU
 what is the answer
v WORK - 3 steps
 AGENT
 ANSWER-TEXT
  v TOOL  task_done - 3.2 s
   ANSWER-TEXT
   RESULT  ANSWER-TEXT
 ANSWER
 ANSWER-TEXT
```

D's `hiddenTools` default is `["taskDone", "pipelineOutput"]`
(`source/llm/config.d:637`), and the C++ mirror deliberately un-hid
`task_done` (`store/typed_thread_store.h:48-57`: "v5 renders the task_done
step; its answer body arrives as the Answer block"). In minimal that is
invisible; in maximal it is three copies of the same sentence per turn.
Fix: not another name list - a wire flag (7.1, Q1 in 8) so D alone decides.

### 4.5 A batch's rows reorder between live and replay

Live path: content block first, then reasoning (`ui.d:775-783`). Replay path:
reasoning first, then content (`source/llm/app_agent/package.d:1074-1080`).
Insertion order is what the store keeps (`blockUpsert()` merges in place,
`typed_thread_store.cpp:72-97`), so a live turn draws content above thinking
and the same turn reloaded draws thinking above content.

The design's canonical intra-batch order (5.2) removes this in the projection,
which is the cheaper place: the store emits thinking, content, cards regardless
of insertion order. The D-side reorder then becomes optional (still worth doing
for the console formatter's parity, but it is no longer load-bearing).

### 4.6 The test seeder disagrees with D about batch ids

`cpp_tui/test/typed_seed.h:151-163` increments `liveBatch` per call, so every
card in a seeded turn gets its own `batchId`. D gives every call of one response
the same id. The `livebatch`, `maxlive` and `maxcancelled` scenes hand-write
per-card ids too (`test_blocks.cpp:1928-2030`). Any batch-keyed fold developed
against these fixtures is developed against the wrong data: fix the seeder with
the fold change, not after it.

### 4.7 The chips line draws under any folded group

`views/thread.cpp:671-680` draws the live chips line under *every* collapsed
group header in the window; only the maximal path is guarded by
`index == drawn.back()` (`:714`). Any block left `Running` in an older turn
paints a chips line under that old header - the reported "three WORK headers up"
symptom (4.12), and it is minimal-mode-only (the missing newest-group guard;
maximal groups were never folded).

### 4.8 A failed run never closes its turn

`source/llm/app_agent/package.d:970-974`: `emitStopTyped()` runs only when the
user stopped; the final answer's `Done` is the only other close. The run loop
exits on `unknownFailure`, `networkFailure` and `agentStuckInLoop` without either
(`source/llm/agent/package.d:612-657`). The turn stays `Running` forever, so
`hasStream()` stays true and the last group reads live for the rest of the
session. This is the root of the leaked-chip symptom (4.12), and it also blocks
the new fold: "the batch's last call responded" would never make the batch stop
being the live tail if the turn itself still says "running".

### 4.9 The orphan path resurrects a live call

`package.d:1125-1140`: a response whose call is not in the same step emits a
result-only block and never calls `ctx.clearLiveCall()`. The next
`streamMessageDone()` re-upserts every recorded live call as `Running`
(`ui.d:793-799`), so a chip that had folded comes back.

### 4.10 The nesting cost in the appendix is overstated

`appendix_more_tui_fixes.md` (since absorbed here) called the height contract
"the hard part, because the height reservation contract flips". There is no such
contract:
`measureHint()` feeds `cell.h` (`views/thread.cpp:454`), and `beginGroup()` uses
`cell.w` and the unit only - `cell.h` never reaches a layout decision
(`store/thread_store.cpp:780-817`; `rectMatchesProfile()` compares
`r.cells == m.textGrid` and nothing else, `ui/layout.cpp:277-281`). The drawn
height is measured after every draw and replaces the estimate (`:846-871`). A
folded batch therefore only makes the *first* frame's rough estimate
(`blocks.size()`) generous, and `test_streaming.cpp:660` ("the hint is the
estimate, and does not move with the fold") keeps passing untouched.

### 4.11 Dead state

`UiState::thread.thinkingOpen` is cleared on reset and never read
(`model/ui_state.h:256`, `store/thread_store.cpp:904`). Nothing needs it once
the batch fold exists.

### 4.12 The turn lifecycle behind 4.7-4.9 (absorbed from the old D11 chapter)

The status lifecycle rides events only, and every event is optional:

- **Pending** rides the stream tick: `messageUpdate` upserts each streamed call
  with a non-empty id as `Pending` and records it in `ctx.liveCalls`
  (`ui.d:785-790`).
- **Running** rides stream-done: `streamMessageDone` re-upserts every recorded
  live call as `Running` - every one, not just the batch that just finished
  (`ui.d:793-799`).
- The **terminal status** rides the step walk: the `ToolMessage` branch upserts
  the call and its response as one block keyed by the wire call id, `Ok` or
  `Failed`, and clears that live call (`package.d:1082-1124`). A response whose
  call is not in this step emits a result-only block and never clears its live
  call (4.9).

The three leak paths, each a way for the loop to end without the terminal event:

1. `unknownFailure` / `networkFailure` end the loop with `keepRunning = false`.
   A step that streamed calls and then failed mid-stream - the response never
   assembled, the harness never ran - leaves those calls `Running`. `process()`
   assigns `rval.chat` only on its success path
   (`source/llm/agent/package.d:431-434`), so a failure status carries an empty
   chat: the step hook still fires for the failed step, walks nothing, and no
   call is finalized. What flips those calls from `Pending` to `Running` is the
   `scope (exit)` beside the request, which calls `streamMessageDone()`
   unconditionally (`:372-376`) - so a failed request also "completes" its
   stream, and the leaked calls look live rather than pending.
2. `agentStuckInLoop` sends only the console message: it is not a stop, so
   `emitStopTyped()` never runs, the live calls never flip and the turn never
   closes. The only closes today are the final answer's `Done` and the stop's
   `Interrupted` (4.8).
3. The orphan path adds a resurrection: a folded chip comes back on the next
   batch's stream-done, and keeps coming back while the context survives (4.9).

Why it reads as random: the store keeps what it is told (`blockUpsert` merges by
id and the last status wins) and the view re-reads every frame, so a stale
`Running` block draws as a live chip under its own group's header - a group
below the newer turns, which is why the line lands three headers up. Which group
goes stale depends on where in the run the turn died, so the position moves
between runs and recurs on every run that dies through either path.

The old chapter's fix was "no turn close on failure, and flip the stale Running
blocks to `Cancelled` in C++". Both are superseded (5.4): no fabricated state,
and the turn closes at the source instead, which is also what lets the fold read
"live". Its other record still stands and is carried here: on turn close D
upserts the `Answer` block (kind `answer`, the latest `task_done` body, and
during a step `task_done` is an ordinary tool block), no new `TuiTurnState` was
added, and until now nothing in the C++ view branched on the turn state - the
fold deliberately does, as a derived read of one fact, never a second copy of D's
semantics.

## 5. The design (agreed in discussion)

### 5.1 The row that owns the fold is the response, labelled `AGENT`

The user's sketch, adopted:

```
v AGENT - [bash] [grep]
  > Thinking
  <content if there was any in the JSON response>
  v TOOL  bash
      ls -R
  v TOOL grep
      <args>
      RESULT ....
```

- One **batch row** per response, labelled `AGENT` (it is an assistant
  response), with the batch's calls as chips beside the label. This row is the
  fold owner. It exists even when the response wrote no content and emitted no
  reasoning - which is exactly the `blocks-maxlive-100x30.txt` shape today (two
  cards, no Thinking row), the case the "Thinking owns its batch" idea could
  not cover.
- Its children, in this canonical order (the model's own order, and stable
  between live and replay):
  1. `Thinking` - the reasoning, its own disclosure (no tool count any more; the
     chips are the count - and no duration, the response owns it).
  2. the content - a body segment, no header of its own (the owner's label is
     the role), drawn at the batch indent.
  3. the tool cards, each its own foldable.
- A batch that is nothing but a content message (no reasoning, no calls) draws
  as `v AGENT` plus its body: master's message shape, unchanged.
- Ink: label in `roleAgent`, chips quiet (`textMid`), the `FAIL` word of the
  owner header in the failure ink when any of the batch's calls failed (the
  cards' own failure word is inside the fold, so the folded header must carry
  it, or a failed batch is silent).
- The owner carries the response's duration (moved off the `Thinking` row):
  `AGENT · 3.2 s · [bash, grep]`. While the response is live there is no
  duration yet - it lands with the step - so the live header is `AGENT ·
  [bash]` and the settled one gains the time.

The one fork in the sketch worth naming: putting the content *after* the
thinking row costs a new rendering shape - a header-less body row with no hit
box (7.3.4) - where "the content is the owner's body" would cost nothing. I
recommend the sketch's order anyway: it is the chronological one, it is what
the live stream fills in (thinking, then text, then cards), and the copy path
is the owner's either way (Q2). If a smaller diff matters more than the order,
the alternative is `AGENT` + content body, then `Thinking`, then the cards, and
no body segment at all.

### 5.2 Fold defaults: one rule for the whole batch

```
turn live      = the newest turn's state is Running
batch newest   = the last batchId of that turn, in insertion order
batch settled  = the batch has >= 1 tool block and no call of it is live
call live      = turn live && batch newest && status is Pending or Running
batch autoOpen = turn live && batch newest && !batch settled
row open       = the user's pin if one is set, else:
                 the owner row      -> batch autoOpen
                 a child row        -> its batch open && batch autoOpen
group open     = the user's pin if one is set, else: turn live (maximal),
                 false (minimal)
```

A leaked `Running` call in a dead turn is not live (nothing closes it - 4.8),
so it cannot hold a batch open; that is what makes "all calls responded" a safe
trigger.

The resulting states:

| state | the row draws |
|---|---|
| the live batch | `v AGENT - 3.2 s - [chips]` (+ once the response ends) with its thinking open and growing, its content drawn, its cards open |
| a settled batch, inside the open group | `> 4.1 s - [bash, grep]` - the role word is dropped (5.2.1), everything inside the fold |
| a batch re-opened by hand | `v AGENT - 4.1 s - [bash, grep]`, thinking folded (`> Thinking`), content drawn, cards folded (`> TOOL  bash - 1.1 s`) |
| the finished turn's group | `> WORK - 23 steps`, everything inside (6.1) |

The user's sketch draws the third state (the mixed fold marks are the
re-opened case); the live state keeps the thinking open, which is the original
requirement that started this work.

#### 5.2.1 The role word on a batch row

A folded batch row drops `AGENT` and keeps the time and the chips
(`> 4.1 s - [bash, grep]`); an open one names its role (`v AGENT - 4.1 s -
[bash, grep]`). Reasons: the stacked settled rows are the turn's log, and a
column of identical `AGENT` words is noise (the user's call); the
`WORK - N steps` header already says whose turn it is; and every other block
row in the app shows its role when its content is visible, so the word appears
exactly when there is something to attribute.

The same turn, live versus finished (the live one keeps the index visible
because the turn is still running):

```
 YOU                          YOU
 read the tree                read the tree
v WORK - 23 steps             > WORK - 23 steps
> 4.1 s - [bash, grep]        ANSWER
> 2.7 s - [read_file]         the tree has 3 files
v AGENT - [execute_command]
  v Thinking
     rebasing the parser ...
  Next I will move the loader out.
  v TOOL  execute_command
      dub test
```

### 5.3 The content: where it lives, in both modes

The content is a child row of its batch (5.1), so maximal shows it while the
batch is live and folds it away with the batch afterwards - right for a
preamble, and the final text survives as the `Answer` row.

Minimal mode is the bug (4.3): the fold keeps the query and the answer and
hides everything else, so the assistant's streaming text never appears. Fix:
*the fold keeps the newest batch's content row too.*

```
minimal, while the model writes:
 YOU
 what is the answer
> WORK - 2 steps
 The answer is **still** arriving. A second paragraph, so the
 body has more than one line.
```

That is master's behaviour restored (the message body under its role row), with
the group header above it. When the turn closes, the `Answer` row takes over
and the content row folds with its batch.

Consequences to keep in mind:
- In minimal, the content is what the user reads live; the chips line and the
  bare `Thinking` shadow line keep their turns (chips while a call runs, the
  shadow line while reasoning streams and there is no content yet), and the
  shadow line is dropped in maximal (4.1).
- A turn that ends without `task_done` keeps its turn `Running` (4.8) - after
  the fix in 5.4 the turn closes, its batch settles, and the last text folds
  with it. That is the accepted cost of the fold-on-finish model; the text is
  one click away.

### 5.4 Liveness without fabricated state

This replaces the old D11 fix on both counts: it refused a turn close on failure
("a failed turn stays open and /c-resumable") and had the C++ store flip stale
`Running` blocks to `Cancelled`. There is no need for either once the turn
closes at the source and liveness is derived.

- Do not write `Cancelled` into the store. The store is a projection of the
  wire; a fabricated terminal state later reads as truth. Derive `call live` as
  in 5.2 instead.
- Close the turn at the source on every run exit: `Interrupted` (never a fake
  `Done`) when the run ends without a final answer, in `runAgent` after
  `runToCompletion`. A failed turn stays `/c`-resumable either way: `/c`
  resumes the history, not the turn row.
- Clear `ctx.liveCalls` on that same close, not only in `emitStopTyped()`. The
  ledger outlives the run: a `/c` resume sends no turn start, so `typedCtx_` is
  the same object (`package.d:962-963, 1016-1025`), and the next
  `streamMessageDone()` would re-upsert the dead run's calls as `Running` - the
  same resurrection as the orphan path, one turn boundary later (4.9, 4.12).
- Re-upsert the turn `Running` when `/c` resumes it: the `/c` path sends no turn
  start (`package.d:962-963`), so without this the resumed run has no live edge
  and the fold treats it as finished.
- `clearLiveCall()` on the orphan path so a landed call cannot be resurrected
  (4.9).

With those, "live" is one fact C++ can read, and no block status is ever
rewritten by the UI.

## 6. The sub-decisions, answered

**1. The foldable row is `AGENT`, not `Thinking`** - agreed, and 5.1 is the
shape. The group draws open in maximal while the turn runs (the index must be
visible exactly when the work is happening) and folds to `> WORK - N steps`
when the turn ends - the user's second decision, which supersedes the earlier
"open always": a finished turn is then the conversation plus one row, which is
what makes a long maximal thread readable at rest, and the group pin opens it
again in one click. The two modes therefore differ in one thing only: minimal
never draws the group open by itself, maximal draws it open while the turn is
live.

**2. Pin lifetime: session-long, and every pin becomes absolute.** Both current
encodings store "the opposite of the default"
(`store/thread_store.cpp:652-661, 695-702`), which is sound only while the
default is constant. With a liveness default it inverts: pin `open` while the
batch is live, the batch settles, the default becomes `closed`, and
`pinned ? !defaultOpen : defaultOpen` answers *open* - the row reopens by
itself. The same trap now exists one level up, because the group's default also
moves (open while the turn runs, closed after): `setExpanded()` writes only
`ForceClosed` in maximal (`:508-513`), i.e. "pin against always-open", which
would flip the moment a finished group became closed by default. So both maps
become absolute: `map<blockId, bool>` for rows and `map<groupId, bool>` for
groups; a write that agrees with the current default drops the entry,
everything else keeps the user's answer until they change it or the session
does.

**3. A card returns folded when its batch is re-opened** - agreed, and 5.2
generalises it: every child of a non-live batch returns folded.

## 7. Implementation guidance

Ordered; each step has its own gate. No new `BlockKind`, no wire change, no
mode-dependent emitter: D keeps emitting everything, C++ keeps deriving.

### 7.1 D side (small, do first - the C++ side depends on the facts)

- `runAgent` (`source/llm/app_agent/package.d:954-975`): after
  `runToCompletion`, close the turn when the run ended without a final answer -
  a `turnUpsert(UiTurnData(ctx.turnId, TuiTurnState_Interrupted,
  ctx.messageCount))` next to the existing `if (stopped) emitStopTyped()`,
  **and clear `ctx.liveCalls` in the same place** (a `/c` resume reuses the same
  `typedCtx_`, so a surviving ledger would flip the dead run's calls to
  `Running` on the next stream-done - 5.4). `emitStopTyped` already does both
  for the stop path; a shared `emitRunEndedTyped(bool stopped)` covers all of
  them without inventing Cancelled statuses for calls that never ran.
- `/c` resume: in the same function, when `runAgentLoop` is forced and
  `typedCtx_` is non-null and the turn is `Interrupted`, upsert it `Running`
  again. One line, right before `emitTurnStartTyped`'s guard.
- `clearLiveCall()` in the orphan branch (`package.d:1130-1140`).
- **Displayability becomes a wire flag** (the user's rule: the C++ side must
  know nothing about `taskDone` / `pipelineOutput`). Today the knowledge is
  duplicated in three places and they already disagree:
  - `LlmConfig.hiddenTools = ["taskDone", "pipelineOutput"]`
    (`source/llm/config.d:637`) with `isHiddenTool()` (`:914-925`) - the
    configurable source of truth, and **not consulted by any emitter** any
    more;
  - the console's own `kHiddenToolNames = ["pipeline_output"]`
    (`source/llm/app_agent/ui.d:609`, introduced with a TODO that calls it a
    bad hidden dependency);
  - the C++ mirror `kHiddenToolNames{"pipeline_output"}`
    (`cpp_tui/store/typed_thread_store.h:57`).
  Fix: add `TuiBlockFlag_Hidden = 8` (`cpp_tui/tui_api.h:649-653`, additive;
  bump `TUI_API_VERSION` to 15 for the record), and set it on every tool-block
  upsert - live tick, the Running flip, the step walk, the orphan result, the
  stop's Cancelled flip - from `llmConf.isHiddenTool(name)` (both name axes,
  which `isHiddenTool` already handles, `config.d:905-907`). Then delete both
  hard-coded lists and make `blockDisplayable()` read the flag. `task_done`
  is hidden by the default config, so the `Answer` row is the only copy of
  the answer text (4.4) - and a user who configures `hiddenTools` finally gets
  what they asked for in the TUI too.
- Replay emits the content block for **every** response: the `ToolMessage`
  branch of `buildTypedTurns` (`package.d:1082-1124`) emits reasoning + tools
  but no `contentBlockId(bi)`, while the live tick always upserts one
  (`ui.d:777-779`). That costs the batch its duration and its content after a
  reload (the `AGENT` row's time comes from the content block, 7.3.3). One
  upsert next to the reasoning block.
- Optional (no longer load-bearing): emit reasoning before content in
  `TypedStreamMessageUpdater::messageUpdate` (`ui.d:775-783`), for console
  parity with replay.
- Gate: `dub test` - the typed-emitter tests around `tests.d:3580-3740` count
  turn upserts and will need the extra close.

### 7.2 Test data

- `test/typed_seed.h`: `tool()` must not bump `liveBatch`; all calls of a
  response share the response's batch id. Add a `nextBatch()` so a scene can
  start a second response.
- Re-key the `livebatch`, `maxlive`, `maxcancelled`, `maxanatomy` scenes to
  one batch id per response.
- New seeding helpers for the shapes the fold needs: a batch with content but
  no reasoning, a batch with calls but no content, two batches in one turn.

### 7.3 Store (`store/thread_store.cpp`, `model/block.h`)

1. In `syncTyped()`, after the displayable filter, bucket the turn's rows by
   `typedBatchId` (first-appearance order, which must equal batch index order -
   assert it). Today the build emits rows in insertion order (`:251-331`).
2. Per batch compute, in the same pass: `hasReasoning`, `hasContent`, the call
   list (title + status), `anyFailed`, `anyLive`, and `settled` (5.2). Keep
   these on a new `BatchEntry` struct beside `blocks`.
3. Emit per batch: the owner row, then its children in the canonical order
   (reasoning, content, cards). The owner row:
   - `kind = BlockKind::Assistant`, `typedTitle = "AGENT"`, empty body,
     `expandable = true`, `ownsBatch = true`;
   - `typedBlockId = "batch#" + batchId` (the pin key; a hash of it keys the
     ImGui id), `typedBatchId = batchId`. It is the first row the C++ side
     invents (no wire block backs it), so give it an id namespace a wire block
     cannot produce and assert the collision-free guard in a test: the wire's
     own ids are call ids plus `query` / `answer` / `content#N` / `reasoning#N` /
     `vision#N` / `notice#N`;
   - `timingS` = the response's duration, taken from the batch's content block
     (`durationMs`, which D stamps when the step completes; `0` on the live
     tick, which is why the live header has no time - Q3);
   - `chips` = the call names as `[a] [b]` (+ `+N` past three), `failedCalls` =
     the count of failed calls, `batchLive` = `batch autoOpen`.
4. Content child: a **body segment**, not a block row - `bodySegment = true`, no
   header, no hit box, drawn by the view with `renderBlockBody()` at the batch
   indent. Reason: `renderBlock()` returns a default answer (and no `drawBody`)
   when `rect.h <= 0` (`views/blocks.cpp:259-262`), and a header-less row has no
   height to claim; the body's height belongs to the rich renderer, so the view
   skips the chrome path entirely for this row.
5. Drop a body-less content row (`body` empty) - the owner already names the
   batch (4.2). A body-less reasoning row likewise.
6. `BlockDesc` additions (`model/block.h`): `bool bodySegment`, `bool ownsBatch`,
   `bool batchOwned`, `bool copyable`, `std::string chips`, `int failedCalls`.
   `batchTools` is retired (the chips carry the count; the `Thinking` header
   keeps only its duration).
7. `blockAt()`: the owner's collapsed `summary` is the chips when the batch has
   calls, and the content's first line when it has none (a content-only batch
   previews its text, the way a collapsed message does today,
   `views/blocks.cpp:232-237`). The owner's `copyable` is "the batch has
   content", and `blockText()` on the owner answers the batch's content body -
   copy belongs to the response, not to the paragraph it happens to live in.
8. `blockExpanded()` / `setBlockExpanded()`: the defaults of 5.2, pins
   absolute for rows (`std::map<std::string, bool>` per group id in place of
   `std::set<std::string> typedRowOpen_`) and for groups (in place of
   `ChatTab::groupOverrides`' ForceOpen/ForceClosed pairs and
   `openWorkGroups`), because both the row default and the group default now
   move with liveness (6.2).
9. `lineCountEstimate`: count the rows that will draw (owner rows always, a
   folded batch's children not). Optional - 4.10 says nothing depends on it -
   but it keeps the first frame honest after a fold.
10. Displayability comes from the wire flag, not from names: `blockDisplayable()`
    reads `flags & TuiBlockFlag_Hidden` (`store/typed_thread_store.cpp:11-18`),
    `kHiddenToolNames` (`typed_thread_store.h:57`) is deleted, and `stepCount()`
    keeps skipping whatever the flag hides (`:167-196`). No tool name survives
    in the C++ tree - that is the user's rule and it also fixes the config
    being ignored (7.1).

### 7.4 View (`views/thread.cpp`, `views/blocks.cpp`)

1. The row loop keeps one bit of state: after an owner row, `batchOpen =
   desc.expanded`; a `batchOwned` row is skipped when `!batchOpen`. That is the
   nesting, and it is why the children must stay contiguous (7.3.3).
2. The `card` indent predicate becomes "is a `batchOwned` row" so the thinking
   row, the content segment and the cards share one indent and one rail
   (`:568-575, 654-664`).
3. `blockHeaderText()`: append `separator + chips` for an owner row (open and
   folded alike), and let `renderBlock` paint the `FAIL` word when
   `failedCalls > 0`; drop the `batchTools` clause from the `Thinking` branch
   (`:193-197`). The owner draws its `AGENT` label only while it is open; a
   folded owner row skips the label and starts with its time (5.2.1), so the
   label decision is a parameter of the header composition, not a kind rule.
   Fitting rule (the user's "just drop the chips if it is too short", which is
   the right one): the header composes label, then the time, then the chips, and
   it *drops the chips* when they do not fit the row, then the time, and never
   the label. Today nothing composes for width - `textAt()` truncates at the row
   edge on the grid and clips on the pixel profile
   (`views/blocks.cpp:82-101`), so a narrow row would cut `[bash, grep]` in half.
   That means one signature change: `blockHeaderText()` (and the measure path
   that calls it) takes the available width in cells/pixels and decides, instead
   of letting the clipper decide. Pin it in a test at 40 columns:
   `AGENT · 3.2 s` with the chips gone, label intact.
4. `bodySegment` rows: the view draws `thread.renderBlockBody()` at the batch
   indent and never calls `measureBlockHeader()`/`renderBlock()` (see 7.3.4).
   No hit box, no copy chip, no menu - the paragraph is the owner's body, and
   the owner carries the copy (`copyable`, 7.3.7).
5. The live edge: keep the chips line and the `Thinking` shadow line for
   minimal only (`:517-556, 671-680`), and guard both with `index ==
   drawn.back()` so an old group can never paint them (4.7).
6. The fold's row exception list (`:581-595`) gains "the newest batch's content
   row while the turn is live" - that is the minimal-mode content fix (5.3).

### 7.5 Tests and goldens

- New scenes, both of them the shape the user judged in 9.1:
  `blocks-maxlong` (a finished 6-response maximal turn: `> WORK - N steps` only)
  and `blocks-maxlonglive` (the same turn mid-run: the group open, the settled
  batches as `> 1.2 s - [bash, grep]` rows, the newest batch open with its
  thinking, content and cards). The harness screen is 100x30
  (`test_blocks.cpp:123-124`), so 6 responses is what fits - the `23 steps` art
  in this file is illustrative, not a golden size.
- Regolden: `blocks-maxanatomy`, `blocks-maxlive`, `blocks-maxcancelled`,
  `blocks-answerlanded`, `blocks-answerchip` (owner rows, chips, `task_done`
  hidden) and the minimal streaming goldens (`streaming-live`,
  `streaming-table`, `streaming-fence`, `streaming-thinking`) - the content now
  draws, which is the visible improvement of 5.3.
- New checks: the owner row carries the chips and the duration; a live batch is
  open with its thinking, content and cards; the last call answering folds the
  batch to `> 3.2 s - [chips]` (no role word, 5.2.1); a reasoning-less batch
  folds the same way; a
  content-only batch previews its first line when folded; a failed call marks
  the folded header; a pin survives the default flipping (6.2); minimal keeps
  the live content row and hides it once the turn closes; the shadow line and
  the chips line never draw in maximal; a `TuiBlockFlag_Hidden` block weights 0
  and draws nothing, whatever its title.
- `test_streaming.cpp:660` (the hint does not move with the fold) stays green
  untouched.
- `test_blocks.cpp:1611` ("an empty stream draws no agent body row") becomes
  true for a real reason (7.3.5) - rewrite its comment, which currently credits
  the store with a drop it does not perform.
- The agents drill-down binds its own `ThreadStoreAdapter`
  (`views/shell.cpp:1207-1219`), so it inherits the fold: check its goldens
  (`inspector-agents-drilldown`) and keep the two mounts' shapes identical.

### 7.6 Docs

`doc/tui_design.md` has no section on the thread modes at all (its only
"maximal" is an unrelated word about markdown spans). The two modes, the batch
fold, the live edge and the anchors deserve one chapter there, and
`doc/todo.md` should carry an entry for the unimplemented work this file
describes (this plan is the gap's record until then).

## 8. The three small questions, answered

**Q1. `task_done` hiding - settled as a wire flag, not a C++ list.** The user's
rule is right and it is a correctness fix, not hygiene: the config's
`hiddenTools` is configurable and the TUI ignores it today. `TuiBlockFlag_Hidden`
set by D from `isHiddenTool()`, all three hard-coded lists deleted (7.1, 7.3.10).
With the default config `task_done` disappears from the thread, so the `Answer`
row is the only copy of the answer text (4.4) - the user agreed to that
conditional.

**Q2. No copy control on the content paragraph** - agreed, and my caveat was
wrong in the part that matters: text *selection* is not the block's, it is
rich_md's. Every body goes through `ui::renderModelText()`, which calls
`RichMd::PushSelectableText(selectable)` around `RenderRaw`
(`cpp_tui/ui/markdown.cpp:294-331`), and the pixel profile selects:
`markdownSelectable(false-profile)` is `true`, the text grid is `false` on
purpose because the terminal gives `Ctrl+C` to `SIGINT`
(`cpp_tui/ui/markdown.cpp:227-242`). So a body segment drawn with the same
`renderBlockBody()` path is mark-and-copy-able in the GUI exactly like any other
rich text, hit box or not. On the grid there is no selection at all (that is
already true for every paragraph today), and the block-menu/`y` copy reaches the
content through the `AGENT` row (`copyable`, 7.3.7). No extra hit boxes - the
user's decision, revisit only if the grid copy becomes a real want.

**Q3. The `AGENT` row keeps the response's duration** - agreed: `AGENT · 3.2 s ·
[bash, grep]`, moved off the `Thinking` row. Rationale recorded: a live time
would only duplicate the clock in the bottom-left corner, so the header gains
its time when the response ends. Two consequences: D stamps `durationMs` at the
step, so the live header has none (`ui.d:777-779` sends 0 on the tick), and the
replay path must emit the content block for a tool response or the time is lost
after a reload (7.1).

## 9. Is the `AGENT` shape better? My assessment

Yes - better than both what ships today and the "Thinking owns its batch" idea I
proposed in round 1. The reasons, in order of weight:

1. **It fixes a hole neither earlier shape covered.** A response with calls and
   no reasoning has no reasoning row; with `Thinking` as the owner those cards
   had no parent (`blocks-maxlive-100x30.txt` is that shape). `AGENT` always
   exists because a response always has a role.
2. **It puts the role back on the response.** `Thinking` as the parent of tool
   calls was a category error: the model *thinks*, then *acts*; both are parts of
   one assistant response, and the row now says so. The nesting finally reads
   like a sentence: the agent said this, it thought that, it ran those.
3. **The content regression is fixed by construction.** The content becomes a
   first-class child of the response instead of a work row that the mode hides -
   master's `AGENT` + body shape is back, and minimal keeps the live one (5.3).
4. **Maximal stops being the mode that hides more.** Chips in the folded header
   mean a settled batch says which tools ran and whether any failed, which
   minimal's chips line already said and `Thinking - 2 tool calls` did not.
5. **The implementation gets simpler, not harder.** The batch id is already on
   the wire, so grouping is a projection change; children stay contiguous, so
   the nesting is one bit in the row loop; the fold derives from turn state, so
   there is no new event, no C++ mutation of block statuses (the old chapter's
   `Cancelled` flip is dropped, 5.4), and the hint contract is untouched (4.10).

What still worries me, honestly, and where each worry landed:

1. **Depth.** A live batch is four levels of disclosure: group, batch, thinking,
   card. Each level is one click and the defaults make the depth invisible
   (live = all open, settled = one row), but it is a lot of structure to teach.
   Mitigation: exactly one rule - "a row is open iff its owner is open and the
   batch is live" - and no other mode-specific folds to remember. Settled.
2. **The stack of rows in a long turn** - judged and settled in 9.1: the log
   drops the repeated role word, and the group folds to `> WORK - N steps` when
   the turn ends, so the wall problem and the noise problem are both gone. The
   cost is one more click to reach the index of a finished turn, accepted.
3. **Header width.** Settled by 7.4.3: drop the chips when they do not fit, then
   the time, never the label.
4. **The header-less body segment.** Closed: the user's order stands, and the
   "new shape" is plumbing, not rendering - the paragraph is one
   `renderBlockBody()` call, the same one every other body uses; the only new
   bit is that the row carries no header, so the loop must not call
   `measureBlockHeader()`/`renderBlock()` for it. It also stays selectable in
   the GUI (Q2), because selection belongs to rich_md, not to the row.
5. **The invented row identity.** Closed: uniqueness among a turn's rows is the
   whole requirement, and the batch id gives it; a one-line guard and a test
   saying so is the end of it.

### 9.1 A long turn, as the design draws it (judged and settled)

A "golden" is this project's word for a captured screen: a text file in
`cpp_tui/test/golden/` holding the grid plus its ink runs, compared byte for
byte by the render tests and regenerated with `--update`. The set is 100 files
today; the thread's own are 16 scenes in `test_blocks` and 5 in
`test_streaming`, and `test_blocks` reports 424 individual checks over its
scenes. "Eyeball the long-turn golden" means: before committing to a row stack,
we add a many-response scene, look at the capture, and a human says whether it
reads well - a test can prove a shape is stable, never that it is pleasant.

Today, a long maximal turn is every card of every response open:

```
v WORK - 23 steps
  v TOOL  bash              <- response 1, and every response after it
   ls -R
   RESULT  42 lines
  v TOOL  grep
   grep -r needle
   RESULT files = 3
   RESULT needle hits = 7
   ... 3 more
  v TOOL  grep              <- response 2
   ...
  (about 200 rows for 12 responses)
```

The design draws the same turn while it runs - the group open, the settled
responses as a log, the live response expanded (this is the shape the user
judged "a lot better than today"):

```
v WORK - 23 steps
> 4.1 s · [bash, grep]
> 2.7 s · [read_file, read_file]
> 1.2 s · [bash]
v AGENT · [execute_command]            <- the live response, no time yet
  v Thinking
     rebasing the parser onto the new loader ...
  Next I will move the loader out of core.
  v TOOL  execute_command
      dub test
```

and once the turn is done, the group folds to its one row - the user's second
decision, and the whole turn at rest is then the conversation plus the index
behind one click:

```
 YOU
 refactor the whole tree, module by module
> WORK - 23 steps
 ANSWER
 the refactor is done: ...
```

Opening that group by hand brings the log back, every row folded:

```
v WORK - 23 steps
> 4.1 s · [bash, grep]
> 2.7 s · [read_file, read_file]
> 1.2 s · [bash]
> 9.8 s · [execute_command]
> 3.3 s · [write_file, bash]
> 0.9 s · [grep]
> 2.2 s · [bash, read_file]
> 5.0 s · [bash]
> 1.4 s · [read_file]
> 6.6 s · [execute_command]
> 2.0 s · [grep, bash]
> 3.1 s · [write_file]
```

Both arts become scenes (`blocks-maxlonglive`, `blocks-maxlong`, 6 responses to
fit the 100x30 harness), so the shape is pinned and the next change has to
argue with it.
