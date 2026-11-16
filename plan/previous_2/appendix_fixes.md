# Thread View Rendering v6: Reworking Turn Anatomy, Modes, and Tool Cards

Follow-up to "Thread rendering v5" (previous/system_design.md). This document answers the post-implementation
feedback on the rendered thread view: it investigates what each reported symptom actually is in the
current implementation, and states the design changes needed where the current shape cannot carry the
fix. It is design-level: implementation tasks live in a later implementation-plan step.

## 1. Problem

After the v5 rendering shipped, the user reported five concrete problems with the thread view
(feedback items 4-8):

4. In minimal mode the user query renders below its work-group header, and while a turn runs the
   current command being executed is not visible - only the WORK header, an occasional chips line,
   and the query. The expected shape is the opposite: the query first, then the work group, with the
   live command visible while it runs.
5. In maximal mode the thinking disclosure renders collapsed.
6a. User queries carry an unwanted "[N]" ordinal prefix.
6b. The composer's Send button never flips to Stop - not while an agent run streams, not
    while compression runs, not while a /plan or /code pipeline runs.
7. Tool cards render with unnecessary markers ("+" before TOOL), a body that repeats the tool name
   the header already shows, and arguments packed onto one line.
8. In the normal thread view, a long turn renders as a wall: ~102 expanded tool cards, the
   thinking disclosure collapsed, the streamed content as a lone AGENT row, all inside a WORK
   group - and this "got worse after the pipeline query fix".

The last point needs explicit resolution because it reads as a regression claim: it is not.
The pipeline query fix did not change how a long normal-view turn renders (see 3.8); the wall
is the maximal-mode rule addressed in 3.3, and the fix drew attention to it.

## 2. How the thread view works today

One typed turn renders as a single group. The adapter builds the group's row list with the WORK
header first, then one row per store block in insertion order: the query (labeled YOU), reasoning
(Thinking), tool steps (TOOL <name> - <time>, with the result status as the header glyph and an
embedded RESULT line), streamed prose (AGENT), and the answer (ANSWER). The query row carries the
turn's "[N]" ordinal prefix, a v5 design feature (turn ordinals) the user never asked for.

Two render modes select what draws:
- Minimal folds the group to its User, Answer, and WorkGroup rows. Under a folded header, a live
  turn may draw a chips line naming running/pending tools - but only while a tool step is actually
  running. While the model streams (no tool yet), the live turn shows only the header and the query;
  the current command is invisible exactly when the user is waiting for it.
- Maximal defaults every row open except Thinking. Applied to a 102-step run this opens every tool
  card while hiding the one thing (thinking) that would summarize them.

The pipeline drill-down reuses the same adapter and rendering for a per-agent transcript. Its turns
are seeded by the pipeline stream path, which emits content with an agent role meta and never a
query block, so the adapter's query rule finds no query row there - correct after the pipeline fix,
but it means the drill-down turn renders its full anatomy: header, AGENT row, thinking, and every
tool card.

The composer's Send/Stop flip is driven by a ready flag: the agent side must clear it when a run
starts and set it again when the run ends. The entry points raise that edge - the main query and
compression call busy()/ready() around their runs - but the edge lands in an empty function: the
messenger's busy half clears nothing, so the flag never leaves ready. The flip itself is correct - a
golden pins the Stop label - but the golden sets the C++ flag directly; no production path drives it.
Pipeline slash commands do not raise the edge at all, and the separate runtime-status edge (phase
and context usage) is display data the composer does not consult for the button.

## 3. Investigation: what each feedback item is

3.1 (item 4, order) The query-below-header order is not a rendering accident: the header-first
anatomy is what v5 specifies. The feedback rejects the anatomy itself. Fixing it means the query
row moves above the group header - a structural change to the group's row list and to the minimal
fold, not a one-line tweak.

3.2 (item 4, live edge) The missing live command is a deliberate minimal-mode gap: the chips line
exists but only draws while a tool step runs. The user wants the current command visible
continuously while the turn runs, folding into the WORK count when done. This is a new live-edge
element, not a bug in the existing one.

3.3 (item 5) Thinking-collapsed-in-maximal is exactly what the v5 design specified. The feedback
reverses that decision. Design revision, not a defect.

3.4 (item 6a) The "[N]" prefix is the v5 turn-ordinal feature, implemented as designed and unwanted
by the user. Remove the display (or the feature - open question 6.1).

