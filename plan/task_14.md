## Task 14: C++ view - minimal draws the live content with no owner on screen

Priority: P0. Dependencies: Task 12.

### Description
The regression fix: in minimal, the newest live batch's content segment draws under the folded
group header even though no owner row is drawn, and it stops drawing when the turn closes.

### Changes Made
- `cpp_tui/views/thread.cpp` (no new branch expected): the segment path from task_12 plus the
  adapter's `segmentVisible` clause are the whole feature; this task is the proof and the
  regoldens.
- `cpp_tui/test_blocks.cpp:1024-1031` (edit): the two assertions that minimal *hides* the
  streamed prose flip to the new truth (a live, newest batch draws its content; an older or
  settled one does not).
- `cpp_tui/test/golden/streaming-live-100x30.txt` and the minimal `blocks-*` scenes that carry a
  live content row (regolden here, with the reason).

```cpp
// The case the design exists for (FR-5, FR-21, C7): the segment draws while its owner row does
// not. Both facts are the adapter's - the view only obeys `visible`:
//   ownerVisible == false (minimal-style fold)  &&  segmentVisible == true (newest, live batch)
// Pin it explicitly, not by reading a screen for a substring:
check(!thread.blockAt(group, ownerRow).visible, "no owner row is drawn in minimal's folded group");
check(thread.blockAt(group, segmentRow).visible, "but the live batch's content still draws");
```

### Verification
- [ ] `make test_blocks test_streaming > build.log 2>&1` clean.
- [ ] The explicit pair above: owner invisible, segment visible, in the same frame.
- [ ] The segment disappears when the turn closes (same fixture, `runStop()` -> invisible), and
      when a newer batch starts (the old segment folds with its batch).
- [ ] `streaming-live-100x30.txt` shows the text: the regression is pinned by a golden, not only
      by a check.
- [ ] An *opened* minimal group draws the segment inside the open owner (both visible), and not
      twice.
- [ ] `blocks-*` minimal scenes regoldened; `./test_blocks` green.
- [ ] Regression: the overview's regression command passes.

### Notes
- Sign-off R-9 lives here: in maximal the content folds with its batch, in minimal it stays until
  the turn closes. One predicate line changes it if the user prefers a single lifetime - do not
  decide it in this task.
- The old assertion at `test_blocks.cpp:1025` ("and the streamed prose - the live edge is the
  fold's") is the D7 regression in test form; rewrite its comment, do not delete the test.

Plan pointers:
- DD-10, FR-5, FR-21, I-11, C7, R-9; design §4.6's minimal art.
- Anchors were verified at plan time; re-verify before each edit.
