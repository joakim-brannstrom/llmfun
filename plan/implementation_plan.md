# Implementation Plan: response-owned batch fold (maximal + minimal thread modes)

## Overview

One fold owner per LLM response: a synthetic `AGENT` row carrying the response's duration,
its tool chips and a disclosure mark, whose children are the response's thinking, its content
segment and one card per call. The fold is derived (turn state + batch position + call
statuses) and each row's `visible` bit is computed once in the adapter; the view draws iff
`visible`. Displayability moves onto the wire (`TuiBlockFlag_Hidden = 8`) and the turn closes on
every run exit, so `task_done`'s block disappears and the answer appears once.

Source: `plan/system_design.md` rev 2 (authoritative for tasks and interfaces),
`plan/maximal_investigation.md` (observed behaviour). Design §11's T1..T26 map 1:1 to
`task_01.md`..`task_26.md` (plus `task_06a.md`, the split of the scenes-first task).

## Task Order

Execute in order; each task ends with the full suite green (NFR-10).

1. task_01.md - D: close the turn on every run exit (P0)
2. task_02.md - D: run-exit probes (P0)
3. task_03.md - D: displayability on the wire (`TuiBlockFlag_Hidden`) (P0)
4. task_04.md - C++: displayability from the flag (P0)
5. task_05.md - Test data: batch ids that match the wire (P0)
6. task_06.md - Scenes first: the four new scenes and their captures (P0)
6a. task_06a.md - Scenes first: the target rows as failing checks (P0)
7. task_07.md - C++ projection: buckets and the owner row (P0)
8. task_08.md - C++ projection: order, drops, placement (P0)
9. task_09.md - C++ model: row vocabulary, disclosure, `Assistant` audit (P0)
10. task_10.md - C++ fold: liveness, absolute pins, the visibility predicate (P0)
11. task_11.md - C++ view: the row loop obeys `visible` (P0)
12. task_12.md - C++ view: the body segment (P0)
13. task_13.md - C++ view: header composition, folded role word, FAIL, disclosure (P0)
14. task_14.md - C++ view: minimal draws the live content with no owner on screen (P0)
15. task_15.md - C++ view: the live edge is minimal-only, newest-only (P0)
16. task_16.md - C++ view: anchors survive a batch folding in place (P0)
17. task_17.md - D: replay parity (P1)
18. task_18.md - C++ projection: the answer batch suppresses its content (P1)
19. task_19.md - Scene review and the new checks (P1)
20. task_20.md - The second mount stays identical (P1)
21. task_21.md - Documentation (P1)
22. task_22.md - Retired names, lying comments, the grep gate (P1)
23. task_23.md - Measured cost (P2)
24. task_24.md - Pin hygiene and "collapse all" (P2)
25. task_25.md - Console parity for the intra-batch order (P2)
26. task_26.md - Grid copy of a body segment: decision, not code (P3)

## Cross-cutting Concerns

- **Two languages, one binary.** A D change that alters an emitted shape needs the matching
  C++ task before a golden can move; the order above always lands the emitter first.
- **`visible` is the only visibility answer.** No task may add a view-side skip, an exception
  list, or a second copy of the fold. A row the view must not draw is `visible == false`.
- **Pins are absolute.** A write stores the user's answer verbatim; nothing drops a pin because
  it equals a default. Pruning happens only in `tuiTurnRemove` / `tuiThreadClear` / eviction.
- **Row indices are not stable.** The projection inserts one owner row per batch, so every
  literal row index in a test moves. Select rows by kind/label predicate
  (`test_streaming.cpp:359-370` is the pattern), not by number. `BlockId` (group, hash(id),
  kind) stays the identity; the hash stays non-injective, so never compare keys across kinds.
- **One chips helper.** `store::chipsText(names)` (task_07 defines it) is the only composer, for the
  owner header and for minimal's live chips line. Notation `[bash] [grep]`, `+N` past three,
  no duplicates (first occurrence wins).
- **Profile units.** Every width goes through `ui::textUnits(m, text)`; the grid separates with
  `" - "`, the pixel profile with `" \xC2\xB7 "`. Never a character count.
- **Golden discipline.** A shape-changing task regoldens the goldens it moves, in that task,
  with a one-line reason; `--update` only after a human has looked at the capture.
- **Batch ids are never parsed.** A response batch is any batch that is *not* the batch
  carrying the turn's query row (design DD-4). No `#` split, no numeric suffix.

## Standing Executor Rules

- **Build/test.** Container environment `llmfun`; ONE shell string per call, shell is `sh`.
  D: `cd llmfun && dub build --config=application` then `dub test` (~1 min; 123 s timeout).
  C++: `cd llmfun/build/tui && make test_blocks && ./test_blocks` (also `test_streaming`,
  `test_thread_window`, `test_typed_thread_store`, `test_thread_anchor`, `test_palette`,
  `test_thread_view`). Redirect compiler output to a log (`> build.log 2>&1`) and inspect with
  `grep -n "error:" | head`; never paste a full compiler log.
