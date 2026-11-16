# Appendix: tool call and result presentation (design)

Date: 2026-11-13. Status: design concluded in discussion; nothing implemented and no other
documents changed. This appendix records what was concluded so the next step (an
implementation plan) can start from it.

## Why

Today a tool call and its result are two formatted text messages. The call batch becomes
one "tool call: ..." sentence, each result a separate "tool result <emoji> <name> <summary>"
message, each with its own display ordinal. Success is an emoji inside the text, so the UI
recovers the outcome by stripping the emoji and pairs a call with its result only by
adjacency. The streaming body embeds the calls as raw JSON. The costs are exactly the ones
the discussion started from: the presentation takes more area than an uninterested reader
wants, call and result are visually disconnected even though the backend already carries a
`toolCallId`, and the UI side does string archaeology to get structure back that D never
should have flattened.

## The root fix: v5 turns and blocks, v4 killed

The thread renders typed turns and blocks (the v5 model already sketched in
`doc/todo_modern_ui.md` sections 8.1-8.3: `TuiTurn`/`TuiBlock`, `TuiBlockKind_Tool` merged by
callId, `TuiBlockStatus` instead of emoji, numbers instead of formatted strings). The v4
chat-message path dies - the D-side sentence formatting, the emoji decoration, the message-type
enum, the stream-body JSON embedding, and the C++ store's marker stripping and adjacency pairing.

The transition is a full v4 -> v5 replacement inside one planned iteration - not spread over
independent features. D and C++ are upgraded together in the same commits, so the two sides
are automatically in sync at every commit: no graceful transition, no dual-source window, no
config-selectable legacy path. A commit either builds with both sides agreeing, or it is not
part of the iteration.

Design decision: pull the thread part of v5 forward (it was post-MVP); kill v4 in the same
iteration rather than patching it, because a v4-lite structured event would be a third format
that v5 then subsumes.

## The tool step

One tool step per `toolCallId`. The call and its result are one unit for the whole lifetime:
the UI never renders a result that is not attached to its call. With the typed model the
merge is an upsert keyed by callId; nothing is recovered from text.

Replay needs no incremental pairing: for stored history D owns both the call and the result,
so it emits already-merged tool blocks directly; only live streaming is call-then-result.

Design decision: the callId crosses the ABI; call and result merge into one block, not two
adjacent rows.

## States, and what "live" means

A tool call is live from the moment it starts being generated until it has finished executing.
That is the user-facing definition, and it is the one the model implements:

- `Pending` - the arguments are streaming.
- `Running` - set when the model's stream for the batch is done. The tool is executing or will
  start soon; a queued sibling call also shows as Running. The presentation does not need to be
  exact here - a bit of lying is accepted by design.
- `Ok` / `Failed` - dormant. The step folds.

No per-tool execution-start event exists: the Pending -> Running flip rides the existing
stream-done, and Running -> Ok/Failed rides the existing result. The agent only emits at
points it already has.

Running is a static green mark - no spinner. The state word keeps it distinguishable from
Pending's dim mark; spinning is an annoying distraction and is not in the design. The rule
covers the tool chips; the now bar's phase line is a separate surface and unchanged.

Design decision: no execution-start emission per tool; the stream-done message flips the
batch's calls to Running, and imprecision about "which call is executing right now" is
accepted.

## Verbosity: minimal and maximal

Two modes. Minimal is the default. The mode lives in the settings sheet (`ui.conf`) only -
no key shortcut and no palette command; switching is rare and a settings row is enough.
A per-group manual override (the user opened this one group) always wins over the mode
default, so looking inside one group never becomes a global flip. The override is
session-local: a fresh session starts at the mode default.

