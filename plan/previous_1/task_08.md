## Task 8: Settings row + per-group override (design P1-7)

Priority: P1. Dependencies: tasks 6, 7. One commit.

### Description
Add the minimal/maximal choice to the Settings sheet (behaviour section) with
ui.conf persistence, and the session-local per-group override that beats the
mode and dies on session switch.

### Changes Made
- llmfun/cpp_tui/model/settings.h: ShellPrefs (117-129) gains the mode field +
  getter/setter beside the existing toggle helpers (131-135); `uiConfText`
  key list (140-143) gains the key; loadUiConf/saveUiConf/flushUiConf
  (153/157/163) round-trip it. `kUiConfVersion` STAYS 2: the key is new and
  defaults to minimal when absent on old files (the acceptance requires
  old-file defaulting, not a version bump).
- llmfun/cpp_tui/views/settings.cpp: a behaviour-section row (allRows ~183
  section) cycling `minimal` <-> `maximal` through the row.id-keyed Choice
  path (activate 495-535); the section name/rows wiring (541-581) picks it up.
- llmfun/cpp_tui/tui.h: ChatTab gains the session-local override map keyed by
  the group's persisted turnId (`std::unordered_map<long long, GroupOverride>`,
  GroupOverride = ForceOpen | ForceClosed) beside the v5 store member.
- llmfun/cpp_tui/views/thread.cpp: rendering consults the override FIRST, the
  mode second: minimal + ForceOpen renders the group maximal-style (task 7
  anatomy); maximal + ForceClosed renders header-only (task 6 shape). A
  toggle back to the mode's default removes the entry.
- llmfun/cpp_tui/tui_api.cpp: `tuiSetTurns` and `tuiThreadClear` clear the
  override map (session switch / clear reset); `tuiTurnRemove` drops the
  turn's entry (no stale overrides survive a compression re-emit, §7).

### Verification
- [ ] The sheet row cycles both words and persists across restart
      (ShellPrefs pattern — the settings-sheet tests).
- [ ] Persistence round-trip: a ui.conf WITHOUT the key loads minimal;
      writing + reloading preserves maximal.
- [ ] Override: opening a group in minimal renders it maximal-style; the
      override survives mode flips within the session; a session switch
      resets it (golden or unit assert on the map).
- [ ] No stale entries: compression re-emission (turnRemove + re-upsert)
      leaves no orphan override.
- [ ] `dub test` + cpp_tui binaries green (known pre-existing reds excluded).

### Notes
- The override key is the persisted turnId (D3/W3: group id = turnId), NOT
  an adapter sequence number — overrides survive upserts/rebuilds by design.
- No keyboard shortcut / palette command for the mode (§11 out of scope) —
  the sheet row is the only mode switch.
- CONFLICT WITH THE PLAN (recorded post-review, I1; see task 6's Notes): the
  cross-cutting invariant "the ~20 Hz C-side throttle is untouched" could not
  survive the task-5 cutover - the throttle guarded the deleted v4 stream path
  (`kStreamUpdateIntervalMs` has zero hits). Typed ticks upsert unthrottled
  and every version bump pays a full syncTyped rebuild (O(thread bytes) per
  tick; bounded in practice). A cheap coalesce is a candidate follow-up task.
- D11 RE-BASELINE, recorded post-review (I2 + C1): the parity goldens were
  re-baselined once more after tasks 6-7 - the accepted delta is exactly the
  `[N] ` ordinal prefix arriving on the query row's drawn body (the open query
  row shows the number D5 puts on it; at task 5 only a closed row's header
  carried it). Any other golden delta is a projection bug. The re-baselined
  goldens also carry the `WORK - N steps` one-group-per-turn header for
  tool-less turns (task 2 contract) - consistent with the design, recorded
  here because task 5's Notes missed it.
- Known-reds update for the task-10 overview doc (post-review): the perf-gate
  FAILs in parity_core_loop no longer occur ("not gated" after the cutover) -
  the standing-rules red-list should say parity_core_loop must be fully green
  (162+ checks), not "green except 2 perf gates".
- Minor cleanups noted post-review (M1/M2), candidates for task 8/9:
  config.d's `isHiddenTool` + the hiddenTools config have zero production
  callers after the cutover (tests only) - delete or note; and task 5's
  console-parity checkbox is satisfied at the emitter level (TuiRecordingSink +
  ConsoleFormatter vs hand-pinned lines), not by replaying the task-4 golden
  file on the live path - wording fix or a follow-up that replays it.
- Decision letters used: D3 (overview).
- Implementation notes (task 8, as built): the frame mirrors the pref into the
  store per frame (`renderShell` after `bind()`) and the same spot invalidates
  the thread's height hints on a mode change - the hint key carries the box a
  group was drawn in (revision/width/row/compact) but not the mode, and a flip
  redraws every group in a different shape (endGroup's promise, now honoured).
  The maximal-mode click path pins `ForceClosed` only; `ForceOpen` exists per
  the design and is honoured by the consults (`groupMinimal()`/`blockExpanded()`
  /`expanded()`), but no caller writes it yet - a future pin writer may. The
  agent drill-down chats keep the factory minimal mode (their `ChatTab`s are not
  prefs-mirrored; the plan's file list scopes the plumbing to the main thread).
- Review-fix round (tasks 7-8 review, ACCEPT with findings; fix commit follows
  this note):
  - F1 (important, fixed): a ForceClosed pin written in maximal survived a
    flip to minimal and swallowed the group's open clicks - the minimal branch
    of `setExpanded`/`setBlockExpanded` wrote only `openWorkGroups` while the
    consults read the pin FIRST, so the group was unopenable by clicking for
    the rest of the session. Fix: a minimal-mode click (open OR close, both
    seams) now also erases the turn's pin - a ForceClosed pin is redundant in
    minimal (the mode draws the fold on its own) and a ForceOpen pin is what
    an explicit close overrides - restoring the map's own invariant ("holds
    only the pin the mode cannot express"). Unit-pinned: pin in maximal ->
    flip to minimal -> open click reports open, draws open, map clean; the
    symmetric close and the group-seam path covered too.
  - F2 (fixed): the per-row defaults in `blockExpanded`/`setBlockExpanded`
    (non-group branch) consulted the GLOBAL mode; they now read the group's
    EFFECTIVE mode (`groupMinimal(entry.view.id)`) - a minimal group pinned
    ForceOpen defaults its tool cards open (the task 7 anatomy task_08
    promises for ForceOpen), and a maximal group pinned ForceClosed keeps
    its rows collapsed. Latent only (no production ForceOpen writer);
    pinned by a unit assert that fails on the pre-fix code.
  - F3 (rejected as built): the O(B^2) batchTools count per group rebuild -
    B is small and rebuilds are fingerprint-gated; a one-pass map count is a
    non-behavioural micro-optimisation, not worth the churn now.
  - F4 (fixed): the tuiSetTurns-clears-pin assert ran on a fresh state's
    empty map (vacuous); the test now inserts a pin before the call.
  - No golden changed: the default path (minimal, no pins) renders exactly
    as before - parity_core_loop stayed 208 text + 81 gui with zero golden
    deltas, per the D11 rule.
