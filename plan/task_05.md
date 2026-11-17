## Task 5: Test data - batch ids that match the wire

Priority: P0. Dependencies: Task 3.

### Description
Make the C++ test data speak the wire's batch shape: one batch per response (its content,
reasoning and all its calls), the query batch for the query/vision/notice rows, and the answer
on its response's batch. Add the seeding shapes the new scenes need.

### Changes Made
- `cpp_tui/test/typed_seed.h:139-163` (edit): `tool()` no longer bumps `liveBatch` (`:157`);
  the card joins the live batch.
- `cpp_tui/test/typed_seed.h` (new members after `tool()`): `nextBatch()`, `content(text)`,
  `notice(text)`, and a `tool` overload that takes an explicit batch.
- `cpp_tui/test_blocks.cpp:369-416` (`ThreadFixture::add`, edit): response batches replace
  `#c`/`#t`/`#a`/`#n` (see the table below).
- `cpp_tui/test_blocks.cpp:1906-2054` (`scenes()`, edit): re-key `livebatch`, `maxlive`,
  `maxcancelled`, `maxanatomy` to the live-batch shape the new `tool()` gives them.
- `cpp_tui/test_typed_thread_store.cpp:63-70` (edit): the `block(...)` helper keeps its
  signature; its *callers* at `:289-292` pass real ids (`42#q` for the query block, `42#0` for
  the work rows) instead of defaulting to `""`.
- `cpp_tui/test/golden/blocks-workgroup-100x30.txt`, `blocks-maxanatomy-100x30.txt`,
  `blocks-maxfailed-100x30.txt` (regolden in this task - see Notes).

```cpp
// typed_seed.h - the wire's batch lifecycle, stated once.
void nextBatch() { ++liveBatch; }         // the next response: new batch, same turn

void content(const std::string& text) {   // a response with content and no reasoning
    if (liveTurnId == 0) return;
    upsertBlock(liveTurnId, "content#" + std::to_string(liveBatch), batchId(), TuiBlockKind_Prose,
                TuiBlockStatus_None, "assistant", text);
}

void notice(const std::string& text) {    // rides the query batch: a plain row, never owned
    upsertBlock(liveTurnId, "notice#" + std::to_string(++noticeSeq), queryBatchId(),
                TuiBlockKind_Notice, TuiBlockStatus_None, nullptr, text);
}
// the two batch ids the seeder composes (the wire's shape, nothing parsed):
std::string batchId() const {
    return std::to_string(liveTurnId) + "#" + std::to_string(liveBatch);
}
std::string queryBatchId() const { return std::to_string(liveTurnId) + "#q"; }
```

| `MsgKind` | batchId before | after | why |
|---|---|---|---|
| `User` (query) | `<turn>#q` | `<turn>#q` | unchanged (DD-4's non-response batch) |
| `Assistant` (content) | `<turn>#c` | `<turn>#0` (response batch) | one response = one batch |
| `ToolCall` / `ToolResponse` | `<turn>#t` | the current response batch | a call belongs to its response |
| `FinalAnswer` | `<turn>#a` | the current response batch | the `Answer` names its batch (DD-9) |
| `System` (notice) | `<turn>#n` | `<turn>#q` | notices ride the query batch (§4.3) |

### Verification
- [ ] `cd llmfun/build/tui && make test_blocks test_typed_thread_store > build.log 2>&1` clean.
- [ ] `./test_blocks` green; the three regoldened scenes show the change and nothing else
      (`git diff --stat` lists exactly those three golden files + the sources).
- [ ] `./test_typed_thread_store` green with the new batch ids (`blockCount` still 2 at `:304`,
      2 at `:327`, 3 at `:331` - the projection does not exist yet).
- [ ] A seeder test pins: `runStart` -> `tool` -> `tool` = ONE batch with two cards; `nextBatch`
      -> `content` = a second batch.
- [ ] Regression: the overview's regression command passes.

### Notes
- **Why the fixture re-key is mandatory (found at plan time).** `ThreadFixture::add` invents a
  separate batch per message (`#c`, `#t`, `#a`, `#n`). Under DD-4 every one of those is a
  *response* batch, so task_07's projection would give each its own `AGENT` owner row - every
  `f.add(...)` scene in `test_blocks` would grow owners nobody meant. Re-keying here keeps the
  post-T7 shape reviewable.
- The three regoldens are **not** a projection change: with a call now sharing its batch, the
  Thinking row's `batchTools` (`store/thread_store.cpp:333-344`) becomes non-zero and
  `blockHeaderText` prints `Thinking - N tool calls` (`views/blocks.cpp:193-197`). That is the
  truthful count; regolden and state the reason in the commit.
- `test_typed_thread_store.cpp:122-128` already uses real ids (`11#0`, `11#1`) - leave it.
- Four other binaries seed live streams (`test_keymap.cpp:527`, `test_nowbar.cpp:1134`,
  `test_palette.cpp:542,1406`, `test_thread_anchor.cpp:543,588`): `updateStream` only, so they
  are unaffected by `tool()`'s change - run them in the regression.

Plan pointers:
- DD-4 (no id parsing), DD-5, §4.3 placement rule, D10; open issue 4 in the overview.
- Anchors were verified at plan time; re-verify before each edit.
