## Task 3: D - displayability on the wire (`TuiBlockFlag_Hidden`)

Priority: P0. Dependencies: none.

### Description
Move "is this tool displayable" onto the wire: one additive flag bit, set at exactly one
constructor from `LlmConfig.isHiddenTool()`, used by every tool-block upsert (six paths). Delete
the console's own hidden-name list.

### Changes Made
- `cpp_tui/tui_api.h:649-653` (edit): add `TuiBlockFlag_Hidden = 8` to `TuiBlockFlags`.
- `cpp_tui/tui_api.h:29` (edit): `TUI_API_VERSION 14` -> `15` (documentation only).
- `source/llm/app_agent/ui.d:606-623` (edit): delete `kHiddenToolNames` (`:609`) and the
  name loop in the console `blockDisplayable` (`:619-621`); the live-status rule at `:617-618`
  stays.
- `source/llm/app_agent/ui.d:626-656` (edit): keep `kToolBlockFlags`, add `ToolFlags` below it.
- `source/llm/app_agent/ui.d:701-746` (edit): `TypedEmitContext` gains `ToolFlags toolFlags;`.
- `source/llm/app_agent/ui.d:785-798` (edit): the Pending and the Running upsert call
  `ctx.toolFlags.of(...)` instead of `kToolBlockFlags`.
- `source/llm/app_agent/package.d:71` + `:1004` + `:1189` (edit): `buildTypedTurns(history,
  ToolFlags flags)`; both call sites pass `currentToolFlags()`.
- `source/llm/app_agent/package.d:160` (replay), `:1113-1115` (step walk), `:1135-1140`
  (orphan), `:1162-1164` (stop flip): `flags.of(name)` / `ctx.toolFlags.of(name)`.
- `source/llm/app_agent/package.d:1015-1025`, `:1032-1037`, `:1196-1205` (edit): the three ctx
  creations set `toolFlags = currentToolFlags();`.

```d
// ui.d - the only way a tool block's flags are built (DD-3). A type, not a convention:
// a future upsert site cannot spell the flags without it.
struct ToolFlags {
    /// The config's dual-axis predicate (`isHiddenTool`). null = nothing is hidden.
    bool delegate(in string) isHidden;

    int of(string name) const {
        return kToolBlockFlags |
            (isHidden !is null && isHidden(name) ? TuiBlockFlag_Hidden : 0);
    }
}
```

```d
// package.d - the config seam the live tick does not have (it holds no LlmConfig).
private ToolFlags currentToolFlags() {
    return ToolFlags((in string name) => this.llmConf.isHiddenTool(name));
}
```

### Verification
- [ ] `dub build --config=application` clean.
- [ ] Recording-sink test: for a hidden name (`task_done`, `taskDone`, `pipeline_output` -
      both axes) each of the six paths writes `(flags & TuiBlockFlag_Hidden) != 0`; for a
      visible name (`bash`) every path writes `flags == kToolBlockFlags`.
- [ ] `hiddenTools = []` (config override): the same six paths write `flags == kToolBlockFlags`.
- [ ] The console still prints a hidden tool's line exactly as before (its rule is unchanged;
      one golden-ish assertion on the formatter's output).
- [ ] `grep -rn "kHiddenToolNames" source/` is empty.
- [ ] Regression: the overview's regression command passes.

### Notes
- `isHiddenTool`'s `static` caches are per-process and not thread-safe; every call site here
  runs on the agent thread - do not call it from a UI thread.
- The `Pending`/`Running` blocks carry the flag too: displayability must not depend on the
  status, or a hidden call would draw while it streams.
- Do not add a `ResponseBatch` bit: DD-17 rejects it and DD-4 derives the same fact.

Plan pointers:
- DD-3, DD-17(b), NFR-3, FR-10; design §6.1 (wire model).
- Anchors were verified at plan time; re-verify before each edit.
