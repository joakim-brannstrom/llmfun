# Codebase notes - what the tree looks like at `a1e7743` (branch `update-ui`)

All anchors below were read, not remembered. `plan/anchors.md` carries the verified table.

## Build and test

- D: `cd llmfun && dub build --config=application`, `dub test` (~1 min, primary gate;
  the container image is `llmfun/app:latest`, environment tag `llmfun`, ONE shell string per
  call, shell is `sh`, timeout 123 s).
- C++: an out-of-source build already exists at `llmfun/build/tui`. Per binary:
  `cd llmfun/build/tui && make test_blocks && ./test_blocks` (also `test_streaming`,
  `test_thread_window`, `test_typed_thread_store`, `test_thread_anchor`, `test_palette`,
  `test_thread_view`, `test_keyframe`-style targets). `cmake ../../cpp_tui` regenerates.
- `test_blocks` and `test_streaming` take `--update` to rewrite their goldens.
- Goldens: `cpp_tui/test/golden/`, 100 files, `blocks-<scene>-100x30.txt`,
  `blocks-<scene>-ascii-100x30.txt`, `blocks-<scene>-16col-100x30.txt`,
  `streaming-<scene>-100x30.txt`.

## Patterns the tasks must follow

- **One descriptor, one view.** `model::BlockDesc` (`model/block.h:111-145`) is plain data;
  `store::ThreadStoreAdapter::blockAt()` fills it (`store/thread_store.cpp:573-630`);
  `views::renderBlock()` draws the chrome (`views/blocks.cpp:255-...`) and the adapter draws
  the body (`renderBlockBody`, `store/thread_store.cpp:714+`). `views::BlockView` is an alias
  of `BlockDesc` (`views/blocks.h:61`).
- **Projection** is `ThreadStoreAdapter::syncTyped()` (`store/thread_store.cpp:175-383`):
  per turn -> `Entry{view, stream, blocks, queryRow, hasQuery, answerRow, hasAnswer}`
  (`store/thread_store.h:397-409`), rebuilt only when `typed.version()` moves.
- **Fold** is read per row in `blockAt()` -> `blockExpanded()` (`:588`, `:632-662`) and written
  by `setBlockExpanded()` (`:664-705`). Pins today: `ChatTab::openWorkGroups` /
  `groupOverrides` (`tui.h:179,188`), adapter-local `typedRowOpen_` (`thread_store.h:434`).
- **Row loop**: `views::renderThreadBody` (`views/thread.cpp:446-719`), prefetches a group's
  descriptors once per frame (`:476-483`), draws through one `drawRow` lambda (`:562-682`).
- **C++ tests** are plain `main()` + `check(bool, "text")` counters (no framework), one
  `test_*.cpp` per surface, compiled by `CMakeLists.txt` with `-DTEST_*_SOURCE`,
  `-DTEST_*_GOLDEN_DIR`. Source-scanning gates already exist (`test_blocks.cpp:466-488` scans
  `views/blocks.cpp`; `test_streaming.cpp:399-430` scans a directory).
- **Scenes**: `test_blocks.cpp:1900-2054` `scenes()` (one `Scene{name, build}` per golden),
  captured by `goldenText(name, grid)` (`:1831-1897`), which hard-codes `screen: 100x30`.
- **Seeding**: `typed_seed::Seeder` (`test/typed_seed.h:33-164`) writes the wire blocks;
  `test_blocks.cpp:311-430` `ThreadFixture::add()` translates a `SeedMsg` into upserts.
- **D tests**: `TuiRecordingSink` (`source/llm/app_agent/ui.d:223-400`) records
  `turnUpserts` / `blockUpserts` / `setTurnsSnapshots` / `turnRemovals` and `countOf(kind)`;
  `typedTestApp(...)` builds an `AgentApp` on a sink; `CannedAgent`
  (`source/llm/app_agent/tests.d:77-91`) feeds a scripted `ProcessResult[]` to `runAgent`.

## Facts that decide task content (verified, not in the design's list)

1. **`stepCount` reads the wire store**, before the projection: `store/typed_thread_store.cpp:167-201`.
   Its only displayability input is `blockDisplayable()` (`:11-18`), i.e. the hidden flag.
2. **The answer's batch is already on the wire**: the `Answer` block is emitted with
   `batchId = finalAnswerBatchId` (live `package.d:1149`, replay `:163-164`), so DD-9 needs no
   new field - read `entry.answerRow.typedBatchId`.
3. **The live tick has no `LlmConfig`**: `TypedStreamMessageUpdater` (`ui.d:753-807`) holds
   `uiMsg`, `contextSize`, `modelName`, `ctx`. The hidden-flag helper needs a config seam
   (`TypedEmitContext` gains one; see task_03).
