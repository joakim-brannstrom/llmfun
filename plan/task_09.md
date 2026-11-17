## Task 9: C++ model - row vocabulary, disclosure, `Assistant` audit

Priority: P0. Dependencies: Task 7.

### Description
Give `BlockDesc` the fields an owner/child/segment needs, make `expandable` a real per-row
decision so the disclosure exists for owners and not for segments, make `blockHasDisclosure`
descriptor-based, and audit every `BlockKind::Assistant` consumer.

### Changes Made
- `cpp_tui/model/block.h:111-145` (edit): seven new fields, `batchTools` retired.
- `cpp_tui/store/thread_store.cpp:573-630` (`blockAt`, edit): fill the new fields; owner
  `label`/`summary`/`copyable`; the segment's shape.
- `cpp_tui/store/thread_store.cpp:256-258` + `:664-705` (edit): `expandable` per row, and the
  `blockText` answer for an owner (`:707-712`).
- `cpp_tui/views/blocks.h:131` + `cpp_tui/views/blocks.cpp:161-166,308` (edit): descriptor-based
  `blockHasDisclosure`.
- `cpp_tui/views/palette.cpp:167-172` + `:185-218` (edit): the `Assistant` audit.
- `cpp_tui/test_blocks.cpp:495-524` (edit): the vocabulary table calls the new predicate.
- `cpp_tui/test_typed_thread_store.cpp:304,327,331` (edit): `blockCount` numbers per §7.2.

```cpp
// model/block.h - the additions (one line each; `batchTools` is removed).
bool visible = true;        ///< resolved by the adapter; the view draws iff true (DD-16)
bool bodySegment = false;   ///< the batch's content: no header, no hit box (DD-7)
bool ownsBatch = false;     ///< the synthetic AGENT owner row (DD-13)
bool batchOwned = false;    ///< a child of an owner: shares its indent and rail
bool copyable = false;      ///< the row's text is reachable through blockText()
std::string chips;          ///< "[bash] [grep] +2", empty when the batch has no calls
int failedCalls = 0;        ///< > 0 paints FAIL on the owner (FR-9)
```

```cpp
// views/blocks.h / .cpp - the disclosure is a property of the row, not of its kind:
// a batch owner is BlockKind::Assistant and still folds (D15, C9a).
bool blockHasDisclosure(const BlockView& block);
//   return block.kind == BlockKind::WorkGroup || block.kind == BlockKind::Thinking ||
//          block.kind == BlockKind::ToolStep || block.ownsBatch;
```

```cpp
// store/thread_store.cpp - expandable per row (DD-13): only a row that HAS a disclosure.
row.expandable = row.kind == model::BlockKind::WorkGroup ||
                 row.kind == model::BlockKind::Thinking ||
                 row.kind == model::BlockKind::ToolStep;
// The owner row is constructed with expandable = true (its own initialiser, next to ownsBatch).
// A body segment and every plain row keep false, so setBlockExpanded's guard (:666) answers
// false instead of pinning a row nothing can open (I-11).
```

```cpp
// views/palette.cpp - the answer-role test must not treat an owner as the answer text.
bool blockIsAnswerRole(const model::BlockDesc& block) {
    if (block.ownsBatch) return false;                  // NEW: an owner is not an answer
    if (block.kind == model::BlockKind::Answer || block.kind == model::BlockKind::Assistant)
        return true;
    ...
}
// lastAnswerBlock: the copy target for a batch is its owner, when the batch HAS text.
//   accept a row when  (desc.hasBody || desc.streaming)  as today,  OR  (desc.ownsBatch &&
//   desc.copyable)                                                        <- FR-22
```

### Verification
- [ ] `make test_typed_thread_store test_blocks > build.log 2>&1` clean.
- [ ] Vocabulary test: `User/Answer/WorkGroup/Assistant/ToolStep/Thinking/System/Code` labels and
      inks unchanged; `blockHasDisclosure` true for exactly WorkGroup, Thinking, ToolStep and an
      `ownsBatch` descriptor (a plain `Assistant` stays false).
- [ ] Owner fields: `ownsBatch`, `hasBody == false`, `expandable == true`, `summary` empty,
      `chips` from task_07's helper, `copyable == (batch has a non-empty content row)`,
      `blockText(k, owner) == that content` and `""` for a calls-only batch.
- [ ] Segment fields: `bodySegment == true`, `batchOwned == true`, `expandable == false`;
      `setBlockExpanded(k, segment, true)` answers false and stores no pin.
- [ ] Palette: `blockIsAnswerRole(owner)` false; `lastAnswerBlock` skips a calls-only owner and
      returns the last *answer*; with no `Answer` row, a text-carrying owner is the target.
- [ ] `streamHasText`/`streamHasThinking` (`store/thread_store.cpp:408-430`) still answer false
      when the only `Assistant` row is an owner with no body - two regression tests.
- [ ] `blockCount` fixtures match the restated contract (§7.2: header + plain rows + owners +
      children + segments).
- [ ] Regression: the overview's regression command passes.

### Notes
- `visible` is added here with the default `true`; **nothing computes it yet** - task_10 owns the
  predicate, and every golden still draws as today at the end of this task.
- The `Assistant` consumer list to audit (verified): `store/thread_store.cpp:273,416,602`,
  `views/blocks.cpp:113,145`, `views/palette.cpp:168,171`, `test_streaming.cpp:363,599`,
  `test_typed_thread_store.cpp:400`, `test_blocks.cpp:506,621,631,1073,1293,1326`.
- Retiring `batchTools` also retires the projection loop at `store/thread_store.cpp:333-344` and
  `views/blocks.cpp:193-197` (the Thinking header's count). The minimal shadow line keeps its
  `Thinking - N tool calls` wording (`views/thread.cpp:546-549`, pinned by
  `test_blocks.cpp:1008`) - it reads a live count, not the descriptor field.

Plan pointers:
- DD-7, DD-13, DD-16, I-5, I-11, C9a/C9b, FR-16/FR-22.
- Anchors were verified at plan time; re-verify before each edit.
