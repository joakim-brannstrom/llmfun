## Task 25: Console parity for the intra-batch order

Priority: P2. Dependencies: Task 17.

### Description
The live stream tick emits content before reasoning while replay emits reasoning before content;
the console formatter should see one order. Emit reasoning first in the live tick. No C++ behaviour
depends on it (the projection canonicalises).

### Changes Made
- `source/llm/app_agent/ui.d:768-791` (`messageUpdate`, edit): the reasoning upsert moves above the
  content upsert; ids, batches and statuses unchanged.
- `source/llm/app_agent/tests.d:3660-3720` (edit): the recorded upsert order (`blockUpserts[1]`,
  `[3]`, `[4]`, `[6]`) swaps to match.

```d
// One order on the wire too (design §7.6, DD-5): reasoning, then content, then the calls.
if (!msg.reasoning.empty)
    uiMsg.blockUpsert(makeBlockData(ctx.turnId, reasoningBlockId(ctx.batchIndex), ctx.batchId,
            TuiBlockKind_Reasoning, TuiBlockStatus_None, TuiBlockFlag_Collapsible, null, null,
            msg.reasoning, 0));
uiMsg.blockUpsert(makeBlockData(ctx.turnId, contentBlockId(ctx.batchIndex), ctx.batchId,
        TuiBlockKind_Prose, TuiBlockStatus_None, 0, null, ctx.role, msg.content, 0));
```

### Verification
- [ ] `dub build --config=application` clean.
- [ ] The live/stop test's recorded order matches (reasoning before content in the same batch).
- [ ] No C++ test moves: the projection's canonical order (task_08) already draws the same rows.
- [ ] The console's own output for a streamed response is byte-identical (it buffers per batch).
- [ ] `dub test` green.
- [ ] Regression: the overview's regression command passes.

### Notes
- This is the *optional* half of C3 ("the D-side reorder stays optional console parity"); if it
  costs more than the one file pair, drop it and record it in `doc/todo.md` instead.
- `streamMessageDone` is unaffected: it re-upserts the calls, not the prose.

Plan pointers:
- DD-5, FR-20, C3/§7.6.1; design §11 T25.
- Anchors were verified at plan time; re-verify before each edit.
