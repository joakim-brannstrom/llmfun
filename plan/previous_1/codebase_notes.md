# Codebase notes — thread rendering v5 (anchors verified 2026-01-06 session)

All paths repo-relative from /workarea (prefix `llmfun/`). grep_files is flaky
(memory: grep_files_transient_failures) — re-grep on empty error. execute_command
env `llmfun` was DOWN this session (docker image missing); anchors came from
read_file/grep_files + RAG. The executor must re-verify anchors per the standing rules.

## D side (source/llm)

- llmfun/source/llmfun_tui.c: the only file that includes cpp_tui/tui_api.h; D imports
  C types via `import llmfun_tui;` (source/llm/tui/package.d:18). New v5 C types are
  visible D-side automatically — no D struct re-declaration.
- llmfun/source/llm/tui/package.d: ABI wrapper class (owns TuiState*): addChatMessage
  359-379 (tuiAddChatMessage at 378), clearChat 381-383, streamChat 421-427
  (tuiUpdateStreamChatMessage 425), streamChatDone 429-431, pipelineMessage overloads
  433-448 (tuiPipelineAgentUpdate/Done). D-side static assert patterns: TuiLogRecord
  pins 249-262, TuiRuntimeStatus.sizeof 579. toTuiString 21-23, shortSummary 37-63,
  timeSuffix 68-76 (console line formatting helpers that survive).
- llmfun/source/llm/app_agent/ui.d: TuiSink interface 25-49; TuiChannelSink 53-147
  (Channel!TUICommands forwards, uiMsg(Ui...) events); TuiBlockedSink 150-221
  (chatMessage writeln 164-166, chatThinkMessage 168-173 = "Thinking: ..."+line,
  finalAnswer 178-180 writeln; stream/status/pipeline no-ops); TuiRecordingSink 225+
  (Call 226-231, chatMessages 239, stamps 240-241, laneUpdates 248-258, toolCalls
  262-269, toolResults ~272+); UiMessenger 404-513 (chatMessage 431-437,
  chatThinkMessage 439-446, finalAnswer 452-458, clearChat 460-462,
  streamChatMessage 472-474, streamChatDone 476-478, pipelineToolCall 490-492,
  pipelineToolResult 494-496, sessionList 502-504, initHistory 506-508);
  StreamMessageUpdater 516-557 (messageUpdate 528-545: splice `\n--- Tool ---\n`
  at 538-541 = DIES; streamMessageDone 547-549); truncatedForWire 564-571 (1000-byte
  wire cap, UTF-8 safe — keep, reuse for block bodies); emitToolCallsFromJson 585-600;
  emitStepToolEvents 607-616; PipelineStreamMessageUpdater 619-656 (stepEvent 645-647).
- llmfun/source/llm/app_agent/package.d: sendPersistedChatMessage 221-227,
  sendPersistedChatThinkMessage 229-235, sendPersistedFinalAnswer 237-239,
  progressCallback 241-244 (compression "Compressing..." message), doCompress 252-272
  (chatDirty when history rewritten), processChatMessage 274-304 (the single v4
  formatter: Message 275-284 "%s: %s", ToolMessage 285-293 "tool call: %(%-s\n%)" +
  finalAnswer same nr, ToolResponse 294-299 "tool result %s: %-s %s" ✅/❌,
  VisionMessage 300-303), processResult 306-329 (offset = displayableCount of
  history before the batch at 315; per-message walk 317-324; chatDirty 328-329),
  activateSession 377+ (replay walk calls processChatMessage at 418-420),
  switchToSession 448+, runToCompletion(&this.processResult) 897, user-query echo
  894-895 (sendPersistedChatMessage(query, TuiChatMessageType_User,
  displayableCount(...))), replay call sites 1199-1201, UiMessenger(new
  TuiChannelSink(tui_)) 1189, default TuiBlockedSink 107.
- llmfun/source/llm/app_agent/package.d: isDisplayable 1051-1060 (hidden tools =
  taskDone + pipelineOutput, LlmConfig), displayableCount 1059+ (both package-level;
  v5: D stops filtering for the TUI path but ordinals still mirror the displayed-
  turn rule; the C-side filter owns render/weight).
- llmfun/source/llm/chat.d: turnId stamps 89-124 (every persisted message carries
  turnId; 0 = system), turnIdOf 444-446, Message 697+, ToolMessage 818+,
  ToolResponse 948+ (toolCallId, toolName, ok, processTime), VisionMessage 585+,
  isFinalAnswer/taskDoneAnswer facts ~1789-1816 (unittest area).
- llmfun/source/llm/app_agent/tests.d: TuiRecordingSink usage pattern 769+, 2852-2925
  (PipelineStreamMessageUpdater + emitStepToolEvents tests), isDisplayable tests
  1814-1829, live-batch ordinal tests 1837-1900.
- llmfun/source/llm/config.d: hiddenTools = ["taskDone","pipelineOutput"] 423,
  isHiddenTool 691-705 (this is the displayability knowledge; v5 keeps the notion
  but the filter moves C-side for rendering/weights).

## C++ side (llmfun/cpp_tui)

- llmfun/cpp_tui/tui_api.h: TUI_API_VERSION 41-44 (currently 11; doc-only),
  ChatMessageParam 101-119, ownership notes 55-70 (strings inbound, non-owning),
  tuiInitQueryHistory 525 (KEEP), v4 chat decls: tuiAddChatMessage 642,
  tuiClearChatMessages 655, tuiUpdateStreamChatMessage 666,
  tuiStreamChatMessageClear 672; pipeline decls 685-699 (tuiPipelineAgentUpdate/
  ToolCall/ToolResult). v5 decls go near these.
