# Implementation Plan: Thread rendering v5 — typed turns and blocks

Source of truth: `plan/system_design.md` (approved, fixes applied) +
`plan/appendix.md` (contract) + `plan/design_review.md` (W1-W14, all closed).
Plan-time survey: `plan/codebase_notes.md` (verified anchors), requirements
digest: `plan/requirements_notes.md`. Ten tasks, ten commits, ordered.

## Standing executor rules (apply to every task)
- Re-verify EVERY file:line anchor before editing it. Anchors were verified at
  plan time; code moves. A moved anchor: fix the task file's anchor (one-line
  note in its Verification), never guess from memory.
- Conflicts: the design + appendix win over today's code; today's code wins over
  task-file anchors (snapshots). Any NEW conflict discovered mid-task: record it
  in that task's Notes section, resolve per the first rule, and continue.
- Known pre-existing reds (never re-baseline): parity_core_loop's 2 perf-gate
  FAILs, test_metrics' gate-ratchet FAIL. Everything else must be green.
- Workarea: all paths repo-relative under /workarea (plan/ = /workarea/plan,
  code = llmfun/...). read_file/edit_file reject ../ paths.
- Build/test: `dub test` from llmfun/ (~15 s; 400 s+ = hang: kill + investigate).
  C++ binaries in llmfun/cpp_tui/build (cmake -S cpp_tui -B cpp_tui/build if
  missing); plain binaries run direct, *_gui need `TERM=xterm xvfb-run -a ./X
  --out .`. Regolden only via the --update flow, then eyeball the diff.
- grep_files intermittently returns a transient empty/error result: re-run once
  before concluding a symbol is absent.
- The llmfun execution environment was down at plan time (missing image); if it
  is still down, use any listed environment for builds/tests — never skip the
  verification step of a task.
- Contract artifacts (verbatim in the tasks) are the single source for shared
  shapes: task 1 = wire types + entry points; task 2 = store API + step-count
  weights + displayability predicate; task 3 = block field contract + emission
  order; task 4 = console line mapping. Consumers RESTATE what they use; they
  never point at another task file for a signature.

## Decision register (plan-level, one line each)
- D1: `tuiSetTurns` carries turns AND blocks (design §5 wins over the older
  3-arg sketch in doc/todo_modern_ui.md §8.2).
- D2: TuiTurn.id / TuiBlock.turnId = the persisted int64 turnId; block id =
  tool callId | "answer" | batch-scoped uid; batchId = "<turnId>#<batchIndex>"
  (0-based), tool-less batches included, not persisted, deterministic on replay.
- D3: the displayability filter is C-side: kHiddenToolNames mirrors today's
  D-side hiddenTools on wire (snake_case) names; the executor verifies the
  exact set against today's isDisplayable behavior and pins it in a unit
  test. Hidden = not rendered + step-weight 0.
- D4: step-count weights BY STATUS: Tool Ok/Failed = 2, Pending/Running/
  Cancelled = 1; response-content Prose = 1; Reasoning = 0 (rides the
  content message); turn-opening query Prose = 0 (user message); Answer = 0
  (counted in its task_done tool block); Notice = 1; hidden = 0.
- D5: the turn ordinal = 1-based position among the session's turns,
  D-assigned, recomputed on compression re-emission, never recomputed
  C-side; renders once, on the turn's query row (today's per-message [nr]
  dies — F8).
- D6: one typed store per v4 ChatTab equivalent (main + one per agent,
  embedded in ChatTab); the pipeline entry points keep their structured
  signatures and are retargeted C-side at the cutover (design §4).
- D7: statuses are emitter-driven upserts (Pending->Running on stream-done,
  Ok/Failed on the result, Cancelled on stop); the store merges, never
  enforces.
- D8: the console parity golden is CAPTURED from today's real output
  (recording sink + today's blocked-sink write rules), never hand-written.
- D9: layout pins live in the C++ contract test + D-side static asserts,
  never in tui_api.h (user convention abi_seam_no_pins); TUI_API_VERSION
  bumps to 12 in task 1.
- D10: ONE C-side displayability predicate shared by the weights, the
  thread rendering, the drill-down, and the console formatter.
- D11: the cutover's "existing goldens pass" acceptance = a ONE-TIME
  re-baseline whose delta is exactly the F8 ordinal change + the
  thinking-attachment shape; ANY other delta = a projection bug (fix the
  projection, not the golden).

## Cross-cutting invariants (hold in every task)
- Upsert-only mutation on the store: a first upsert creates the slot, later
  upserts merge in place; the only destructive ops are tuiTurnRemove (which
  cascades), tuiSetTurns/tuiThreadClear (snapshots), and eviction.
- The store never re-parses or re-scans on upsert; the ~20 Hz C-side
  throttle (model/stream_state.h kStreamUpdateIntervalMs) is untouched.
- D emits everything, mode-independently; minimal/maximal are C-side
  projections over the same store; the inspector (lanes, Activity) is
  untouched and keeps the full feed.
- The step count is recomputed per render from the weights — never stored.
- Hidden blocks: emitted by D, stored, filtered at render/weight time only.

## Commit table
| Task | Commit | Depends | One line |
|---|---|---|---|
| 1 | additive | - | v5 wire types + ABI entry points + pins |
| 2 | additive | 1 | C++ typed store + adapter rewrite (dual-source, v4 live) |
| 3 | additive | 1 | D-side typed emitter: replay/live/compression |
| 4 | additive | 3 | console formatter over typed events + parity golden |
| 5 | CUTOVER (one commit, both sides) | 2,3,4 | flip to typed path, delete v4 message path |
| 6 | additive | 5 | minimal mode + live folding + goldens |
| 7 | additive | 5,6 | maximal mode + goldens |
| 8 | additive | 6,7 | settings row + per-group override |
| 9 | additive | 5 (+6-8) | cross-cutting battery (harness + units) |
| 10 | docs-only | 9 | doc sweep (todo_modern_ui, todo, tui_design) |

## Known risks (flag to the design owner if they fire)
- Task 4: today's console can write the "Thinking: " prefix from BOTH the
  content and calls messages; v5 has one Reasoning block per batch. The
  captured golden is the arbiter (the task pins the resolution rule).
- Task 5: the golden re-baseline delta must be exactly D11's; anything
  else means the typed projection is wrong.
- Task 2: the exact kHiddenToolNames set must be verified against today's
  isDisplayable behavior (D3), not assumed.

## Out of scope (appendix §11 — do not implement)
Session list/now bar/chip strip/log/runtime-status v5 models; cross-turn
search index; tuiMoveCall; agents-tab lane internals and inspector
internals; a key shortcut or palette command for the mode; any
config-selectable legacy path.

## Plan-time verification status
- All anchors in the task files were read from the working tree at plan
  time (plan/codebase_notes.md); no anchor is from memory alone.
- The contracts in tasks 1-4 are transcriptions of design §4-§5 decisions
  plus the appendix constraints; where the design left a detail open, the
  task pins it under a decision letter (D2/D4/D5) with the rationale.
- Nothing in this plan was executed; every task carries its own
  verification checklist for the executor.
