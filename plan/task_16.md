## Task 16: C++ view - anchors survive a batch folding in place

Priority: P0. Dependencies: Task 11, Task 14.

### Description
A batch folding mid-stream (or the live batch settling) must not move the group's anchor row: the
sampled header position stays where the user is reading, and the height hint still does not move
with the fold.

### Changes Made
- `cpp_tui/views/thread.cpp:686-699` (edit): sample `headerRows` from the header row itself, not
  from "the cursor after the query row", so an owner/segment appearing above cannot shift it.
- `cpp_tui/views/thread.cpp:712-718` (edit): the `endGroup` hint keeps using the drawn height.
- `cpp_tui/test_thread_anchor.cpp` (new test): the mid-stream fold case; the settled-batch case.

```cpp
// The anchor contract (task 25's mechanism, unchanged): the group's header row is the anchor.
// A fold changes how many rows follow the header, never where the header starts.
const float headerScreenY = ImGui::GetCursorScreenPos().y;   // BEFORE the header row is drawn
headerRows.emplace_back(group.id, headerScreenY - ImGui::GetWindowPos().y);
```

### Verification
- [ ] `make test_thread_anchor test_streaming > build.log 2>&1` clean.
- [ ] `test_streaming.cpp:655-661` untouched and green (the hint is the estimate).
- [ ] New test: seed a live run, record the header's screen y, settle the batch (content gains a
      duration, the owner folds), redraw - the header y is unchanged and the scroll offset holds.
- [ ] New test: fold a batch by hand mid-stream (`setBlockExpanded` on the owner) - the header y
      is unchanged.
- [ ] The `Load earlier` paging and the first-group latch still work (`test_thread_window`).
- [ ] Regression: the overview's regression command passes.

### Notes
- The design's T16 names `test_streaming.cpp`'s hint test as the one that must stay untouched;
  the *new* assertions belong in `test_thread_anchor.cpp`, which owns the anchor surface, so the
  hint test's file stays byte-identical.
- A segment whose body grows while the turn streams is the hard case: the header row above it is
  already drawn, so only the rows *below* move - assert that, not the total height.

Plan pointers:
- FR-14-adjacent (anchor stability), task 25's contract; design §10.1.2 and constraint 5.
- Anchors were verified at plan time; re-verify before each edit.
