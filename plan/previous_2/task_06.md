## Task 6: Maximal-mode rebalance (D3)

Priority: P1. Dependencies: task 4. Projection-only; no new D-side events
(PL-8; design §9 group 4, part 2).

### Description
Rebalance maximal: per-response Thinking rows (open by default, count-only
label, trailing summary removed), tool cards auto-open only for the newest
batch, no second count row, live fold-on-finish, the re-expand shape, the
mid-stream open rule, and the override interplay. Minimal and maximal render
the IDENTICAL shape with identical click behavior; the modes differ only in
fold defaults (F3).

### Changes Made
- Per-batch Thinking rows (projection, `llmfun/cpp_tui/store/thread_store.cpp`
  + the adapter): one Thinking row per response batch, grouping that
  response's thinking with its N calls — the row's label is the CALL
  COUNT only ("Thinking - 2 tool calls"); the trailing summary today's
  disclosure renders is REMOVED; the open body is the full thinking text.
  "Thinking" bare when the batch carries 0 calls (I3). Steps and tool
  calls never mix in one count (D8).
- Fold defaults (replace the v5 rule `defaultOpen = !groupMinimal &&
  kind != Thinking` at `llmfun/cpp_tui/store/thread_store.cpp:642-643`):
  - thinking renders OPEN by default in maximal (reversing v5); folded
    in minimal;
  - tool cards auto-open only in the NEWEST batch (maximal); older
    batches render as folded headers; minimal folds every card;
  - NO second count row inside the group — the header's step count is
    the only count (the group row list does not gain a batch counter).
- Fold-on-finish (live, both modes): when the turn finishes, a group
  AT ITS MODE DEFAULT folds to its header with the counts — the same
  finished shape minimal shows. An explicit mid-stream manual open
  (a user override) SURVIVES the finish fold and keeps the finished
  shape visible (W4b — precedence pinned: override > fold-on-finish).
  A passive maximal watcher sees the open live tail fold at completion
  — the recorded ACCEPTED CONSEQUENCE, not a bug (W10; assert it).
- Re-expand (finished turn): every Thinking row renders open, tool rows
  as folded headers — nothing re-opens; the newest-batch rule has no
  bearing on a finished turn.
- Mid-stream group open: re-applies that MODE's fold defaults (maximal
  opens every Thinking row, minimal keeps them folded) and renders the
  newest batch's full details (arguments AND result) open in BOTH
  modes — the one live exception.
- Overrides (v5 F7 mechanics unchanged): opening a group in minimal /
  collapsing one in maximal writes the session-local override keyed by
  the persisted turn id (:85 GroupOverride; the group id remains the
  persisted turnId); it beats the mode defaults. setBlockExpanded
  (:647-688) and groupAt (:378-381) keep their row-pin mechanics; the
  DEFAULT derivation is what changes.
- The group's own disclosure state is unchanged (maximal still renders
  the group header unfolded in maximal mode — only the thinking/card
  defaults move, design §6).

### Verbatim contract — maximal defaults (task-6-owned; minimal = v5's)
- Live maximal group: open; every Thinking row open (label = call count
  only); newest batch's cards open (args + RESULT), older batches'
  cards folded headers; the live edge at the group's bottom (task 5).
- Finish: groups at mode default fold to header + counts; explicit
  mid-stream manual opens survive; re-expand = thinking open, cards
  folded, nothing else re-opens.
- Mid-stream open in either mode: mode defaults + newest batch's full
  details open in both modes.
- Overrides beat defaults (v5 F7 unchanged, keyed by turn id,
  session-local).

### Verification
- [ ] Golden frames (regolden + eyeball): the long-turn wall scenario
      (maximal, thinking + newest batch open, older cards folded —
      `llmfun/cpp_tui/test/golden/blocks-maxanatomy-100x30.txt`
      regenerated here); finished group folded; finished re-expand;
      mid-stream opened in minimal AND maximal.
- [ ] View/store tests: newest-batch auto-open (live only); no second
      count row; re-expand shape; mid-stream open rule in both modes;
      override interplay (ForceOpen/ForceClosed still beat defaults);
      a mid-stream manual open survives the finish fold (W4b pin);
      the no-Pending-chip + live-edge-both-modes pins from task 5 hold
      under maximal too (W4 pins live in the shared tests).
- [ ] D8 units pinned: call+result = 2 steps, prose = 1, thinking = 0
      (weights at `llmfun/cpp_tui/store/typed_thread_store.h:73-87`
      UNCHANGED — a test asserts them; a thinking+2-calls response
      counts 0 steps by itself and its calls 2/1 while live).
- [ ] The accepted consequence asserted (the maximal tail folds at
      completion) — one test comment + assertion so it is not
      re-litigated as a bug (W10).
- [ ] The maximal-anatomy golden regenerated + EYEBALLED (this is the
      wall scenario the feedback reported — the reviewer looks at it).
- [ ] `dub test` green; cpp_tui suite green incl. xvfb GUI binaries.

### Notes
- PL-8: this task adds NO D-side events and NO new blocks — the
  per-batch Thinking row is the projection grouping the batch's
  existing Reasoning block with its Tool blocks. If the store's
  batch boundaries cannot express that grouping, STOP and report —
  that would be a data-model gap this plan did not foresee.
- The drill-down inherits ALL of this via the shared adapter (D7) —
  no pipeline special-casing anywhere; wrong drill-down data is a
  population bug, fix the population (F7).
- Open question 6.4 stays a recorded caveat — no fold-bug work on an
  unconfirmed report (design §7).
- Plan-time anchors to re-verify (they move): thread_store.cpp
  378-381/604-645/647-688, thread_store.h 77/85/100-108/186/193,
  typed_thread_store.h 73-87.

Plan pointers (task-specific only — standing rules live in the overview):
- Decision letters used: PL-8, PL-9, PL-12 (defined in the overview).
- Task 7 runs the cross-cutting battery over the finished shapes.
