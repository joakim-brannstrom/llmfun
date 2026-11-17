## Task 4: C++ - displayability from the flag

Priority: P0. Dependencies: Task 3.

### Description
Read displayability off the wire flag in the C++ store and delete the compiler-side tool-name
list, so a hidden tool weighs 0 and draws nothing whatever its name.

### Changes Made
- `cpp_tui/store/typed_thread_store.cpp:11-18` (edit): `blockDisplayable` returns
  `(b.flags & TuiBlockFlag_Hidden) == 0` for a tool block, `true` for every other kind.
- `cpp_tui/store/typed_thread_store.h:46-57` (edit): drop `kHiddenToolNames` (`:57`), rewrite the
  comment to name the flag; keep the `blockDisplayable` declaration (`:56`).
- `cpp_tui/store/typed_thread_store.h:33-44` (`TypedBlock`): no change expected - `flags`
  (`:36`) already carries the bit; assert that in a test rather than adding a field.
- `cpp_tui/store/thread_store.cpp:253-254` (`syncTyped`, no change expected): the
  `!blockDisplayable(block)` skip stays the only filter.
- `cpp_tui/test_typed_thread_store.cpp` (edit): the hidden-tool fixtures set
  `TuiBlockFlag_Hidden` instead of relying on a name; the `pipeline_output` fixture at `:292`
  becomes a flag-only block (any title).

```cpp
bool blockDisplayable(const TypedBlock& b) {
    if (b.kind != kBlockKindTool)
        return true;
    return (b.flags & TuiBlockFlag_Hidden) == 0;
}
```

### Verification
- [ ] `cd llmfun/build/tui && make test_typed_thread_store > build.log 2>&1` clean.
- [ ] Unit test: a tool block with `flags |= TuiBlockFlag_Hidden` is not displayable, weighs 0 in
      `stepCount` (`:167-201`) and produces no row; the same block without the bit is a row.
- [ ] Unit test: a *visible* block titled `pipeline_output` is a row (the name no longer
      decides) - the inverse of today's list.
- [ ] `grep -rn "kHiddenToolNames\|pipeline_output\|task_done" cpp_tui/store cpp_tui/views
      cpp_tui/model` is empty.
- [ ] `./test_blocks` and `./test_streaming` unchanged (their scenes carry no hidden block).
- [ ] Regression: the overview's regression command passes.

### Notes
- The wire's `TuiBlockFlag_Hidden` is a C enum in `tui_api.h`; `TypedBlock::flags` is an `int`
  copied verbatim by `tuiBlockUpsert` (`tui_api.cpp:891-913`) - no plumbing needed.
- The old list was only `{"pipeline_output"}` (`typed_thread_store.h:57`); its test list moves
  with this task, not later.

Plan pointers:
- DD-3, I-9, FR-10. Anchors re-verified per standing rules.
