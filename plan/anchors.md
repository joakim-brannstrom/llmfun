# Master anchor list (verified at plan time against `llmfun` @ a1e7743)

Line numbers are evidence and rot; the **symbol** is what to grep. Each task restates only
the anchors it edits and re-verifies them before the edit (Standing Executor Rules).

## D - emitter

| Anchor | Symbol / content today |
|---|---|
| `source/llm/app_agent/ui.d:606-623` | `kHiddenToolNames` (`:609`) + console `blockDisplayable()` (`:611-623`) |
| `source/llm/app_agent/ui.d:626-627` | `kToolBlockFlags = Collapsible | DefaultOpen | Selectable` |
| `source/llm/app_agent/ui.d:629-646` | `responseBatchId` / `queryBatchId` / `contentBlockId` / `reasoningBlockId` |
| `source/llm/app_agent/ui.d:652-656` | `makeBlockData(turnId, id, batchId, kind, status, flags, title, meta, body, durationMs)` |
| `source/llm/app_agent/ui.d:701-746` | `TypedEmitContext` (`liveCalls`, `batchId()`, `recordLiveCall`, `clearLiveCall`) |
| `source/llm/app_agent/ui.d:768-791` | `TypedStreamMessageUpdater::messageUpdate` (content `:777-779`, reasoning `:780-783`, Pending call `:785-790`) |
| `source/llm/app_agent/ui.d:793-799` | `streamMessageDone` (Pending -> Running) |
| `source/llm/app_agent/ui.d:223-400` | `TuiRecordingSink` (`turnUpserts`, `blockUpserts`, `setTurnsSnapshots`, `countOf`) |
| `source/llm/app_agent/package.d:118-175` | `buildTypedTurns`: Message `:113-126`, **ToolMessage `:127-164`** (no content block; Answer `:162-164`), Vision `:169-174` |
| `source/llm/app_agent/package.d:921-992` | `runAgent` (slash branch `:931-944`, forced loop `:941`, run bracket `:954-975`, `emitStopTyped` `:970-971`, stuck-loop message `:972-974`) |
| `source/llm/app_agent/package.d:1003-1013` | `emitReplayTyped` (calls `buildTypedTurns`) |
| `source/llm/app_agent/package.d:1015-1025` | `emitTurnStartTyped` (turn `Running` + query block) |
| `source/llm/app_agent/package.d:1032-1153` | `emitStepTyped`: step walk `:1067-1146` (Message `:1068-1081`, ToolMessage `:1082-1124`, orphan `:1125-1140`, Answer + Done `:1148-1152`) |
| `source/llm/app_agent/package.d:1158-1167` | `emitStopTyped` (Cancelled flips `:1162-1164`, `liveCalls = null` `:1165`, Interrupted `:1166`) |
| `source/llm/app_agent/package.d:1277-1279` | `continueAgent()` (`/c`) sets `forceRunAgentLoop` |
| `source/llm/app_agent/tests.d:77-91` | `CannedAgent` (scripted `ProcessResult[]`) |
| `source/llm/app_agent/tests.d:1963-2000` | run-level `networkFailure` test: **asserts the turn stays open** (`:1992`) - moves with T1 |
| `source/llm/app_agent/tests.d:3555-3620` | replay test: `blocks.length == 7` (`:3590`) - moves with T17 |
| `source/llm/app_agent/tests.d:3660-3745` | step-level live/stop tests (survive: they drive `emitStepTyped`/`emitStopTyped` directly) |
| `source/llm/agent/package.d:370-376` | `scope(exit) streamMessageDone()` |
| `source/llm/agent/package.d:612-657` | statuses that leave the loop (`unknownFailure` `:612`, `networkFailure` `:618`, stuck loop `:629,642,655`) |
| `source/llm/config.d:637` | `hiddenTools = ["taskDone", "pipelineOutput"]` |
| `source/llm/config.d:911-925` | `isHiddenTool(string)` (both name axes, static cache) |

## Wire / C ABI

| Anchor | Content |
|---|---|
| `cpp_tui/tui_api.h:26-29` | `TUI_API_VERSION 14` |
| `cpp_tui/tui_api.h:649-653` | `TuiBlockFlags` (`Collapsible=1, DefaultOpen=2, Selectable=4`) |
| `cpp_tui/tui_api.h:663-697` | `TuiBlock` (`batchId` doc at `:663-668`) |

## C++ store

