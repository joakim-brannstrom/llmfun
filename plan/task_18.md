## Task 18: C++ projection - the answer batch suppresses its content

Priority: P1. Dependencies: Task 8, Task 6.

### Description
The answer text appears once: the batch the `Answer` block names contributes no content child, so a
finished maximal turn draws `ANSWER` + its text and nothing above it duplicates it.

### Changes Made
- `cpp_tui/store/thread_store.cpp` (task_07/08's drop pass, edit): one more drop, keyed on the
  answer row's batch.
- `cpp_tui/test_typed_thread_store.cpp` (new unit test): the drop and the "no answer, no drop"
  case.
- `cpp_tui/test/golden/blocks-maxlong-100x30.txt`, `blocks-answerlanded-100x30.txt` (regolden).

```cpp
// DD-9, I-10: D names the batch on the Answer block itself (`finalAnswerBatchId` becomes the
// block's batchId - live package.d:1149, replay package.d:163-164), so the projection reads it
// and needs no heuristic:
const std::string answerBatch = entry.hasAnswer ? entry.answerRow.typedBatchId : std::string();
// in the drop pass: a CONTENT child of `answerBatch` is dropped.
```

### Verification
- [ ] `make test_typed_thread_store test_blocks > build.log 2>&1` clean.
- [ ] Unit test: a turn whose answer names batch `1#2` draws no content child for `1#2`; batch
      `1#1`'s content still draws; the answer text appears exactly once.
- [ ] Unit test: a turn with no `Answer` row drops nothing (every batch keeps its content).
- [ ] `blocks-answerlanded` and `blocks-maxlong` regoldened: one copy of the answer text.
- [ ] R-3's trade-off recorded in the task report (a differing preamble is lost).
- [ ] Regression: the overview's regression command passes.

### Notes
- The drop must not change `WORK - N steps`: `stepCount` reads the wire store (task_08's tests
  prove the independence) - re-run them here.
- A batch whose *only* child was this content row now has no child: DD-15 then also removes its
  owner, and the owning step count stays what the wire says (an accepted hole, pinned by
  `blocks-maxlong` if it shows).

Plan pointers:
- DD-9, I-10, DD-15, FR-12, R-3; design §4.4, §0.3.
- Anchors were verified at plan time; re-verify before each edit.