- llmfun/cpp_tui/tui_api.cpp: stream throttle check 131 (kStreamUpdateIntervalMs),
  tuiAddChatMessage impl 565+, tuiClearChatMessages impl 582+. v5 impls forward
  into TuiState's typed store here.
- llmfun/cpp_tui/tui.h: ChatMessage 85-97 (summary/text/thinking/type/id/toolName/
  toolArgs/toolResult/toolOk — DIES at cutover), ChatTab 236-284 (outputLineOpen 244,
  openWorkGroups 248, expansionDefaultThroughId 254, outputLines 255,
  outputLineNextId 256, MaxChatMessages=1000 257, renderGroups 259-261,
  StreamChatMessageId 265, hasStreamMsg/streamMsg 270-271, pendingStreamMsg/
  pendingStream 282-283), AgentStream 287-298 (per-agent ChatTab + streamLastApplyMs),
  ChatTabLeftPanel 304-307 (agents vector, MaxAgents=16), kAgentChatMaxMessages 311,
  drill-down adapter member ~501. GroupKind (v4 renderer enum) 164.
- llmfun/cpp_tui/store/thread_store.h: GroupKind 83, GroupView 93-99 (id/revision/
  oneLineSummary/lineCountEstimate), ThreadStoreAdapter 102+ (bind(TuiState&) 118,
  bind(ChatTab&,ThreadState&) 125, dropChat 131, groupCount 185, groupAt 189,
  hasStream 194, storedGroupCount 200, blockAt 216-219), block seam notes 202-209.
- llmfun/cpp_tui/store/thread_store.cpp: splitSummary 181-..., buildRenderGroups
  372-448 (adjacency pairing — DIES), expansion helpers 440-502, sync 602+,
  adapter bind/sync 574-666 (buildRenderGroups call 666), stream block keys
  744-801, 1391 (model::BlockId{groupId, kStreamMessageId, kind}).
- llmfun/cpp_tui/model/block.h: BlockKind 38+, BlockState 58+ (None/Ok/Failed —
  v5 needs None semantics kept + Cancelled added or mapped), BlockDesc 110-112.
- llmfun/cpp_tui/model/settings.h: kUiConfVersion=2 111, ShellPrefs 117-129,
  toggle helpers 131-135, uiConfText key list 140-143, loadUiConf 153,
  saveUiConf 157, flushUiConf 163. New mode key joins this set (bump kUiConfVersion
  or default-on-missing — existing fresh-keys-default behavior).
- llmfun/cpp_tui/views/settings.cpp: allRows 152-... (Appearance rows 156-181,
  Behaviour section starts 183), activate 495-535 (row.id-keyed Choice cycling),
  settingsSectionName 541-555 (kSettingsSectionBehaviour), settingsRows 557-581.
- llmfun/cpp_tui/views/thread.cpp: renderThreadBody 332+ (window/page arithmetic
  350-367, Load-earlier row 369-394), renderThreadState 70, renderAgentTranscript
  175-232 (drill-down, renders through the same adapter), renderThread 286-323,
  group/step row drawing ~470-570 (collapsed work group 546-563).
- llmfun/cpp_tui/model/ui_state.h: ThreadState 187+ (anchorGroupId 338 area,
  pageGroups/drawnGroups 346-349 consumers), ui.thread 406-409.
- llmfun/cpp_tui/model/thread_view.h: ThreadView enum 21+ (Loading/Ready/Offline/
  Empty), deriveThreadView 41 — NOT the v5 two-mode projection; untouched.
- llmfun/cpp_tui/model/stream_state.h: kStreamUpdateIntervalMs=33 41 (C-side
  throttle; design says ~20 Hz cadence stays).
- Tests: llmfun/cpp_tui/test_tui_api_v4.cpp (contract test; offset-pin block
  900-1022 pattern; case 1f = ChatMessageParam layout), test_blocks.cpp (goldenText
  1632, scene compare 1763/1812; goldens blocks-*-100x30), test_streaming.cpp
  (goldenText 947, compare 990; streaming-live/cancelled/fence/thinking/table
  goldens), test_thread_window.cpp (adapter move/drop pins), test/
  parity_core_loop.cpp (headless harness; tuiAddChatMessage use 573, clear 831;
  goldenText 1292). CMakeLists.txt: test binary registration pattern 839-844
  (test_thread_window), 937-942 (test_blocks, `--update` regolden flow).
  Goldens live in llmfun/cpp_tui/test/golden/*.txt.

## Conventions (from memories + repo)

- No size/offset static_asserts IN tui_api.h itself (user decision abi_seam_no_pins);
  pins live in the C++ contract test (test_tui_api_v4.cpp pattern) and D-side
  static asserts (source/llm/tui/package.d pattern). TUI_API_VERSION bump kept.
- Build/test: D `dub test` from llmfun/ (~15 s run; 400+ s = hang); C++ binaries
  build into llmfun/cpp_tui/build/ (cmake -S cpp_tui -B cpp_tui/build if missing);
  plain binaries run direct, *_gui need `TERM=xterm xvfb-run -a ./X --out .`;
  known pre-existing reds: parity_core_loop 2 perf-gate FAILs, test_metrics
  gate-ratchet FAIL — never re-baseline (memory workarea_and_test_pitfalls).
- workarea: /workarea is the tool workarea root; plan/ is /workarea/plan;
  read_file/edit_file reject ../ paths; use repo-relative llmfun/... paths.
- The design doc + appendix use module-level anchors only; THIS plan uses file:line
  anchors per the implementation-plan skill (executor re-verifies each).
