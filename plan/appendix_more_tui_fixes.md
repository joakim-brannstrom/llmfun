# Appendix: more TUI fixes

Continues the earlier design trail's D-numbering.

## D11. Typed turn lifecycle: structural liveness, turn-done only on normal completion

### The bug

The reported symptom is a live tool chip printed three '> WORK' headers up,
almost randomly placed, and it never folds away. The placement machinery is
not at fault: the folded header's edge and the answer-row edge both re-read
the store's block states every frame and draw one chip per Running tool step
of the group they fire for - the live-edge contract working as written.

The bug is upstream: a turn's tool blocks can stay Running in the store after
the turn is over, so a group that is no longer the newest still reads Running
and the renderer - correctly - draws its chips line under that older group's
header, below the newer turns.

### The event-only lifecycle

The status lifecycle rides events only, and every event is optional:

- Pending rides the stream tick: messageUpdate upserts each streamed call
  (non-empty id) as Pending and records it in ctx.liveCalls.
- Running rides stream-done: streamMessageDone re-upserts every ctx.liveCall
  as Running - every recorded call, not just the batch that just finished.
- The terminal status rides the step walk: the ToolMessage branch upserts
  call+response blocks keyed by the wire call id, Ok/Failed, and clears each
  live call; a response whose call is not in this step emits a result-only
  block and never clears its live call.

### The leak paths

Nothing sweeps at turn end, and the loop can exit without the terminal events
having fired:

- unknownFailure / networkFailure end the loop with keepRunning = false. A
  step that streamed calls and then failed mid-stream - the response never
  assembled, the harness never ran - leaves those calls Running. The step
  hook still runs for the failed step, but result.chat is chat.lastResponses,
  and the streamed-and-lost calls' finalization is not in whatever that slice
  holds.
- agentStuckInLoop: emitStopTyped runs only when the stop was user triggered;
  the stuck path is not a stop, so only the console message goes out. The
  live calls never flip and the turn never closes - the only turn closes
  today are the final answer's Done and the stop's Interrupted.
- The orphan path adds a resurrection: a call whose response arrives without
  its call entry in the same step emits its result-only block but stays in
  ctx.liveCalls, so the next batch's stream-done re-upserts it Running - a
  chip that folded comes back, and keeps coming back while the context
  survives.

### Why it reads as random

The store keeps what it is told (blockUpsert merges by id and the last status
wins), and the view re-reads every frame, so the stale Running block draws as a
live chip under its own group's header - a group below the newer turns, which
is why the line lands three headers up. Which group goes stale depends on where
in the run the turn died, so the position moves between runs; it recurs because
every run that dies through either path leaks again.

### Fix (decided)

- No turn-done on failure exits: a failed turn stays open and /c-resumable;
  turn done fires only for normal completion. The failed-and-never-continued
  turn draws its chips until the next boundary - no event says its calls are
  dead, and none should be sent.
- Liveness is structural in C++: a Running tool block is live only while it
  belongs to the newest turn's newest unfinished step. At the boundary - a
  newer step's Thinking arriving, or a newer turn starting - the store folds
  the stale chips AND flips the stale Running blocks to Cancelled. Store-side,
  event-triggered, no new event type, no D-side semantics duplicated into C++.
- The stop keeps Interrupted as a small visual indication; a stopped turn must
  be /c-resumable without adverse effects. A /c continuation sends no turn
  start (the guard requires a non-empty query), so the turn row reads
  Interrupted until the next terminal close - stored but never rendered, since
  nothing in the C++ view branches on the turn state.
- The ANSWER block: the turn close upserts a new block with kind=answer and
  the latest task_done's body; during a step, task_done is an ordinary
  Tool-kind block. No new TuiTurnState and no ABI change: nothing branches on
  Done vs Interrupted.
- The ANSWER block: the turn close upserts a new block with kind=answer and
  the latest task_done's body; during a step, task_done is an ordinary
  Tool-kind block. No new TuiTurnState and no ABI change: nothing branches on
  Done vs Interrupted.

## D12. Maximal mode: the Thinking fold owns its batch; a batch folds when its tools are done

### The violations (verified)

The design trail's maximal fold intent: Thinking renders open by default
(reversing the v5 rule), the newest batch's tool cards auto-open while older
batches' cards render collapsed into their headers, and the group folds to its
header with the counts when the turn finishes. HEAD implements none of it.
Four verified violations:

1. The Thinking row draws collapsed by rule: thread_store.cpp:654-661 derives
   defaultOpen as !minimal && kind != Thinking - the comment states "with only
   the batch's thinking disclosure collapsed behind its meta"; the intent opens
   Thinking by default.
2. The tool cards draw open always: no batch-liveness logic exists anywhere in
   the fold derivation; the intent auto-opens the newest batch's cards and
   renders older batches' cards collapsed into their headers.
3. The group draws open always: thread_store.cpp:646 - "F5: a maximal batch
   draws open, always"; the intent folds the group to its header with the
   counts when the turn finishes.
4. The tool cards are independent rows that merely hang visually indented
   beneath the Thinking row (thread.cpp:568-575, Indent(m.pad2)); they draw
   regardless of the Thinking row's fold state, so they are outside the fold
   the Thinking header owns.

