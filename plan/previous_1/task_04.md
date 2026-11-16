## Task 4: Console (blocked-sink) formatter over typed events + parity golden (design P0-3, part 2)

Priority: P0. Dependencies: task 3 (the typed events). One additive commit; the
formatter is wired into the live run at the cutover (task 5) — design §9: "built
in P0-3 behind the recording sink, wired at cutover".

### Description
Rebuild the one-shot console output over the typed events: TuiBlockedSink's v5
methods format today's lines locally (same text, same emoji — design §6; the
shared D-side sentence formatter dies with the v4 path at the cutover). The
parity golden is captured from TODAY's output first, then the formatter must
reproduce it byte-identically. The same C-side displayability filter applies
(D3/D10): hidden blocks produce no lines.

### Changes Made
- llmfun/source/llm/app_agent/ui.d: `ConsoleFormatter` — a small accumulator with
  one method per typed event (`onSetTurns`, `onTurnUpsert`, `onBlockUpsert`,
  `onTurnRemove`, `onThreadClear`) returning/exposing `string[] lines`; pure,
  no stdout, unit-testable. TuiBlockedSink's v5 methods (task 3 stubs) drive it
  and writeln the produced lines (replacing the v4 chatMessage/chatThinkMessage/
  finalAnswer writes at cutover; the v4 writes stay until then).
- llmfun/source/llm/app_agent/tests.d: the parity golden, built in two halves:
  1. Capture: drive TODAY's path (current senders) over a scripted session with
     TuiRecordingSink; build the expected string by applying today's blocked-sink
     write rules (ui.d:164-180: `writeln("Thinking: ", thinking)` then
     `writeln(msg)`; `writeln(msg)` otherwise) to the recorded (msg, thinking)
     pairs. Pin the expected string IN the test.
  2. Reproduce: drive `ConsoleFormatter` with the task-3 emitter's typed events
     for the same scripted session; assert byte equality with 1.

- Line mapping contract (the formatter reproduces today's sentences from blocks):
  - Live query echo: turn upsert + query prose block -> `writeln(body)`.
  - Replay query (setTurns, meta="user"): `writeln("user: <body>")`.
  - Content prose (meta = role): `writeln("<role>: <body>")`; when the batch has
    a Reasoning block: `writeln("Thinking: ", reasoning)` first (today's
    chatThinkMessage order).
  - Tool blocks: today's call line covers ALL calls of one batch's ToolMessage —
    the formatter buffers the batch's tool blocks and, on the batch's last tool
    merge, writes `writeln("tool call: " ~ join(batch call bodies, "\n"))` then
    per call in batch order `writeln("tool result <✅|❌> <title>: <meta>")`
    (emoji: Ok -> ✅, Failed -> ❌). Where today's sentences carried per-message
    thinking, the formatter writes the batch's Reasoning where today wrote it —
    the captured golden is the arbiter of the exact positions (see Notes risk).
  - Answer block: `writeln(body)`. Notice block: `writeln(body)`.
  - Hidden blocks (kHiddenToolNames): no lines; the batch join skips them.
  - setTurns blocks format as today's replay lines; upserts as today's live
    lines (the query echo difference: raw query vs "user: " prefix).

### Verification
- [ ] Parity golden green: the scripted session (thinking + content + 2 tool
      calls + results + task_done answer + a compression notice) renders
      byte-identical lines to the captured today-string.
- [ ] Hidden-block case: pipeline_output blocks produce no console lines and
      do not shift the batch join (matching the captured bytes).
- [ ] `dub test` green; the live run path untouched (wiring = task 5).

### Notes
- D8: the golden is captured from today's real output, never hand-written.
- KNOWN RISK (flag to the design owner if it fires): today's console can write
  the "Thinking: " prefix from BOTH the content message and the calls message
  (chat.d stamps thinking on both). v5 has one Reasoning block per batch. If
  the captured golden contains the prefix twice with the same text, the
  formatter rule above reproduces it from the one block; if the two differ,
  the Reasoning block carries the content-msg thinking and the calls-msg
  thinking needs a home (tool block meta) — resolve with the design owner,
  the golden stays the arbiter.
- The console formatter applies the SAME displayability predicate as the
  thread view and the weights (D10) — no console-local filter.
- Decision letters used: D3, D8, D10 (overview).
