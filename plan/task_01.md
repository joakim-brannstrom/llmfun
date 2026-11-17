## Task 1: D - close the turn on every run exit

Priority: P0. Dependencies: none.

### Description
Give every run exit one terminal turn upsert: a stopped run keeps its `Cancelled` flips, a
failed or stuck run closes `Interrupted` without fabricating a block status, and `/c` re-opens
the same turn. Nothing about block states changes here.

### Changes Made
- `source/llm/app_agent/ui.d:701-746` (`TypedEmitContext`, edit): add `bool closedByAnswer;`
  next to `messageCount` - "the step walk already upserted `Done`".
- `source/llm/app_agent/package.d:1148-1152` (`emitStepTyped`, edit): set
  `ctx.closedByAnswer = true;` inside the `if (finalAnswerSeen)` block, before the
  `turnUpsert(Done)`.
- `source/llm/app_agent/package.d:1155-1167` (`emitStopTyped`, edit): add
  `emitRunEndedTyped(bool stopped)` **after** it (same file, next member).
- `source/llm/app_agent/package.d:964-975` (`runAgent`, edit at `:970-971`): replace
  `if (stopped) this.emitStopTyped();` with `this.emitRunEndedTyped(stopped);`.
- `source/llm/app_agent/package.d:928-944` + `:954-956` (`runAgent`, edit): remember the forced
  loop and re-open an interrupted turn (snippet below).
- `source/llm/app_agent/package.d:1132-1140` (orphan branch, edit): `ctx.clearLiveCall(a.toolCallId);`
  after the orphan's `blockUpsert` (a landed live call must not stay in `liveCalls`).

```d
/// One close for every run exit. A stop keeps its honest `Cancelled` flips; any other
/// exit leaves the calls' last real status and closes the turn `Interrupted` - unless
/// the step walk already closed it `Done` with the Answer.
package void emitRunEndedTyped(bool stopped) {
    auto ctx = typedCtx_;
    if (ctx is null)
        return;
    if (stopped) {
        emitStopTyped();
        return;
    }
    if (ctx.closedByAnswer)
        return;
    ctx.liveCalls = null;
    uiMsg.turnUpsert(UiTurnData(ctx.turnId, TuiTurnState_Interrupted, ctx.messageCount));
}
```

```d
// runAgent: the forced loop is the `/continue` resume (`continueAgent()`).
bool runAgentLoop;
bool resuming;                       // NEW, next to runAgentLoop (:928)
...
rval = slashCommands_.execute(this, query);
query = null;
resuming = forceRunAgentLoop;        // was: runAgentLoop = forceRunAgentLoop; (:941)
runAgentLoop = forceRunAgentLoop;
...
if (runAgentLoop) {
    // `/c`: the interrupted turn re-opens before its next batch streams.
    if (resuming && typedCtx_ !is null)
        uiMsg.turnUpsert(UiTurnData(typedCtx_.turnId, TuiTurnState_Running,
                typedCtx_.messageCount));
    this.doCompress(false);
```

### Verification
- [ ] `cd llmfun && dub build --config=application > build.log 2>&1` clean (grep `error:`).
- [ ] `tests.d:1963-2000` (`app_agent_echo`-area run test) updated: the `networkFailure` run now
      emits two turn upserts, `[1].state == TuiTurnState_Interrupted`, and still one query block.
- [ ] A stop run still ends `Cancelled` blocks + `Interrupted` (`tests.d:3735` unchanged).
- [ ] `agentStuckInLoop` closes `Interrupted` (new assertion next to the stuck-loop message test).
- [ ] `/c`: turn start -> `emitStopTyped` -> `runAgent("/c")` with a canned `ok` result re-upserts
      `Running` for the same turn id before any block of the next batch.
- [ ] The step-level tests that drive `emitStepTyped`/`emitStopTyped` directly still pass
      (`tests.d:3660-3745`), including `messageCount == 6` at `:3717`.
- [ ] Regression: the overview's regression command passes.

### Notes
- Do **not** close from `processResult` or from the step walk: the `processResult`-level
  "no final answer -> the turn stays open" assertions (`tests.d:1863, 2253`) are the contract
  that only the *run* closes.
- Do not write `Cancelled` on a failure exit: the calls keep their last real status (DD-2).
- `emitStopTyped` keeps `typedCtx_` alive on purpose - `/c` resumes that turn.
- `tests.d:1987-1993` is a *run-level* assertion whose comment says "the turn stays open"; it
  moves with this task even though design §10.4 does not name it.

Plan pointers:
- DD-2 (derived liveness, no fabricated status), FR-11, D12/D13 (design §2).
- Anchors were verified at plan time; re-verify before each edit.
