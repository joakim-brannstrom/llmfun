## Task 23: Measured cost

Priority: P2. Dependencies: Task 11.

### Description
Put a number on NFR-9: the existing scaled-thread cost test must measure the new projection and the
new `blockExpanded`, on a thread that actually has work rows, and keep its budgets.

### Changes Made
- `cpp_tui/test_thread_window.cpp:531-541` (fixture, edit): each turn gains a response batch - a
  content row and two tool cards - so bucketing and the fold read have something to do.
- `cpp_tui/test_thread_window.cpp:552-559` (the frame loop, edit): add
  `thread.blockExpanded(k, 1)` and one `thread.blockCount(k)` per group.
- `cpp_tui/test_thread_window.cpp:597-616` (budgets, edit only if the numbers justify it, with the
  measured value in the reason).

```cpp
// The frame the cost test samples must be the frame the render loop runs: groupAt +
// measureHint + expanded today; add the two reads the new projection introduced -
// blockCount() (one per group) and blockExpanded() (one per drawn row, i.e. per owner).
for (std::size_t k = first; k < groups; ++k) {
    const tui::store::GroupView view = thread.groupAt(k);
    sink = sink + thread.measureHint(view.id);
    sink = sink + (thread.expanded(view.id) ? 1.0 : 0.0);
    const std::size_t blocks = thread.blockCount(k);
    sink = sink + static_cast<double>(blocks);
    if (blocks > 1) sink = sink + (thread.blockExpanded(k, 1) ? 1.0 : 0.0);
}
```

### Verification
- [ ] `make test_thread_window > build.log 2>&1` clean.
- [ ] The printed `cost (...)` line shows `build(1000) <= 150 ms`, `frame(1000) <= 2 ms` and
      `ratio <= 2.0` with the work rows in place.
- [ ] If a budget fails, the fix is measured: move `rowPins`/`groupPins` to
      `std::unordered_map` and re-run; report both numbers.
- [ ] `entryWalks`'s expectation (`:615`) still holds (80 per frame) or is restated with the new
      read count and a reason.
- [ ] `./test_streaming` and `./test_blocks` unaffected.
- [ ] Regression: the overview's regression command passes.

### Notes
- The fixture's current turns carry only a query and an answer (`Msg::User` +
  `Msg::FinalAnswer`), so the test as it stands would measure the *old* projection - that is why
  the fixture changes first.
- Take the minimum of five runs, as the test already does; a single sample of a microsecond loop is
  scheduler noise.
- NFR-5's O(1)/linear claims are what this measures; do not turn the test into a benchmark suite.

Plan pointers:
- NFR-5, NFR-9, M2; design §8 "Performance".
- Anchors were verified at plan time; re-verify before each edit.
