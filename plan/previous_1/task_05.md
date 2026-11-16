## Task 5: Cutover — flip to the typed path, delete the v4 message path (design P1-4)

Priority: P1. Dependencies: tasks 2, 3, 4. **ONE commit, both sides** (design §9.2):
parts A and B land together. Same iteration as task 1 (appendix N1).

### Description
Flip every live path to the v5 typed events/store and delete the v4 message
path on both sides. After this commit the app runs entirely on typed
turns/blocks; rendering is unchanged except the one-time F8 delta (D11).

### Changes Made — part A (wiring)
- D side (llmfun/source/llm/app_agent/package.d): the live run path calls the
  typed emitters — query echo -> tuiTurnUpsert(Running) + query prose block;
  the stream path constructs the typed stream updater (task 3); the
  step-completion walk emits merged tool blocks + the next batch + the Answer
  block; stop -> Cancelled upserts + turn Interrupted; compression (doCompress
  252-272) re-emits affected turns with recomputed ordinals.
- D side: activateSession/switchToSession (377+, 448+) call emitReplayTyped ->
  ONE tuiSetTurns snapshot; the live emitter re-attaches by turnId (§7).
- C side (tui_api.cpp): the pipeline entry points (tui_api.h:685-699) KEEP
  their signatures; their impls retarget onto the addressed agent's typed
  store (design §4 decision) — the drill-down transcript (renderAgentTranscript
  175-232) renders through the adapter seam from the per-agent typed store.
  The lane mirror + Agents tab consume the mirror, untouched (§6).
- C side: the adapter's source selector flips to the typed-backed projection
  (task 2); the console blocked sink writes the ConsoleFormatter lines (task 4).
- D side console: the blocked sink's v5 methods become the live writes.

### Changes Made — part B (deletion, same commit)
- D: processChatMessage (274-304), sendPersistedChatMessage/ThinkMessage/
  FinalAnswer (221-239), StreamMessageUpdater's `--- Tool ---` splicing
  (528-549), the v4 addChatMessage head~body~tail + streamChat splice
  (tui/package.d:359-379, 421-427), adjacency pairing + marker stripping.
  isDisplayable/displayableCount (1051-1060): delete the TUI-path call sites;
  keep the functions only if non-TUI callers remain (grep first, then decide;
  the D-side displayability knowledge otherwise dies — the filter is C-side).
- C: ChatMessageParam + the v4 chat entry points (tui_api.h:101-119, 642-672;
  tui_api.cpp:565-582 impls); ChatMessage (tui.h:85-97); the ChatTab output-line
  store (outputLines/outputLineNextId/MaxChatMessages 255-257); buildRenderGroups
  adjacency pairing (thread_store.cpp:372-448) + splitSummary; the v4 stream
  body keys; the adapter's v4-backed path + selector.
- test_tui_api_v4.cpp: delete the v4 chat/packing cases (incl. the ChatMessageParam
  case 1f); task 1's v5 pins stay.

### Verification
- [ ] One commit: flip + delete together (git show --stat reviewed).
- [ ] No reference to any removed symbol either side (grep the deleted names).
- [ ] Both builds green; `dub test` green; all cpp_tui test binaries green
      (known pre-existing reds excluded — overview).
- [ ] Existing goldens pass on the typed store with a ONE-TIME re-baseline whose
      delta is exactly D11 (the F8 ordinal text change + the thinking-attachment
      shape). ANY other delta = a projection bug: fix the projection, not the
      golden.
- [ ] Console output byte-identical (task 4's golden now runs on the live path).
- [ ] The thread view renders today's row shape through the typed projection
      (mode work = tasks 6-8, AFTER this commit).

### Notes
- D11 (overview): the acceptance "existing goldens pass on the typed store" is
  satisfied with the one-time F8 re-baseline; the delta must be mechanically
  reviewable (per-row: the [nr] message number -> the turn's ordinal; the
  thinking expansion now sourced from the batch's Reasoning block).
- The pipeline entry points survive with their structured shape (§4) — only
  their C-side targets change. Do NOT delete them.
- stop/cancel: in-flight -> Cancelled is emitter-driven (task 3); the C-side
  cancel path feeds the emitter exactly as today's cancel recording works
  (verify the current stop wiring before touching it).
- Decision letters used: D11 (overview).
