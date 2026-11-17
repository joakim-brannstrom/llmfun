## Task 24: Pin hygiene and "Collapse all"

Priority: P2. Dependencies: Task 10.

### Description
Two halves of DD-6: a pin for a turn that is gone is memory kept for nothing (prune it on turn
removal, clear and eviction), and the user needs an escape hatch that clears every pin at once.

### Changes Made
- `cpp_tui/tui_api.cpp:874-878` (`tuiSetTurns`, edit): clear both pin maps (a snapshot replaces the
  thread).
- `cpp_tui/tui_api.cpp:880-889` (`tuiTurnUpsert` -> `evictForCapacity`, edit): prune the pins of
  every turn the store no longer holds.
- `cpp_tui/tui_api.cpp:915-931` (`tuiTurnRemove`, `tuiThreadClear`, edit): erase the turn's entries
  from both maps.
- `cpp_tui/store/thread_store.h` + `.cpp` (new member): `void collapseAll();` - clears both maps
  and bumps nothing (the next frame reads them).
- `cpp_tui/views/palette.cpp:112-113,301,764-812` + `cpp_tui/model/keymap.h:86-97` (edit): a
  "Collapse all" action row (the palette is the keyboard path: `Ctrl+K`, type, Enter - no new
  chord).
- `cpp_tui/test_typed_thread_store.cpp` (new test): pruning and `collapseAll`.

```cpp
// The escape hatch (M3/DD-6): pins are absolute, so a user who pinned twenty rows needs one
// action that forgets them all. It writes no status and stores no default - it only clears.
void ThreadStoreAdapter::collapseAll() {
    if (!bound()) return;
    chat_->rowPins.clear();
    chat_->groupPins.clear();
    expandedGroup_ = 0;
}

// The prune (M3): a turn the store no longer holds takes its pins with it. Same shape as the
// existing `groupOverrides.erase(turnId)` calls in tui_api.cpp.
```

### Verification
- [ ] `make test_typed_thread_store test_palette > build.log 2>&1` clean.
- [ ] `tuiTurnRemove(id)` removes the turn's row pins and its group pin; a neighbour's pins stay.
- [ ] `evictForCapacity` (a 1001st turn) prunes the evicted turn's pins.
- [ ] `collapseAll()` clears both maps and the next frame draws every group on its default.
- [ ] The palette action exists, is reachable with `Ctrl+K` + typing, and the help/palette golden
      (`test_palette`) regoldens with the reason.
- [ ] `doc/ui_keys.md` lists the action (task_21 owns the doc edit; note the cross-reference).
- [ ] Regression: the overview's regression command passes.

### Notes
- Prune by *turn id*, never by row index: the row list churns while a response streams.
- Do not make `collapseAll` write `false` into every live row's pin: that would pin against the
  default and defeat the fold the user asked to restore.
- The action needs no chord; a new chord would collide with the terminal's own bytes
  (`doc/ui_keys.md:114-125`).

Plan pointers:
- DD-6, M3, C15, FR-7; `llmfun/doc/ui_keys.md` for the palette-row convention.
- Anchors were verified at plan time; re-verify before each edit.
