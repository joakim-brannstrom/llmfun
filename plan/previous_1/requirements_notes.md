# Requirements notes — Thread rendering v5 (typed turns and blocks, two modes, tool presentation)

Source: plan/system_design.md (rev with W1-W14 fixes applied) + plan/appendix.md +
plan/design_review.md + plan/design_notes.md. Implementation plan maps 1:1 to the
design's task list P0-1..P3-9 (the design's section 10 tasks become the impl tasks).

## Scope

Replace the v4 message-path ABI (formatted chat sentences) with typed turns and
blocks end to end; kill v4 outright in the same iteration (no dual-source window,
no legacy path). Two render modes on the C++ side (minimal default / maximal) with
per-group session-local override. Console one-shot output stays byte-identical
(formatter rebuilt over typed events, sink-side).

## Functional requirements (F1-F10, from design section 2)

- F1 typed thread events over the ABI; store upserts keyed (turnId, block id); no sentence parsing.
- F2 tool step = one unit per toolCallId (call + result); Pending->Running->Ok/Failed/Cancelled; replay emits merged blocks.
- F3 work group per turn, header "WORK - <N steps>"; N computed C-side from store weights (finished tool = 2, live call = 1, prose = 1) over the C-side displayability filter; D emits every block it has.
- F4 minimal mode: user/answer blocks with bodies; work groups header-only; newest live group adds one chips line (names only, one chip per live step, Running green, Pending dim, fold on dormancy mid-batch included); streamed answer = `[task_done]` chip only until result.
- F5 maximal mode: batches (thinking disclosure collapsed, meta "N tool calls"; content block; tool cards "TOOL <name> [- <duration>]" — live header has no duration until durationMs exists; args as body; Result open by default; tokensIn/tokensOut only when non-zero; cards indented under the batch's thinking behind a shared left rule; failed red inside expanded groups only).
- F6 mode `minimal`|`maximum` in ui.conf via settings sheet row (behaviour section); minimal factory default; no shortcut/palette command.
- F7 per-group override session-local, keyed by the group's persisted turn id; beats the mode; dies on session switch.
- F8 one ordinal per turn (1-based among displayed turns); steps/thinking/results consume none.
- F9 console output byte-identical to today (same lines, same emoji).
- F10 replay parity: identical thread rendered twice; expansion state preserved.

## Non-functional / constraints

- N1 both sides agree per commit; v4 payload dies in the same iteration.
- N2 bounded memory: capped store, eviction per whole turn (oldest first, never the live turn), height-hint eviction, no array-of-everything rendering.
- N3 honesty: nothing renders that did not come from the run; stop/cancel never invents completed state (Cancelled is real).
- N4 1000-message sessions open/scroll without per-frame re-scan.
- C1 flat C wire, layout-pinned structs (D static asserts + C++ offset pins per existing contract test; note: user decision abi_seam_no_pins — no NEW size/offset asserts at the seam beyond the existing contract-test pin pattern; keep the TUI_API_VERSION pin).
- C2 stream tick cadence stays C-side ~20 Hz (kStreamUpdateIntervalMs); D keeps sending deltas.
- C3 now bar, chip strip, logs, runtime status, agents-tab lanes: unchanged surfaces.
- C4 no new per-tool execution-start event; Pending->Running rides existing stream-done; Ok/Failed rides result upsert; Cancelled rides stop.

## Key data-model facts (from design section 4; exact, must appear in tasks)

- TuiTurn { id = persisted turnId, ordinal (1-based display position), state Running|Done|Interrupted, messageCount (informational) }.
- TuiBlock { id (tool callId | "answer" | batch-scoped ids for thinking/content), batchId = "<turnId>#<batchIndex>" (recomputed deterministically on replay; stable within turn lifetime in one run; not persisted; tool-less batches carry one too), kind Prose|Reasoning|Tool|Answer|Notice, status None|Pending|Running|Ok|Failed|Cancelled, flags Collapsible|DefaultOpen|Selectable, title, meta, body, durationMs, tokensIn, tokensOut }.
- Entry points: tuiSetTurns (bulk snapshot), tuiTurnUpsert, tuiBlockUpsert (only mutating path; first upsert creates slot, later upserts merge in place), tuiTurnRemove (cascades to the turn's blocks), tuiThreadClear. Session actions/runtime status/chips/logs/query-history entry points unchanged from v4.
- Store topology: one typed store per v4 ChatTab equivalent (main thread + one per pipeline agent); pipeline entry points scoped by agentId; lane mirror + Agents tab consume the mirror.
- Group id = persisted turnId (overrides/expansion survive upserts, re-emission, rebuilds within a session).
- Eviction: per whole turn, oldest first, never the live turn; height-hint budget evaluated per rendered row as today.
- Displayability filter lives C-side next to the step-count weights; store never assumes a stored block is displayable; console formatter applies the same filter.
- Replay of an interrupted turn: in-flight calls emit as Cancelled (honest, not fabricated Ok).
- Result for unknown callId (live): recorded and merged when the call lands; never silently dropped.
- task_done: not special-cased; chip label from block meta set by presentation.

## Design-to-code mapping (from design_notes verified facts + review section 1)

- D side: source/llm/app_agent/processChatMessage = single v4 formatter (dies); StreamMessageUpdater splices `--- Tool ---` into stream bodies (dies); emitStepToolEvents/emitToolCallsFromJson = existing pipeline structured path (retarget onto typed store, scoped by agentId). chat.d Message/ToolMessage/ToolResponse + turnId persistence; agent.d per-step processResult walks history tail; activateSession/switchToSession replay. ui/messenger.d sinks: channel (live TUI), blocked (one-shot console), recording (tests).
- C++ side: cpp_tui/tui_api.h v4 chat path (tuiAddChatMessage / tuiUpdateStreamChatMessage / tuiStreamChatMessageClear / tuiClearChatMessages) dies; tuiInitQueryHistory stays. Pipeline path tuiPipelineAgentUpdate/ToolCall/ToolResult retargets. ChatTab.outputLines + streamMsg/pendingStreamMsg die; kMaxChatMessages=1000. store/thread_store.cpp buildRenderGroups adjacency pairing + splitSummary die. BlockEntry/BlockKind/BlockState{None,Ok,Failed} in model/block.h. views/thread.cpp renders from adapter; ThreadStoreAdapter seam kept. views/settings.* sheet rows; ShellPrefs persisted keys; fresh keys default. Inspector Activity = log ring, untouched. Stream throttle ~20 Hz exists. test/golden/*.txt grid snapshots + headless harness = test infra. TUI_API_VERSION currently 11, doc-only.

## Acceptance criteria (global)

- Contract tests pin layout/offsets for new structs (D static asserts + C++ offset pins, pattern of existing v4 contract test case 1f); both sides build per commit.
- Store unit tests: upsert merge, status machine, batch grouping, step-count weights, whole-turn eviction (oldest first, never live), remove cascade, group id = turnId stable across re-emission/rebuild, unknown-callId result recorded+merged, clear.
- Emission-order tests on recording sink: replay snapshot vs live sequence, merged replay blocks, Pending->Running->Ok ordering, answer-on-done, interrupted-turn replay (Cancelled), session switch mid-run.
- Console parity golden text (byte-identical to today's output).
- Golden frames: both modes; live folding mid-batch; failed step in expanded group; ordinal+step count together; replay parity; 1000-message open/scroll.
- Stop mid-batch -> Cancelled renders honestly.
- Full suites green (D: dub test; C++: cpp_tui binaries) after every task.

## Out of scope

Session list/now bar/chip strip/log/runtime-status v5 models; cross-turn search; tuiMoveCall; agents-tab lane internals; inspector internals; key shortcut/palette command; config-selectable legacy path; theme/color configurability; approval cards; keymap changes.
