## Task 17: D - replay parity

Priority: P1. Dependencies: Task 1, Task 7.

### Description
Replay must reconstruct the same rows as live, duration included: the replay walk emits a content
block for every response, not only for a `Message` one.

### Changes Made
- `source/llm/app_agent/package.d:127-166` (`buildTypedTurns`, `ToolMessage` branch, edit): emit
  the content block next to the reasoning block, unconditionally, with `durMs`.
- `source/llm/app_agent/tests.d:3555-3620` (edit): the replay test's `blocks.length == 7` (`:3590`)
  becomes 8 and the positions after the new block shift.

```d
// A ToolMessage is a response like any other: its content block carries the response's
// duration, or replay has no duration for any response that called a tool (D9/T17).
if (!a.thinking.empty)
    out_.blocks ~= makeBlockData(a.turnId, reasoningBlockId(bi), bid, TuiBlockKind_Reasoning,
        TuiBlockStatus_None, TuiBlockFlag_Collapsible, null, null, a.thinking, durMs);
out_.blocks ~= makeBlockData(a.turnId, contentBlockId(bi), bid,
    TuiBlockKind_Prose, TuiBlockStatus_None, 0, null, a.role.to!string, a.content, durMs);
```

### Verification
- [ ] `dub build --config=application` clean.
- [ ] The replay test: one snapshot, 8 blocks, `content#1` present with `durationMs` from the
      `ToolMessage`, the Answer still last and still naming its batch.
- [ ] Replaying a finished turn reproduces the live run's row list: same block ids, same batch
      ids, same order (compare the two recordings' id/batch sequences, not a rendered screen).
- [ ] A response with no content (`a.content` empty) writes a body-less content block; the C++
      projection drops it (task_08) - assert the *D* side still emits it (the projection owns the
      drop, NFR-2).
- [ ] `dub test` green.
- [ ] Regression: the overview's regression command passes.

### Notes
- Keep the live tick's order (content, then reasoning) alone: the projection canonicalises
  (DD-5). The console order is task_25's.
- `emitCompressionTyped()` (`:1187-1194`) uses the same walk, so the compression re-emit gains the
  content blocks too - that is the same snapshot contract, no extra code.

Plan pointers:
- DD-5, FR-2, FR-14, D9, C3; design §7.6.2.
- Anchors were verified at plan time; re-verify before each edit.