| Anchor | Symbol / content |
|---|---|
| `cpp_tui/store/typed_thread_store.h:33-44` | `TypedBlock` (`flags` `:36`) |
| `cpp_tui/store/typed_thread_store.h:46-57` | displayability comment, `blockDisplayable` `:56`, `kHiddenToolNames` `:57` |
| `cpp_tui/store/typed_thread_store.h:75-77` | `kTurnStateRunning/Done/Interrupted` |
| `cpp_tui/store/typed_thread_store.h:79-93` | step-count weights + `stepCount` |
| `cpp_tui/store/typed_thread_store.h:102-117` | `turnRemove`/`threadClear`/`evictForCapacity`/`version` |
| `cpp_tui/store/typed_thread_store.cpp:11-18` | `blockDisplayable` (name list) |
| `cpp_tui/store/typed_thread_store.cpp:167-201` | `stepCount` (wire store: prose 1, tool Ok/Failed 2, notice 1, hidden 0) |
| `cpp_tui/store/thread_store.h:74-85` | `GroupKind`, `GroupOverride` |
| `cpp_tui/store/thread_store.h:92-93` | `kQueryRow` / `kAnswerRow` sentinels |
| `cpp_tui/store/thread_store.h:107-118` | `GroupView` (`lineCountEstimate` `:112`) |
| `cpp_tui/store/thread_store.h:193-203` | `minimalMode()` `:196`, `groupMinimal(id)` `:203` |
| `cpp_tui/store/thread_store.h:239-265` | `blockCount` `:243`, `blockAt` `:251`, `blockExpanded` `:257`, `setBlockExpanded` `:260`, `blockText` `:265` |
| `cpp_tui/store/thread_store.h:278-297` | `renderBlockBody` `:282`, `expanded` `:296`, `setExpanded` `:297` |
| `cpp_tui/store/thread_store.h:344-349` | `commitSeen` (no-op) |
| `cpp_tui/store/thread_store.h:362-395` | `BlockEntry` (`expandable` `:373`, `typedBatchId` `:383`, `batchTools` `:386`) |
| `cpp_tui/store/thread_store.h:397-409` | `Entry` (`queryRow` `:405`, `answerRow` `:407`) |
| `cpp_tui/store/thread_store.h:431-437` | `typedRevision_`, `typedFingerprints_`, `typedRowOpen_` `:434`, `syncTyped` `:437` |
| `cpp_tui/store/thread_store.cpp:175-383` | `syncTyped` in full: header `:219-225`, queryId `:234-250`, rows `:251-332` (`expandable = true` `:258`), batchTools `:333-344`, estimate `:347`, stream mark `:381-382` |
| `cpp_tui/store/thread_store.cpp:408-430` | `streamHasText` (body-gated `:416-417`), `streamHasThinking` (`:427`) |
| `cpp_tui/store/thread_store.cpp:455-513` | `minimalMode` `:460`, `groupMinimal` `:462-472`, `expanded` `:474-487`, `setExpanded` `:489-513` |
| `cpp_tui/store/thread_store.cpp:551-571` | `blockEntryAt` `:555-564`, `blockCount` `:566-571` |
| `cpp_tui/store/thread_store.cpp:573-630` | `blockAt` (field copy `:580-588`, labels `:597-615`, previews `:616-628`) |
| `cpp_tui/store/thread_store.cpp:632-662` | `blockExpanded` (group branch `:637-647`, defaults `:654-661`) |
| `cpp_tui/store/thread_store.cpp:664-705` | `setBlockExpanded` (guard `:666`, relative pin `:695-701`) |
| `cpp_tui/store/thread_store.cpp:707-712` | `blockText` |
| `cpp_tui/store/thread_store.cpp:714-760` | `renderBlockBody` (markdown vs verbatim `:725`, table source `:742-743`) |
| `cpp_tui/store/thread_store.cpp:890-913` | `reset()` clears `thinkingOpen` `:903`, `typedRowOpen_` `:910` |
| `cpp_tui/tui.h:174-198` | `ChatTab`: `openWorkGroups` `:179`, `groupOverrides` `:188`, `threadMinimalMode = true` `:197` |
| `cpp_tui/tui_api.cpp:874-878` | `tuiSetTurns` clears `groupOverrides` |
| `cpp_tui/tui_api.cpp:880-889` | `tuiTurnUpsert` -> `evictForCapacity` `:888` |
| `cpp_tui/tui_api.cpp:915-931` | `tuiTurnRemove` `:915-922`, `tuiThreadClear` `:924-931` |

## C++ model / view