3.5 (item 6b) Send-never-flipping is a real wiring bug with an exact location: the busy edge is
raised by the entry points - the main query and compression call busy()/ready() around their runs -
but it lands in an empty function: the TUI messenger's busy half clears nothing, while its ready
half sets the flag back to ready. The flag is therefore ready forever, and the composer's flip
(busy = not ready) never fires. The flip logic itself is golden-tested green, but the test sets the
C++ flag directly; the agent-side drive is missing exactly one body. The pipeline slash handlers
additionally never raise the edge. The fix is one missing body plus raising the edge at the entry
points that lack it (D6).

3.6 (item 7) Three separate contributors, all verified:
- The "+" marker is the tool card's status glyph: a check mark whose ASCII fallback is "+". It
  reads as noise rather than as status. Either drop the glyph or pick an unambiguous one (open
  question 6.2).
- The body repeats the tool name because the body builder prefixes the summarized arguments with
  "name(" while the header already shows the name.
- The arguments render one-per-line from the summarizer but the card then appends a one-line
  RESULT row, so the card reads as a jumble of markers, a repeated name, and a collapsed result.

3.7 (item 8, where it renders) The wall is the NORMAL thread view - a long main-agent turn in
maximal mode - not the pipeline drill-down. The maximal-anatomy golden shows the exact shape the
feedback describes: the WORK header first, the query below it, the thinking disclosure collapsed
(the small closed arrow) while every tool card below renders open with its arguments and a RESULT
line, and the streamed content as a lone AGENT row. With a 102-step run that anatomy becomes the
reported wall. It renders through the same terminal test stack the goldens use, so the ncurses-based
tests show it too; no screenshot is needed to confirm it.

3.8 (item 8, the wall and "got worse") The wall is item 5's maximal rule (everything open except
thinking) applied to a long run. It predates the pipeline query fix: that fix's regenerated goldens
touched only folded minimal scenes (a label shift), the maximal-anatomy golden was not regenerated
at all, and a long main-view turn renders identically before and after the fix because its query is
the turn's first block and carries the user meta under both the old and new query rules. So "got
worse" is the fix drawing attention to a pre-existing shape, not a regression it caused. The real
fix is 3.3's reversal plus a cap on how many cards auto-open (design decision D3).

## 3.9 Visual before and after

Approximate terminal sketches of the visual items. "Before" is what ships today -
the same shapes the golden tests render, except the header's token fields, which
D4 drops: the golden seeds set them, the app never does. "After" is the shape the design decisions
specify. Sketches use ">" for a folded disclosure and "v" for an open one - ascii
stand-ins for the arrows the real view draws. Each pair names its feedback item
and the decision that moves it.

Item 4 (D1, D2) - minimal mode, a live turn: the WORK header renders above its own
group's query, the chips line draws only while a tool step runs, and a streaming
thinking row renders as its own folded line. After: the query opens the turn;
running tools render as an orange, name-only, non-clickable chips line under the
WORK header; a streaming thinking renders as a folded, count-only Thinking row;
finished tools fold into the WORK count.

Before (a tool is running):

    > WORK - 12 steps
     [grep] [edit_file]
     YOU
     [3] add the stop button

After (D1 + D2):

     YOU
     add the stop button
     > WORK - 12 steps
       [edit_file] [read_file]

Before (the model streams thinking, no tool yet): the header and the query are all
there is - no thinking row. After (D1 + D2): the folded, count-only Thinking row
renders while the model streams thinking.

    > WORK - 12 steps                   YOU
     YOU                                add the stop button
     add the stop button                > WORK - 12 steps
                                          > Thinking

When the turn finishes the live line folds away and the group keeps its count:

    > WORK - 2 steps                    YOU
     YOU                                what is v5?
     [1] what is v5?                    > WORK - 2 steps
     ANSWER                             ANSWER
     the typed store                    the typed store

Items 5 + 8 (D3) - maximal mode, a long turn: the thinking disclosure stays
collapsed while every tool card below renders open, so a 102-step run becomes a
wall. After: thinking open, only the newest batch of cards open, older cards
folded into a count; the query opens the turn (D1).

Before (102 steps, abbreviated):

    v WORK - 102 steps
     YOU
     [7] run the big refactor
    > Thinking - 40 tool calls          <- collapsed even in maximal
     AGENT
       + TOOL  bash - 1.1 s
       ls -R
       RESULT  42 lines
       ...                              <- all 102 cards open, page after page

