## Task 8: C++ projection - order, drops, placement

Priority: P0. Dependencies: Task 7.

### Description
Make one batch's block read chronologically and drop what must not draw: children ordered
reason -> content -> cards and contiguous, body-less rows gone, an owner with no surviving child
absent, and an interleaved plain row emitted after the block. Pin `stepCount`'s independence
with two config tests.

### Changes Made
- `cpp_tui/store/thread_store.cpp:251-344` (edit on task_07's rewrite): three ordered passes -
  bucket, then emit with the canonical child order, then the drops.
- `cpp_tui/test_typed_thread_store.cpp` (new unittests): the drop and order cases + the two
  `stepCount` config cases.

```cpp
// The child order inside one batch block (DD-5, I-4): reasoning, then content, then the call
// cards in call order - whatever order the wire sent. Live sends content first, replay sends
// reasoning first; the projection is the one that decides.
enum class ChildRank { Reasoning = 0, Content = 1, Call = 2 };
// rank(BlockEntry): Thinking -> Reasoning, Assistant/Code prose with a body -> Content,
// ToolStep -> Call. Anything else in a response batch keeps insertion order after the calls.

// The drops (design §C5.5, DD-15, I-2):
//   a) a content/reasoning row whose body is empty  -> dropped (D6: no bare AGENT row);
//   b) an owner row whose every child was dropped   -> not emitted at all.
// NOT in this task: the answer batch's content child (task_18, DD-9).
```

```cpp
// stepCount is *not* touched by these drops: it walks the wire store
// (store/typed_thread_store.cpp:167-201), one layer below the projection, and its only
// displayability input is blockDisplayable() - i.e. the hidden flag (DD-12, design §0.3).
// Two tests pin exactly that:
//   default config  ("taskDone" + "pipelineOutput" hidden): a finished task_done response
//                   weighs 2 LESS than with the card visible (query 0 + content 1 + the
//                   settled call 2 -> hidden: content 1);
//   hiddenTools = [] : the same turn's count is unchanged versus today's expectation.
```

### Verification
- [ ] `make test_typed_thread_store > build.log 2>&1` clean.
- [ ] Order: a batch whose wire order is content-then-call draws reasoning, content, then the
      card (assert the emitted row kinds in sequence).
- [ ] Contiguity (I-3): a batch's children are adjacent rows; no plain row sits between them.
- [ ] Placement (H8): with a notice between two rows of one batch, the notice row's index is
      after the batch block's last child (`test_blocks` `maxnotice`'s two checks go green).
- [ ] Empty rows (I-2, DD-15): a batch with only an empty content row emits no owner and no row.
- [ ] `stepCount` tests: default config -> -2 per hidden settled call; `hiddenTools = []` ->
      unchanged. Both assert `stepCount(store, turnId)` directly, not a rendered header.
- [ ] `WORK - N steps` in `blocks-maxlong` is 30 (regolden if the header text moved).
- [ ] Regression: the overview's regression command passes.

### Notes
- The placement rule is not extra code if the emission walks the wire-order row list: emitting a
  batch's block at its first row and consuming its later rows *is* the rule. Do not sort the
  output by batch.
- `allSettled`/`durationS` are computed in the bucket pass, before any drop (DD-8): dropping a
  body-less content row must not lose the response's duration.
- `stepCount`'s -2 is the *sign-off* item R-4; do not "fix" it by subtracting a prose row.

Plan pointers:
- DD-5, DD-8, DD-9 (not here), DD-15, DD-12, I-2, I-3, I-4; §4.3, §5.C5.5, §0.3.
- Anchors were verified at plan time; re-verify before each edit.
