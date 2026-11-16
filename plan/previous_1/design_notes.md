# design_notes - checkpoint for system_design.md (thread v5 / tool presentation)

Source: plan/appendix.md (the "tool presentation" appendix). Convention from memory
(plan_appendix_style, appendix_f_system_design_done): appendix = high-level DESIGN doc,
this node produces plan/system_design.md = design + prioritized task list P0..P3,
module-level anchors only (no file:line), explicit "Design decision:" statements,
acceptance criteria per task, dependency notes.

## Verified code facts (all checked this session, paths relative to llmfun/)

D side (source/llm):
- chat.d: Message{content,thinking,role,toolCalls}, ToolMessage{toolCalls+thinking,
  isFinalAnswer via task_done args}, ToolResponse{toolCallId,content,ok,processTime};
  every persisted message carries turnId (TurnId stamps at persistence, one per user query,
  reused for compress summaries).
- agent.d (LlmAgent): per-step processResult walks the history tail (getMessages +
  result.chat) and emits it; runAgent emits the turn's user query first;
  activateSession/switchToSession replay full history per session.
- app_agent.d: processChatMessage is the single formatter for both TUI and one-shot
  console paths; StreamMessageUpdater splices ordered tool calls into the streamed
  body on every chunk; emitStepToolEvents/emitToolCallsFromJson already emit
  structured per-agent tool call/result rows for the pipeline (agents) path.
- utility.d: summarizeToolResponse / summarizeToolCalls produce the one-line
  summaries; formatToolArguments pretty-JSON.
- ui/messenger.d (UiMessenger): sinks = channel (live TUI), blocked (one-shot
  console writeln), recording (tests). Now bar phase line separate surface.

C++ side (cpp_tui):
- tui_api.h/.cpp v4: ChatMessageParam path = tuiAddChatMessage / tuiUpdateStreamChatMessage /
  tuiStreamChatMessageClear / tuiClearChatMessages / tuiInitQueryHistory (keep).
  Pipeline path = tuiPipelineAgentUpdate/ToolCall/ToolResult (structured, per-agent).
  Message layout is a pinned wire contract (D static asserts, test_tui_api_v4 case 1f).
- tui.h ChatMessage fields: text/thinking/toolName/toolArgs/toolResult/toolOk/kind;
  kMaxChatMessages=1000; ChatTab.outputLines + streamMsg/pendingStreamMsg.
- store/thread_store.cpp: buildRenderGroups pairs ToolCall+ToolResult by ADJACENCY,
  splits "[nr] body [time]" summaries (splitSummary), work group step count =
  displayed messages; openWorkGroups/outputLineOpen = expansion sets;
  BlockEntry/BlockKind/BlockState{None,Ok,Failed} in model/block.h.
- views/thread.cpp renders groups from adapter; views/settings.* sheet rows;
  ShellPrefs persisted ui.conf keys Version/SendKey/UiLog/RailOpen/InspectorOpen/
  InspectorTab/WindowWidth/WindowHeight/FontScaleMain; fresh keys load at defaults.
- Agent drill-down (renderAgentTranscript) renders per-agent ChatTab through the
  same ThreadStoreAdapter seam -> if v4 payload dies, drill-down must move to the
  typed store too. Lane mirror + Agents tab consume the mirror, not the store.
