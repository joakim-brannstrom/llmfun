# Implementation plan - response-owned batch fold (llmfun)

Output: `plan/implementation_plan.md` + `task_01.md`..`task_26.md` + `task_06a.md` (27 task files),
`plan/anchors.md` (verified anchor table), `plan/requirements_notes.md`, `plan/codebase_notes.md`.
Design source: `plan/system_design.md` rev 2 (26 tasks T1..T26); the plan maps them 1:1 plus one
split (`task_06a` = the target-shape checks, split because task_06 exceeded the 8 KB target).

## Plan-level findings not in the design (all verified against the tree at a1e7743)

1. **Fixture batch ids are the hidden cost.** `test_blocks.cpp:369-415` `ThreadFixture::add()`
   invents `#c` / `#t` / `#a` / `#n` per message, and `test_typed_thread_store.cpp:63-70`'s
   `block()` defaults `batchId = ""`. Under DD-4 ("a response batch is any batch that is not the
   query row's batch") every one of those becomes an owner row, so every `f.add(...)` scene would
   silently grow owners. task_05 re-keys the fixture first (one response batch per assistant/tool
   group, notices/vision on `#q`, the answer on its response's batch).
2. **`plainVisible = groupOpen` must not cover the turn-level rows.** The query and the ANSWER are
   `kQueryRow`/`kAnswerRow`, not group-level plain rows; applying the fold to them hides every
   answer in minimal (the factory default). task_10 spells `queryVisible/answerVisible = true`.
3. **I-5 has an exception**: it holds for thinking/cards, not for the content segment (the glossary
   calls the segment a child, but FR-5/B1 show it with no owner row drawn).
4. **`lineCountEstimate` must stay mode-independent** (`1 + plainRows + ownerRows`): it is computed
   in `syncTyped` while visibility is per frame, and `test_streaming.cpp:655-661` pins that the hint
   does not move with the fold.
5. **A 40-column golden is impossible** in `test_blocks` (fixed 100x30 harness, `goldenText` writes
   `screen: 100x30`): the 40-column pass is a pure-function assertion on
   `blockHeaderText(..., 40.0f)`.
6. **Hand-authored target goldens are not feasible before the layout exists**, so B5's "failing
   goldens first" became "green captures + red `checkOwnerShape()` checks" (task_06/06a).
7. **The `Answer` block already carries its batch id** (`finalAnswerBatchId` -> `batchId`), so DD-9
   needs no new wire field: read `entry.answerRow.typedBatchId`.
8. **The live stream updater has no `LlmConfig`**, so the hidden-flag helper needs a seam:
   `ToolFlags` in `TypedEmitContext`, set from `currentToolFlags()` at the three ctx creations.
9. **`Done` is already upserted by the step walk**, so `emitRunEndedTyped` needs a
   `ctx.closedByAnswer` bit or it would clobber `Done` with `Interrupted`.
10. **§10.4's blast radius is incomplete**: add `test_streaming.cpp:381-385` + `:709-721`
    (`blockHeaderText` signature), `:673-688` (literal row indices), `test_blocks.cpp:1024-1031`
    (minimal prose), `:118-416` (fixture batches), `test_typed_thread_store.cpp:63-70` (batch ids),
    `test_thread_window.cpp:531-541` (the cost fixture seeds no work rows), and the run-level
    `networkFailure` assertion at `tests.d:1987-1993`.
