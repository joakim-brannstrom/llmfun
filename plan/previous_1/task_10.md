## Task 10: Doc sweep (design P3-9)

Priority: P3. Dependencies: task 9. One commit.

### Description
Mark the delivered design notes and update the C API boundary documentation to
the v5 surface.

### Changes Made
- doc/todo_modern_ui.md: mark the thread v5 / tool-card deferral note as
  delivered by this design; KEEP the non-thread remainder (§9.5).
- doc/todo.md: mark the live-tool-tab and stats-window lines delivered.
- doc/tui_design.md: update the C API boundary section — ChatMessageParam and
  the v4 chat path are gone; the v5 turn/block surface (tuiSetTurns/
  tuiTurnUpsert/tuiBlockUpsert/tuiTurnRemove/tuiThreadClear, TuiTurn/TuiBlock,
  the batchId and ordinal contracts) replaces them (W10).

### Verification
- [x] The three docs say exactly what landed; no stale v4 references remain
      in them (grep ChatMessageParam / tuiAddChatMessage in doc/).
- [x] No code changes in this commit.

### Notes
- Keep the deferral notes' non-delivered items intact — only the delivered
  lines change.

### Implementation notes (task 10, as delivered in commit 2fae582)
- doc/todo.md: struck through the live-tool-tab and stats-window lines with
  delivery notes (Agents inspector tab streams the live tool use; the now
  bar's status row carries the run's latency, token use and context live).
- doc/todo_modern_ui.md: §8.11's post-MVP items 7 (store + adapter) and 8
  (thread v5) are marked **Delivered** by the thread v5 design (the typed
  store is the only renderer — no dual-renderer phase; the v4 message path
  was deleted at the cutover); item 7's stale `tuiAddChatMessage` sketch is
  reworded ("the chat-message entry point"). The §9.1 "No v5 turn/block
  model" bullet and the "ribbon and tool cards arrive in P3" line are
  marked **Delivered**. The other deferrals stay deferred (Context Lens,
  inspector tabs, universal search, canvas, minimap, semantic zoom, themes,
  density, virtualization, the keymap overhaul).
  NOTE: the "thin adapter" paragraph, the §9.2 deferred list and Appendix B's
  header sketch (`TUI_API_VERSION 12`, no message entry point) were already
  corrected in the earlier task 5-6 commit (3cee562) and are left as-is.
- doc/tui_design.md: the C API Boundary diagram line now names
  `TuiTurn/TuiBlock (typed turns, blocks, ordinals)` instead of
  `ChatMessageParam`; the "Calls existing C++ functions" line names
  `tuiTurnUpsert`; the thinking/reasoning paragraph refers to a
  `TuiBlockKind_Reasoning` block; API Version History generation 1 no longer
  names the removed param ("the bundled chat-message param (removed in
  v5)") and a new generation 6 records the v5 turn/thread surface (the v4
  chat path removed; TuiTurn/TuiBlock + tuiSetTurns/tuiTurnUpsert/
  tuiBlockUpsert/tuiTurnRemove/tuiThreadClear; batchId + ordinal snapshot
  replay contracts).
- Verified with `grep -rn "ChatMessageParam\|tuiAddChatMessage" doc/` — no
  matches. `git show --stat` confirms only the three docs changed
  (25 insertions, 11 deletions); commit 2fae582 on update-ui. Docs only,
  no code changes.
