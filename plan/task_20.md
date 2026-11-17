## Task 20: The second mount stays identical

Priority: P1. Dependencies: Task 19.

### Description
The agents drill-down binds its own `ThreadStoreAdapter`, so it inherits the fold; prove the two
mounts draw the same shape for the same turn, and regolden the drill-down.

### Changes Made
- `cpp_tui/test_inspector.cpp` (or the existing drill-down test file, edit): one shape-equality
  test.
- `cpp_tui/test/golden/inspector-agents-drilldown-100x30.txt` (regolden).

```cpp
// The same turn, drawn twice: the main thread and the drill-down. Compare the *rows*, not
// pixels: kind, label, visible and expanded per row - the shape the fold produces.
std::vector<Row> shapeOf(store::ThreadStoreAdapter& a, std::size_t group) {
    std::vector<Row> out;
    for (std::size_t b = 0; b < a.blockCount(group); ++b) {
        const model::BlockDesc d = a.blockAt(group, b);
        out.push_back({d.kind, d.label, d.visible, d.expanded, d.ownsBatch, d.bodySegment});
    }
    return out;
}
```

### Verification
- [ ] `make test_inspector > build.log 2>&1` clean.
- [ ] The shape-equality test: the same seeded turn through both adapters yields identical row
      vectors (kind, label, visible, expanded, ownsBatch, bodySegment).
- [ ] A pin set on the main mount does *not* leak into the drill-down (they hold separate
      `ChatTab`s) - one assertion.
- [ ] `inspector-agents-drilldown` regoldened with the reason (the owner rows appear).
- [ ] `./test_blocks`, `./test_streaming` unaffected.
- [ ] Regression: the overview's regression command passes.

### Notes
- If the drill-down's own `ThreadState.agentThread` carries a stale `thinkingOpen`, the deletion in
  task_10 already removed the field; nothing to migrate.
- R-7's mitigation is exactly this test: a divergence must fail here, not be found by eye.

Plan pointers:
- FR-17, C13, R-7; design §5 C13 (`views/shell.cpp:1198-1221`).
- Anchors were verified at plan time; re-verify before each edit.
