# Implementation Plan: Thread rendering v6 — query-first anatomy, live edge, mode rebalance, tool card shape, busy edge

Source of truth: `plan/system_design.md` (v6; review fixes 1-10 from
`plan/design_review_v6.md` applied) + `plan/appendix_fixes.md` (contract,
D1-D8 + sketches). Prior iteration (v5, shipped): `plan/previous/` (its plan
format is the style template). Plan-time survey: `plan/codebase_notes.md`
(verified anchors), requirements digest: `plan/requirements_notes.md`.
Eight tasks, one commit each (task 7 may add fix commits), ordered.

## Standing executor rules (apply to every task)
- Re-verify EVERY file:line anchor before editing it. Anchors were verified
  at plan time (plan/codebase_notes.md); code moves. A moved anchor: fix
  the task file's anchor (one-line note in its Verification), never guess
  from memory.
- Conflicts: the design + appendix win over today's code; today's code wins
  over task-file anchors (snapshots). Any NEW conflict discovered mid-task:
  record it in that task's Notes, resolve per the first rule, and continue.
- Known pre-existing reds (never re-baseline): parity_core_loop's 2
  perf-gate FAILs, test_metrics' gate-ratchet FAIL. Everything else green.
- Workarea: all paths repo-relative under /workarea (plan/ = /workarea/plan,
  code = llmfun/...). read_file/edit_file reject ../ paths.
- Build/test: `dub test` from llmfun/ (~15-20 s healthy; a hang is usually
  ONE unittest formatting a backtrace). C++: cmake -S cpp_tui -B
  cpp_tui/build (from llmfun/) if cpp_tui/build is missing; plain test
  binaries run direct, *_gui need `TERM=xterm xvfb-run -a ./X --out .`;
  regolden ONLY via the --update flow, then eyeball the diff.
- The llmfun execution environment takes AT MOST 1 command argument (one
  shell string).
- grep_files intermittently returns a transient empty/error result:
  re-run once before concluding absence. NEVER conclude from a truncated
  grep (it once hid package.d:1434 — task 1's enumeration and task 4's
  sweep depend on this).
- edit_file: always pass mode; verify every edit with read_file (results
  can lie).
- Comments: semantic only — no byte counts, line numbers, or plan
  references in code comments (user convention).
- Contract artifacts (verbatim in the tasks) are the single source for
  shared shapes: task 1 = the busy-edge contract + the verified
  entry-point enumeration; task 2 = the TuiBlock wire delta + the
  atomic pair-set merge; task 3 = the unpacker contract (sort, caps,
  fallback); task 4 = the post-cutover turn anatomy + turn ordering;
  task 5 = the live-edge + card render contract. Consumers RESTATE
  what they use; they never point at another task file for a signature.

## Decision register (plan-level, one line each; "PL" = plan decision)
- PL-1: the pair transport on the wire = two parallel `const String*`
  arrays (resultKeys/resultValues) + `String resultRaw` + u32
  resultCount/resultMore/resultReserved, appended after `reserved` in
  that order — the flat-C analogue of the tuiSetTurns array pattern;
  strings inbound, C copies during the call; offsets derived + pinned
  both sides in the same commit.
- PL-2: TUI_API_VERSION bumps 12 -> 13 (task 2) -> 14 (task 4); the
  design's C1 wording ("v6"/"v7") labels design generations, not the
  macro. CONFLICT with the design text — recorded in Open issues below.
- PL-3: `uiAgentBusy()` = the ready-clear ONLY; `playNotification()`
  stays the ready half's completion cue; no busy ping.
