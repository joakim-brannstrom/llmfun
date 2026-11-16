## Task 1: Real busy edge — uiAgentBusy body + entry-point brackets (D6)

Priority: P0. Dependencies: none. D-side only; no wire change; shippable alone
(design §9 group 1 — "the one true bug").

### Description
The composer's Send/Stop flip is golden-green but dead in production: the turn
entry points raise a busy edge that lands in an empty `uiAgentBusy()` body, so
the ready flag never leaves ready (appendix 3.5). Give the busy half its body,
make every run entry point raise the edge, make the brackets exception-safe, and
point the run-in-flight predicates at the flag.

### Pre-step (required before any edit — W3)
Enumerate every path that raises or must raise the busy edge. Search-verified,
no truncated greps (the truncated-grep lesson once hid package.d:1434):
grep `busy`/`ready`/`runInFlight`/`agentRunInFlight_` across
`llmfun/source/llm/` and re-verify each candidate call site. Known at plan time:
- userQuery run bracket: `llmfun/source/llm/app_agent/package.d:1453-1473`
  (`app.runInFlight = true; ... app.uiMsg.ready(); app.runInFlight = false;`).
- doCompress: `llmfun/source/llm/app_agent/package.d:366-382` (`uiMsg.busy;` at
  ~371, then `agent_.compress(...)`) — bracket exists, lands in the empty half.
- /plan and /code pipeline handlers: `llmfun/source/llm/app_agent/slash_pipeline.d:17-50`
  (planHandler 26-35, codeHandler ~37-50) — NO busy()/ready() bracket today.
- one-shot mode: `llmfun/source/llm/app_agent/package.d:1397-1402`
  (`app.oneShotQuery -> app.runAgent(...); startExit(...)` ) — no bracket today.
- drag&drop-triggered runs and session-resume paths: verified at plan time that a
  file drop does not itself start an agent run (DropWorker); RE-VERIFY both, and
  add them to the bracket set if the re-verification finds a run start.
- the second in-flight bookkeeping inside runAgent:
  `llmfun/source/llm/app_agent/package.d:925-929`
  (`agentRunInFlight_ = true; scope (exit) agentRunInFlight_ = false;`).
Record the verified set verbatim in the task's Notes before editing; the
acceptance lists it.

### Changes Made
- `llmfun/source/llm/tui/package.d` (~1093): give `uiAgentBusy()` its body — clear
  the ready flag, mirroring `uiAgentReady()` (~1095-1098: `ui.setReadyStatus(true);
  playNotification();`). Busy half = the ready-clear only (PL-3).
- Same file: make `uiAgentReady()` and `uiAgentBusy()` idempotent-safe per §7
  (busy while busy = clear; ready = set; plain bool, frame-safe readers).
- `llmfun/source/llm/app_agent/slash_pipeline.d` (planHandler 26-35, codeHandler
  ~37-50): bracket each pipeline run with busy()/ready() in ALL paths (I6) —
  exception-safe (PL-5). The handlers own the UI lifetime of the run; the
  pipeline engine stays UI-agnostic (appendix §5 rejected alternative).
- `llmfun/source/llm/app_agent/package.d`:
  - userQuery bracket (1453-1473): keep the edge, make the restore
    exception-safe (`scope (exit)` ready + flag restore, PL-5).
  - doCompress (366-382): same exception-safe restore.
  - one-shot (1397-1402) and any additional path the pre-step names: add the
    bracket. If the enumeration finds a path that can overlap a live run,
    convert the plain-bool bracket to a nesting counter instead (F6 fallback).
  - runAgent's `agentRunInFlight_` (925-929): collapse onto the ready flag per
    F6 "single source of truth" (PL-6) — predicates read the flag; grep for
    other `agentRunInFlight_` consumers first and record what you find.
- C++ side (no logic change expected — it works once the flag moves):
  - `llmfun/cpp_tui/tui.cpp:989-995` tuiSetReadyStatus — verify, don't touch.
  - `llmfun/cpp_tui/views/shell.cpp:456-457` composer input wiring — untouched.
  - `llmfun/cpp_tui/tui.h:471` predicate `!state.readyStatus &&
    !state.cancelRequested` — untouched (composer flip).
  - `llmfun/cpp_tui/views/palette.cpp:276,616,669,728,735,835` — the stop
    affordances: verify each read is the ready-flag predicate per F6 (stop
    affordances revive with the flag); fix any read that bypasses it.
  - Stop targets the run in flight regardless of the session shown: the global
    stop path (`llmfun/source/llm/tui/package.d:945-972`) — verify; no change
    expected under the at-most-one-run assumption (state it in Notes).

### Verbatim contract — the busy edge (task-1-owned; consumers restate)
- The ready flag is the single source of truth for "a run is in flight" (F6).
- `uiAgentBusy()` clears it; `uiAgentReady()` sets it (+ its existing
  notification). Both cross to C++ as today (tuiSetReadyStatus 0/1,
  `llmfun/cpp_tui/tui.cpp:989-995`).
- Every turn entry point brackets its run: busy() before, ready() after, in
  all paths including unwinding (scope-exit, PL-5).
- The runtime-status phase edge stays display-only and untouched (C3).
- At most one run at a time (the composer refuses a second query while one
  is in flight) — stated assumption; nesting counter if it breaks (F6).

### Verification
- [ ] Pre-step enumeration recorded (Notes) with file:line for every path;
      one-shot + drag&drop + session resume explicitly answered.
- [ ] D-side unittests: uiAgentBusy clears the flag (THE regression test for
      the shipped bug — today nothing asserts the busy half); each named
      entry point raises the edge; an exception unwinding out of a run
      restores ready (I6); stop affordances read the flag.
- [ ] `dub test` green from llmfun/.
- [ ] Composer flips to Stop during a main query, compression, and a /plan
      or /code run, back to Send after — driven by the REAL D-side edge,
      never by poking `state.readyStatus` directly (PL-4; today's
      composer-busy golden masks the bug exactly this way). Re-drive the
      composer-busy golden(s) accordingly (regolden via --update + eyeball).
- [ ] Phase edge (sendRuntimeStatus) untouched — grep its call sites.
- [ ] At-most-one-run assumption stated in Notes, or the nesting counter landed.
- [ ] C++ suite green (no C++ change expected; run to prove it).

### Notes
- PL-3: busy half sets no notification — `playNotification()` is the ready
  half's completion cue; a busy ping is out of scope.
- PL-5: plain-bool non-refcounted brackets BY DESIGN under at-most-one-run;
  the nesting counter is the named fallback, not a belt-and-suspenders extra.
- PL-6: collapsing `agentRunInFlight_` must not orphan a consumer — grep first,
  record findings, then collapse.
- Known plan-time facts to re-verify (they move): uiAgentBusy ~1093, ready
  ~1095-1098, doCompress 366-382, one-shot 1397-1402, userQuery 1453-1473,
  slash handlers 17-50, stop paths tui/package.d:945-972.

Plan pointers (task-specific only — standing rules live in the overview):
- Decision letters used: PL-3, PL-4, PL-5, PL-6 (defined in the overview).
- Task 7 re-runs this edge across all entry points in the battery; the
  golden re-drive here is the regression proof, not the full matrix.