After (D3):

     YOU
     run the big refactor
    v WORK - 102 steps
       ...                              <- older rows render as headers only
    > Thinking - 1 tool call             <- an earlier response, folded
       > TOOL  read_file - 0.1 s
    v Thinking - 2 tool calls             <- the newest response, open by default
      thinking about the refactor ...    <- the open body (full text), not a summary
       v TOOL  bash - 1.1 s     <- its calls, the newest batch open
         ls -R
         RESULT  42 lines
      [edit_file] [read_file]             <- the live chips line, same as minimal

The two-level fold (both modes - the same shape, different fold defaults). Expanding WORK
shows the tool rows with their details folded; clicking a tool row opens its details. Minimal
reaches this only by expanding WORK; maximal live pre-opens the newest batch's details, so
its rows look like the opened one below, and older batches look like the folded ones:

    v WORK - 12 steps
    > Thinking - 2 tool calls
       > TOOL  bash - 1.1 s
       v TOOL  edit_file - 0.3 s    <- clicked: its details are open
         path=main.rs
         RESULT  12 lines written
       > TOOL  read_file - 0.1 s
      [edit_file] [read_file]             <- the live tools, orange, same as minimal

(The maximal WORK header is unfolded in maximal mode in the before sketch - the
group's own disclosure state is unchanged by these items; only the thinking and
card defaults move.)

Item 6a (D5) - the "[N]" ordinal prefix on the query body. Before:

    YOU
    [1] what is v5?

After (D5):

    YOU
    what is v5?

Item 7 (D4) - the tool card: a "+" marker, the header's name repeated in the
body, arguments packed after it. Before:

    + TOOL  bash - 1.1 s      <- "+" is the check mark's ascii fallback
    bash(cmd=ls -R, timeout=30)         <- the header's name, repeated in the body
    RESULT  42 lines

After (D4):

    v TOOL  bash - 1.1 s
      cmd=ls -R                           <- arguments only, one pair per line
      timeout=30
      RESULT  42 lines

The RESULT row (D4) - a JSON object result unpacks one level on the D side and
crosses the seam as two parallel string vectors (keys, values); arrays and
non-JSON render as-is. Before:

    + TOOL  bash - 1.1 s
    RESULT  {"cmd": "ls -R", "cwd": "/workarea", "timeout": 30}   <- a one-lined JSON blob

After (D4):

    v TOOL  bash - 1.1 s
      RESULT  cmd = ls -R                  <- one dim line per pair, values stringified
      RESULT  cwd = /workarea
      RESULT  timeout = 30

Item 6b (D6) - the composer button during a run. Before:

    > run the big refactor_
    [ Send ]                            <- stays Send for the whole run

After (D6):

    > run the big refactor_
    [ Stop ]                            <- flips while a run is in flight, back
                                           to Send when the run ends

## 4. Design decisions

D1. Turn anatomy is query-first. The query row renders as the turn's opening row, above the work
    group header; the group contains only work rows (thinking, tool steps, streamed prose, answer
    stays a turn-level row). The minimal fold keeps query and answer; the WORK header sits between
    them only while the group has rows to show.

D2. Minimal-mode live edge. While any tool runs, the chips line draws at the group's live
    edge - its bottom, under the newest rows (directly under the header in minimal, where no
    rows render) - so it stays visible while streaming; under the header it would scroll out of
    view on a long live turn. It shows the running tools' names only, orange, not clickable -
    their details live inside WORK once they finish. While the model streams thinking, the folded
    Thinking row renders instead (count only, no summary). While the model streams content with
    no tool running, nothing extra draws - minimal is minimal. Finished tools keep today's colors;
    live tools are orange. While tools run the group's count updates live with them - the chips
    line and the live count are minimal's two live elements. When the turn finishes the live line
    folds away and the group keeps its count.

D3. Maximal mode rebalance. Each response that carries thinking renders one Thinking row grouping
    it with its calls - a long run renders many small rows, not one row with every call. Thinking
    renders open by default (reversing the v5 rule). Tool cards auto-open only for the most recent
    batch; older tool cards render collapsed into their headers - no second count row inside the
    group, the header's step count is the only count (minimal mode folds finished tools into the
    group count). The user can still open any card. This removes the
    102-card wall while keeping maximal informative for the live tail.
    The Thinking row's label is the count only - the trailing summary today's disclosure renders
    is removed (previously agreed); the open body is the full thinking text. Finished tools fold
    into WORK in minimal mode. Auto-open is live-only: while the turn runs the newest batch renders
    open and older ones stay folded; when the turn finishes the group folds to its header with the
    counts - the same finished shape minimal shows; the group stays open for the whole live
    turn and folds only when the turn finishes. Re-expanding a finished group renders every
    Thinking row open with the tool rows as folded headers - the same disclosure shape as
    Thinking, header only - nothing re-opens, the
    newest-batch rule has no bearing on a finished turn. Opening the group mid-stream re-applies
    that mode's fold defaults - maximal opens every Thinking row, minimal keeps them folded - and
    renders the latest messages' full details (arguments and result) open in both, the one live
    exception: watching the live tail is the point. Minimal and maximal render the
    identical shape with identical click behavior; the modes differ only in fold defaults -
    maximal pre-opens thinking and the newest batch's details, minimal folds everything.

