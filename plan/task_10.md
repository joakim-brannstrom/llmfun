## Task 10: C++ fold - liveness, absolute pins, the visibility predicate

Priority: P0. Dependencies: Task 7, Task 9.

### Description
Replace the mode+kind fold with the derived one: one function that answers "does this row draw"
and one that answers "is its body open", both reading the turn's liveness, the batch's position
and the user's absolute pins. Split `groupMinimal()` into `groupOpen()` + `groupStyleIsMinimal()`.

### Changes Made
- `cpp_tui/tui.h:174-198` (edit): `openWorkGroups` (`:179`) and `groupOverrides` (`:188`) become
  the two pin maps; `threadMinimalMode` (`:197`) stays.
- `cpp_tui/store/thread_store.h:193-203` (edit): `groupMinimal(id)` replaced by two functions.
- `cpp_tui/store/thread_store.h:431-437` (edit): drop `typedRowOpen_` (`:434`); add the two
  private resolvers.
- `cpp_tui/store/thread_store.cpp:455-513` (rewrite): `minimalMode`, `groupOpen`,
  `groupStyleIsMinimal`, `expanded`, `setExpanded`.
- `cpp_tui/store/thread_store.cpp:573-630` (`blockAt`, edit): `out.visible = resolveVisible(...)`,
  `out.expanded = resolveOpen(...)`.
- `cpp_tui/store/thread_store.cpp:632-705` (rewrite): `blockExpanded` = `resolveOpen`,
  `setBlockExpanded` = absolute pin.
- `cpp_tui/store/thread_store.cpp:890-913` (`reset`, edit): clear the pin maps' turn entries,
  drop the `typedRowOpen_` clear (`:910`).
- `cpp_tui/model/ui_state.h:256` (edit): delete `thinkingOpen`; `store/thread_store.cpp:903`
  stops clearing it.
- `cpp_tui/views/thread.cpp:448` (edit): `groupMinimal` -> `groupStyleIsMinimal` (the rest of the
  loop is task_11's).
- `cpp_tui/tui_api.cpp:874-878,915-931` (edit): the three sites that cleared `groupOverrides`
  now erase the turn's entries from both pin maps (task_24 adds the eviction path).

```cpp
// tui.h ChatTab - absolute session state (DD-6). The key is the persisted turnId, the row key
// is the wire block id (or "batch#<batchId>" for an owner), never a row index.
std::map<std::int64_t, std::map<std::string, bool>> rowPins; // turnId -> rowId -> open
std::map<std::int64_t, bool> groupPins;                      // turnId -> open
```

```
// thread_store.cpp - the predicate, verbatim from design §7.5:
turnLive(g)       = entry.turnLive
batchLive(b)      = turnLive && b.isNewest && !b.allSettled              // I-12
groupOpen(g)      = groupPins[g] ?? (mode == maximal && turnLive)
styleIsMinimal(g) = mode == minimal && !groupOpen(g)                     // the SHAPE (DD-14)

ownerVisible(b)   = groupOpen(g)                    // owners never draw under a minimal-style fold
ownerOpen(b)      = rowPins[ownerId(b)] ?? batchLive(b)
childVisible(b)   = ownerVisible(b) && ownerOpen(b)
childOpen(b)      = rowPins[childId] ?? batchLive(b)
segmentVisible(b) = (ownerVisible(b) && ownerOpen(b))
                    or (styleIsMinimal(g) && turnLive && b.isNewest)
plainVisible(r)   = groupOpen(g)                    // a GROUP-LEVEL plain row: notice, vision
queryVisible      = true                            // turn-level: the fold KEEPS the query ...
answerVisible     = true                            // ... and the answer, in BOTH modes (§4.6)
queryOpen         = true
answerOpen        = true
```

```
// Two clarifications the design's §7.5 leaves implicit; both are load-bearing:
//  * `plainVisible = groupOpen` is for GROUP-LEVEL plain rows only (notices, vision). The
//    query and answer rows are TURN-level (kQueryRow/kAnswerRow) and draw in both modes even
//    when the group is folded - §4.6's "maximal at rest" art shows ANSWER under `> WORK`.
//    Treating them as plain rows would hide every answer in minimal (the factory default).
//  * I-5 ("a child's visibility implies its owner is visible and open") holds for the
//    *pinned* children - thinking and the cards. The content segment has its own clause
//    (I-11, B1/FR-5) and is deliberately visible with no owner row drawn.
```

```cpp
// The writer: absolute, always stored (DD-6, I-13). No "drop the entry when it equals the
// default" - that is the relative encoding rev 1 got wrong.
bool ThreadStoreAdapter::setBlockExpanded(size_t k, std::size_t b, bool open) {
    const BlockEntry* block = blockEntryAt(k, b);
    if (block == nullptr || !bound() || !block->expandable)
        return false;                       // <- a real guard now (task_09)
    const Entry& entry = groups_[k];
    chat_->rowPins[static_cast<std::int64_t>(entry.view.id)][block->typedBlockId] = open;
    expandedGroup_ = entry.view.id;
    return open;
}
```

### Verification
- [ ] `make test_blocks test_thread_window test_streaming > build.log 2>&1` clean.
- [ ] Every cell of the mode matrix (design §4.5) as a unit test: group style, group default
      open, owner drawn, owner default open, children visible, content lifetime, live edge.
- [ ] I-13: pin a row closed, flip the mode (and the turn's liveness), the pin still answers
      closed; the same for a group pin.
- [ ] B2: a settled batch inside a re-opened group opens its owner -> content visible, thinking
      and cards closed.
- [ ] DD-14 + the turn-level rows: an *opened minimal* group reports `groupStyleIsMinimal == false`
      (draws maximal-style), a *folded maximal* group reports `false` too (never the minimal
      chips), and in a folded group of either mode the query row and the ANSWER row stay visible
      while a notice row does not.
- [ ] A live batch whose owner the user closed hides its segment as well (`segmentVisible`).
- [ ] A leaked `Running` call in a `Done` turn holds nothing open (`turnLive` false).
- [ ] Regression: the overview's regression command passes.

### Notes
- **L task.** If the edit turns out to need more than the file list above, stop and report for a
  `task_10a` split (the resolvers + pins first, the callers second) - do not push through.
- `groupOpen()` needs the *turn's* liveness, which task_07 stored as `Entry::turnLive`;
  `Entry::stream` stays for the shell's live-answer mark.
- `expanded(id)` keeps its signature (`test_thread_window.cpp:557` calls it in the cost loop).
- `groupStyleIsMinimal` must be consulted by the view's *shape* decisions only (cards, live edge,
  chips); visibility is never re-derived there (DD-16).

Plan pointers:
- DD-2, DD-6, DD-10, DD-11, DD-14, DD-16, I-5, I-11, I-12, I-13; §4.5, §7.5.
- Anchors were verified at plan time; re-verify before each edit.
