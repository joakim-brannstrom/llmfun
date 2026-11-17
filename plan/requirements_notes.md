# Requirements notes - response-owned batch fold (from `plan/system_design.md` rev 2)

Source of truth: `plan/system_design.md` (rev 2, 1224 lines). Code baseline `a1e7743`,
branch `update-ui`, tree at `./llmfun`. Design §11 lists 26 tasks T1..T26; this plan turns
them 1:1 into `task_01.md` .. `task_26.md`.

## Scope

In: C++ typed-store projection + row model (`cpp_tui/store`, `cpp_tui/model`), the thread
view and both profiles (`cpp_tui/views`), the D emitter's turn lifecycle / wire flag / replay
parity (`source/llm/app_agent/`, `source/llm/agent/package.d`, `source/llm/config.d`), the
C++ test data / scenes / goldens, `doc/tui_design.md`.
Out: console formatter layout (only emission order), new `BlockKind`, new wire event,
per-response emitter branching, persistence (batch ids are in-memory only).

## Acceptance criteria (whole feature)

- One `AGENT` owner row per response that produced visible work: duration + chips +
  disclosure mark, focusable, Space/Enter + click toggles, owns its children.
- Children draw reason -> content -> cards, live == replay.
- Owner open only while the batch is the live tail and unsettled; a settled owner folds to
  `> 3.2 s - [chips]` (grid `-`, pixel `·`); re-opened shows content with thinking/cards folded.
- Minimal: newest live batch's content draws under the folded header, with no owner row
  drawn, until the turn closes.
- Pins absolute (FR-7), pruned only on eviction/clear.
- Displayability from the wire flag `TuiBlockFlag_Hidden = 8`; no tool-name list in C++;
  no `batchId` parsing anywhere (NFR-3).
- Turn closes on every run exit; `/c` re-opens; no fabricated block status.
- 100 goldens byte-exact; every shape-changing task regoldens what it moves; full suite
  green at the end of every task (NFR-10).

## Hard constraints

1. D + C++ are one binary: ABI additive only (`TUI_API_VERSION` 14 -> 15, doc only).
2. `task_done`'s answer arrives as an `Answer` block whose `batchId` is the answer's batch.
3. Test harness screen is 100x30 (`test_blocks.cpp:123-124`) - scenes fit six responses.
4. No persisted state, no migration; pins are session state.
5. `test_streaming.cpp:655-661` ("the hint is the estimate and does not move with the fold")
   stays green untouched.
6. Minimal is the factory default (`tui.h:197` `threadMinimalMode = true`).

## Requirement ids used by tests (from design §3)

FR-1 owner row + affordance; FR-2 order; FR-3 fold/re-open; FR-4 thinking visible while
streaming; FR-5 minimal live content; FR-6 group fold; FR-7 absolute pins; FR-8 folded owner
drops the role word; FR-9 FAIL ink; FR-10 wire displayability; FR-11 close on every exit;
FR-12 answer appears once; FR-13 width-aware header; FR-14 replay parity; FR-15 live edge
minimal-only/newest-only; FR-16 segment copy (pixel); FR-17 drill-down identical; FR-18 no
header for empty rows; FR-19 `lineCountEstimate`; FR-20 console intra-batch order; FR-21 two
content lifetimes; FR-22 owner is the copy target; FR-23 one chips notation.
NFR-1 determinism, NFR-2 one source per fact, NFR-3 no D vocabulary in C++, NFR-4 additive
ABI, NFR-5 cost, NFR-6 both profiles at 40 and 100 columns, NFR-7 no mode-dependent emission,
NFR-8 no new logging, NFR-9 measured cost, NFR-10 green at every boundary.

## Invariants (§6.5) - tests refer to these ids

I-1 one owner per batch; I-2 owner iff >=1 surviving child; I-3 children contiguous; I-4
reason->content->cards; I-5 child visible => owner visible and open; I-6 no repeated row id
in a turn; I-7 bucket order = insertion order; I-8 no status written by C++; I-9 hidden ==
weight 0, draws nothing; I-10 the answer's batch contributes no content child; I-11 a body
segment is never gated on its own `expanded`; I-12 `isNewest` = last response batch of its
turn in insertion order; I-13 a pin survives any default change.

## Sign-off items the code cannot decide (surface, do not silently choose)

- R-3 (DD-9): a final response whose preamble differs from its answer loses the preamble.
- R-4 (DD-12): `WORK - N steps` drops by 2 per hidden settled call.
- R-9 (DD-10): content lifetime differs by mode (maximal folds with its batch, minimal keeps
  it until the turn closes). One-line alternative recorded in design §0.3.