Minimal mode: work groups render header-only ("WORK - N steps"). Only the live steps of the
newest group render, as one chips line under the header: tool names only, one line, one chip
per live step. The Running chip is green, Pending chips are dim. A step folds the moment it
is dormant, even while siblings of the same batch are still live. No failure indication
anywhere in minimal mode - not on the live row (neutral; red would be color shifting on
something that folds in seconds) and not on the folded header (a failed tool call is
"who cares, the agent solves it"). The final answer streams as nothing but the
`[task_done]` chip while it is being generated; the answer body appears when it is done,
like any other result. The purpose of minimal is to be minimal: a streamed answer is a
distraction too, so minimal shows nothing but the live edge of the work.

The work group's step count is the number of LLM messages the group covers: a generated call
adds 1, its result adds 1, so a finished call adds 2. The header is a counter of the work's
messages, not of tool calls.

Maximal mode: work groups render open. The group covers the turn; inside it the design shows
the batches. One batch is one model response: its thinking disclosure (meta "N tool calls"), the
response's content block, and the batch's tool cards - one card per callId, header
"TOOL <name> - <duration>", the call (arguments) as the body, and the Result open by default -
maximum means maximum. The cards hang indented beneath the batch's thinking sharing a left
rule, so the one-to-many connection is visible without a new widget. Failed steps are red
inside the expanded group.

Design decision: minimal is the default; the mode is a settings-sheet row only; per-group
override beats the mode; chips are names only, one chip per live step; Result is open by
default in maximal; failures are red only inside an expanded group; the work group is per
turn with the batches inside it.

## Ordinals

One number per turn. Tool steps, thinking and result bodies consume no ordinals - only the
turn carries one. This also removes today's numbering gap where a call and its result took two
different numbers. The turn's ordinal and the work group's step count coexist: one is
position, the other is message count.

Design decision: the display ordinal moves from per-message to per-turn.

## Inspector pairing

The inspector's Activity view keeps the full live feed regardless of the thread's mode.
Minimal mode quiets the main window; it loses nothing, because the power path still sees
everything.

Design decision: the two-mode contract covers the thread only; the inspector is unchanged.

## Rejected alternatives

- A v4-lite structured tool-step event beside the message path - a third format that v5
  subsumes.
- Per-tool execution-start events - unneeded; stream-done is precise enough for the UI.
- Auto-open a group on failure - surprise expansion is worse than a hidden failure.
- A failure mark on folded headers - the user does not want failures advertised in minimal
  mode.
- Red on the live row - it folds too fast; color shifting only annoys.
- A key shortcut and a palette command for the mode - a settings row is enough for a rarely
  switched mode.
- Special handling for the `task_done` call - it is a tool step like any other while streaming
  (chip only); the answer it carries renders when done, as the answer.
- A transient dual-source window for the migration - withdrawn: both sides upgrade together in
  the same commits, so there is no window to bridge.
- Streaming the answer in minimal mode - rejected: the purpose of minimal is to be minimal, and
  a streamed answer is a distraction.
- A spinner on the live chips - retracted: Running is a static green mark; spinning is an
  annoying distraction.
- A count chip collapsing duplicate names in the chips line - rejected: a chip is one live
  step; two identical names are two distinct steps, and hiding that repeats the flattening
  this design exists to kill.

## Sizing

Coarse, for ordering only: the D side replaces its message formatting with typed turn/block
emissions and needs no new execution hooks; the ABI gains the v5 types and loses the v4
message path; the C++ store loses its text archaeology and gains the typed store; the thread
view gains the two renderings and the live folding; the settings sheet gains one row. Golden
frame scenarios cover both modes, live folding mid-batch, a failed step inside an expanded
group, and a turn whose ordinal and step count show together. The one-shot CLI console path
is unaffected.

## Not in scope here

Theme and color configurability, approval/ask cards, the agents tab, keymap changes, and the
inspector internals are separate work. The exact glyph set (Pending/Running marks, failure
mark) is an implementation-plan detail, not a design decision. When the transition lands, two
doc notes need a sweep: the post-MVP deferral note for the thread's v5 and tool cards in
`doc/todo_modern_ui.md`, and the live-tool-tab and stats-window lines in `doc/todo.md`.
