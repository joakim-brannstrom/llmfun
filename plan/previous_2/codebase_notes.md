# Codebase notes — verified anchors (Phase 2 survey, 2026-11-15)

Repo checked out at `llmfun/` in the workarea (llmfun/ = repo root). All anchors
below were read from the working tree this session (grep/read_file), not from memory.
Paths repo-relative under llmfun/.

## Wire + bindings (tasks 2, 4)

- `cpp_tui/tui_api.h:29` — `#define TUI_API_VERSION 12` (current generation).
- `cpp_tui/tui_api.h:655-663` — TuiTurn: id, ordinal(659), state, messageCount, reserved.
- `cpp_tui/tui_api.h:667-685` — TuiBlock: turnId,id,batchId,title,meta,body,durationMs,tokensIn(679),tokensOut(680),kind,status,flags,reserved; comment 670 "durationMs 0 = unknown; tokensIn/tokensOut 0 = not rendered".
- `cpp_tui/tui_api.h:689-694` — tuiSetTurns/tuiTurnUpsert/tuiBlockUpsert signatures.
- `cpp_tui/test_tui_api_v4.cpp:1147-1203` — caseThreadTurnBlockFields: TUI_API_VERSION==12 pins at 1029, 1159, 1657; TuiTurn 24B offsets 1163-1168; TuiBlock 128B offsets 1172-1186 (turnId 0, id 8, batchId 24, title 40, meta 56, body 72, durationMs 88, tokensIn 96, tokensOut 104, kind 112, status 116, flags 120, reserved 124); vocabulary pins 1190-1203. THIS is the offset-pin test pattern.
- `cpp_tui/test_clear_output_state.cpp:141` — another `TUI_API_VERSION == 12` pin (must bump with task 2 and again with task 4).
- `cpp_tui/test_tui_api_v4.cpp` comment ~1149-1156: "bump one version per ABI event" — the version discipline the design's C1 encodes.
- `source/llm/tui/package.d:36-114` — D conversions: toTurnData 45-47 (copies ordinal), toTuiTurn 50-57, toBlockData 60-64, toTuiBlock 68-83, toTuiTurns 86-91, toTuiBlocks 93-98, toTurnDataAll/toBlockDataAll 102-114. Header comment 38-42: String borrowing + C copies inbound.
- `source/llm/tui/package.d:1236` — `int ordinal;` in UiTurnData ("1-based display position, D-assigned"); `:1242` UiBlockData struct (fields mirror wire); `:1260-1268` UiSetTurns/UiBlockUpsert payloads.
- `source/llm/tui/package.d:46,53` — ordinal copies in the conversions (task 4 edits).
- `cpp_tui/tui_api.cpp:127,808` — `turn.ordinal = agent.typedOrdinal` (pipeline lane paths); `:851,886` — `t.ordinal = turn(s)->ordinal` in tuiSetTurns/tuiTurnUpsert copies (task 4); `:871-872,911-912` tokensIn/Out copies.
- `cpp_tui/tui_api.cpp:714-802` — C-side pipeline lane upserts (upsertTypedBlock call sites, Prose/Reasoning/Tool blocks with meta summarize lines).
- `cpp_tui/views/shell.cpp:705` — versions sheet string "tui api <TUI_API_VERSION>" (display-only, no change).

## D-side emitter + payloads (tasks 1, 2, 3, 4)

