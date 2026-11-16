## Task 4: Cutover — ordinal removal + query-first anatomy (D5 + D1)

Priority: P1. Dependencies: tasks 2, 3. **ONE commit, both sides** (N1): the
ordinal removal is a breaking wire change and cannot split across commits; the
projection flip rides it (design §9 group 3).

### Description
Remove the turn ordinal everywhere (display + data model, D5), bump the ABI
for the breaking layout change, and flip the adapter/thread view to the
query-first turn anatomy (D1). After this commit: no "[N]" prefix anywhere, no
ordinal field anywhere, the query row opens the turn above the WORK header, the
answer stays a turn-level row, the group's row list carries only work rows, and
the WORK header renders only while the group has rows.

### Changes Made — part A (ordinal removal, both sides, same commit)
- `llmfun/cpp_tui/tui_api.h`: TuiTurn (:655-663) loses `ordinal` — a breaking
  layout change; bump `TUI_API_VERSION` 13 -> 14 (:29, PL-2); shift + re-pin
  every subsequent struct offset in the contract test.
- `llmfun/source/llm/tui/package.d`: UiTurnData loses `ordinal` (:1236); the
  conversions drop it (toTurnData/toTuiTurn :45-57, copies at :46,53);
  static asserts re-pinned for the new TuiTurn layout.
- D emitter: stop assigning ordinals — live turn open (:986-995
  `typedCtx_.ordinal = typedOrdinalNext_++`), per-batch (:1009), live turn
  state upserts (:1121,1137), replay walk (:110-111 `nextOrdinal++`),
  compression re-emission (:1156-1175, the typedCtx_.ordinal reset dies with
  the feature — F5 removes the recomputation v5 required).
- `llmfun/cpp_tui/store/thread_store.h`: GroupView loses `ordinal` (:100-108,
  field at :107) — W1 names it explicitly; do not leave the dead field.
- `llmfun/cpp_tui/store/thread_store.cpp`: `entry.view.ordinal = turn.ordinal;`
  (:209) dies; the "[N]" summary prefix (:604-610) dies; the drawn ordinal in
  renderBlockBody (:714-721) dies.
- `llmfun/cpp_tui/store/typed_thread_store.h:27-38`: TypedTurn loses ordinal.
- Tests that pin ordinals — update at the cutover, do not preserve them:
  `llmfun/cpp_tui/test/typed_seed.h` (10, 44-46, 83-141: ordinal = seed order);
  `llmfun/cpp_tui/test_typed_thread_store.cpp` (:257-262, :314);
  `llmfun/cpp_tui/test_blocks.cpp` (1888-1982; the "ordinal sits next to the
  header's step count" pin 1914-1916);
  `llmfun/cpp_tui/test/parity_core_loop.cpp` (the upsertTurn helper's ordinal
  574-585, ordinal-sequence pins 615-661, 1077, 1113, 1673-1681, 1726-1731,
  1766-1788, 1812-1818; the compression line "re-emitted with a recomputed
  ordinal" 1817 — DIES, replaced by the order pins below);
  `llmfun/cpp_tui/test_palette.cpp:466` ("[1] ordinal prefix");
  `llmfun/cpp_tui/test_e2e_gui.cpp:678-680` (ordinal 9 cap assert);
  `llmfun/source/llm/app_agent/tests.d` (the ordinal assertions: 1930,
  1985-1986, 2014, 2138, 2215, 2278-2279, 3370, 3400, 3492, 3556, 3584-3590,
  3602-3633).
- Do NOT touch `llmfun/source/llm/tui/tests.d:352-355` (phase ordinals — an
  unrelated feature that happens to share the word).

### Changes Made — part B (query-first projection + order pins, same commit)
- Adapter projection (`llmfun/cpp_tui/store/thread_store.cpp/.h`): the query
  block and the answer block surface as TURN-LEVEL rows outside the group;
  the group's row list carries only work rows (thinking, tool steps, streamed
  prose; notices keep rendering as group rows, I1). The store data is
  untouched — the query remains the turn's first block; this is projection
  + render order only (design §6).
- Thread view render: the query row draws above the group header; the
  answer row after the group; the WORK header renders between them only
  while the group has rows; an empty group (a turn with no work rows)
  renders headerless — query and answer stand alone (F1).
- Minimal fold: keeps query and answer visible always (the header sits
  between them only while rows exist) — the fold rules themselves are
  task 5/6; only the anatomy changes here.
- Turn ordering after ordinal removal (W1/PL-10): live turns order by the
  store's turn-array insertion order (first upsert appends, re-upserts
  keep the slot by turnId); replay order comes from the history sequence.
  Store test (extend `llmfun/cpp_tui/test_typed_thread_store.cpp`):
  turn order pinned across a compression re-emission AND an emitter
  re-attach after a session switch — insertion order held, no reorder.
