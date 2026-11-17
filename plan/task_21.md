## Task 21: Documentation

Priority: P1. Dependencies: Task 19.

### Description
Write the modes chapter the code now implements: the glossary, the fold, the one visibility rule,
the live edge, pins and anchors - and replace stale `file:line` anchors with symbol names.

### Changes Made
- `llmfun/doc/tui_design.md` (edit): new chapter "Thread modes: the response-owned fold"
  (§§ below), and the thread/block sections updated where they describe the old fold.
- `llmfun/doc/todo.md` (edit): the remainder (R-9's single lifetime, the grid-copy decision of
  task_26, any check left unwritten).
- `llmfun/doc/ui_keys.md` (edit): the "Collapse all" palette row from task_24, if task_24 lands
  first.

```
## Thread modes: the response-owned fold
1. Vocabulary: turn, response, batch, owner row, child, body segment, settled, live tail, pin,
   live edge. Two profiles (grid `" - "`, pixel `" \xC2\xB7 "`).
2. The fold: groupOpen = groupPins ?? (maximal && turnLive); styleIsMinimal = minimal &&
   !groupOpen; ownerOpen = rowPins ?? batchLive; a settled batch folds to
   "> <duration> - [chips]".
3. Visibility (the one rule): ownerVisible = groupOpen; childVisible = ownerVisible &&
   ownerOpen; the content segment = that OR (minimal-style && live && newest); a plain row
   follows its group. The view draws iff `visible` and keeps no exception list.
4. The live edge: chips while a call runs / the Thinking shadow while reasoning streams, in
   minimal style and for the newest group only.
5. Pins are absolute and session state; pruned with their turn and by "Collapse all".
6. Anchors and hints: the group header is the anchor; the height hint is an estimate and does
   not move with the fold.
```

### Verification
- [ ] `dub test` and the C++ suite unchanged (docs-only task, but the suite must be green).
- [ ] The chapter's rule text is *quoted from* `store/thread_store.cpp`'s predicate (same names:
      `groupOpen`, `groupStyleIsMinimal`, `ownerVisible`, `ownerOpen`, `segmentVisible`).
- [ ] Every `file:line` reference in the new chapter is a symbol instead; a grep for `\.cpp:[0-9]`
      in the new chapter is empty.
- [ ] `doc/todo.md` names R-9, task_26's decision and the "40-column pass" note (overview open
      issue 3).
- [ ] The chips art in the doc uses the shipped notation (`[bash] [grep]`, `+N`), not the
      investigation's `[bash, grep]`.
- [ ] Regression: the overview's regression command passes.

### Notes
- Do not restate the design document: the *code* is the authority now, and the chapter should be
  readable by someone who has never seen `plan/`.
- The investigation (`plan/maximal_investigation.md`) stays as the record of the old behaviour; the
  new chapter says so in one line.

Plan pointers:
- C14, §10.1/§10.2; overview open issues 1 and 3.
- Anchors were verified at plan time; re-verify before each edit.