- `source/llm/app_agent/package.d:66` comment; `:110-111` replay walk ordinal assign (`nextOrdinal++` into UiTurnData); `:134,153-157` replay summarize meta (summarizeToolCalls/summarizeToolResponse 1000 caps).
- `source/llm/app_agent/package.d:986-995` — live turn open: typedCtx_.ordinal = typedOrdinalNext_++ then `uiMsg.turnUpsert(UiTurnData(turnId, typedCtx_.ordinal, TuiTurnState_Running, 0))`.
- `source/llm/app_agent/package.d:1009` — another typedOrdinalNext_++ (per-batch).
- `source/llm/app_agent/package.d:1064,1079-1083,1110` — LIVE walk tool block emission: summarizeToolCalls(a.toolCalls,1000), meta = summarizeToolResponse(*resp,1000), summarizeToolResponse(a,1000) — the meta carries today's RESULT text.
- `source/llm/app_agent/package.d:1121,1137` — live turn state upserts carrying ctx.ordinal (task 4 drops ordinal).
- `source/llm/app_agent/package.d:1156-1175` — compression setTurns re-emission + typedCtx_.ordinal reset (task 4 removes; W1 store test target).
- `source/llm/app_agent/package.d:366-382` — doCompress: `uiMsg.busy;` at ~371 then `agent_.compress(...)` (bracket exists, lands in empty busy half; task 1 pre-verified path).
- `source/llm/app_agent/package.d:1397-1402` — one-shot mode: `if (app.oneShotQuery) { app.runAgent(app.conf_.prompt); startExit(...); return; }` (no busy/ready bracket; enumeration target).
- `source/llm/app_agent/package.d:1453-1473` — userQuery run bracket: `app.runInFlight = true; ... final switch (app.runAgent(query)) ... app.uiMsg.ready(); app.runInFlight = false;`.
- `source/llm/app_agent/package.d:925-929` — inside runAgent: `agentRunInFlight_ = true; scope (exit) agentRunInFlight_ = false;` (a second in-flight bookkeeping the enumeration must reconcile onto the ready flag per F6).
- `source/llm/app_agent/slash_pipeline.d:17-50` — registerPipelineCommands + planHandler (26-35): `app.uiMsg.pipelineClear; ... runPlanPipeline(...)` — NO busy/ready bracket (task 1 adds); codeHandler follows (~37-50).
- `source/llm/app_agent/ui.d:43-45` — UiSink interface (setTurns/turnUpsert/blockUpsert); `:123-132` TypedChannelSink overrides; `:197-207` second sink (blocked); `:367-388` recording sink (UiBlockData[] blockUpserts etc.); `:479-487` TuiBlockedSink.
- `source/llm/app_agent/ui.d:611` — blockDisplayable(UiBlockData b) (D-side filter).
- `source/llm/app_agent/ui.d:652-666` — makeBlockData + summarizedCallBody: `summarizeToolCallArguments(parseJSON(arguments), 1000)` (args cap 1000 stays per W5).
- `source/llm/app_agent/ui.d:699` — UiBlockData body field ("the call's summarized name-args line"); `:763` streaming call body; `:809,837,854,944` TypedStreamMessageUpdater internals (pendingTools, onBlockUpsert, handleBlock, upsertTool).
- `source/llm/utility.d:126` summarizeToolResponse, `:137` summarizeToolCalls, `:172` summarizeToolCallArguments (one `key=value` line per arg, 1000-char cap).
- `source/llm/app_agent/tests.d` — ordinal assertions to update at cutover: 1930, 1985-1986, 2014, 2138, 2215, 2278-2279, 3370, 3400, 3492, 3556, 3584-3590, 3602-3633; user-query turn unittest at 1947+.
- `source/llm/tui/tests.d:352-355` — phase ordinals test (unrelated, do not touch).
- Busy edge D-side: `source/llm/tui/package.d:~1093` — `void uiAgentBusy() {}` EMPTY body (the bug); `~1095-1098` uiAgentReady: `ui.setReadyStatus(true); playNotification();`. Interface decl `~694` region; dispatch sites 1077-1085 (uiMsg(UiBlockUpsert) etc.).

## C++ store + adapter + views (tasks 2, 4, 5, 6)

