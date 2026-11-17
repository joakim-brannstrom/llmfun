## Task 22: Retired names, lying comments, the grep gate

Priority: P1. Dependencies: Task 10, Task 13, Task 17.

### Description
Sweep the corpses and make the sweep permanent: no retired identifier and no tool-name literal in
the C++ production tree, the comments that credit the store with behaviour it no longer has
rewritten, and a source-scanning test that fails if any of them returns.

### Changes Made
- `cpp_tui/store/thread_store.cpp:648-661` (edit): the comment that says "a maximal batch draws
  open, always" and "maximal draws every content row open" now states the derived fold.
- `cpp_tui/store/typed_thread_store.h:46-57` (edit): the displayability comment states the flag
  (task_04's edit finishes the code; this finishes the prose).
- `cpp_tui/test_blocks.cpp:1604-1621` (edit): the empty-stream comment ("minimal folds the work
  rows") rewritten to the body-less drop's reason.
- `cpp_tui/test_typed_thread_store.cpp` (edit): the `blockCount` messages match §7.2's contract.
- `cpp_tui/test_streaming.cpp:399-430` (`checkSource`, edit): the gate (below).

```cpp
// The gate. Production sources only: a fixture may name a hidden tool (that is what a fixture
// is for), a shipped source may not. A new file needs a new row here - the list is the closed
// set of files that could reintroduce one of these.
const char* kProduction[] = {"store/thread_store.cpp", "store/thread_store.h",
                             "store/typed_thread_store.cpp", "store/typed_thread_store.h",
                             "views/thread.cpp", "views/blocks.cpp", "views/palette.cpp",
                             "model/block.h", "tui.h", "tui_api.cpp"};
const char* kRetired[] = {"thinkingOpen", "batchTools", "groupOverrides", "openWorkGroups",
                          "typedRowOpen_", "kHiddenToolNames", "task_done", "taskDone",
                          "pipeline_output", "groupMinimal"};
// for each file, for each token: the source must not contain it.
```

### Verification
- [ ] `make test_streaming test_blocks > build.log 2>&1` clean.
- [ ] `grep -rn "thinkingOpen\|batchTools\|groupOverrides\|openWorkGroups\|typedRowOpen_\|kHiddenToolNames\|groupMinimal" llmfun/cpp_tui/{store,views,model}` is empty.
- [ ] `grep -rn "task_done\|taskDone\|pipeline_output" llmfun/cpp_tui/{store,views,model}` is
      empty (fixtures may keep them; production may not).
- [ ] The gate fails if a token is reintroduced - prove it once by adding and removing one.
- [ ] The three rewritten comments no longer claim a fold the code does not have.
- [ ] Regression: the overview's regression command passes.

### Notes
- `groupMinimal` is on the list because the split into `groupOpen`/`groupStyleIsMinimal` is the
  point of DD-14: a returning `groupMinimal` would silently re-conflate style with openness.
- Do not scan the whole tree recursively: `test/*.h` and `test_*.cpp` must be free to name the
  hidden tools, and a recursive scan would either fail on them or need an ignore list that rots.
- Comments only where they explain *why*: do not restate the predicate in three files.

Plan pointers:
- NFR-8, D14, §8 "Observability"; design §10.4's "lying comments".
- Anchors were verified at plan time; re-verify before each edit.
