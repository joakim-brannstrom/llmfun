## Task 7: C++ projection - buckets and the owner row

Priority: P0. Dependencies: Task 4, Task 5.

### Description
Bucket a turn's work rows by `batchId` (every batch except the one carrying the query row),
compute the batch facts once, and emit one synthetic `AGENT` owner row per batch at the batch's
first row. Children stay in insertion order in this task; task_08 orders and drops them.

### Changes Made
- `cpp_tui/store/thread_store.h:356-395` (edit, private): add `kNoBatch`, `BatchEntry`, and the
  two new `BlockEntry` fields; `cpp_tui/store/thread_store.h:397-409` (edit): `Entry` gains
  `bool turnLive` and `std::vector<BatchEntry> batches`.
- `cpp_tui/store/thread_store.h` (public, next to `blockCount`/`blockAt`): one read-only
  accessor for the tests, `std::size_t batchCount(size_t k) const;`.
- `cpp_tui/store/thread_store.cpp:204-225` (edit): set `entry.turnLive`, and build the header
  row as today.
- `cpp_tui/store/thread_store.cpp:251-344` (rewrite of the row loop): collect rows, bucket,
  emit owner + children, `chipsText`, the bucket-order assert.
- `cpp_tui/store/thread_store.cpp` (new, above `syncTyped`): `chipsText`'s body.
- `cpp_tui/store/thread_store.h` (public, next to `batchCount`): the `chipsText` declaration -
  the view composes its live chips line with the same function (task_15).

```cpp
inline constexpr std::size_t kNoBatch = static_cast<std::size_t>(-1);

/// One response's facts, computed once per rebuild (DD-5, DD-8, DD-12).
struct BatchEntry {
    std::string batchId;
    std::size_t firstRow = 0;    ///< index in the wire-order row list
    bool isNewest = false;       ///< I-12: the last response batch of its turn
    bool hasReasoning = false;
    bool hasContent = false;
    bool anyFailed = false;
    bool allSettled = true;      ///< every call reached Ok/Failed/Cancelled
    int failedCalls = 0;
    std::string chips;           ///< chipsText(call names, call order)
    double durationS = 0.0;      ///< the content block's duration, read BEFORE any drop
};

// BlockEntry additions (one line each, next to `typedBatchId`):
std::size_t batch = kNoBatch;  ///< index into Entry::batches, kNoBatch for a plain row
bool ownsBatch = false;        ///< the synthetic AGENT owner row
bool batchOwned = false;       ///< a child of an owner row

// Entry additions:
bool turnLive = false;                  ///< the turn's state == Running
std::vector<BatchEntry> batches;        ///< insertion order (I-7)
```

```cpp
/// The one chips composer (DD-18, FR-23): one token per distinct tool name, first occurrence
/// wins, `[a] [b] [c]` up to three, then `+N` for the tokens dropped from the list.
std::string chipsText(const std::vector<std::string>& toolNames) {
    std::vector<std::string> unique;
    for (const std::string& n : toolNames) {
        if (n.empty()) continue;
        if (std::find(unique.begin(), unique.end(), n) == unique.end()) unique.push_back(n);
    }
    std::string out;
    for (std::size_t i = 0; i < unique.size() && i < 3; ++i) {
        if (!out.empty()) out += ' ';
        out += '[' + unique[i] + ']';
    }
    if (unique.size() > 3) out += " +" + std::to_string(unique.size() - 3);
    return out;
}
```

```cpp
// The emission rule (design §4.3/§4.4 DD-4/DD-15), verbatim:
//  1. the query row's batch is the non-response batch: its rows stay plain (never owned);
//  2. every other batch is bucketed in insertion order; `firstRow` is the index of its first
//     row in the wire-order list; isNewest = the last bucket;
//  3. the block is emitted where its FIRST row was: owner row, then its children in
//     insertion order (task_08 canonicalises);
//  4. a row of a batch whose block was already emitted is consumed by it.
// Owner row fields (DD-13):
//   kind = BlockKind::Assistant, label = "AGENT", ownsBatch = true, expandable = true,
//   hasBody = false, copyable = hasContent, chips = batch.chips, failedCalls = batch.failedCalls,
//   timingS = batch.durationS, key = std::hash<std::string>{}(typedBlockId),
//   typedBlockId = "batch#" + batchId, batch = <its index>
// Debug assert (I-7): the bucket vector's order equals the first-row order.
```

### Verification
- [ ] `make test_typed_thread_store > build.log 2>&1` clean.
- [ ] Unit test: a turn with 3 response batches + query/notice rows yields 3 batches, 3 owner
      rows, and each owner's `key == hash("batch#" + batchId)` (I-1, I-6).
- [ ] Unit test: the query batch never grows an owner (`batchCount` counts only response
      batches); a turn whose query row is missing gives every batch an owner (DD-4).
- [ ] Unit test: a *malformed* batch id (`"#"`, `"abc"`, `"1#q#2"`) is bucketed exactly like any
      other id - nothing parses it (NFR-3).
- [ ] Unit test: `chipsText` - `{}` -> `""`, one -> `[bash]`, three -> `[a] [b] [c]`, four ->
      `[a] [b] [c] +1`, duplicates collapse (`[bash] [bash] [grep]` -> `[bash] [grep]`).
- [ ] Unit test: the owner carries the content block's `durationMs` (DD-8), including when the
      content row is body-less (read before any drop).
- [ ] Every `blocks-*` golden moves (owners appear in maximal scenes) - regoldened in this task
      with the reason "the owner row exists".
- [ ] Regression: the overview's regression command passes.

### Notes
- Do **not** order, drop or re-place yet: children keep insertion order and a batch's later rows
  still draw where they were (I-3 is task_08's). Keeping the steps apart is what makes the
  `maxnotice` ordering reviewable.
- `isNewest` is computed on the *response* buckets only (I-12); a turn with no response batch has
  none.
- `allSettled` is false while any call is `Pending`/`Running`/`None`; `Cancelled` counts as
  settled (the fold must settle on a stop).
- The `batchTools` loop (`:333-344`) is now redundant with `BatchEntry::hasContent` but stays
  until task_09 retires the field.

Plan pointers:
- DD-1, DD-4, DD-8, DD-12, DD-13, DD-15, DD-18, I-1, I-6, I-7, I-12; §4.3, §6.2.
- Anchors were verified at plan time; re-verify before each edit.
