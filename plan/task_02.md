## Task 2: D - run-exit probes

Priority: P0. Dependencies: Task 1.

### Description
Make FR-11 verified instead of counted: one test per run-exit path drives `runAgent` through a
scripted agent and asserts both the emitted turn state and the *store shape* a projector sees
(no `Running` block survives a closed turn), plus a replay of a finished run that equals the
live shape.

### Changes Made
- `source/llm/app_agent/tests.d` (new unittests, module scope helper): a
  `probeApp(...)` factory and four exit probes + one replay probe.

```d
// The seam is `tests.d:77-91 CannedAgent` + `tests.d:3535-3549 typedTestApp`; both are
// unittest-local today, so this task adds ONE module-scope copy under `version (unittest)`:
private class ScriptedAgent : Agent {
    import llm.types : ProcessResult;
    import my.filter : ReFilter;
    ProcessResult[] script;   // served in order, then repeats the last
    this(string name, LlmConfig conf) { super(name, conf, null, null, null, ReFilter.init); }
    override ProcessResult process(bool delegate() interrupt) @trusted nothrow {
        const i = served < script.length ? served : script.length - 1;
        ++served; return script[i];
    }
    size_t served;
}
```

```d
// The store shape a closed turn must have: every block the sink recorded for the turn is
// terminal, and the turn's own last upsert is its terminal state.
auto lastState = (TuiRecordingSink s, long turnId) {
    int st = -1;
    foreach (t; s.turnUpserts) if (t.id == turnId) st = t.state;
    return st;
};
auto runningBlocks = (TuiRecordingSink s, long turnId) {
    int n;
    foreach (b; s.blockUpserts)
        if (b.turnId == turnId && (b.status == TuiBlockStatus_Pending ||
                                   b.status == TuiBlockStatus_Running)) n++;
    return n;
};
```

### Verification
- [ ] `dub build --config=application` clean.
- [ ] Exit 1 - final answer: a scripted `ok` step carrying a `task_done` `ToolMessage` +
      `Answer` ends `Done`, and `runningBlocks == 0`.
- [ ] Exit 2 - stop: `stopAgent()` + a `ToolMessage` with a live call ends `Interrupted`,
      the call's last recorded status is `Cancelled` (never `Ok`), `runningBlocks == 0`.
- [ ] Exit 3 - `networkFailure`: ends `Interrupted`, `runningBlocks == 0`, no `Failed` block is
      written that the transport did not report.
- [ ] Exit 4 - `agentStuckInLoop`: ends `Interrupted`, the console message still explains.
- [ ] Exit 5 - `/c` resume after a stop: `turnUpserts` shows the same id `Running` again and the
      new batch's blocks land under it.
- [ ] Replay: `emitReplayTyped()` after a finished run emits a snapshot whose rows equal the
      live run's rows (same ids, same batch ids, same order), duration included.
- [ ] One manual live run recorded in the task's report (mode, exit path, the `AGENT` row seen).
- [ ] Regression: the overview's regression command passes.

### Notes
- The probe asserts the *emitted* shape, not a rendered screen: the C++ side is not in this
  task (T7+ is, and its own tests assert the projection).
- `dub test` timeout is 123 s; run the new probes alone while iterating:
  `dub test -- -i '.*run_exit.*'`.
- A `CannedAgent`-style script returns the last element forever; make every probe's script
  terminal (`keepRunning = false`) so a bug cannot spin.

Plan pointers:
- FR-11, R-4; design §10.2 ("one run per exit path"). Anchors re-verified per standing rules.