| Anchor | Symbol / content |
|---|---|
| `cpp_tui/model/block.h:38-49` | `BlockKind` (`Assistant` `:42`) |
| `cpp_tui/model/block.h:111-145` | `BlockDesc` (`batchTools` `:126` - retired, `expandable` `:128`, `expanded` `:129`, `hasBody` `:132`) |
| `cpp_tui/model/ui_state.h:256` | `thinkingOpen` (dead) |
| `cpp_tui/views/thread.cpp:446-719` | `renderThreadBody` loop: `groupMinimal` `:448`, `beginGroup` `:456`, `blockCount` `:463`, prefetch `:476-483`, live-edge inputs `:491-516`, `drawLiveEdge` `:521-556` (chips `:528`, shadow `:545-552`), `drawRow` `:562-682`, card indent `:573-575`, fold skip `:581-584`, `rowOpen` `:592-595`, measure/render `:597-598`, toggle `:616-619`, copy `:620-633`, body `:639-653`, card rail `:654-664`, folded-header live edge `:671-680`, query row `:684-685`, header sample `:691-699`, row loop `:700-703`, answer row `:705-706`, after-group live edge `:712-717`, `endGroup` `:718`, assert `:729` |
| `cpp_tui/views/blocks.h:61` | `using BlockView = model::BlockDesc` |
| `cpp_tui/views/blocks.h:103-143` | `measureBlockHeader` `:103`, `blockHasDisclosure` `:131`, `blockHeaderText` `:143` |
| `cpp_tui/views/blocks.cpp:39-43` | `separator(bool textGrid)` (`" - "` / `" \xC2\xB7 "`) |
| `cpp_tui/views/blocks.cpp:161-166` | `blockHasDisclosure(kind)` |
| `cpp_tui/views/blocks.cpp:189-239` | `blockHeaderText(block, draw, textGrid)` |
| `cpp_tui/views/blocks.cpp:245-253` | `measureBlockHeader` |
| `cpp_tui/views/blocks.cpp:255-333` | `renderBlock` (guard `:261-262`, `toggled` `:299`, ink `:304-305`, disclosure `:308-323`, header call `:327`, FAIL `:329-333`) |
| `cpp_tui/views/palette.cpp:167-172` | `blockIsAnswerRole` (`Assistant` counts) |
| `cpp_tui/views/palette.cpp:185-218` | `lastAnswerBlock` (`hasBody || streaming` gate `:199,210`) |
| `cpp_tui/views/shell.cpp:1198-1221` | drill-down mount binds `agentThreadAdapter` `:1206-1209` |
| `cpp_tui/ui/primitives.cpp:117-131` | `ui::textUnits(m, text)` |
| `cpp_tui/ui/metrics.h:150,163` | `metricsFor(textGrid, fontScale, dpiScale)` / `metricsFor()` |

## C++ tests / goldens

| Anchor | Content |
|---|---|
| `cpp_tui/test_blocks.cpp:123-124` | `kScreenW = 100`, `kScreenH = 30` |
| `cpp_tui/test_blocks.cpp:118-416` | `ThreadFixture` + `add()` (batches `#c` `:394`, `#t` `:401,406`, `#a` `:390`, `#n` `:411`) |
| `cpp_tui/test_blocks.cpp:264-266` | `headerText(block, textGrid)` helper -> `blockHeaderText` |
| `cpp_tui/test_blocks.cpp:466-488` | `checkSource` (source-scan gate pattern) |
| `cpp_tui/test_blocks.cpp:495-524` | vocabulary table (`blockHasDisclosure(kind)` `:522`) |
| `cpp_tui/test_blocks.cpp:594-605` | separator pins (`YOU - 1.2 s` / `YOU · 1.2 s`), `WORK - 4 steps` |
| `cpp_tui/test_blocks.cpp:615-625` | `Thinking - 3.2 s` `:618`, streaming row `:621-625` |
| `cpp_tui/test_blocks.cpp:1008` | `Thinking - 1 tool call` (shadow line - unaffected) |
| `cpp_tui/test_blocks.cpp:1024-1031` | minimal hides the streamed prose (T14 flips) |
| `cpp_tui/test_blocks.cpp:1287-1334` | focus/copy identity (`content#1` `:1293`, `Thinking - 2.0 s` `:1305`, `content#0` `:1326`) |
| `cpp_tui/test_blocks.cpp:1604-1621` | empty stream: `!grid.contains("AGENT")` `:1611` |
| `cpp_tui/test_blocks.cpp:1900-2054` | `scenes()` (16 scenes) |
| `cpp_tui/test_blocks.cpp:1831-1833` | `goldenText` header (`screen: 100x30`) |
| `cpp_tui/test_typed_thread_store.cpp:63-70` | `block(...)` helper (`batchId = ""` default) |
| `cpp_tui/test_typed_thread_store.cpp:289-334` | `blockCount` fixtures (`:304`, `:327`, `:331`), `Assistant` at `:400` |
| `cpp_tui/test_streaming.cpp:359-370` | `streamBodyRow()` (predicate scan - safe) |
| `cpp_tui/test_streaming.cpp:381-385` | `headerOf(fixture, block)` -> `blockHeaderText` |
| `cpp_tui/test_streaming.cpp:399-430` | `checkSource` over `TEST_STREAMING_SOURCE_DIR` |
| `cpp_tui/test_streaming.cpp:655-661` | hint == estimate, does not move (MUST stay untouched) |
| `cpp_tui/test_streaming.cpp:673-688` | `headerOf(fixture, 2)` literal row indices |
| `cpp_tui/test_streaming.cpp:709-721` | `blockHeaderText` calls (`ANSWER - 2.5 s`) |
| `cpp_tui/test_thread_window.cpp:522-617` | scaled-thread cost test (fixture `:531-541`, loop `:552-559`, gates `:599-616`) |
| `cpp_tui/test/typed_seed.h:33-164` | `Seeder` (`tool()` bumps `liveBatch` `:157`) |
| `cpp_tui/CMakeLists.txt:960-980` | `test_blocks` target + `TEST_BLOCKS_SOURCE`/`TEST_BLOCKS_GOLDEN_DIR` |
| `cpp_tui/CMakeLists.txt:985-997` | `test_streaming` target + `TEST_STREAMING_SOURCE_DIR` |
| `cpp_tui/CMakeLists.txt:841-846` | `test_thread_window` target |
| `cpp_tui/CMakeLists.txt:864` | `test_typed_thread_store` include dir |