- PL-4: "the real edge" = the D-side uiAgentBusy/uiAgentReady calls
  crossing to C++ (tuiSetReadyStatus); composer goldens are re-driven
  through that crossing, NEVER by poking `state.readyStatus` (that is
  how today's golden masks the shipped bug).
- PL-5: brackets are plain-bool, non-refcounted, exception-safe
  (`scope (exit)`) BY DESIGN under the at-most-one-run assumption
  (F6); the nesting counter is the named fallback if the task-1
  enumeration finds an overlap path.
- PL-6: `agentRunInFlight_` (app_agent/package.d:925-929) collapses onto
  the ready flag per F6's single-source-of-truth; grep consumers first.
- PL-7: the D4 body fix is a D-SIDE payload change — summarizedCallBody
  (ui.d:652-666) and the streaming body (:763) drop the "name(" wrapper;
  C++ renders the body verbatim; the args summarizer + its 1000-char
  cap are unchanged (W5).
- PL-8: the per-batch Thinking row is a C-SIDE projection grouping the
  batch's existing Reasoning block with its Tool blocks; no new
  D-side events (C4/C5, design §3).
- PL-9: Cancelled tool card = header-only, no marker, no FAIL, no
  RESULT row (W7 recorded decision; N3 honesty; golden-visible via the
  interrupted-turn replay frame).
- PL-10: turn ordering after ordinal removal = store turn-array
  insertion order (live; upsert keeps the slot by turnId) / history
  sequence (replay); store test pins compression re-emission + emitter
  re-attach (W1 fix).
- PL-11: the RESULT caps (100 pairs / 200 codepoints) live with the
  D-side unpacker (I5); the args keep the existing summarizer's
  1000-char cap (W5); caps are RESULT-only.
- PL-12: every golden re-baseline's delta = exactly the expected
  label/order/shape change named in the task; ANY other delta = a
  projection bug — fix the projection, not the golden (the v5 D11
  discipline, carried).
- PL-13: task 8 is docs-only (no code/golden/test changes).

## Cross-cutting invariants (hold in every task)
- C2: the ~20 Hz C-side stream tick throttle is untouched; D keeps
  sending chunk deltas.
- C5: v5 emission architecture stands — typed turns/blocks, upsert
  keyed by (turnId, block id), batchId = "<turnId>#<batchIndex>",
  group id = the persisted turnId, D emits everything
  mode-independently, displayability filter C-side. v6 changes only
  what the appendix changes.
- Upsert-only store mutation (first upsert creates, later merges);
  the only destructive ops remain tuiTurnRemove (cascade),
  tuiSetTurns/tuiThreadClear (snapshots), eviction (per whole turn,
  never the live turn).
- The store never re-parses or re-scans per frame; the pair set is
  computed once at emission (N4) and bounded by the caps (N2).
- Step counts recomputed per render from the weights (D8); hidden
  blocks stored, filtered at render/weight time only.
- The console formatter's output is byte-identical (F9); the
  runtime-status phase edge stays display-only (C3); no per-tool
  execution-start ABI event (C4).
- The drill-down inherits everything through the shared adapter (D7);
  no pipeline special-casing in the generic view; wrong data = a
  population bug, fix the population.

## Commit table
| Task | Commit | Depends | One line |
|---|---|---|---|
| 1 | additive | - | real busy edge: uiAgentBusy body + brackets + flag predicates |
| 2 | additive | - | ABI pair transport (v13) + offsets + store merge |
| 3 | additive | 2 | D-side pair emission: unpacker + live/replay walks |
| 4 | CUTOVER (one commit, both sides) | 2,3 | ordinal removal (v14) + query-first anatomy + order pins |
| 5 | additive | 4 | live edge (both modes) + tool card shape + D-side body fix |
| 6 | additive | 4 | maximal rebalance: per-batch thinking, newest-batch auto-open, fold rules |
| 7 | additive (+fixes) | 1,5,6 | cross-cutting hardening battery |
| 8 | docs-only | 7 | doc sweep (tui_design, todo_modern_ui, stale v5 lines) |

Commit groups (design §9): [1] [2,3] [4] [5,6] [7] [8]; tasks 1 and 2 are
independent and may land in either order.

## Known risks (flag to the design owner if they fire)
- Task 2: the offset re-pin touches 4 version-pin sites + the whole
  TuiBlock pin block — a missed pin = a lying ABI; grep
  `TUI_API_VERSION` before declaring the bump done.
- Task 4: the cutover is the largest diff of the iteration (breaking
  wire change + projection flip + ~15 test files). If parts A and B
  cannot land as ONE green commit, STOP — splitting violates N1.
- Task 5: PL-7 (the body-wrapper fix) changes streaming payloads
  mid-turn — the streaming goldens may shift in ways the completed
  frames do not; eyeball both.
- Task 6: if the store's batch boundaries cannot express the per-batch
  Thinking row grouping, that is an unmodeled data-model gap — stop
  and report, do not invent policy.
- Task 1: collapsing `agentRunInFlight_` may orphan an unknown consumer
  — grep first (PL-6).

## Open issues for user review (plan-time, unresolved by the plan)
- OI-1 (PL-2, design-vs-code conflict): the design's C1 says "task 2
  bumps TUI_API_VERSION to v6, task 4 to v7", but the macro is an
  integer at 12 with `== 12` pins in test_tui_api_v4.cpp:1029,1159,1657
  and test_clear_output_state.cpp:141. The plan resolves this
  conservatively: monotonic macro bumps 12 -> 13 (task 2) -> 14 (task
  4); "v6"/"v7" read as design-generation labels. If the user intended
  the macro to jump to 6/7 (a renumbering, not a bump), tasks 2 and 4
  need that one-line change instead — trivial, but it must be picked
  BEFORE task 2 lands.
- OI-2: the design's §5 says stream ticks upsert Pending tool calls
  "as today", while D4/W4a kill the Pending chip and PL-7 changes the
  call body payload. The plan reads "as today" as the upsert RHYTHM,
  not the payload text (the payload fields named in D4/D5 change).
  Recorded so the executor does not "restore" the wrapper or the chip.

## Out of scope (design §11 — do not implement)
Pipeline engine internals (the engine stays UI-agnostic); agents-tab
lanes and inspector internals; the now bar; session list; chip strip;
logs; runtime-status models; cross-turn search; tuiMoveCall; any
config-selectable legacy path; removing the token fields from the wire
(I4 — they stay, unwired); any fix for the unconfirmed 6.4 scenario.

## Plan-time verification status
- All anchors in the task files were read from the working tree at plan
  time (plan/codebase_notes.md — this session, post-review); no anchor
  is from memory alone.
- The contracts in tasks 1-5 are transcriptions of design §4-§8 + the
  appendix's D1-D8 + the review's W-fixes (all 10 applied); where the
  design left a detail open, the task pins it under a PL letter with
  the rationale.
- Nothing in this plan was executed; every task carries its own
  verification checklist for the executor.