The goldens pin today's shape: blocks-maxanatomy-100x30.txt (a finished maximal
turn: folded Thinking row, open cards, open group) and
streaming-thinking-100x30.txt (live thinking: folded Thinking row, no body).

Before and after (the fold change, in the session's sketch conventions -
">" folded, "v" open). Today's shape, a batch with its calls landed:

    YOU
    run the big refactor
    v WORK - 5 steps
    > Thinking - 2 tool calls             <- folded by rule, live and finished alike
      v TOOL  bash - 1.1 s                <- the cards draw open always, no batch liveness
        cmd = ls -R
        RESULT  42 lines
      v TOOL  grep - 3.2 s
        grep -r needle
        RESULT files = 3
        RESULT needle hits = 7
        ... 3 more
    [bash] [grep]                         <- the live chips line

Finished, the same shape minus the chips line: nothing folds in maximal today.
With the change, the batch's life. Mid-batch, one call done, one still running:

    YOU
    run the big refactor
    v WORK - 5 steps
    v Thinking - 2 tool calls             <- OPEN for the whole life of the batch
      thinking about the refactor ...     <- the thinking text visible, growing while it streams
      v TOOL  bash - 1.1 s                <- the cards hang inside the Thinking fold
        cmd = ls -R
        RESULT  42 lines
      v TOOL  grep                          <- the running call's card, open, its args streaming in
        grep -r needle
    [bash] [grep]                         <- the live chips line, unchanged

The moment the batch's last call responds, the batch folds - the Thinking row
draws as its header only, the text and the cards inside the fold:

    YOU
    run the big refactor
    v WORK - 5 steps
    > Thinking - 2 tool calls             <- folded the moment its last call responded
    > Thinking - 1 tool call              <- an older batch: its header only

Opening a folded Thinking row reveals the body plus the cards, each card still
its own foldable. The finished turn's group shape is sub-decision 1: the group
folded to "WORK - N steps", or as drawn - open with the folded Thinking-row
headers listed beneath it. A manual re-open pin's lifetime is sub-decision 2.

### The proposed shape (user's comment, in discussion)

Maximal mode, per the user's comment (drawn as before/after art in the session):

- A batch's Thinking row draws OPEN - the thinking text visible, growing while
  it streams - for the whole life of the batch: from the first streamed
  thinking until the batch's last call responds.
- The batch folds the moment its last call responds: the Thinking row draws as
  its header only (the count), with the thinking text and the tool cards hidden
  inside the fold. Mid-batch (one call done, another still running) it stays
  open.
- Opening a folded Thinking row reveals the body plus the tool cards, each
  card still its own foldable.
- Older batches draw their Thinking-row headers only.
- This replaces the design trail's trigger: that shape folds the whole group
  when the turn finishes; the proposal folds each batch when its own tools are
  done - immediate, and derivable store-side from the same call states the
  chips line already reads (thread.cpp:491-494).

### The whole turn, as interpreted

The same reading carried through a whole finished turn - two batches and the
final answer. The group drawn open, sub-decision 1's second option:

    YOU
    run the big refactor
    v WORK - 8 steps
    > Thinking - 2 tool calls             <- the first batch: folded the moment its tools were done
    > Thinking - 1 tool call              <- the second batch: its header only
    AGENT
    The refactor is done ...              <- the final response, outside the group

The same turn with the group folded, sub-decision 1's first option (the design
trail's shape, the same shape minimal shows):

    YOU
    run the big refactor
    > WORK - 8 steps
    AGENT
    The refactor is done ...

And the first batch re-opened by hand after it folded - the state sub-decision 2
governs:

    v WORK - 8 steps
    v Thinking - 2 tool calls             <- re-opened by hand: the body returns as it streamed
      thinking about the refactor ...
      > TOOL  bash - 1.1 s                <- the cards return folded, each still its own foldable
      > TOOL  grep - 3.2 s
    > Thinking - 1 tool call              <- the second batch: still folded

One more reading to pin down falls out of the last sketch: the bullet says each
card is still its own foldable, but not the state it returns in - drawn folded
above, it could equally return open (as the live auto-open draws the newest
batch's cards). Whether a card's fold state survives the batch's fold is
sub-decision 3.

### Open sub-decisions (pending the user's answer)

Three sub-decisions are open and were forwarded to the user; this chapter records
them as decided once answered:

1. The finished turn's group shape: "WORK - N steps" folded to its header
   (the design trail's group-fold, the same shape minimal shows) vs the group
   staying open with the folded Thinking-row headers listed beneath it.
2. The lifetime of a manual re-open pin on a batch that folded on completion:
   until closed by hand, until a new turn starts, or until the next step's
   thinking arrives.
3. The fold state a tool card returns in when its Thinking row is re-opened
   after the batch folded: folded (as drawn in the interpretation art) or open
   (as the live auto-open draws the newest batch's cards).

Session assessment of effort: the fold defaults are a store-side derivation
change plus the goldens and the streaming checks (easy, roughly an hour); the
nesting is the structural change and the hard part, because the height
reservation contract flips - measureHint goes from "the estimate, and does not
move with the fold" (pinned by test_streaming.cpp:660) to fold-aware, else folded
batches keep reserving space for rows that no longer draw (moderate, 2-3 hours).
Total roughly half a day including the dub test gate and the golden
regenerations. The live-chip fix (D11's settled shape) is unimplemented; folding
it into this work would touch the seam once.