- **Regression.** Every task's Verification ends with the full suite green: `dub test`,
  `make test_blocks && ./test_blocks`, `./test_streaming`, `./test_thread_window`,
  `./test_typed_thread_store`, `./test_thread_anchor`, `./test_palette`, `./test_thread_view`.
  A task that leaves a golden red is not done. One exception, by design: task_06a lands the new
  scenes' *shape checks* red (B5's "the shape exists as a failing test") - every golden is green
  there, and every other task is fully green.
- **Stop-and-re-plan.** If a task's real work exceeds its read set (more than ~5 source files)
  or its size budget, stop and report it for a re-plan. Never push through a task that has
  doubled.
- **Anchors.** Verify the anchored content before each edit (`read_file`); if it differs from
  the anchor's description, re-anchor by symbol (`grep -n`) and proceed - never guess a line.
- **Decisions (design §4.4/§6.5, one line each).**
  DD-1 the owner is the response (`AGENT`), always present. DD-2 liveness is derived; no
  `Cancelled` is ever written. DD-3 displayability is the wire flag. DD-4 a batch is a response
  batch iff it is not the query row's batch - no id parsing. DD-5 canonical child order
  reason -> content -> cards. DD-6 pins absolute, always stored. DD-7 the content is a
  header-less body segment. DD-8 the owner carries the duration; `Thinking` carries none.
  DD-9 the `Answer`'s batch suppresses its content child. DD-10 one content predicate with a
  mode parameter. DD-11 the live edge is minimal-only and newest-only. DD-12 `stepCount` stays
  flag-driven (-2 per hidden settled call). DD-13 owner identity `batch#<batchId>`,
  `expandable`, `hasBody == false`, `copyable`. DD-14 `groupOpen()` + `groupStyleIsMinimal()`
  replace `groupMinimal()`. DD-15 no owner row without a surviving child. DD-16 visibility is
  one adapter predicate. DD-17 the owner is invented in C++, not on the wire. DD-18 one chips
  helper for both modes.
  I-1..I-13 (design §6.5) are the invariants the tests cite: one owner per batch; owner iff a
  surviving child; children contiguous; reason -> content -> cards; child visible implies owner
  visible and open; no repeated row id; bucket order = insertion order; no C++-written status;
  hidden weighs 0 and draws nothing; the answer's batch has no content child; a segment is
  never gated on its own `expanded`; `isNewest` = last response batch in insertion order; a pin
  survives any default change.
- **Do not reintroduce** (task_22 enforces): `thinkingOpen`, `batchTools`, `groupOverrides`,
  `openWorkGroups`, `typedRowOpen_`, `kHiddenToolNames`, a tool-name list in `cpp_tui/`
  production code, `task_done` / `pipeline_output` literals in `cpp_tui/` production code,
  `<turnId>#<n>` parsing, a view-side fold exception list.

## Open issues (design-vs-code conflicts, conservative reading chosen)

1. **Chips art vs rule.** §4.6's art shows `[read_file] [read_file]`; DD-18/FR-23 say "no
   duplicates". The plan follows the rule (dedupe, `+N` = dropped distinct tokens); task_19/21
   make the art and the docs agree. Ask the user at review if the count matters.
2. **The factory default is `tui.h:197`**, not `thread_store.h:194` (a comment).
3. **§10.2's "40-column pass per profile"** is a pure-function assertion on
   `blockHeaderText(..., 40.0f)`: the `test_blocks` harness is fixed at 100x30.
4. **The test fixture invents `#c`/`#t`/`#a`/`#n` batches** (`test_blocks.cpp:369-415`) and
   `test_typed_thread_store.cpp`'s helper defaults `batchId = ""`. Under DD-4 every such batch
   is a response batch, so the fixtures would grow owner rows nobody meant. task_05 re-keys
   them to the wire shape (one response batch per assistant/tool group, notices/vision on the
   query batch, the answer on its response's batch).
5. **`plainVisible` is about group-level rows only.** Design §7.5's `plainVisible = groupOpen`
   reads as if the query/answer rows follow the group; they are turn-level and must draw in a
   folded group (otherwise minimal, the factory default, hides every answer). task_10 states the
   turn-level case explicitly. Likewise I-5 holds for thinking/cards, not for the content segment -
   the glossary calls the segment a child, but FR-5/B1 deliberately show it with no owner drawn.
6. **`lineCountEstimate` stays mode-independent** (header + plain rows + owner rows) because it
   is computed at sync time while visibility is per frame, and `test_streaming.cpp:655-661`
   pins that the hint does not move with the fold.

Anchor master list: `plan/anchors.md`. Requirements and invariants: `plan/requirements_notes.md`.
Codebase survey and the conflicts above: `plan/codebase_notes.md`.
