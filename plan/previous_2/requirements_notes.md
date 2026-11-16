# Requirements notes — Thread rendering v6 implementation plan

Source of truth: `plan/system_design.md` (v6, review-complete: fix list 1-10 from
`plan/design_review_v6.md` applied and verified). Contract/spec: `plan/appendix_fixes.md`.
Prior iteration (v5, shipped): `plan/previous/` (its plan format is the style template).
Checkpoint notes: `plan/design_notes.md`.

## Goal

Break the design's eight design-level tasks (section 10) into executor-ready
implementation tasks: verbatim contracts, verified line anchors, per-task
verification. The plan does NOT write implementation code; `code-task` executes.

## Scope (from design §1-§2, §11)

Implement D1-D8 in one iteration:
- D1 query-first turn anatomy (adapter projection + view render order; minimal fold keeps query+answer; header only while group has rows; empty group → no header).
- D2 live edge, BOTH modes (orange name-only chips for Running calls; folded count-only Thinking row while thinking streams; nothing while prose streams; no Pending chip; ≤1 live element; live step count; folds on finish).
- D3 maximal rebalance (Thinking row per response batch, open by default, label = call count only, trailing summary removed; newest-batch-only card auto-open; fold-on-finish only for groups at mode default — explicit mid-stream manual open survives; re-expand = thinking open/cards folded; mid-stream open = mode defaults + newest details open in both modes; overrides still beat defaults).
- D4 tool card shape (disclosure arrow, no status glyph, FAIL red on failure, no token fields; body = args key=value one per line, summarizer 1000-char cap unchanged; RESULT = dim `key = value` lines from D-sent pair set; packed fallback for array/non-JSON/zero-pair; caps 100 pairs / 200 chars + moreCount; C renders, never re-parses).
- D5 ordinal removal, display + data model (TuiTurn/UiTurnData.ordinal, D emitter assignment, GroupView.ordinal, "[N]" prefix; ordering = store insertion order live / history sequence replay).
- D6 real busy edge (uiAgentBusy body clears ready flag; search-verified entry-point enumeration BEFORE task closes; exception-safe brackets; /plan /code brackets; stop affordances read the flag; Stop targets the run in flight regardless of session shown; at-most-one-run assumption stated, nesting-counter fallback).
- D7 drill-down inherits everything via the shared adapter; no pipeline special-casing in the generic view.
- D8 counts pinned (WORK = steps: call+result=2, prose=1; Thinking counter = tool calls; units never mix).

Explicit out of scope (§11): pipeline engine internals, agents-tab lanes, inspector, now bar, session list, chip strip, logs, runtime-status models, cross-turn search, tuiMoveCall, config-selectable legacy path, removing token fields from the wire (I4), any 6.4 fix.

## Hard constraints

- C1: every layout-changing commit bumps its own TUI_API_VERSION and re-pins asserts: task 2 (additive pairs) → v6; task 4 (ordinal removal) → v7.
- C2: ~20 Hz C-side stream tick throttle untouched; D keeps chunk deltas.
- C3: now bar, chip strip, logs, runtime status, agents tab unchanged; scope = thread view + drill-down (inheritance) + composer button.
- C4: no new per-tool execution-start ABI event; status transitions ride stream-done/result/stop.
- C5: v5 emission architecture stands (typed turns/blocks, upsert keyed by (turnId, block id), batchId = "<turnId>#<batchIndex>", group id = turnId, D mode-independent, C-side displayability filter).
- N1: no dual-source window; ordinal removal rides ONE cutover commit.
- N2: bounded memory (capped store, per-turn eviction, pair set bounded by caps).
- N3: honesty rules (nothing renders that did not come from the run; FAIL never silent; Cancelled header-only, no invented state).
- N4: no per-frame re-scan; D unpacks once at emission; C renders strings.
- F9: console output byte-identical (parity golden).

## Interpretation letters (design-stated)

I1 notices stay group rows; I2 ≤1 live element; I3 "Thinking" bare when 0 calls; I4 tokens stay on wire, renderer stops drawing; I5 caps live with D unpacker; I6 exception-safe brackets.

## Task shape (design §9-§10 — commit groups)

1. P0 busy edge (D-side only) — independent.
2. P0 additive: task 2 (ABI v6 pair fields, both sides build, goldens unchanged) then task 3 (D pair emission behind recording sink).
3. P1 CUTOVER: task 4, one commit, both sides (ordinal removal + v7 + query-first projection).
4. P1 rendering: task 5 (minimal live edge + card shape) then task 6 (maximal rebalance).
5. P2 hardening (task 7), then P3 doc sweep (task 8).

Deps: 3←2; 4←2,3; 5,6←4; 7←1,5,6; 8←7. Tasks 1 and 2 independent.

## Acceptance anchors (from design §8 testing strategy — must appear in task verifications)

- Wire pins: static asserts D-side + offset tests C-side for v6 (pairs added) and v7 (ordinal removed); version-bump contract test.
- D emission tests: no ordinal on turn upserts; sorted pair set (byte-wise key order, dup-key last-wins, codepoint-safe 200-char truncation, 100-pair cap, moreCount), packed fallback, nested compact JSON; replay parity; Cancelled no pairs; cap boundaries (100→moreCount 0, 101→1; value exactly 200 chars truncates nothing; multi-byte UTF-8 char at cut stays valid).
- Busy-edge tests: uiAgentBusy clears flag (the regression test for the shipped bug); brackets userQuery/doCompress; /plan /code raise edge; exception unwind restores ready; stop affordances read flag; verified entry-point set listed in acceptance + each named path has a test.
- Store tests: pair fields stored/merged, atomic four-field pair-set replacement, ordinal gone from TuiTurn AND GroupView, turn order pinned across compression re-emission + emitter re-attach, step weights unchanged.
- Adapter/view tests: query-first ordering, empty-group header suppression, minimal fold, live-edge selection + dormancy folding incl. mid-batch, maximal newest-batch auto-open, finished fold + re-expand, mid-stream open both modes, override interplay, W4 pins (no Pending chip; manual open survives finish fold; live edge both modes), no "[N]" anywhere.
- Goldens: regolden + eyeball; composer-busy re-driven from the REAL D-side edge (not poking the C++ flag — that is how today's golden masks the bug).
- Console parity golden byte-identical.

## Environment facts (memories: cpp_tui_build_test_golden_workflow, llmfun_execute_command_usage, workarea_and_test_pitfalls)

- dub test from llmfun/; ~20 s when healthy; a "hang" may be one failing unittest formatting a backtrace.
- cpp_tui binaries build into cpp_tui/build (cmake -S cpp_tui -B cpp_tui/build if missing); *_gui need `TERM=xterm xvfb-run -a ./X --out .`; regolden only via --update flow + eyeball diff.
- llmfun execute_command env takes AT MOST 1 command argument (one shell string).
- grep_files transient failures: re-run once before concluding absence. NEVER conclude from a truncated grep (the lesson that hid package.d:1434).
- edit_file: always pass mode; results can echo pre-edit lines / lie about noOp — verify by read_file after each edit.
