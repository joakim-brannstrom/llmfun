## Task 9: Cross-cutting battery (design P2-8)

Priority: P2. Dependencies: task 5; extends with tasks 6-8. One commit.

### Description
The hardening battery over the live v5 path, driven through the headless
harness plus targeted unit tests — every §7 failure mode and §8 scenario,
end to end.

### Changes Made
- llmfun/cpp_tui/test/parity_core_loop.cpp: the harness drives the real run
  path (post-cutover: the typed emitters) over scripted sessions and
  goldens the renders (goldenText 1292 pattern; the v4 tuiAddChatMessage
  calls at 573/831 become the v5 emissions). Scenarios (§7/§8):
  1. Replay parity: the same session rendered twice -> byte-identical;
      expansion/override state survives the re-render.
  2. Stop mid-batch: in-flight calls -> Cancelled rendered honestly, the
      partial answer stays, no fabricated Ok.
  3. Compression re-emission: affected turns re-emitted (remove + upsert),
      ordinals recomputed, no stale group with orphaned block ids.
  4. 1000-message session: open + scroll through the turn-paged window,
      height-hint eviction unchanged, bounded render cost.
  5. Session switch mid-run: the in-flight turn belongs to its session;
      switching back rebuilds the store from the snapshot; the live emitter
      re-attaches by turnId.
  6. Interrupted-turn replay: in-flight calls replay as Cancelled.
- llmfun/cpp_tui test (store unit): upsert storm — thousands of upserts
  (content ticks + call/result merges) leave the store consistent and the
  render correct; asserts the no-re-parse/no-re-scan property structurally
  (single index hit per upsert), NOT a timing gate (the two pre-existing
  perf-gate FAILs in parity_core_loop stay excluded — never re-baseline).
- llmfun/cpp_tui test (store unit): result-for-unknown-callId (live): the
  result is recorded under its callId and the merged step renders when
  the call lands (§7 — never silently dropped).

### Verification
- [x] All listed scenarios green in the headless harness (xvfb-run pattern
      for *_gui binaries; llmfun_cpp_tui_test_workflows memory).
- [x] The 1000-message scenario completes without unbounded growth (the
      kMaxTurns eviction from task 2 engages; no perf gate added).
- [x] Replay parity is byte-identical including the ordinal sequence.
- [x] `dub test` + cpp_tui binaries green (known pre-existing reds excluded).

### Notes
- The D-side battery mirrors here where the emitter is involved:
  compression re-emission and interrupted-turn replay have task-3
  recording-sink tests; this task re-runs them THROUGH the live path.
- No new perf gates (the known-red policy); correctness only.

### Implementation notes (task 9)
- STATUS 2026-11-15: verified COMPLETE and GREEN in the current tree; the battery
  code was committed inside the "task 5-6" (3cee562: parity checkBattery +
  helpers) and "task 7-8" (28a8206: store battery) commits, so no separate
  task-9 commit exists. Re-verified this session: dub test 1065/0;
  parity_core_loop 208 text + 81 gui checks passed; test_typed_thread_store
  112 checks passed (-v). The pre-existing perf-gate reds were not touched.

### Implementation notes (task 9, original)
- parity_core_loop.cpp gained `checkBattery()` (hooked in runText after
  checkFocusMode, before checkPerf), plus two harness helpers: `toolBlock()`
  (a Tool block with the tool name on `title`, the kToolBlockFlags set, so
  the merged-step shape D's walk emits is expressible) and `findBlock()` (a
  per-turn scan; the store has no block-by-id index). 44 new checks;
  parity_core_loop is now 208 text + 81 gui, fully green, NO golden changes
  (the battery's parity is an in-memory byte comparison, not a golden file).
- Scenario notes: (1) replay parity compares goldenText twice; the
  override survival pins a work group ForceClosed through the task-8
  maximal-mode seam (setExpanded writes `ChatTab::groupOverrides` keyed
  by the group id = the persisted turn id) and re-compares bytes. The
  ordinal check is relative (turnAt(1).ordinal == turnAt(0).ordinal + 1):
  g_turnSeq is a process-global and the battery runs after the loop
  phase, so absolute ordinals are not stable across phases. (3)
  compression re-emission rebuilds the replay scene itself (the battery
  shares one harness, and scenario 2's tuiThreadClear already dropped
  the earlier turns); the pin is placed, then both turns are removed
  and one summary turn re-emitted - the pin dies with the turn (task
  8's tuiTurnRemove erase), the re-emitted ordinal is 1, the group
  list follows, the summary renders. (4) the 1000-turn scenario
  asserts structurally: turnCount()==1000 after 1002 upserts (the
  W2 eviction, oldest first), the window is the grid's bounded page
  (pageGroups==40), drawnGroups==40 (NOT the store), the "960 more"
  count row, each "Load earlier" press grows drawnGroups by exactly
  one page (40 -> 80 -> 160), and hints.size() <= drawnGroups+1
  (height hints cover the drawn groups only). The thread's visible
  clip starts at the window's TOP (no live turn = no bottom anchor),
  so the newest turn is asserted through groupAt(999).id == 1002,
  not through a screen row. (5) the switch mirrors stepSelectSession's
  D-half shape (new active snapshot + tuiThreadClear + replay); the
  live emitter's turn is absent from the new session's store, and
  the re-attach by turnId merges in place (blockCount constant, no
  duplicate slot). (6) the interrupted call replays Cancelled, and
  a per-turn scan asserts NO block of the replayed turn claims Ok.
- Store battery (test_typed_thread_store.cpp, one block, 9 checks):
  the upsert storm (1500 content ticks + 300 call/result merges on
  one turn) asserts the no-re-parse property structurally - slots
  never duplicate, last write wins, and version() bumps exactly
  once per mutating call (2100 + the turn upsert) - plus the render
  side through a bound adapter (one group, "WORK · 2 steps"); the
  result-for-unknown-callId merge lands under the call's id, then
  the call's upsert merges in - ONE settled step, Ok status, step
  weight 2, never a dropped half. 105 checks total, green.
- No perf gates added; the two historical perf-gate FAILs stay gone
  (never re-baseline).
