## Task 6: Minimal mode + live folding (design P1-5)

Priority: P1. Dependencies: task 5 (cutover). One commit.

### Description
Implement the minimal (default) thread rendering over the typed store: turn
content with bodies, work groups header-only with the live chips line, and the
folding rules — including mid-batch folding and the answer-chip streaming edge.

### Changes Made
- llmfun/cpp_tui/views/thread.cpp (renderThreadBody 332+, the group/step row
  drawing ~470-570, Load-earlier row 369-394): the minimal projection per
  design §6:
  - User (turn-opening Prose) and Answer blocks render with bodies — they are
    the turn's content, not its work. The turn's ordinal (D5) renders ONCE, on
    the query row, replacing today's per-message `[nr]`; the work header and
    the answer row carry no number.
  - Work groups render header-only: `WORK · N steps` — N from the store's
    stepCount (D4 weights over the D3 filter), recomputed per render, never
    stored.
  - The newest live group adds ONE chips line under its header: tool names
    only, one chip per live step. Chip label = `[` ~ block title ~ `]`
    uniformly (so the streamed answer shows as the `[task_done]` chip on
    the live edge until its result arrives, then the answer body appears
    — W8: presentation mapping, no D-side special case). Running = green,
    Pending = dim.
  - Folding: a group folds the moment its last live step goes dormant
    (mid-batch folding included — batch 1 done + batch 2 live keeps the
    group open with batch 2's chips only). Completed groups: header-only.
  - The mode source is a module constant defaulting to minimal until task 8
    wires ShellPrefs; maximal is a no-op branch until task 7.
  - The window/page arithmetic and the Load-earlier row keep today's shape,
    now paging over turns (anchorGroupId / pageGroups / drawnGroups,
    model/ui_state.h:338-349).
- Per-group overrides are task 8 (opening a group in minimal renders it
  maximal-style); task 6 ships the override-map-free minimal defaults
  (expansionDefaultThroughId, tui.h:254, keeps its role for the default).

### Verification
- [ ] Golden frames (§8; the test_blocks/test_streaming pattern — goldenText,
      scene compare, `--update` regolden flow, goldens in test/golden/):
      - idle thread (completed turns: header-only groups + content rows)
      - live batch with mixed statuses (an Ok step, a Running step, a
        Pending step -> chips show only the live two, correct colors)
      - post-completion fold (the chips line gone, header-only)
      - the streamed-answer edge: `[task_done]` chip while streaming, the
        answer body after the result
      - a turn whose ordinal and step count show together (§8)
- [ ] Upsert storms render stably (the v4 ~20 Hz C-side throttle is GONE with
      the v4 stream entry point it guarded - stream_state.h was rewritten in
      task 5 and kStreamUpdateIntervalMs no longer exists; see the Notes
      conflict entry below).
- [ ] `dub test` + cpp_tui binaries green (known pre-existing reds excluded).

### Notes
- CONFLICT WITH THE PLAN (recorded post-review, I1): the cross-cutting
  invariant "the ~20 Hz C-side throttle is untouched" could not hold - the
  throttle guarded the v4 stream path, and task 5 deleted that path whole
  (stream_state.h rewritten; `kStreamUpdateIntervalMs` has zero hits). Typed
  ticks now upsert unthrottled, and every version bump pays a full syncTyped
  rebuild (O(thread bytes) per tick on long threads; in practice bounded,
  ~2-5x v4's 30/s ceiling). A cheap coalesce (park the newest snapshot at the
  boundary, or throttle version bumps) is a candidate follow-up task.
- D4 weights are the store's (task 2); the view recomputes N per render —
  never caches it in the group.
- The chip-label mapping (`[` ~ title ~ `]`) is uniform BY DESIGN (W8): if
  the design owner wants a different label shape for ordinary tools vs
  task_done, that is a one-line presentation change — flag, do not invent
  per-tool cases.
- The inspector (Agents tab lanes, Activity) is untouched (§6).
- Decision letters used: D4, D5 (overview).