- `llmfun/cpp_tui/test/parity_core_loop.cpp` — replace the 1817
  recomputed-ordinal pin with the order pins above.
- `llmfun/cpp_tui/test/parity_core_loop.cpp` — replace the 1817
  recomputed-ordinal pin with the order pins above.
- Emitter fix the re-attach pin exposed: `llmfun/source/llm/app_agent/
  package.d` `activateSession` clears `typedCtx_` before the switch
  replay — the old session's live-ctx turn id must not filter the fresh
  session's snapshot (every session's ids restart at 1, so the stale
  filter could drop a persisted turn of the NEW session; the fresh
  history cannot contain the old live turn anyway, and the snapshot's
  wholesale replace wipes the live state). The NR test's session-switch
  doc now stamps `turn_id` like a real persisted session, which is what
  exposed the stale filter.

### Verbatim contract — the post-cutover turn (task-4-owned; tasks 5-6 restate)
- Turn anatomy, top to bottom: query row (the turn's opening row),
  [WORK header + work rows] while the group has rows, answer row.
- No ordinal anywhere: no field on TuiTurn/UiTurnData/TypedTurn/GroupView,
  no assignment, no "[N]" prefix, no drawn number (search-verified).
- Turn order = store insertion order (live) / history sequence (replay);
  ordering is turnId's job (D5).
- Empty group -> no WORK header (F1).
- Notices stay group rows (I1); the drill-down keeps having no query row
  (its turns are agent runs, D7 — correct, unchanged).

### Verification
- [x] ONE commit: git show --stat reviewed — parts A and B together; no
      intermediate commit renders ordinals or the old anatomy.
- [x] Sweep clean either side: grep `ordinal` AND `"[N]"` across
      llmfun/source/llm + llmfun/cpp_tui — every remaining hit is deleted
      here or the unrelated tui/tests.d:352-355 phase test (noted).
- [x] TUI_API_VERSION pins all at 14 (test_tui_api_v4.cpp 1029/1159/1657,
      test_clear_output_state.cpp:141); contract test re-pinned for the
      shifted layouts; D static asserts compile.
- [x] Both builds green; `dub test` green (1100 passed, 0 failed); all 54
      cpp_tui suites green via the replaced run_suite.sh (no exclusions —
      the parity perf gates pass this session; the runner classifies a
      perf-gate failure as the plan's known-red: informational, non-gating).
- [x] Regolden minimal goldens + eyeball: the delta is ONLY the expected
      label/order change (no "[N]", query above header, answer after
      group). ANY other delta = a projection bug — fix the projection,
      not the golden (PL-12; the v5 D11 discipline).
- [x] Store/view tests green: empty group renders headerless (new
      thread-view test in test_blocks.cpp); turn order pinned across
      compression re-emission + emitter re-attach (new store tests in
      test_typed_thread_store.cpp).
- [x] Console parity golden byte-identical (F9 — the console never
      rendered ordinals; the NR derivation test pins it and passes).

### Notes
- The maximal-anatomy golden is regenerated HERE or at task 6, whichever
  lands its shape first (design §9.3) — if the maximal shape is still
  v5's at this commit, regolden it at task 6 and record that here.
- tokensIn/tokensOut STAY on the wire (I4) — the renderer stops drawing
  them in task 5; this task does not touch them.
- The composer flip is task 1's; nothing here.
- Plan-time anchors to re-verify (they move): every file:line listed above.

Plan pointers (task-specific only — standing rules live in the overview):
- Decision letters used: PL-2, PL-10, PL-12 (defined in the overview).
- N1 is the acceptance's spine: if part A and part B cannot land as one
  green commit, STOP and report — splitting is a design violation.
