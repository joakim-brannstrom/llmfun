## Task 11: C++ view - the row loop obeys `visible`

Priority: P0. Dependencies: Task 10.

### Description
Delete the view's fold exception list: the loop draws a row iff `desc.visible`, indent follows
ownership rather than the mode, and `lineCountEstimate` counts the rows a folded turn draws.

### Changes Made
- `cpp_tui/views/thread.cpp:446-448` (edit): keep one local for the shape,
  `const bool styleMinimal = thread.groupStyleIsMinimal(group.id);`.
- `cpp_tui/views/thread.cpp:562-584` (`drawRow`, edit): the fold skip (`:581-584`) becomes
  `if (!desc.visible) return true;` - no `expanded` test, no kind list.
- `cpp_tui/views/thread.cpp:573-575` (edit): `const bool card = desc.batchOwned &&
  desc.kind == model::BlockKind::ToolStep;` (the indent is ownership, not the mode).
- `cpp_tui/views/thread.cpp:592-596` (edit): `const bool rowOpen = desc.expanded;` (the adapter
  already folds the answer's openness into the row).
- `cpp_tui/views/thread.cpp:491-516` (edit): the live-edge scan skips `!d.visible` rows - a live
  edge never names a row that does not draw.
- `cpp_tui/views/thread.cpp:665-681` (edit): drop the `return false` header case; the folded
  header still draws its live edge (`if (b == 0 && desc.kind == WorkGroup && !desc.expanded)`),
  and the children after it simply report `visible == false`.
- `cpp_tui/store/thread_store.cpp:345-347` (edit): the estimate.

```cpp
// The loop's whole visibility contract (design §7.3.1/§C7). Nothing else in the view may
// decide whether a row draws - the adapter already folded the owner's openness into it (I-5).
if (!desc.visible)
    return true;                 // paints nothing, the loop's caller keeps going
```

```cpp
// lineCountEstimate (FR-19): the rows a *folded* turn draws - the header, the plain rows and
// the owner rows. Mode- and pin-independent on purpose: it is computed in syncTyped(), while
// visibility is resolved per frame, and `test_streaming.cpp:655-661` pins that the hint does not
// move with the fold. The hint is replaced by the drawn height after one draw.
entry.view.lineCountEstimate = std::max(1, 1 + plainRows + ownerRows);
```

### Verification
- [ ] `make test_blocks test_streaming > build.log 2>&1` clean.
- [ ] A row with `visible == false` paints nothing, takes no focus, opens no menu, and the row
      after it still draws (assert with a synthetic descriptor list and with a real folded group).
- [ ] Thinking, content and cards of one batch share one indent and one rail (card rail still
      drawn from the card's top to its bottom).
- [ ] The prefetch path is intact: one `blockAt` per row per frame, no adapter probe inside the
      loop (`thread.blockCount(index)` once at `:463`).
- [ ] `measureHint` is unchanged after a fold toggle (`test_streaming.cpp:655-661` untouched and
      green).
- [ ] `blocks-maxlong` / `blocks-maxlonglive` shapes: only the `visible` rows draw - regolden the
      `blocks-*` set this task moves (the row loop is where the fold becomes a drawn fact).
- [ ] Regression: the overview's regression command passes.

### Notes
- The `groupMinimal` local at `:448` still feeds the live-edge and card decisions until tasks 12
  and 15 finish; keep it, but never let it decide visibility.
- `rowOpen` losing its minimal special case is safe because the answer/query rows are now
  `visible` and `expanded` in both modes (the adapter resolves `plainVisible`/`plainOpen`).
- Do not add an exception for the segment here: task_12 gives it its own draw path, and the
  adapter has already admitted it (I-11).

Plan pointers:
- DD-16, I-5, FR-18/FR-19, §7.3, C7.
- Anchors were verified at plan time; re-verify before each edit.