- Inspector Activity tab = log ring (projectForActivity), not chat messages -> untouched.
- Stream throttling to ~20 Hz already exists C-side (kStreamUpdateIntervalMs, task 38).
- test/golden/*.txt grid snapshots + headless harness pattern is the test infrastructure.

## doc/todo_modern_ui.md section 8 (read this session) - the v5 sketch being pulled forward

8.2 thread store v5: TuiTurn{id,ordinal,state,blocks}; TuiBlock{id,kind,status,flags,
title,meta,body,durationMs,tokensIn,tokensOut,batch?}; kinds Prose/Reasoning/Tool(+Answer/Notice
render kinds); status Pending/Running/Ok/Error/Cancelled; functions tuiSetTurns/tuiTurnUpsert/
tuiBlockUpsert/tuiTurnRemove/tuiThreadClear/tuiSetRuntimeStatus. Thread view = projection
of the store. Appendix narrows scope to the THREAD part only: no session list, no now bar,
no chip strip, no logs v5, no runtime status v5, no cross-turn search index, no tuiMoveCall.

## Design decisions I resolved (appendix + my resolution; must appear as "Design decision:" lines)

1. Batch identity: TuiBlock carries a batchId string; D numbers batches per turn (per model
   response), recomputes deterministically on replay; thinking/content/tool blocks of one
   response share it. Not persisted; stable within the turn's lifetime in one process run.
2. Status enum gains None (prose/answer/user blocks carry no outcome -> no green check)
   plus Cancelled for in-flight calls after a stop; tool steps use Pending/Running/Ok/Failed.
3. Agent drill-down migrates to typed turns in the same iteration (v4 ChatMessage payload dies
   entirely); pipeline emitters emit typed events scoped by agentId; lane mirror + Agents tab
   untouched; inspector Activity untouched.
4. One-shot console path keeps today's byte-identical output via a sink-side formatter over
   typed events (TuiBlockedSink renders turns/blocks to the same sentence lines incl. emoji).
5. Live status machine reuses existing events: Pending->Running rides existing stream-done,
   Running->Ok/Failed rides the result upsert; no new execution-start ABI event.
6. Work group step count computed C-side from store weights (finished tool=2, live call=1,
   prose=1), respecting today's displayability filter; D does not send the count.
7. Ordinal = 1-based position among displayed turns, D-computed; turnId stays the persisted key.
8. Modes: minimal (default) renders user/answer blocks with bodies, work groups header-only
   ("WORK - N steps") + one chips line (names only; Running green, Pending dim; fold on
   dormancy) under the newest live group; streamed answer = [task_done] chip only until done.
   Maximal renders batches: thinking disclosure (meta "N tool calls"), content block, tool
   cards ("TOOL <name> - <duration>", call args as body, Result open by default), cards indented
   under the batch sharing a left rule, failed red only inside expanded groups.
9. Mode lives in ui.conf (ShellPrefs field + settings sheet row, behaviour section); per-group
   override session-local in UiState keyed by group id, override beats mode, fresh session resets.
10. D emits everything mode-independently; C++ projects by mode; inspector sees full feed always.
11. Replay emits already-merged tool blocks (D owns call+result); only live streaming is
    call-then-result; replay parity = same session renders identically twice, expansion survives.
12. Cutover commit shape: additive ABI commit first, then ONE commit flipping D emitter + C++
    store + deleting the v4 path (both sides agree per commit; no dual-source window).
13. task_done call: no special casing - a tool step like any other while streaming.

## Task list (dependency-ordered)

P0-1 v5 types + ABI surface (additive; version bump; D static asserts; round-trip packing tests).
P0-2 C++ typed thread store + adapter rewrite over same seam (upsert merge by callId, status
     machine, bounded window + FIFO eviction w/ height hints, batch grouping, step count).
P0-3 D-side typed emitter (replay snapshot + live call-then-result + turn upserts) + console
     sink formatter; recording-sink emission-order tests.
P1-4 Cutover commit: flip emitter+store, delete v4 message path both sides (sentences to TUI,
     stream-body tool splicing, adjacency pairing, marker stripping, ChatTab store), drill-down
     to typed turns. Build green, no v4 symbol refs, goldens pass on typed store.
P1-5 Minimal mode rendering + live chips folding (goldens: idle thread, live batch mixed
     statuses, post-completion fold).
P1-6 Maximal mode rendering (goldens: batch anatomy, failed step in expanded group,
     tool card + duration).
P1-7 Settings row + ShellPrefs key + per-group override (sheet cycles words; override
     session-local; persistence round-trip).
P2-8 Cross-cutting battery: replay parity, 1000-message perf, upsert storm, stop mid-batch
     (Cancelled), compression re-emission.
P3-9 Doc sweep: doc/todo_modern_ui.md section 8 note + doc/todo.md L30/L31 delivered notes.

Out of scope (appendix): session list/now bar/chips/logs/runtime-status v5, cross-turn search,
tuiMoveCall, agents tab lane internals, inspector internals, keybinding/palette additions.

## Review round 1 (design_review.md)

Verdict: APPROVE WITH FIXES. All fixes applied to system_design.md before the implementation-plan step.

Applied (weakness -> fix): W1 displayability filter owned C-side (store never assumes displayable; render/weight-time filter; console formatter applies the same filter). W2 eviction per whole turn, oldest first, never the live turn. W3 group id = persisted turnId (overrides/expansion survive upserts, re-emission, rebuilds). W4 interrupted-turn replay emits in-flight calls as Cancelled. W5 store topology: one typed store per v4 ChatTab equivalent; pipeline entry points scoped by agentId. W6 tuiTurnRemove cascades to the turn's blocks. W7 live tool card header shows "TOOL <name>" until durationMs exists; non-zero tokens render, zero/absent do not. W8 [task_done] chip label comes from block meta via the presentation mapping, not a special case. W9 new tests: unknown-callId, session switch mid-run, interrupted-turn replay, remove-cascade/group-id invariants. W10 doc sweep extended to doc/tui_design.md (C API boundary section). W11 console formatter wording: built in P0-3, wired at cutover. W12 live path reworded as upsert-only (no remove/steps subtractions). W13 console "unaffected" clarified as output-identical, not code-untouched. W14 batchId exact format "<turnId>#<batchIndex>", stable within turn lifetime, not persisted; tool-less batches carry one too.

Also: per-group overrides keyed by the group's persisted turn id; stop mid-batch -> Cancelled added to the P2-8 battery; task list P0-2/P0-3/P1-4/P1-7/P2-8/P3-9 updated to carry the fixes and their acceptance criteria.
