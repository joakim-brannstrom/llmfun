## Task 26: Grid copy of a body segment - decision, not code

Priority: P3. Dependencies: Task 21.

### Description
The grid has no selection today (the terminal owns `Ctrl+C`), so a minimal-mode content segment
cannot be copied with the mouse there. Record the user's answer; write no code unless they ask for
it.

### Changes Made
- `llmfun/doc/tui_design.md` (edit): one paragraph under the modes chapter, naming the decision
  and its reason (the pixel profile already works through rich_md's selection).
- `llmfun/doc/todo.md` (edit): the open item, if the answer is "not now".

```
// The recorded decision (design §11 T26, FR-16):
//   pixel profile: mark-and-copy inside a body segment works - it is rich_md's own selection.
//   grid profile:  no selection exists and none is added; the copy path is the block menu / `y`
//                  on the owner, which copies the whole batch content (blockText(owner)).
//   verdict: <the user's answer>, and if "not now", doc/todo.md carries the follow-up.
```

### Verification
- [ ] No code changes (`git status` shows docs only).
- [ ] The paragraph names the pixel path, the grid path, and the owner's copy target.
- [ ] `doc/todo.md` carries the item if the answer deferred it.
- [ ] Regression: the overview's regression command passes.

### Notes
- Do not bind `Ctrl+C` for a grid copy: the terminal consumes it as `SIGINT`
  (`doc/ui_keys.md:125`), and the registry must keep no row for it (`test_blocks` asserts that).
- If the user asks for a grid copy later, the cheapest shape is a menu item on the *owner* that
  copies the batch content - already implemented in task_09/FR-22.

Plan pointers:
- FR-16, FR-22, T26; design §8 "Accessibility / selection".
- Anchors were verified at plan time; re-verify before each edit.