D4. Tool card shape. The card is a foldable like the Thinking rows: the header carries a
    disclosure arrow - ">" folded, "v" open - and no status glyph: success stays silent, a
    failure renders a textual FAIL (red) on the header, folded or open; no token fields - the
    renderer's "N in / N out" header counts stay unwired and out of the sketches. Folded, the
    card is the header only; open, the body follows - arguments only, one `key=value` pair per
    line, no repeated name prefix, no "[N]" prefix.
    The embedded RESULT row renders a JSON object result unpacked one level - one dim
    `RESULT <key> = <value>` line per pair, same styling as today; nested values render as
    compact JSON text. Only objects unpack - an array result renders as-is, one dim line, like
    non-JSON (a large array unpacked per index would rebuild the wall the newest-batch rule
    removes). The unpacked pairs cap at 100 and every value caps at 200 characters - past
    either, the first 100 keys / first 200 characters render and one dim "... K more" tail
    closes the line or the card; realistic tool results stay far below (read_file returns
    {"content": "..."} and renders as `RESULT content = <the file text>`, text cut at the cap).
    Transport, both rows: the D side sends raw data - the pairs sorted by key, each set handed
    over as two parallel string vectors (keys, values) - and the C++ side renders, never
    re-parsing; a packed string crossing the seam is what produced the blob. A zero-pair
    object renders compact as-is, one dim `RESULT {}` line.

D5. No ordinals. The "[N]" prefix disappears from the query row and the ordinal leaves the
    data model with it - ordering is turnId's job and session restore reads the history sequence;
    nothing else renders or stores them.

D6. The busy edge becomes real. The ready flag is the single source of truth for "a run is in
    flight"; the runtime-status phase edge stays display-only. The messenger's busy half clears
    the flag (mirroring what its ready half already does), and every turn entry point raises the
    edge: the main query and compression already call busy()/ready() and start working once the
    body exists; the /plan and /code handlers call them around the pipeline run. Run-in-flight
    predicates elsewhere (stop affordances) read the same flag and revive with it.

D7. Scope. D1-D6 are stated for the normal thread view; the pipeline drill-down shares the
    adapter and the rendering and inherits all of them. The pipeline gets no special casing in the
    generic chat-message view - pipeline-specific shaping hides inside the adapter. If pipeline
    messages render wrong because the data structure holds the wrong data, that is a bug in how the
    structure is populated - fix the population, not the view. The drill-down keeps having no query
    row (an agent run has none) - correct, and unrelated to this wall.

D8. Counts. The WORK count is steps: every message between the harness and the model inference
    server is +1 - a tool call and its tool answer are +2 together, a plain message +1. The Thinking
    row's counter is tool calls, not steps: one model response can carry thinking plus N tool calls,
    and the row groups that response's thinking with its N calls ("Thinking - 2 tool calls"). Steps
    and tool calls are different units and never mix in one count.

## 5. Alternatives considered

- Keeping the header-first anatomy and instead restyling the header so the query visually dominates:
  rejected - it leaves the reading order wrong, which is the core of item 4.
- Capping maximal mode by a fixed row budget (open at most N rows): rejected in favor of D3's
  newest-batch rule; a fixed budget hides the live tail on exactly the long runs where it matters.
- Dropping the RESULT row from tool cards: rejected; the outcome is useful, and dim one-line rendering
  is not what the feedback objected to.
- Raising the busy edge inside the pipeline engine instead of the slash-command handlers:
  rejected - the handlers own the UI lifetime of the run, and the engine is UI-agnostic. Deriving
  the button's busy state from the runtime-status phase edge instead of the ready flag: also
  rejected - the phase edge is display data the pipeline handlers do not send either, and
  conflating display phase with run state would widen the coupling.

## 6. Open questions

6.4 The item-4 paste's first "| YOU" row: which turn does it belong to? Nobody can
    confirm it from the tree (no session data in the sandbox), so it stays open,
    kept for the record.
