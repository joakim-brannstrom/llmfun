## Task 7: Cross-cutting hardening battery (P2)

Priority: P2. Dependencies: tasks 1, 5, 6 (the shapes must exist; the
battery exercises them together). Test-only + fixes for whatever it catches.

### Description
Run the full scenario matrix over the finished v6 rendering (design §8):
the paths interact (upsert storms during live edges, compression while
overrides exist, session switches mid-run), and the shipped busy-edge bug
is exactly the kind of gap a battery catches. Each scenario below lands
as a named test in the existing suites; any failure is fixed (product
or projection), never re-baselined away.

### Changes Made — the battery (one named test per scenario)
- Replay parity x2, expansion preserved: the same stored session
  replayed twice renders identical pair sets and identical fold
  states (`llmfun/cpp_tui/test/parity_core_loop.cpp`).
- Upsert storm: idempotent merges, throttled as today; the pair set
  is part of the merged block and never grows per frame.
- Stop mid-batch: in-flight calls -> Cancelled (header-only, no
  marker), the partial answer stays, Send restored (the unwinding
  fired ready — task 1's exception-safe brackets).
- Compression re-emission: affected turns re-emitted (upsert/remove)
  as today; notices keep rendering; turn order held (the task 4
  order pins); no ordinals to recompute (the feature is gone).
- 1000-message session: opens and scrolls without per-frame re-scan
  cost (N4 — store lookup, not text parse; the pair set adds no
  per-frame work).
- Unknown-callId result (live): recorded under its callId, merged
  when the call lands; the pair set rides the same upsert.
- Session switch mid-run: store rebuilt from snapshot, emitter
  re-attaches by turnId; the ready flag belongs to the RUN, not the
  session — Stop stays shown until the run ends, and a Stop click
  targets the run in flight regardless of the session shown (F6).
- The busy-edge battery across ALL task-1 entry points: main query,
  compression, /plan, /code, one-shot, + every path task 1's
  enumeration named — the composer flips to Stop for each and back
  to Send after, driven by the REAL edge (PL-4).
- Suites green as a whole: `dub test` (D-side); the full cpp_tui
  suite incl. xvfb GUI binaries (`llmfun/cpp_tui/test_e2e_gui.cpp`,
  test_palette, test_topbar, ...).

### Verification
- [ ] Every scenario above is a NAMED green test (list them in the
      commit message; a scenario without a test does not count).
- [ ] The headless harness runs the full matrix green.
- [ ] `dub test` green from llmfun/.
- [ ] cpp_tui suite green incl. xvfb GUI tests (pre-existing reds
      excluded — overview).
- [ ] Any product fix this battery forced is in the same commit as
      its regression test; goldens re-driven only where the fix
      changed a rendered shape (regolden + eyeball, PL-12 discipline).

### Notes
- No new shapes, no re-baselines: the battery asserts the shapes
  tasks 1-6 landed. If a scenario needs a rule that is not in the
  design, STOP and report the gap — do not invent policy here.
- The 6.4 caveat (a turn that ended without a final answer) renders
  as today — recorded, not tested-into-a-fix (design §7).
- Plan-time anchors to re-verify: parity_core_loop.cpp
  574-585/615-661/1077/1113/1673-1681/1726-1731/1766-1788/1812-1818/1894-1925,
  test_typed_thread_store.cpp 52-61/531-597, test_thread_window.cpp
  245-306/388/552, test_blocks.cpp 373-428/1223/1888-1982,
  test_e2e_gui.cpp 389-441/677-682, test_palette.cpp 466.

Plan pointers (task-specific only — standing rules live in the overview):
- Decision letters used: PL-4, PL-10, PL-12 (defined in the overview).
