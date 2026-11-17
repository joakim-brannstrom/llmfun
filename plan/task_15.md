## Task 15: C++ view - the live edge is minimal-only, newest-only

Priority: P0. Dependencies: Task 10.

### Description
The chips line and the `Thinking` shadow line exist because the minimal fold hides rows; draw them
only in a minimal-style group and only for the newest drawn group, and compose the chips with the
one helper.

### Changes Made
- `cpp_tui/views/thread.cpp:517-556` (`drawLiveEdge`, edit): chips from `store::chipsText(...)`
  over the *running* calls' names (`:528`), no second notation.
- `cpp_tui/views/thread.cpp:665-681` (edit): the under-the-header site fires only when
  `styleMinimal` (a folded maximal group never paints it).
- `cpp_tui/views/thread.cpp:707-717` (edit): the after-group site keeps its `index ==
  drawn.back()` guard and gains the `styleMinimal` guard.
- `cpp_tui/test_blocks.cpp:1008` family (edit): the shadow line's wording is unchanged, but its
  fixtures now draw through the new guards.

```cpp
// One chip per RUNNING call, composed by the projection's helper (DD-18, FR-23).
std::vector<std::string> running;
for (const model::BlockDesc& d : rows)
    if (d.visible && d.kind == model::BlockKind::ToolStep && d.state == model::BlockState::Running)
        running.push_back(d.summary);
const std::string chips = store::chipsText(running);
```

### Verification
- [ ] `make test_blocks > build.log 2>&1` clean.
- [ ] Maximal draws neither the chips line nor the shadow line, at 100 and 40 columns, live and
      settled (the D5 double-`Thinking` symptom is gone).
- [ ] Minimal draws exactly one live element for the newest group: chips while a call runs, else
      the shadow line while reasoning streams and no content has arrived, else nothing.
- [ ] An older turn with a `Running` block paints nothing (D11).
- [ ] The chips line and the owner header spell the same names the same way (`[bash] [grep]`),
      `+N` included.
- [ ] `blocks-livebatch`, `blocks-maxlive`, `blocks-maxcancelled`, `streaming-*` regoldened where
      this task moves them.
- [ ] Regression: the overview's regression command passes.

### Notes
- The shadow line keeps `Thinking - N tool calls` (`:546-549`); that count is the *shadow's* own
  (pending calls included) and is not the retired `BlockDesc::batchTools` - it reads the drawn
  rows' state.
- Both sites already carry an `index == drawn.back()` guard on one path and none on the other;
  the missing one is the under-the-header case.

Plan pointers:
- DD-11, FR-15, FR-23, D5, D11, C11; design §4.6's maximal-mid-turn art.
- Anchors were verified at plan time; re-verify before each edit.