4. **`Done` is already upserted by the step walk** (`package.d:1148-1152`). A run-end close
   must not clobber it -> the context needs a `closedByAnswer` bit (task_01).
5. **`buildTypedTurns`'s `ToolMessage` branch emits no content block** (`package.d:127-164`) -
   replay has no `durationMs` for the response (D9/T17).
6. **`groupMinimal()` conflates style and openness** (`thread_store.cpp:462-472`) and
   `blockExpanded`'s group branch answers `true` in maximal (`:645-646`).
7. **`expandable = true` for every row** (`thread_store.cpp:258`), so `setBlockExpanded`'s
   guard (`:666`) is vacuous.
8. **`blockHasDisclosure` takes a kind** (`views/blocks.cpp:161-166`, decl `views/blocks.h:131`),
   used at `:308` and pinned by `test_blocks.cpp:522`.
9. **`blockHeaderText` takes `bool textGrid`** (`views/blocks.cpp:189`, decl `views/blocks.h:143`);
   call sites: `views/blocks.cpp:327`, `test_blocks.cpp:265,590`, `test_streaming.cpp:384,709,711,718,721`.
10. **The harness screen is fixed 100x30** (`test_blocks.cpp:123-124`, used at `:222,229,443,2180`;
    `goldenText` writes `screen: 100x30` at `:1833`). A 40-column *screen* would need a
    variable-size harness; a 40-unit *header* pin is a pure-function test (task_13).
11. **`lineCountEstimate` is computed at sync time** (`thread_store.cpp:347`), while visibility
    is resolved per frame -> the estimate must stay mode-independent or
    `test_streaming.cpp:655-661` (hint == estimate, untouched) breaks (task_11).
12. **`test_blocks.cpp:1024-1031`** asserts minimal *hides* the streamed prose; T14 flips it.
13. **`test_streaming.cpp:673-688` indexes rows literally** (`headerOf(fixture, 2)`), and
    `streamBodyRow()` (`:359-370`) scans; owner rows inserted into the list move every literal
    index. Prefer a predicate over a literal.
14. **`test_typed_thread_store.cpp`'s `block()` helper defaults `batchId = ""`** (`:63-70`);
    its `blockCount` fixtures (`:289-292`) then put the query and the work rows in ONE batch,
    which under DD-4 means "the query batch" -> no response batch. Fixtures need real ids.
15. **`test_blocks.cpp:118-416 ThreadFixture::add()` invents `#c` / `#t` / `#a` / `#n`
    batches per message** - one response's content and calls are *different* batches there.
    Under DD-4 each becomes its own owner row: every `f.add(...)` scene would gain owners
    nobody wants. T5 must re-key the fixture to the wire shape (one response batch per
    assistant/tool group; notices and vision on the query batch; the answer on the response's
    batch). This is the single biggest hidden cost in the plan.
16. **The cost test seeds no work blocks** (`test_thread_window.cpp:531-541`: only
    `Msg::User` / `Msg::FinalAnswer`), so it would not measure the new bucketing at all (T23).

## Design-vs-code conflicts (conservative interpretation chosen; user may override)

- **C1 - chips example contradicts DD-18/FR-23.** Design §4.6 line 434 shows
  `> 2.7 s · [read_file] [read_file]` while DD-18/FR-23 say "no duplicates". The plan follows
  the *rule* (dedupe, first occurrence wins, `+N` = dropped distinct tokens) and treats the art
  as stale; T19/T21 make the art and the docs agree with the code.
- **C2 - `thread_store.h:194` is a comment, not the default.** The factory default lives at
  `tui.h:197` (`ChatTab::threadMinimalMode = true`).
- **C3 - §10.2's "40-column pass per profile"** cannot be a golden (fixed 100x30 harness):
  it is a pure-function assertion on `blockHeaderText(..., availUnits = 40)` in both profiles.
- **C4 - §10.4's blast radius is incomplete.** It omits `test_streaming.cpp:381-385` +
  `:709-721` (the `blockHeaderText` signature), `:673-688` (literal row indices),
  `test_blocks.cpp:1024-1031` (minimal prose), `test_blocks.cpp:118-416` (fixture batches),
  `test_typed_thread_store.cpp:63-70` (batch ids), `test_thread_window.cpp:531-541` (cost
  fixture), `test_palette.cpp` / `test_thread_view.cpp` / `test_thread_anchor.cpp` /
  `test_keymap.cpp` (live-stream fixtures that call `updateStream`/`seed.tool`).
- **C5 - `blockHasDisclosure(kind)` is used by a table test** (`test_blocks.cpp:522`); making
  it descriptor-based changes that test's shape, not only its value.
