# Design notes — thread rendering v6 (appendix_fixes.md → system_design.md)

Checkpoint for the v6 design. Source spec: plan/appendix_fixes.md; prior design: plan/previous/system_design.md (v5).

## Phase 1 — requirements (done)

Feedback items → verified root causes (memory `thread_view_feedback_root_causes` + RAG spot checks):
- #4 order: header-first anatomy is v5 design; rejected → D1 (query-first). Structural, adapter+view.
- #4 live edge: chips line draws only while a tool runs; nothing while thinking streams → D2 (new live-edge element set).
- #5: maximal collapses thinking, opens all cards — v5 rule as designed → D3 reversal.
- #6a: "[N]" prefix = v5 ordinal feature (thread_store.cpp ~605: `"[" + ordinal + "] "` on typedQuery rows) → D5 removal (display + data model).
- #6b: `void uiAgentBusy() {}` (tui/package.d ~1093); `uiAgentReady()` sets `setReadyStatus(true)`. userQuery (app_agent/package.d ~1453-1473) and doCompress raise busy/ready into the empty half; slash_pipeline.d never raises it. Composer/palette read `state.readyStatus` (palette.cpp ~657-679, ~824-845) → D6 one missing body + missing brackets.
- #7: "+" = icons.cpp check-mark ASCII fallback; body repeats "name(" (summarizedCallBody); args one-per-line; RESULT one-line blob → D4 card shape.
- #8: maximal wall on a long NORMAL-view turn (golden blocks-maxanatomy-100x30.txt); not a regression from the pipeline query fix (3.8) → fixed by D3 + D1.

Requirements: F1 query-first anatomy; F2 minimal live edge; F3 maximal rebalance; F4 tool card shape + RESULT unpack; F5 no ordinals; F6 busy edge real; F7 scope (drill-down inherits, D7); F8 count units (D8). MoSCoW: F1,F2,F4,F6 Must; F3 Must; F5 Should (rides cutover); F7,F8 Must-as-scoping.

## Phase 2-6 — decisions (from appendix §4, no additions beyond stated interpretations)

- D1 query+answer become turn-level rows; group = thinking + tool steps + streamed prose. Fold keeps query+answer; header only while group has rows.
- D2 live edge: chips line (Running calls, orange, name-only, non-clickable, folds on dormancy incl. mid-batch) under the header in minimal; folded Thinking row (count only) while thinking streams; nothing while prose streams; ≤1 live element; live count updates with tools.
- D3 maximal: one Thinking row per response batch (label = "N tool calls" count only, trailing summary removed); thinking open by default; tool cards auto-open newest batch only (live-only); group folds to header+counts when the turn finishes; re-expand = thinking rows open, tool rows folded, nothing re-opens; mid-stream open re-applies mode defaults + newest details open in BOTH modes; overrides (ForceOpen/ForceClosed, turnId-keyed) still beat defaults.
- D4 card: disclosure arrow, no status glyph (FAIL textual red on failure), no token rendering, body = args one key=value per line (no name prefix), RESULT = dim `key = value` lines; unpack one level, JSON objects only, D sorts by key + caps (100 pairs / 200 chars + moreCount), zero-pair/array/non-JSON → packed raw line; C renders, never re-parses.
- D5 ordinal leaves display AND data model (TuiTurn/UiTurnData lose ordinal; wire break → rides cutover commit).
- D6 ready flag = single source of truth; uiAgentBusy body = setReadyStatus(false); /plan + /code handlers bracket the pipeline run (exception-safe); phase edge stays display-only; stop affordances read the flag.
- D7 drill-down inherits everything; no pipeline special-casing in the generic view; population bugs fixed at population; drill-down keeps no query row.
- D8 WORK count = steps (call+result=2, prose=1 — v5 weights unchanged); Thinking counter = tool calls; units never mix.

## Stated interpretations (appendix-silent points)

- I1: Notice blocks stay group rows (render as today); D1 moves only query+answer.
- I2: at most one live element (thinking row or chips line) — naturally exclusive by run phase order.
- I3: Thinking label "Thinking" bare when the response carries 0 calls.
- I4: token fields stay layout-pinned on the wire; renderer + goldens stop using them.
- I5: caps applied D-side; moreCount rides the pair set for the "... K more" tail.
- I6: busy/ready brackets exception-safe (scope exit) so a failed run restores Send.
- Open question 6.4 kept as caveat, no work.

## Review round (design_review_v6.md, APPROVE WITH FIXES) - fix list 1-10 applied to plan/system_design.md

All ten fixes applied; remaining steps are verify + report:
- Verify task 4 names GroupView.ordinal + version bump to v7; task 2 says "bump TUI_API_VERSION to v6"; task 5 says Pending-no-chip / both-mode live edge / Cancelled header-only / args keep summarizer cap; task 6 has override-survives + accepted-consequence acceptance (edits sent; some noOp notes lie - F5 and Live-edge edits reported noOp but DID apply; verify by reading, never by the note).
- Apply Notes bullet: record 6.1 (glyph) closed by D4 and 6.2 (ordinal feature) closed by D5, 6.4 stays caveat - NOT YET APPLIED.
- Verify whole file numbering 9 -> 10 -> task sections -> Notes -> 11 reads cleanly.
- Update memory topic thread_view_v6_design_status (review round applied, fixes 1-10).
- pipelineOutput with the applied-fixes report; NO task_done this turn.
- Always pass mode=replace to edit_file; missing mode errors the call.

## Task shape (P0-P3, 8 tasks)

1 P0 busy edge (D-side only, no wire dep) · 2 P0 ABI v6 additive (pairs + bump, both sides build, old goldens pass) · 3 P0 D pair emission (live+replay+console parity) · 4 P1 cutover (ordinal removal both sides + adapter/view query-first flip) · 5 P1 minimal live edge + card shape goldens · 6 P1 maximal rebalance goldens · 7 P2 hardening battery · 8 P3 doc sweep. Deps: 3←2; 4←2,3; 5,6←4; 7←1,5,6; 8←7.

Test assets: golden workflow per memory (make -j8, xvfb for *_gui, --update regolden + eyeball diff, dub test ~110 s).
