# Design review: system_design.md — Thread rendering v5 (typed turns/blocks, two modes, tool presentation)

Reviewer: system-design-review pass, 2026-01-06.
Reviewed: plan/system_design.md against plan/appendix.md, plan/design_notes.md, and the indexed
source (cpp_tui/tui_api.h, cpp_tui/store/thread_store.*, cpp_tui/views/thread.cpp, cpp_tui/views/shell.cpp,
source/llm/app_agent/*, doc/todo_modern_ui.md, doc/tui_design.md).

Verdict: **Approve with fixes.** The architecture is sound, the appendix contract is honored, and the
code survey is accurate. Three specification gaps (displayability-filter ownership, eviction granularity,
group-id stability for per-group overrides) should be closed as one-line spec decisions before the
implementation plan is written; the rest are minor clarifications and test additions.

## 1. Verified against source (spot checks)

- v4 ABI facts check out: `tuiAddChatMessage`, `tuiUpdateStreamChatMessage`, `tuiStreamChatMessageClear`,
  `tuiClearChatMessages` exist in cpp_tui/tui_api.h; `TUI_API_VERSION` is currently 11 and documented as
  documentation-only; ChatMessageParam path is real; the v5 entry points in section 4 match the
  doc/todo_modern_ui.md sketch (tuiSetTurns/tuiTurnUpsert/tuiBlockUpsert/tuiTurnRemove/tuiThreadClear).
- D-side facts check out: `processChatMessage` in source/llm/app_agent/package.d is the single formatter with
  the `isDisplayable` filter; TuiSink has exactly the three sinks (channel/blocked/recording) the design relies on;
  `emitStepToolEvents`/`emitToolCallsFromJson` exist for the pipeline path.
- C++ facts check out: buildRenderGroups exists; group ids are adapter-assigned monotonic (D-GROUPID) with
  renumbering only on full rebuild; renderAgentTranscript binds a per-agent ChatTab through ThreadStoreAdapter;
  kStreamUpdateIntervalMs throttling exists; module-level anchors only (no file:line) per house convention.

## 2. Strengths

- S1 Full appendix traceability. Every appendix design decision maps to a numbered section and appears as an
  explicit "Design decision:" statement. All the appendix's rejected alternatives are respected in the design:
  no dual-source window (N1), no per-tool execution-start event (C4), no task_done special-casing, no spinner,
  no count chip, no key/palette shortcut, no red in minimal mode.
- S2 Accurate "What dies" list. The deletion inventory (sentence formatting, `[nr]` in sentences, `--- Tool ---`
  splicing, JSON dump in stream bodies, splitSummary, adjacency pairing, marker stripping, ChatMessage store) matches
  what the code actually contains. Nothing mythical is scheduled for deletion.
- S3 Honest status machine. Riding stream-done -> Running, result -> Ok/Failed, stop -> Cancelled adds no new ABI
  event and still yields a truthful UI, including the new Cancelled value that today's model (None/Ok/Failed) cannot express.
- S4 C-side step-count computation (finished call = 2, live call = 1, prose = 1) exactly matches the appendix's
  message-count definition and avoids duplicating displayability logic on the D side.
- S5 Commit shape is realistic: additive commit (both sides build, nothing carried), then ONE cutover commit flipping
  emitter+store+drill-down and deleting v4 on both sides; C++ store is dual-run (v4 live until P1-4) so P0-2 is
  independently testable. The console formatter is built in P0-3 behind the recording sink and wired at cutover.
- S6 Console parity is taken seriously: byte-identical output via a sink-side formatter with a golden text test, so the
  appendix's "one-shot console path is unaffected" holds at the output level even though the shared formatter dies.
- S7 Testing strategy covers the wire (layout pins both sides), the store, the emitter ordering (recording sink), the
  presentation (golden frames per mode, mid-batch folding, failed-in-expanded-group, ordinal+step-count together), replay
  parity, and the 1000-message perf case. Task acceptance criteria are concrete and dependency-ordered.
- S8 The TuiBlockStatus extension (None for prose/answer, Cancelled for post-stop in-flight) fixes a real defect in the
  doc/todo_modern_ui.md sketch (which would paint a green check on prose).

## 3. Weaknesses, risks, and missing elements

### High — should be resolved in the design before implementation planning

- W1 Displayability-filter ownership is ambiguous. Section 5 says step counts are computed C-side "over the same
  displayability filter as today" and that hidden tools stay "hidden (and uncounted)", but the design never states WHERE
  the filter lives in v5: does D emit blocks for hidden tools (and the C++ store filters at render/weight time), or does D
  not emit them at all? This decides three things at once: (a) whether "a result for an unknown callId ... renders when the
  call lands" can actually happen for a hidden tool (if D never emits the call, the stored result never renders — which is
  correct, but the section 7 wording "never silently dropped" then reads as contradicted); (b) whether the store can assume every
  upserted block is displayable; (c) what the console formatter must filter. Pick one owner (recommended: D emits everything it has,
  the store/renderer filters — one filter, C-side, next to the weights) and state it in section 4 or 5.
- W2 Eviction granularity is unspecified for v5. N2 says "capped stored messages, FIFO eviction, height-hint eviction" and
  P0-2 says "bounded window + FIFO + height-hint eviction", but the unit is undefined: blocks or turns? Evicting blocks out of a
  live turn mid-stream yields a turn header with a wrong "N steps" and a chips line referencing evicted steps; evicting a turn the
  emitter is still upserting into fights the upsert merge. Specify: eviction operates on whole turns (oldest first, never the live turn),
  and the height-hint budget is evaluated per rendered row as today. One sentence in section 4 (data model) and one in P0-2's acceptance.
- W3 Per-group overrides keyed by "group id" are not guaranteed stable across the events this design itself introduces. Today's
  group ids are adapter-assigned monotonic ids (D-GROUPID) derived from the first output line of the group, with renumbering on full rebuild.
  In v5, groups are turns; if the v5 group id is NOT pinned to the persisted turnId, a compression re-emission (section 7) or a store rebuild can
  renumber groups and silently orphan the session-local override map (F7) and the expansion latches. State it as an invariant: "the v5 group id is the
  persisted turnId; overrides and expansion state keyed by it survive upserts, re-emission, and rebuilds within a session" (they already die on session switch per F7).

### Medium — clarify or add a line

- W4 Interrupted turns on replay are unspecified. Section 7 covers stop mid-run (Cancelled) and session switch, but not the replay of a
  persisted turn that was interrupted while a call was in flight: does its call block emit as Cancelled (recommended — honest), as Failed, or not at
  all? Turn state has Interrupted; block statuses on replay come from stored results, and an in-flight call has none. One line in section 5 (replay) plus one
  emission-order test.
- W5 Store topology is implied but not stated. The design says "the C++ side replaces the v4 output-line store with a typed store" (singular) and separately that
  drill-down renders "a per-agent typed store". In reality a ChatTab exists both for the main thread and per pipeline agent (state.left.agents). State the rule once: wherever a
  ChatTab exists today there is a typed store tomorrow; the ABI entry points are scoped by the existing handle (main) or agentId (pipeline); the lane mirror and Agents tab remain mirror-fed.
- W6 tuiTurnRemove block-cascade is implicit. Blocks are keyed by (turnId, block id) and there is no tuiBlockRemove, so turn removal must cascade to its blocks. That is clearly the intent ("no stale group survives with
  orphaned block ids") but should be stated as a store invariant in section 4 so P0-2's unit tests pin it.
- W7 Live tool-card header has no duration. Maximal cards are "TOOL <name> - <duration>", but a live card (Pending/Running) has no processTime yet. Specify the live header (e.g. "TOOL <name>" until the result lands) so golden frames for
  live batches are deterministic. Same for tokensIn/tokensOut: per-block token numbers may not exist in D's data; state "0/absent = field not rendered".
- W8 task_done chip label vs "not special-cased" tension. Section 5 says task_done is a tool step like any other; section 6's minimal chips line is "tool names only", yet the streamed answer shows a `[task_done]` chip. The chip label is
  presentation derived from the block (meta/title), not D-side special-casing — say exactly that in one sentence, or the implementer will either special-case in D or drop the label.
- W9 Missing tests for three section-7 edge cases. The test matrix (section 8, P2-8) has no test for: (a) a result arriving for an unknown/hidden callId on the live path; (b) session switch mid-run (store rebuilt from snapshot, emitter
  re-attaches by turnId); (c) replay of an interrupted turn (W4). Add all three to P2-8; (a) also needs a store unit test (result under unknown callId is stored, not dropped).
- W10 The doc sweep misses doc/tui_design.md. P3-9 covers doc/todo_modern_ui.md and doc/todo.md, but doc/tui_design.md documents the C API boundary including ChatMessageParam and the v4 chat path; deleting the v4 message path makes it stale in the same
  iteration. Add it (plus the D-side binding headers' doc comments, if any) to P3-9.

### Low — polish

- W11 P0-3 vs P1-4 console-formatter wording: P0-3's acceptance includes "console parity golden text" while section 9 says "the console formatter lands here" (cutover). Both are consistent (built early behind the recording sink, wired at cutover) but
  P1-4 should say "console formatter wired" to avoid reading as a duplicate.
- W12 Section 5's "Blocks never stream in as new rows mid-turn: they upsert into slots the store already knows, or the store learns them as they first appear" is self-undermining as written (a first upsert IS a new row). Rewrite as: "upsert is the only mutating path;
  a first upsert creates the slot, every later upsert merges in place."
- W13 The appendix says the one-shot console path is "unaffected" while the design correctly moves its formatter to the sink. The design is right; add half a sentence in section 6 noting the appendix statement refers to output, not code, to preempt a review
  ping.
- W14 batchId is a string with a derivation ("batch index within the turn") given in prose; give the exact format (e.g. "<turnId>#<batchIndex>") in section 4 so replay determinism is testable, and note that a batch with no tool calls (pure prose response) still exists as
  thinking+content sharing that batchId.

## 4. Concrete suggestions (actionable)

1. Add one "Design decision:" line to section 4 or 5: the displayability filter is C-side (store weights + rendering); D emits every block it has, including hidden tools' calls/results; "never silently dropped" then means stored, with rendering governed by the filter (fixes W1).
2. Add to section 4: eviction is per whole turn, oldest first, never the live turn; height-hint budget unchanged (fixes W2); the v5 group id is the persisted turnId (fixes W3); tuiTurnRemove cascades to the turn's blocks (fixes W6); batchId format "<turnId>#<batchIndex>", recomputed deterministically on replay (fixes W14).
3. Add to section 5 (replay): an in-flight call of an interrupted turn replays as Cancelled (fixes W4).
4. Add to section 6: live maximal card header is "TOOL <name>" until durationMs exists; tokensIn/tokensOut render only when non-zero; the `[task_done]` chip label comes from the block's meta, set by presentation, not by a D-side special case (fixes W7, W8).
5. Add to section 4 (ABI scope): one typed store per ChatTab equivalent — main thread plus one per pipeline agent, pipeline entry points scoped by agentId (fixes W5).
6. Extend P2-8 with: result-for-unknown-callId (store + render), session switch mid-run, replay of an interrupted turn (fixes W9). Extend P3-9 with doc/tui_design.md (fixes W10).
7. Reword section 5's "never stream in as new rows" sentence and P1-4's console line (fixes W11, W12); add the appendix "unaffected = output" half-sentence to section 6 (fixes W13).
8. After fixes 1–4 land, re-check that section 8's store test list gains the three invariants (eviction granularity, group id = turnId, remove-cascade) so the implementation plan can turn each into a unit test directly.

## 5. Dimensions scorecard

- Requirements clarity: strong (F1–F10 map 1:1 to the appendix; constraints C1–C4 explicit). The only clarity gap is filter ownership (W1).
- Architecture: strong. The typed-store + adapter-seam + projection design preserves every seam that exists (ThreadStoreAdapter, lane mirror, inspector log ring) and the emitter adds no new execution hooks.
- Scalability/perf: good on paper (store lookup, upsert merge, C-side throttle reused, 1000-message test); eviction granularity is the one unspecified scaling behavior (W2).
- Reliability: good — status machine is honest, Cancelled added, upsert storms addressed; interrupted-turn replay (W4) and unknown-callId live path tests (W9) missing.
- Security/privacy: not materially applicable to a local TUI wire (flat C ABI, no injection surface change: bodies are data, rendered as markdown as today). No action.
- Cost-efficiency: good — the design deletes more code than it adds on the C++ side (text archaeology, adjacency pairing, marker stripping) and reuses throttling and bounded-window patterns.
- Maintainability: good — module-level anchors only, explicit "Design decision:" lines, "What dies" list; console formatter duplication is a known, golden-guarded cost.
- Documentation quality: strong overall; one stale-doc miss (tui_design.md, W10) and two wording nits (W12, W13).
- Risk: the single-commit cutover is the biggest execution risk (no fallback path by design, per N1). Mitigation already in the plan: P0-2 dual-runs the C++ store and P0-3 pre-builds the emitter behind the recording sink, so the cutover commit is mechanical. Acceptable.

## 6. Summary judgment

The design faithfully implements the appendix, kills v4 cleanly, and its code survey matches reality. None of the findings invalidate the architecture; W1–W3 are spec decisions the designer should write down now (each is one or two sentences), W4–W10 are clarifications and test/doc additions, W11–W14 are polish. With those applied, proceed to the implementation plan.