- `cpp_tui/store/typed_thread_store.h:27-38` — TypedTurn (ordinal at 29) + TypedBlock (gets result pair fields task 2; loses ordinal task 4 via wire).
- `cpp_tui/store/typed_thread_store.h:50-51` — blockDisplayable + kHiddenToolNames{"pipeline_output"}.
- `cpp_tui/store/typed_thread_store.h:73-87` — stepCount weights (D8 pins; unchanged).
- `cpp_tui/store/typed_thread_store.h:89-130` — TypedThreadStore API: setTurns 92 ("loads turns (array order)"), turnUpsert 93, blockUpsert 94-95, turnRemove 96, turnAt 99 (insertion order), evictForCapacity 105, version 113.
- `cpp_tui/store/thread_store.h:100-108` — GroupView (typedTurn 106, ordinal 107 = dies task 4); `:77` GroupKind; `:85` GroupOverride; `:186` minimalMode; `:193` groupMinimal; `:204` groupAt; `:233` blockCount; `:240` blockAt ("[nr] body" decoration resolved; comment 237-239).
- `cpp_tui/store/thread_store.cpp:209` — `entry.view.ordinal = turn.ordinal;` (dies task 4); `:284-285,569-570` tokens copy; `:378-381` groupAt; `:604-610` "[N]" summary prefix (dies task 4); `:615-645` blockExpanded incl. `defaultOpen = !groupMinimal && kind != Thinking` at 642-643 (dies task 6/D3); `:647-688` setBlockExpanded (row pin mechanics; D3 rework); `:697-751` renderBlockBody: drawn ordinal 714-721 (dies), RESULT line 741-749 `"RESULT  " + oneLine(block->typedMeta)` (becomes pair lines task 5).
- `cpp_tui/views/blocks.cpp:152-166` blockStateGlyph (status glyph — dies D4, FAIL stays); `:172-174` blockHasDisclosure (add ToolStep); `:197-255` blockHeaderText: tool header 206-227 (name 211-214, timing 215-216 stays, tokensIn/Out 217-226 DIE D4), group steps 240-246 (stays).
- `cpp_tui/views/thread.cpp:455` — groupAt use in the view render.
- `cpp_tui/model/block.h:129-130` — BlockDesc tokens fields.
- `cpp_tui/ui/icons.cpp` — `{"check mark", "+"}` ascii fallback (the "+" marker; glyph dies, no icon edit needed beyond removing the glyph use).
- `cpp_tui/tui.h:279` — `bool readyStatus{true};` `:471` predicate `!state.readyStatus && !state.cancelRequested`.
- `cpp_tui/tui.cpp:989-995` — tuiSetReadyStatus: `if (!ready && state.readyStatus) ...; state.readyStatus = ready;` — the entry uiAgentBusy's body drives; comment 989 already names uiAgentBusy() -> tuiReadyStatus(0).
- `cpp_tui/views/shell.cpp:456-457` — `in.busy = !state.readyStatus; in.stopping = state.cancelRequested;` (composer input wiring — UNTOUCHED, works once flag moves).
- `cpp_tui/views/composer.cpp:854,1118,1192-1195,1202,1322-1337` — flip logic golden-green, untouched.
- `cpp_tui/views/palette.cpp:276,616,669,728,735,835` — readyStatus reads (stop affordances; verify vs flag per F6).
- Composer Stop shows during runs once flag moves (design §6); Stop targets run in flight regardless of session shown (global stop-agent path — `source/llm/tui/package.d:945-972` stopAgent paths + `/stop` handling).

## Tests to extend/pin (tasks 5, 6, 7)

- `cpp_tui/test_typed_thread_store.cpp:52-61` helpers; `:257-262,314` ordinal/preview tests (cutover updates); `:531-597` wire-shape tests.
- `cpp_tui/test/typed_seed.h:10,44-46,83-141` — seed helpers (ordinal = seed order; task 4/5 updates).
- `cpp_tui/test_blocks.cpp:373-428,1223,1888-1982,1914-1960` — typed scene tests incl. "ordinal sits next to the header's step count" 1914-1916.
- `cpp_tui/test_thread_window.cpp:245-306,388,552` — GroupView tests.
- `cpp_tui/test/parity_core_loop.cpp:574-585,615-661,1077,1113,1673-1681,1726-1731,1766-1788,1812-1818,1894-1925` — battery: upsertTurn helper with ordinal, ordinal-sequence pins, cancelled stop, compression re-emission ("the summary turn re-emitted with a recomputed ordinal" 1817 — DIES task 4, replaced by order pins).
- `cpp_tui/test_palette.cpp:466` — "[1] ordinal prefix is the projection's (D11)" (dies task 4).
- `cpp_tui/test_e2e_gui.cpp:389-441,677-682` — typed store e2e (ordinal 9 cap assert 678-680 — update task 4).
- Goldens dir `cpp_tui/test/golden/` — blocks-maxanatomy-100x30.txt (the wall), blocks-maxlive/maxfailed (RESULT lines at 11-15), composer goldens; regolden via --update flow only.
- Composer-busy golden masks the bug by poking the C++ flag directly — task 5/7 re-drive from the REAL D-side edge (design §8; test_topbar.cpp:870-872 shows the pattern comment).

## Version-bump reality check (design-vs-code note for the plan)

Design C1 says "task 2 bumps TUI_API_VERSION to v6, task 4 to v7" but the macro is an
integer currently at 12 with pins asserting ==12 in three places (test_tui_api_v4.cpp:1029,1159,1657;
test_clear_output_state.cpp:141). Conservative interpretation recorded for the plan: the macro
monotonically bumps 12 -> 13 (task 2, additive pairs) -> 14 (task 4, ordinal removal); the
design's "v6"/"v7" are design-generation labels, not macro values. Recorded as a
design-vs-code conflict note for user review.

## Environment

- Build/test D: `dub test` from llmfun/ (~15-20 s healthy; hang = one unittest backtrace).
- C++: cmake -S cpp_tui -B cpp_tui/build; plain binaries run direct; *_gui need `TERM=xterm xvfb-run -a ./X --out .`; regolden --update + eyeball.
- llmfun execute_command env: AT MOST 1 command arg (one shell string).
- grep_files transient failures: re-run once. Never conclude from truncated greps.
- edit_file: always pass mode; verify edits by read_file (results can lie).
