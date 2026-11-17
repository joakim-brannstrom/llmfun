# System Design (rev 2): the response-owned batch fold (maximal + minimal thread modes)

- **Subject**: `llmfun` C++ TUI thread rendering (`cpp_tui/`) and the D emitter that
  feeds it (`source/llm/app_agent/`).
- **Revision**: 2 — addresses the design review of revision 1
  (B1–B5 blocking, H1–H8 high, M/L items). The change log is §0.2; the two
  review points this revision does **not** accept are marked and argued in §0.3.
- **Authority**: **this file is authoritative for tasks and interfaces**;
  `plan/maximal_investigation.md` is authoritative for *observed behaviour* (the
  probe grids, the symptom mechanism, the user's judged shapes). Where the two
  differ, this file wins for what to build and the investigation wins for what
  the app does today.
- **Code baseline**: verified against `a1e7743` (branch `update-ui`) on
  2026-11-16. Line numbers are evidence and will rot; the **symbol names** in
  §13 are the durable anchors and should be what a reader greps.
- **Deliverable**: the prioritized task list in §11 (`implementation-plan` turns
  it into code tasks; `code-task` executes them).

---

## 0. Revision log

### 0.1 Review verdict being answered

"Approve the architecture, reject the document as implementation-ready." Five
blocking contradictions (B1–B5), eight high issues (H1–H8), the medium/low list
and the requirement gaps. The architecture (§4.1 ownership, derived liveness,
wire displayability, additive ABI) is unchanged.

### 0.2 What changed in this revision

| Review item | Change |
|---|---|
| **B1** minimal content unreachable | §7.5 replaces per-row `expanded` reasoning with **one visibility predicate computed by the adapter** (`BlockDesc::visible`); the content segment is admitted by a rule of its own, never by the nesting bit. C7/§7.3 state the evaluation order. |
| **B2** re-opened batch empty | children's *visibility* = the owner's resolved openness; children's *openness* = `rowPins[id] ?? batchLive`. The `&& batchAutoOpen` conjunct is gone from both. §7.5 + DD-16. |
| **B3** body segment had no rule | `bodySegment` never reads its own `expanded`; its visibility is §7.5's segment clause; new invariant **I-11**. |
| **B4** owner had no disclosure mark | `blockHasDisclosure()` becomes descriptor-based and true for `ownsBatch`; `expandable` becomes a per-row decision (owner true, segment false) and DD-13 lists it. New C9a. |
| **B5** goldens red T5→T15 | New **T6** creates the two target scenes *before* the projection work; every shape-changing task regoldens what it moves; T19 is scene review + new checks only. §10.1 restated. |
| **H1** `Assistant` consumers unaudited | New **C9b** + task **T9** acceptance: `blockIsAnswerRole()`/`lastAnswerBlock()` (`views/palette.cpp:167-218`), `streamHasText()`/`streamHasThinking()` (verified safe: both require a non-empty body), `blockAt`'s label switch, `blockInk`/`defaultBlockLabel`. |
| **H2** pins dropped when equal to the default | DD-6 rewritten: pins are **absolute and always stored**; pruning on turn eviction and clear only; new T24 adds the escape hatch. |
| **H3** DD-4 parsed D's id format | DD-4 rewritten: **no string parsing anywhere**. A batch is a response batch iff it is *not the batch carrying the turn's query row* — a fact the projection already computes. The `TuiBlockFlag_ResponseBatch` alternative and the "owner row on the wire" alternative are both recorded and rejected with reasons (§4.4 DD-17). |
| **H4** content flaps at settle in maximal | Partly accepted: the two modes keep different content lifetimes, **stated as one rule with a mode parameter** and flagged for sign-off (R-9). The maximal lifetime (fold with its batch) is the approved model from the investigation §5.2/5.3; the single-lifetime alternative is one line and is recorded. |
| **H5** step count "-2 is probably -3" | **Rejected with evidence**: `stepCount()` reads the typed store, not the projection, so DD-9's drop cannot move it. §5.C5.6 states the arithmetic and T8 adds the two config cases as tests. |
| **H6** header signature / blast radius | §7.4 signature keeps the metrics (`blockHeaderText(desc, draw, const ui::Metrics&, float availUnits)`), the grid keeps `" - "`; both profiles pinned at 40 columns; the `Thinking` row's duration retirement is stated in DD-8/T13; §10.4 lists the real blast radius. |
| **H7** D-side unverified | New task **T2**: one probe per run-exit path (final answer, stop, network failure, stuck loop, `/c` resume) asserting the *store shape*, plus a manual live run. T17 adds a replay grid comparison. |
| **H8** interleaved plain rows undefined | §4.3 + C5 carry the placement rule ("the block is emitted at its first row's index; later rows of the batch are consumed by it; a plain row that lay between is emitted after the block") and T5/T6 seed a notice mid-response. |
| M1 `batchIsNewest` undefined | §6.5 I-12 + §7.5 definition ("the last response batch of its turn in insertion order"). |
| M2 cost unmeasured | NFR-9 + T23: extend `test_thread_window`'s existing scaled-thread cost test to cover `sync` + `blockExpanded`. |
| M3 pins never pruned, no escape hatch | §6.4 pruning on eviction/clear; T24 adds "collapse all". |
| M4 ink lost | C8's ink table. |
| M5 Thinking duration | DD-8 + T13. |
| M6 `blockCount` contract | §7.2 restated; the affected fixtures (`test_typed_thread_store.cpp:304, 327, 331`) named in T9. |
| M8 no sizes | Every task carries S/M/L. |
| M9 no rollback | §8 "Rollback" — explicit, with the reasons a toggle is not added. |
| Requirement gaps | FR-21 (content lifetime), FR-22 (owner affordance + keyboard), FR-23 (one chips notation with `+N`), NFR-9, NFR-10. |
| L1 no default mode stated | Minimal is the factory default (`thread_store.h:194`); §4.5 says so. |
| L2 no glossary/legend | §1.1. |
| L3 traceability gaps | §12 rows widened (FR-16 → T9/T12, FR-13 → T13, `/c` → T1, FR-19 → T11). |
| L4 pseudocode names collide | `ownerOpen`, `childOpen`, `segmentVisible`, `groupOpen` (§7.5) — one vocabulary, and the view no longer keeps a competing bit. |
| L5 compat one-directional | §8 table has both directions. |
| L6 notation drift | Chips are `[bash] [grep]` everywhere (one helper, FR-23); the arts say which profile they show; `·` is the pixel separator, `-` the grid's. |

### 0.3 Two review points not accepted (with evidence)

1. **H5 (step count is "-3").** `stepCount(const TypedThreadStore&, turnId)` walks
   `store.blockAt(turn.id, k)` (`store/typed_thread_store.cpp:167-196`) — the
   **wire store**, before the projection exists. DD-9's suppression and the
   body-less drop happen in `ThreadStoreAdapter::syncTyped`, one layer above, so
   they cannot change the number. The only input that moves is
   `blockDisplayable()`, i.e. the hidden flag: **−2 per hidden settled call**
   (the `Ok` weight). This revision keeps −2, states the mechanism, and turns it
   into two tests (default config → −2; `hiddenTools = []` → unchanged).
2. **H4 (content must not fold at settle in maximal).** The investigation's
   approved model folds a batch the moment its last call returns (§5.2's state
   table: "a settled batch, inside the open group → `> 4.1 s · [chips]`"), and
   §5.3 says the content is the preamble that "folds away with the batch
   afterwards". A single lifetime would leave a paragraph on screen under a
   *folded* owner row during tool execution — the "text with no owner" shape the
   design exists to remove. This revision therefore keeps the two lifetimes,
   states them as one rule with a mode parameter (§7.5), and records the
   single-lifetime variant as sign-off item R-9 because it is a UX call, not an
   architecture call.

---

## 1. Purpose, scope, and vocabulary

The thread view has two modes that disagree about what "work" is. Minimal hides
the assistant's streamed text; maximal draws every tool card of every response
open forever and draws the thinking nowhere. This design replaces both fold
models with **one owner per LLM response** — an `AGENT` row carrying the
response's duration and its tool chips — whose fold is **derived** from
liveness, and it moves the last piece of tool knowledge (which tools are
displayable) onto the wire.

### 1.1 Glossary and art legend

| Term | Meaning |
|---|---|
| **turn** | one user query and everything the agent did for it; one `TuiTurn`, one group |
| **response** | one model reply inside a turn; its blocks share `batchId = <turnId>#<n>` |
| **batch** | the projection's bucket for one response |
| **owner row** | the synthetic `AGENT` row that represents a response; the fold owner |
| **child** | a row owned by an owner row: `Thinking`, the content segment, one card per call |
| **body segment** | the header-less row that draws the response's content |
| **settled** | a batch that has ≥1 tool call and none of them still live |
| **live tail** | the newest batch of a turn whose state is `Running` |
| **pin** | an absolute user expansion choice, keyed by row/group id |
| **live edge** | minimal's chips line / `Thinking` shadow line under a folded group |
| **`v` / `>`** | art legend: disclosure open / disclosure closed |

Two render profiles: **grid** (character cells, `LC_ALL=C` safe, `" - "`
separator) and **pixel/GUI** (`" · "` separator). Arts below show the pixel
form; the grid draws the same shape with `-`.

### 1.2 In scope / out of scope

In: the C++ typed store projection and row model, the thread view and both
profiles, the D emitter's turn lifecycle / wire flags / replay parity, the C++
test data, scenes and goldens, the mode documentation.

Out: the console formatter's layout (only emission order), any new `BlockKind`,
any new wire event, any per-response emitter branching, persistence (the typed
store is in-memory; batch ids are not persisted).

---

## 2. Context and defect register

D emits, per turn: one `Prose` block with `meta = "user"` (the query, batch
`<turnId>#q`), then per response a batch `<turnId>#<n>` of reasoning / content /
tool blocks, then the `Answer` block and the turn's close. Vision and notice
blocks ride the query batch. C++ projects in three steps: flat rows
(`ThreadStoreAdapter::syncTyped`), the fold derivation (`blockExpanded` /
`expanded` / `groupMinimal`), the draw (`renderThreadBody` + `blockHeaderText`).
Every symptom comes from the first two; the renderer only draws what the
derivation says. The full evidence per symptom is the investigation §3–§4 and
§13 here.

| # | Defect | Evidence |
|---|--------|----------|
| D1 | `Thinking` is the one kind excluded from maximal's open default | `thread_store.cpp:659-660`, `:695-696` |
| D2 | No batch-liveness logic: mode + kind are the only inputs | `thread_store.cpp:659-661` |
| D3 | Cards are independent rows; nothing ties them to the row above | `views/thread.cpp:568-575, 654-664` |
| D4 | A maximal group draws open always | `thread_store.cpp:645-646` |
| D5 | The live `Thinking` shadow line draws twice in maximal | `views/thread.cpp:503-556` |
| D6 | An empty content block draws a bare `AGENT` row | `typed_thread_store.cpp:11-18`; `ui.d:777-781` |
| D7 | The streamed content is invisible in minimal (regression vs master) | golden `streaming-live-100x30.txt` |
| D8 | A `task_done` response repeats its text three times in maximal | `config.d:637` vs `typed_thread_store.h:48-57` |
| D9 | Batch rows reorder between live and replay | `ui.d:775-783` vs `package.d:1075-1087` |
| D10 | The test seeder gives every call its own batch id | `test/typed_seed.h:151-163` |
| D11 | The chips line draws under any folded group | `views/thread.cpp:671-680` vs `:714` |
| D12 | A failed / stuck run never closes its turn | `package.d:970-974`; `agent/package.d:612-657` |
| D13 | The orphan path resurrects a landed live call | `package.d:1125-1140` + `ui.d:793-799` |
| D14 | Dead state (`thinkingOpen`, `batchTools`) | `ui_state.h:256`; `thread_store.cpp:904`; `model/block.h:123-126` |
| D15 | A synthetic `Assistant` owner would draw a role rule, not a disclosure mark, and every row is `expandable` | `views/blocks.cpp:161-166, 308`; `thread_store.cpp:258` |

The height-reservation worry is **not** a constraint: `measureHint()` feeds
`cell.h` (`views/thread.cpp:454`) and no layout decision reads it; the drawn
height replaces the estimate after the first draw.

---

## 3. Requirements

### 3.1 Functional (MoSCoW)

| ID | Requirement | Priority |
|----|-------------|----------|
| FR-1 | Every response that produced visible work draws exactly one owner row, labelled `AGENT` when open, carrying its duration and chips; the row has a **disclosure mark**, takes focus, toggles by click and by Space/Enter, and owns its children's visibility | Must |
| FR-2 | Children draw in chronological order — reasoning, content, cards — identical between live and replay | Must |
| FR-3 | A batch's owner is open only while the batch is the live tail and unsettled; once settled it folds to `> 3.2 s · [chips]`; re-opening it shows its children with thinking and cards folded and the content drawn | Must |
| FR-4 | The thinking body is visible while it streams | Must |
| FR-5 | Minimal shows the newest live batch's content row under the folded group header, until the turn closes | Must |
| FR-6 | The group draws open while its turn is live in maximal and folds to `WORK · N steps` when the turn ends; minimal never opens a group by itself | Must |
| FR-7 | Pins are absolute and survive the default changing under them | Must |
| FR-8 | A folded owner row drops the role word: `> 4.1 s · [bash] [grep]` | Must |
| FR-9 | A batch with any failed call says `FAIL` on the folded header, in the failure ink | Must |
| FR-10 | Tool displayability comes from the wire: a hidden tool draws nothing and weighs 0, whatever its name and whatever config says | Must |
| FR-11 | The turn closes on every run exit (`Interrupted` without a final answer); `/c` re-opens it; no block status is ever fabricated by the UI | Must |
| FR-12 | The `Answer` row is the only copy of the answer text once the turn closes | Must |
| FR-13 | Header composition is width-aware: chips drop before the time, the label never drops; chips drop one at a time so `[bash] +3` survives a narrow row | Should |
| FR-14 | Replay reconstructs the same rows as live, duration included | Should |
| FR-15 | The minimal live edge never draws in maximal and never draws under an older group | Should |
| FR-16 | Body segments stay mark-and-copy-able in the pixel profile; the block-menu/`y` copy reaches the content through the owner | Should |
| FR-17 | The agents drill-down mount inherits the same fold and draws identical shapes | Should |
| FR-18 | Empty content/reasoning rows never draw a header of their own | Should |
| FR-19 | `lineCountEstimate` counts the rows that will draw | Could |
| FR-20 | The console formatter sees the same intra-batch order as replay | Could |
| FR-21 | Two modes, two content lifetimes, one rule: a content segment is visible while its owner is open, **or** while its batch is the live tail in minimal (R-9) | Should |
| FR-22 | The owner is the copy target for its batch's content, and the "copy the answer" walk never targets an owner whose batch has no text | Should |
| FR-23 | The owner's chips use the same composition helper as the minimal chips line: `[bash] [grep]`, `+N` past three, no duplicates | Must |

### 3.2 Non-functional

| ID | Requirement |
|----|-------------|
| NFR-1 | **Determinism**: 100 goldens stay byte-exact; every visual change is pinned by the golden of the task that made it |
| NFR-2 | **One source of truth per fact**: liveness derived from turn state + batch position + call status; displayability from the wire flag; visibility from one adapter predicate; the user's answer from the pin map. No fabricated status |
| NFR-3 | **No knowledge of D's vocabulary in C++**: no `task_done`/`pipeline_output` string, and **no parsing of the `batchId` format** |
| NFR-4 | **Additive ABI only**: one flag bit + the version marker; no new `BlockKind`, no new message |
| NFR-5 | **Cost**: `blockUpsert()` stays O(1); the projection stays one linear pass per version bump; a frame's fold read is a map probe plus a precomputed bit |
| NFR-6 | **Both profiles**: every shape draws correctly on the grid and in the pixel profile, at 40 and 100 columns |
| NFR-7 | **No mode-dependent emission**: D never learns about minimal/maximal |
| NFR-8 | **No new runtime logging**; new invariants are test-visible or debug asserts |
| NFR-9 | **Measured cost**: the existing scaled-thread cost test (`test_thread_window`) must stay within its current budget with the batch projection and `blockExpanded`'s new form |
| NFR-10 | **Green at every boundary**: the full suite (D + C++ + goldens) passes at the end of every task, not only at the end of the plan |

### 3.3 Constraints

1. D and C++ are one binary; the ABI stays documented and additive
   (`TUI_API_VERSION 14` → 15).
2. The `task_done` answer arrives as an `Answer` block; a card for it is not
   needed.
3. The test harness screen is 100x30, so scenes fit six responses.
4. No persisted state, no migration; pins are session state.
5. `test_streaming.cpp:660` ("the hint is the estimate and does not move with
   the fold") stays green untouched.
6. Minimal is the **factory default** mode (`thread_store.h:194`).

---

## 4. Architecture

### 4.1 Ownership

```
         D (emit)                      C++ (derive)                        draw
 ┌───────────────────────┐   ┌──────────────────────────────┐   ┌────────────────────┐
 │ run loop + agent      │   │ TypedThreadStore             │   │ renderThreadBody   │
 │ stream ticks          │──▶│  turns + blocks (wire mirror)│──▶│  row loop          │
 │ step walk / replay    │   │ ThreadStoreAdapter           │   │ renderBlock()      │
 │ turn close            │   │  groups → batches → rows     │   │ renderBlockBody()  │
 └───────────────────────┘   │  + one visibility predicate  │   └────────────────────┘
   owns: what happened          owns: grouping, order, fold,        owns: chrome, ink,
   + the Hidden flag            and whether a row draws             indent, rail, width
```

- **D owns the facts**: blocks, ids, batch ids, statuses, durations, and whether
  a tool is displayable.
- **The adapter owns the shape and the visibility**: groups → batches → rows,
  the invented owner row, the canonical order, every fold *default*, and the
  single `visible` answer the view obeys.
- **The view owns the pixels**: chrome, ink, indent, rail, width fitting.

The load-bearing rule: a row's visibility and openness are **derived reads** of
one fact set (turn state, batch position, call statuses) plus the user's pin —
never a second copy of D's semantics, never a status the UI wrote, and never a
decision made in two places (B1's lesson: the exception list moves into the
adapter).

### 4.2 Control flow

```
runAgent
 ├─ emitTurnStartTyped(query)                 turn=Running, query Prose (#q)
 ├─ runToCompletion → process()
 │    ├─ stream tick   → content#N, reasoning#N, per-call Tool Pending (batch <turn>#<n>)
 │    │                 (scope(exit): streamMessageDone → recorded calls Running)
 │    └─ step walk     → per response: reasoning + content + one Tool per call
 │                       (Ok/Failed + duration), Answer + turn=Done on task_done
 └─ run exit: stopped            → emitStopTyped (calls Cancelled, turn Interrupted)
              no final answer    → emitRunEndedTyped (turn Interrupted, liveCalls cleared)  ← NEW
              final answer seen  → already Done
```

Replay (`emitReplayTyped` → `buildTypedTurns`) is one snapshot: historical turns
`Done`, every response emitting reasoning + content + calls.

### 4.3 Data flow of one maximal turn

```
wire block                     projection
 query   Prose #q          →   queryRow      (turn-level, above the header)
 vision  Prose #q          →   plain row     (never batch-owned)
 notice  Notice #q         →   plain row     (never batch-owned)
 content#0  Prose t#0      ┐
 reasoning#0 Reasoning     ├→  batch t#0: owner row + children (reasoning, content, cards)
 call1      Tool t#0       ┘
 content#1  Prose t#1      ┐
 notice#2   Notice #q      │   the notice lay between two rows of t#1
 call2      Tool t#1       ┘→  batch t#1 block, then the notice as a plain row after it
 answer     Answer t#k     →   answerRow     (turn-level, below the group)
```

**Placement rule (H8):** a batch's block is emitted at the index of its **first
row**; rows of the same batch that appear later are consumed by the block; a
plain row that lay between them is emitted **after** the block. Without this the
two candidate orders ("first row wins" vs "insertion order") differ and no
invariant can see it.

### 4.4 Design decisions

**DD-1 — The fold owner is the response (`AGENT`).** A response always has a
role, so the owner always exists — the case "Thinking owns the batch" could not
cover (a response with calls and no reasoning). *Rejected*: group owns the fold
(no per-response granularity); `Thinking` owns it (absent when no reasoning).

**DD-2 — Liveness is derived, never written.** `batchLive = turnLive && isNewest
&& !settled`. **No `Cancelled` is written into the store** — a fabricated
terminal state later reads as truth. *Rejected*: flip stale `Running` rows to
`Cancelled` in C++ (the old D11 plan).

**DD-3 — Displayability is a wire flag.** `TuiBlockFlag_Hidden = 8`, set at one
D-side choke point from `LlmConfig.isHiddenTool()` (both name axes). All three
hard-coded lists die (`config.d:637`, `ui.d:609`, `typed_thread_store.h:57`).

**DD-4 — Batches are buckets keyed by `batchId`; a batch is a *response* batch
iff it is not the batch carrying the turn's query row.** No string parsing of
`<turnId>#<n>` (NFR-3): the projection already identifies the query row
(`syncTyped` computes `queryId`), and the query batch is by construction the
batch of that row (vision and notices ride it). Consequence: a turn whose query
row is missing simply has no non-response batch, and every batch gets an owner —
correct either way.

**DD-5 — Canonical child order = reasoning, content, cards**, regardless of
insertion order (live emits content first, replay reasoning first). Removes D9 in
the projection; the D-side reorder stays optional console parity.

**DD-6 — Pins are absolute and always stored.** `map<id,bool>` for rows and for
groups; a write stores the user's answer verbatim, including when it equals the
current default. *Rejected* (from rev 1): "drop the entry when it equals the
default" — that is a relative encoding again and loses the user's last explicit
instruction the moment a default moves. Pruning happens only at
`tuiThreadClear()` and on turn eviction; T24 adds the "collapse all" escape.

**DD-7 — The content is a header-less body segment.** `bodySegment = true`, no
header, no hit box, drawn by the view with `renderBlockBody()` at the batch
indent. `renderBlock()` returns a default answer and no `drawBody` when
`rect.h <= 0`, and a header-less row has no height to claim. *Rejected*: the
content as the owner's own body (cheaper, but loses the chronological order the
user asked for).

**DD-8 — The owner carries the response's duration; the `Thinking` row carries
neither duration nor count.** D stamps `durationMs` at the step (0 on the tick),
so the owner's header has no time while the response is live and gains it when
the response settles. The projection therefore reads the batch's content-block
duration **before** dropping a body-less content row, and **zeroes the `Thinking`
row's `timingS`** (otherwise the duration draws twice — the current test
`test_blocks.cpp:618` pins `Thinking - 3.2 s`, and moves). Never fall back to
`max(call durations)`.

**DD-9 — The `Answer`'s own batch suppresses its content child.** D names the
batch on the `Answer` block (`finalAnswerBatchId`), so the projection knows it
without heuristics and the answer text appears once. Trade-off: a final response
whose preamble differs from its answer loses that preamble (sign-off R-3).
*Note*: this drop lives in the projection, **above** `stepCount()`, so it does
not move `WORK · N steps` (see §0.3).

**DD-10 — Minimal keeps the live tail's content; maximal keeps it inside the
batch.** One predicate with a mode parameter (§7.5): `ownerVisible && ownerOpen`
in maximal, `+ turnLive && isNewest` in minimal. Sign-off R-9 records the
single-lifetime alternative.

**DD-11 — The live edge draws only in minimal style, for the newest group
only.** The chips line and
the `Thinking` shadow line exist because the minimal fold hides the rows; in a
drawn (maximal-style) group the rows are on screen. Both sites are guarded by
`index == drawn.back()`.

**DD-12 — `stepCount()` stays flag-driven.** It skips `!blockDisplayable()` rows,
so hiding `task_done` lowers the count by 2 per hidden settled call. Accepted:
the count then describes what the opened group shows.

**DD-13 — The owner is the first row the C++ side invents.** Id
`batch#<batchId>` (pin key), `key = hash(id)`, `kind = Assistant`,
`ownsBatch = true`, **`expandable = true`**, `hasBody = false`,
**`copyable = batch has content`**. The wire's ids are call ids plus `query` /
`answer` / `content#N` / `reasoning#N` / `vision#N` / `notice#N`, so the
namespace cannot collide (assert) and a per-turn uniqueness assert catches the
hash residual. `expandable` must be decided **per row** — today every row is
`expandable = true` (`thread_store.cpp:258`), which makes §7.2's guard vacuous.

**DD-14 — `groupMinimal()` conflates style with openness and is replaced by two
functions.** `groupOpen(g)` = pin ?? (maximal && turnLive) decides the fold;
`groupStyleIsMinimal(g)` = `mode == minimal && !groupOpen(g)` decides the *shape*
(rows behind a header, un-indented cards, the live edge). An open minimal group
therefore draws maximal-style — the shape today's code promises ("a minimal batch
the user opened draws maximal-style", `thread_store.cpp:467-475`) — while a folded
maximal group draws maximal-style too, so it can never paint the minimal chips
line. Today's single function cannot express both.

**DD-15 — No owner row without a surviving child.** A batch whose every child is
dropped contributes no row (the sibling of D6).

**DD-16 — Visibility is one adapter predicate, not a view exception list.** Each
`BlockDesc` carries `visible`; the view skips `!visible` rows and does nothing
else. This is what makes B1/B3 impossible: the content segment's admission is a
clause of the same function that decides the fold. `expanded` still means "the
body of *this* row is open", which a body segment never has (I-11).

**DD-17 — Alternatives recorded and rejected.** (a) *The owner row on the wire*:
D emits one empty `Assistant` block per response. Rejected: D would then carry a
display-only row it cannot suppress (DD-15) or order (DD-5), the console
formatter would draw it, and replay would need the same rule anyway; the
invented row's cost is one namespace plus one assert, both pinned by tests.
(b) *`TuiBlockFlag_ResponseBatch = 16`*: machine-readable, but a second bit to
keep set on six upsert sites for a fact the projection already derives from the
query row (DD-4). Rejected on NFR-2, not on cost.

**DD-18 — Chips are composed by one helper, in both modes.** `[bash] [grep]`,
no duplicates, `+N` past three, composed identically by the owner header and by
minimal's chips line (which today builds `"[" + d.summary + "]"` per running
call). The investigation's arts show `[bash, grep]`; that notation is rejected
because the shipped minimal line cannot produce it without a second renderer.

### 4.5 Mode matrix

Minimal is the factory default (`thread_store.h:194`); both modes stay pinned by
goldens.

| | minimal | maximal |
|---|---|---|
| group style (shape) | minimal while folded, maximal-style once opened | maximal always |
| group default open | never | while the turn is live |
| group when the turn ends | closed | closed (`WORK · N steps`) |
| owner row drawn | only when the group is opened by hand | yes, when the batch has a surviving child |
| owner default open | n/a | live tail && unsettled |
| children visible | when the group is opened by hand, via the owner | owner visible && owner open |
| children default open | n/a | live tail && unsettled |
| content while the batch is the live tail | drawn under the folded header | drawn inside the open owner |
| content once settled | drawn until the turn closes, then folds | folds with its owner |
| live edge | chips line while a call runs; `Thinking` shadow while reasoning streams and no content yet, **only while the group is folded** | none |

### 4.6 Target shapes

Maximal, mid-turn (the live batch open, settled ones as log lines):

```
 YOU
 refactor the whole tree, module by module
v WORK · 23 steps
> 4.1 s · [bash] [grep]
> 2.7 s · [read_file] [read_file]
v AGENT · [execute_command]           <- live: no duration yet
  v Thinking
     rebasing the parser onto the new loader ...
  Next I will move the loader out of core.
  v TOOL  execute_command
      dub test
```

Maximal, at rest:

```
 YOU
 refactor the whole tree, module by module
> WORK · 23 steps
 ANSWER
 the refactor is done: ...
```

Maximal, group re-opened by hand:

```
v WORK · 23 steps
> 4.1 s · [bash] [grep]
> 2.7 s · [read_file] [read_file]
> 1.2 s · [bash]
```

Maximal, one batch re-opened by hand — thinking and cards folded, content drawn:

```
v AGENT · 4.1 s · [bash] [grep]
  > Thinking
  I moved the loader out of core, then ran the suite.
  > TOOL  bash · 1.1 s
  > TOOL  grep · 0.8 s
```

Minimal, while the model writes (the regression fix):

```
 YOU
 what is the answer
> WORK · 2 steps
 The answer is **still** arriving. A second paragraph, so the
 body has more than one line.
```

---

## 5. Components

Task ids refer to §11.

### C1 — D: run-end turn close and the ledger

- `emitRunEndedTyped(bool stopped)` used by **all** exits: stopped → `Cancelled`
  flips + `Interrupted`; ended without a final answer (`unknownFailure`,
  `networkFailure`, `agentStuckInLoop`) → `Interrupted` only, plus
  `ctx.liveCalls = null`.
- Called in `runAgent` after `runToCompletion`, in place of the bare
  `if (stopped) emitStopTyped()`.
- `/c` resume: when the loop is forced, `typedCtx_` non-null and the turn
  `Interrupted`, upsert the turn `Running` before the turn-start guard.
- `clearLiveCall(callId)` on the orphan branch.
- *Not* in the step walk: `processResult`-level assertions ("no final answer →
  the turn stays open") stay valid; only run-level turn-upsert counts move.

### C2 — D: the `Hidden` flag

- `TuiBlockFlag_Hidden = 8`; `TUI_API_VERSION` → 15 (documentation-only marker).
- One helper `toolBlockFlags(name)` = `kToolBlockFlags | (isHiddenTool(name) ?
  Hidden : 0)` — the **only** way a tool block's flags are built, used at every
  upsert: live tick, stream-done flip, step walk, orphan result, stop flip,
  replay. Make it a type/accessor, not a convention, so a future upsert site
  cannot forget it.
- Delete the console's `kHiddenToolNames` (`ui.d:609`).

### C3 — D: replay parity

- `buildTypedTurns`'s `ToolMessage` branch emits the content block next to the
  reasoning block, unconditionally — it carries the response's `durationMs`.
- Optional, out of this task's acceptance: emit reasoning before content in the
  live tick (T25).

### C4 — C++: displayability from the flag

- `blockDisplayable()` reads `flags & TuiBlockFlag_Hidden`; delete
  `kHiddenToolNames` and its test list; `stepCount()` keeps skipping hidden
  blocks.

### C5 — C++: the batch projection (`syncTyped`)

1. Keep the turn-level lift (query / answer). Plain rows (notices, vision,
   pre-batch blocks) stay group-level.
2. Bucket the rest by `batchId`, in insertion order (I-7), excluding the batch
   that carries the query row (DD-4).
3. Per batch compute: `hasReasoning`, `hasContent`, the call list (title,
   status), `anyFailed`, `allSettled`, `isNewest`, `chips`, `failedCalls`, and
   `durationS` **before any drop** (DD-8).
4. Emit the owner row at the batch's first row index, then its children in
   canonical order (DD-5), contiguous (I-3); the placement rule of §4.3 applies
   to interleaved plain rows.
5. Drop body-less content/reasoning rows, the answer batch's content child
   (DD-9), and an owner with no surviving child (DD-15).
6. `stepCount()` untouched: it counts *wire* blocks (the hidden flag is its only
   input; §0.3).

### C6 — C++: the fold and the visibility predicate

One function answers both questions (DD-16). The **batch facts** (`turnLive`,
`isNewest`, `allSettled`) are computed once in the projection; the **pin
lookups and the final `visible`/`expanded` are resolved in `blockAt()`**, which
the view calls once per row per frame — `blockAt` already calls
`blockExpanded(k,b)` today (`thread_store.cpp:588`), so a pin click takes effect
on the next frame without a store version bump:

```
resolveVisible(group, row, pins)   // in blockAt()
resolveOpen(group, row, pins)      // in blockAt(), stored in out.expanded
```

- `groupOpen(g)` = pin ?? (mode maximal && turnLive).
- `styleIsMinimal(g)` = `mode == minimal && !groupOpen(g)` — an opened minimal
  group draws maximal-style (DD-14).
- `ownerOpen(b)` = pin(ownerId) ?? batchLive(b).
- `childVisible(b)` = `ownerVisible(b) && ownerOpen(b)`; `childOpen(b,r)` =
  pin(r.id) ?? batchLive(b) — **no `&& batchAutoOpen` on the child's visibility**
  (B2).
- `bodySegmentVisible(b)` = `ownerVisible && ownerOpen` **or** (`styleIsMinimal(g)
  && turnLive && isNewest`) — the segment never reads its own `expanded` (B3).
- `plainVisible` = `groupOpen(g)`.
- `setBlockExpanded`/`setExpanded` store absolute pins (DD-6).
- Retire `typedRowOpen_`/`groupOverrides`/`openWorkGroups`/`thinkingOpen`/
  `batchTools`; `groupMinimal()` replaced by `groupOpen()` + `groupStyleIsMinimal()`.

### C7 — C++ view: the row loop

- Skip a row iff `!desc.visible`. The loop keeps **no** nesting state and no
  exception list: the adapter has already folded the owner's openness into each
  child's `visible` (I-5), and the only thing a child takes from its owner is the
  shared indent and rail.
- The indent predicate is "is a `batchOwned` row", so thinking, content and cards
  share one indent and one rail.
- A segment admitted by the minimal clause draws with no owner row on screen —
  that is the FR-5 case and must be a test, not a hope.

### C8 — C++ view: header composition

- `blockHeaderText(const BlockView&, const BlockDraw&, const ui::Metrics&, float availUnits)`.
  The metrics keep `textGrid` alive (grid `" - "`, pixel `" · "`); both the
  measure and the draw path pass the same `availUnits` from
  `ui::textUnits(m, text)`.
- Order: label → duration → chips. Drop chips **one at a time from the right**
  (`[bash] [grep] [read]` → `[bash] [grep] +1` → `[bash] +2`) before dropping the
  duration; the label never drops. Folded owner rows start with the duration.
- Ink: owner label `roleAgent`; chips `textMid`; `FAIL` `stateDanger`; the
  disclosure mark `textLow` (focused: `accent`), matching `views/blocks.cpp:305`.
- `FAIL` paints from `failedCalls > 0` on an owner row as well as
  `state == Failed` on a card, after the fitted text.
- The `Thinking` header carries no count and no duration (DD-8).

### C9a — C++: the disclosure affordance

`blockHasDisclosure()` becomes descriptor-based and returns true for
`BlockKind::WorkGroup | Thinking | ToolStep` **and `desc.ownsBatch`**
(`views/blocks.cpp:161-166, 308`). Without it the owner is clickable but shows a
role rule instead of an open/closed mark (D15).

### C9b — C++: the `Assistant` consumer audit

`BlockKind::Assistant` now also means "batch owner, no body of its own". Audit
and pin in tests:
- `views/palette.cpp:167-172` `blockIsAnswerRole()` must exclude `ownsBatch`
  rows, and `lastAnswerBlock()` must accept the owner **only** as a copy target
  for a non-empty batch content (`copyable`), so "copy the answer" cannot target
  a calls-only batch.
- `streamHasText()`/`streamHasThinking()` (`thread_store.cpp:408-431`) are safe
  today (both require a non-empty body); keep them as regression tests.
- `blockAt`'s label switch, `blockInk`, `defaultBlockLabel` — no change expected,
  asserted by test.

### C10 — C++: the body segment

A `bodySegment` row draws through `renderBlockBody()` at the batch indent, never
through `measureBlockHeader()`/`renderBlock()`: no header, no hit box, no copy
chip, no menu, and no `expanded` of its own (I-11). Selection stays rich_md's
(pixel profile); the copy reaches the text through the owner (FR-22).

### C11 — C++ view: the live edge

Both sites become minimal-only and newest-group-only: the fold case under the
header gains `index == drawn.back()`; the after-group site keeps its guard. The
chips notation comes from the shared helper (DD-18).

### C12 — test data, scenes, goldens

- `typed_seed.h`: `tool()` no longer bumps `liveBatch`; `nextBatch()` starts a
  response; new helpers: content-without-reasoning, calls-without-content,
  two-batches-in-one-turn, hidden tool, **notice between two rows of one batch**,
  **four calls (for `+N`)**.
- Re-key `livebatch`, `maxlive`, `maxcancelled`, `maxanatomy`.
- Scenes are created **before** the projection work (T6) so the shape exists as a
  failing test the projection drives to green.

### C13 — the second mount (agents drill-down)

`views/shell.cpp:1206-1220` binds its own `ThreadStoreAdapter`, so it inherits
the fold. Keep the two mounts' shapes identical, as a test.

### C14 — docs

`doc/tui_design.md` gains the modes chapter (glossary, fold, visibility rule,
live edge, pins, anchors); `doc/todo.md` carries the remainder.

### C15 — pin hygiene (T24)

Prune pins for evicted turns; one "collapse all" affordance that clears both pin
maps (M3).

---

## 6. Data design

### 6.1 Wire model (additive)

| Field | Change |
|-------|--------|
| `TuiBlock.flags` | `TuiBlockFlag_Hidden = 8` added to `TuiBlockFlags` |
| `TUI_API_VERSION` | 14 → 15, documentation only |
| everything else | unchanged — `batchId` already carries the response identity |

### 6.2 Projection model

```
Entry (per turn)                     BatchEntry (per batch)               BlockDesc (per row, from blockAt)
  GroupView view                       std::string batchId                  kind, key, group, label, summary
  bool stream        (kept: shell)     bool isNewest                        state, hasBody, expandable
  bool turnLive                        bool hasReasoning, hasContent        new fields: bodySegment, ownsBatch,
  bool turnMinimalStyle                bool anyFailed, allSettled           batchOwned, copyable, chips,
  std::vector<BlockEntry> blocks       int failedCalls; std::string chips  failedCalls
  BlockEntry queryRow/answerRow        double durationS                     computed per call: expanded, visible
  bool hasQuery/hasAnswer
```

`BlockDesc` additions: `visible`, `bodySegment`, `ownsBatch`, `batchOwned`,
`copyable`, `chips`, `failedCalls`; `batchTools` retired.

### 6.3 Identity and keying

| Thing | Key | Stability |
|-------|-----|-----------|
| group | the persisted `turnId` | turns are never re-created |
| row (pin key) | `typedBlockId` — a wire id, or `batch#<batchId>` for the owner | stable within a turn; batch ids are deterministic on replay |
| row (in-frame key) | `std::hash(typedBlockId)` | non-injective by construction; `BlockId` also carries `kind` |
| batch | `batchId` | **never parsed** (DD-4) |

Pins are keyed by id, never by row index: the row list churns while a response
streams.

### 6.4 State model

```
ChatTab / ThreadState
  std::map<std::int64_t, std::map<std::string, bool>> rowPins;    // groupId -> rowId -> open
  std::map<std::int64_t, bool>                        groupPins;  // groupId -> open
```

Absolute; written on every user toggle; pruned on `tuiThreadClear()` and on turn
eviction (T24). Replaces `typedRowOpen_`, `ChatTab::openWorkGroups`,
`ChatTab::groupOverrides`.

### 6.5 Invariants

| ID | Invariant | Enforcement |
|----|-----------|-------------|
| I-1 | A response batch has at most one owner row | one owner per bucket |
| I-2 | An owner exists iff the batch has ≥1 surviving child | emit gate |
| I-3 | A batch's children are contiguous | emit order + placement rule |
| I-4 | Children ordered reasoning → content → cards | emit order |
| I-5 | A child's visibility implies its owner is visible and open | visibility predicate |
| I-6 | No row id repeats within a turn | debug assert + unit test |
| I-7 | Bucket order equals insertion order | debug assert |
| I-8 | No block status is ever written by the C++ side | review + test |
| I-9 | Hidden blocks weigh 0, draw nothing, name nothing | `blockDisplayable` |
| I-10 | The `Answer`'s batch contributes no content child | projection drop |
| I-11 | A body segment is never gated on its own `expanded` | predicate shape + test |
| I-12 | `isNewest` = the last response batch of its turn in insertion order | projection |
| I-13 | A pin survives any default change | absolute pins + the default-flip test |

---

## 7. Interfaces

### 7.1 Wire (C ABI)

Additive only: `TuiBlockFlag_Hidden = 8`, `TUI_API_VERSION 15`.

### 7.2 Store adapter surface

| Member | Contract |
|--------|----------|
| `blockCount(k)` | rows that exist: the group header + plain rows + owner rows + children + segments. **Restated**: it is no longer "1 + displayed wire blocks" — the fixtures in `test_typed_thread_store.cpp:304, 327, 331` move. |
| `blockAt(k,b)` | owner (`ownsBatch`), child (`batchOwned`), segment (`bodySegment`), plain row; **`visible` and `expanded` are answered by the adapter, not derived by the view**. The view **prefetches** the group's rows once per frame (`views/thread.cpp:476-483`), it does not probe per row. |
| `blockExpanded(k,b)` | the resolved openness (§7.5); identical to `desc.expanded` |
| `setBlockExpanded(k,b,open)` | stores an absolute row pin; `false` when the row is not `expandable` |
| `expanded(id)` / `setExpanded(id,open)` | group openness from `groupPins` |
| `groupStyleIsMinimal(g)` | `minimalMode && !groupOpen(g)` — the shape (DD-14) |
| `groupOpen(g)` | `groupPins` ?? (maximal && turnLive) |
| `blockText(k,b)` | the owner answers its batch's content body |
| `streamHasText()` / `streamHasThinking()` | unchanged; covered by C9b's tests |

### 7.3 View ↔ adapter contract

1. The view draws a row iff `desc.visible`; it does not consult `expanded` for
   visibility and does not keep an exception list.
2. `desc.expanded` decides only whether that row's own body/disclosure is open.
3. A `bodySegment` never calls `measureBlockHeader()`/`renderBlock()`.
4. **Evaluation order** (B1): visibility is computed in the adapter *before* the
   view sees the row, so "the owner is not drawn" and "the segment draws" are
   compatible by construction.

### 7.4 Header composition (pseudocode)

```
headerText(desc, draw, m, availUnits):
    parts = []
    if !desc.ownsBatch or draw.expanded: parts += label()   # a folded owner drops the word
    if desc.timingS > 0:                 parts += timing(desc.timingS)
    if desc.chips != "":                 parts += chips
    while textUnits(m, join(parts, sep(m))) > availUnits and chips has >1 token:
        shorten chips by one token        # "[bash] [grep] [read]" -> "[bash] [grep] +1"
    if still too wide: drop the timing
    return join(parts, sep(m))            # sep = " - " on the grid, " · " in the GUI
```

`FAIL` is painted after the fitted text from `failedCalls > 0` (owner) or
`state == Failed` (card).

### 7.5 Fold and visibility predicate (pseudocode)

```
turnLive(g)       = turn state == Running
batchLive(b)      = turnLive && b.isNewest && !b.allSettled          # I-12
groupOpen(g)      = groupPins[g] ?? (mode == maximal && turnLive)
styleIsMinimal(g) = mode == minimal && !groupOpen(g)   # the shape, not the fold

ownerVisible(b)   = groupOpen(g)                        # owners never draw under a minimal-style fold
ownerOpen(b)      = rowPins[ownerId(b)] ?? batchLive(b)
childVisible(b)   = ownerVisible(b) && ownerOpen(b)
childOpen(b)      = rowPins[childId] ?? batchLive(b)
segmentVisible(b) = (ownerVisible(b) && ownerOpen(b))
                    or (styleIsMinimal(g) && turnLive && b.isNewest)
plainVisible(r)   = groupOpen(g)
```

Consequences: a settled batch inside a re-opened group shows its owner folded;
clicking it opens the owner (`ownerOpen` true) with thinking and cards folded
(`childOpen` false) and the content drawn — §4.6's fourth art. A live batch whose
owner the user closes hides its segment too. A leaked `Running` call in a dead
turn is not live (the turn is not), so it cannot hold a batch open — which is
what makes "all calls responded" a safe trigger and removes the need for a
fabricated `Cancelled`.

### 7.6 D emitter contract

1. Every response's blocks share one `batchId`; a turn's query/vision/notice
   blocks share the query batch (the format itself is *not* a C++ contract).
2. Every response emits a content block, live and on replay, carrying
   `durationMs` when the response settled.
3. Every tool block carries `TuiBlockFlag_Hidden` iff `isHiddenTool(name)`.
4. Every run exit upserts a terminal turn state; `liveCalls` is empty afterwards.

---

## 8. Cross-cutting

**Performance.** One pass per version bump (bucketing is O(rows)); the per-frame
fold read is a pin probe plus a stored bit. The frame no longer derives
visibility per row from three maps. NFR-9 and T23 put a number on it: the
existing scaled-thread cost test runs the new `sync` + `blockExpanded` against a
thread ten times longer and must hold its budget (use `unordered_map` for the pin
maps if it does not).

**Determinism.** 100 goldens, byte-exact, ink runs included. Every shape-changing
task regoldens what it moves, with a one-line reason; `--update` only after a
human has looked at the capture.

**Resilience.**

| Failure | Behaviour |
|---------|-----------|
| `unknownFailure` / `networkFailure` mid-stream | turn closes `Interrupted`; calls keep their last real status; nothing draws live; the fold settles |
| `agentStuckInLoop` | same close; the console message still explains |
| user stop | unchanged (`Cancelled` flips, `Interrupted`) |
| `/c` resume | turn `Running` again; the new batch is the live tail |
| orphan tool response | result-only block in its own batch; its live call cleared |
| session switch / replay | snapshot rebuild; the live turn is dropped as today |
| turn eviction | group and its pins go together (T24) |

**Observability.** No new logging. Two debug asserts (I-6, I-7) exist for
development builds, matching the tree's convention (`views/thread.cpp:727`). A
grep gate in the test tree asserts that no retired identifier and no tool name
returns (`thinkingOpen`, `batchTools`, `kHiddenToolNames`, `task_done`,
`pipeline_output` in `cpp_tui/`).

**Compatibility.**

| Emitter | UI | Result |
|---------|----|--------|
| old (no flag) | new | nothing hidden: `task_done` cards reappear; no owner change (DD-4 needs no flag) |
| new | old | the flag bit is ignored: same shape as today |
| new | new | the design |

**Rollback.** No runtime toggle is added: the two modes already bracket the
behaviour (minimal keeps the fold closed; maximal can be pinned), the `Hidden`
flag is config-driven (`hiddenTools = []` restores the `task_done` card without a
rebuild), and a shape regression in use is a `git revert` of one branch, not a
half-on feature. Adding a third switch would double the golden matrix for a
transitional state nobody would test.

**Accessibility / selection.** Segments render through the same rich-text path,
so the pixel profile keeps mark-and-copy; the grid has no selection today and
gains none (the terminal gives `Ctrl+C` to `SIGINT`). The copy affordance lives
on the owner (FR-22), which is reachable by keyboard.

**Security.** No new input parsing (explicitly: no `batchId` parsing), no new
external data.

---

## 9. Risks and sign-off

| ID | Risk | Mitigation / status |
|----|------|---------------------|
| R-1 | Depth: group → batch → thinking → card | One rule (§7.5), defaults hide the depth, documented |
| R-2 | Invented owner identity | `batch#` namespace, per-turn uniqueness assert (I-6), test |
| R-3 | DD-9 loses a differing preamble | Recorded; the `Answer` is the response's message. **Sign-off** |
| R-4 | DD-12 changes `WORK · N steps` (−2 per hidden settled call) | Tested with and without `hiddenTools`. **Sign-off** |
| R-5 | A turn ending without `task_done` folds its last text | Accepted; it is one click away, and the turn now closes |
| R-6 | Bucket-order assert may fire on unexpected emitter order | Release builds ignore it; the projection is defined either way |
| R-7 | The two mounts could diverge | T20 compares them in a test |
| R-8 | A fully hidden batch has steps but no rows | Follows from DD-12/DD-15; pinned by a golden |
| R-9 | **Content lifetime differs by mode** (§0.3) | Sign-off: keep the two lifetimes, or make it "live tail until the turn closes" in both (one predicate line + one golden) |
| R-10 | The owner's disclosure is a new chrome case | T13's golden at 40 columns and both profiles |

---

## 10. Verification

### 10.1 Gates

1. **Every task** ends with the full suite green: `dub test`, `make test_blocks
   && ./test_blocks`, `./test_streaming`, `./test_thread_window`.
2. **Every shape-changing task regoldens the goldens it moves, in that task**,
   with a one-line reason. No task lands with a known-red golden.
3. A golden diff must be explainable by the task that produced it.

### 10.2 Probes

- The four live probes from the investigation, re-run in both modes.
- A 40-column pass per profile for the header rule (`AGENT - 3.2 s` on the grid,
  `AGENT · 3.2 s` in the GUI).
- One run per exit path (T2), one replay comparison (T17).

### 10.3 Golden work

- **New scenes (T6, before the projection)**: `blocks-maxlong` (finished
  six-response turn: only `> WORK · N steps`), `blocks-maxlonglive` (the same
  turn mid-run), plus `blocks-maxnotice` (a notice between two rows of one
  batch) and a four-call batch for the `+N` chips.
- **Regolden, per task**: `blocks-maxanatomy`, `blocks-maxlive`,
  `blocks-maxcancelled`, `blocks-answerlanded`, `blocks-answerchip`,
  `blocks-livebatch`, `inspector-agents-drilldown`, and the minimal streaming
  family (`streaming-live`, `streaming-table`, `streaming-fence`,
  `streaming-thinking`).
- **New checks**: owner chips + duration + disclosure mark; a live batch open
  with thinking, content and cards; the last call folds it to `> 3.2 s ·
  [chips]` with no role word; a reasoning-less batch folds the same way; a
  content-only batch previews its first line; a failed call marks the folded
  header; a pin survives the default flipping (I-13); minimal keeps the live
  content row **with no owner row drawn** and hides it when the turn closes; the
  shadow line and chips line never draw in maximal; a hidden block weighs 0 and
  draws nothing; the query batch never grows an owner; an owner with no child is
  absent; no row id repeats; a re-opened settled batch shows its content;
  `stepCount` with `hiddenTools = []` is unchanged and with the default is −2 per
  hidden settled call.

### 10.4 Blast radius (the real one)

| Test | Why it moves |
|------|--------------|
| `test_blocks.cpp:264` `headerText` helper | the signature gains the metrics + width |
| `:594-596` separator pins | kept, extended to owner rows in both profiles |
| `:618` `Thinking - 3.2 s` | the Thinking duration retires (DD-8) |
| `:1305` `rowOf(..., "Thinking - 2.0 s")` | the duration is on the owner row now |
| `:1008` `Thinking - 1 tool call` | unaffected: the shadow line keeps its wording in minimal |
| `:1611` empty stream | becomes true for the right reason (the body-less drop); comment rewritten |
| row focus/copy identity checks (`BlockId{1, hash("content#1"), Assistant}`) | row indices and identities move |
| `test_typed_thread_store.cpp:304, 327, 331` | `blockCount`'s contract is restated |
| `test_streaming.cpp:660` | **untouched** |

---

## 11. System Design Tasks

Ordered; sizes S/M/L. Execute in order; every task ends green (NFR-10).

### Critical (P0)

1. **T1 — D: close the turn on every run exit (M)**
   - Acceptance: one `emitRunEndedTyped(bool stopped)` covers stop and every
     failure exit (`unknownFailure`, `networkFailure`, `agentStuckInLoop`); the
     turn reaches `Interrupted`; `ctx.liveCalls` is empty afterwards; `/c`
     re-upserts an `Interrupted` turn `Running`; the orphan branch calls
     `clearLiveCall()`; no block status is written that D did not observe; the
     `processResult`-level "turn stays open" assertions still pass.
   - Deps: none.

2. **T2 — D: run-exit probes (M)**
   - Acceptance: a fake-transport test drives `runAgent` through each exit path
     (final answer, stop, network failure, stuck loop) and asserts the emitted
     turn state **and** the resulting store shape (a closed turn holds no
     `Running` block); a replay of a finished run equals the live grid; one manual
     live run confirms the documented shape. This is the task that makes FR-11
     verified rather than counted.
   - Deps: T1.

3. **T3 — D: displayability on the wire (`TuiBlockFlag_Hidden`) (S)**
   - Acceptance: flag `8`, `TUI_API_VERSION` 15; `toolBlockFlags(name)` is the
     only constructor of tool flags and is used at the live tick, stream-done
     flip, step walk, orphan result, stop flip and replay; the console list is
     deleted; a recording-sink test walks all six paths; a seventh path cannot be
     written without the helper (type, not convention).
   - Deps: none.

4. **T4 — C++: displayability from the flag (S)**
   - Acceptance: `blockDisplayable()` reads the flag; `kHiddenToolNames` gone; no
     tool-name string in `cpp_tui/` (grep gate); `stepCount()` unchanged except
     that hidden blocks weigh 0; a unit test pins weight 0 and non-rendering.
   - Deps: T3.

5. **T5 — Test data: batch ids that match the wire (M)**
   - Acceptance: `typed_seed::tool()` does not bump `liveBatch`; all calls of one
     response share its batch id; `nextBatch()`; helpers for
     content-without-reasoning, calls-without-content, two batches, hidden tool,
     **notice between two rows of one batch**, **four calls**; the four existing
     scenes re-keyed.
   - Deps: T3.

6. **T6 — Scenes first: the shape exists as a failing golden (M)**
   - Acceptance: `blocks-maxlong`, `blocks-maxlonglive`, `blocks-maxnotice` and a
     `+N` chips scene exist with their *target* captures committed as the
     expected files; they fail before T7–T15 and are the acceptance surface for
     them. Human review of the captures happens here (before the implementation)
     and at T19 (after).
   - Deps: T5.

7. **T7 — C++ projection: buckets and the owner row (M)**
   - Acceptance: work rows bucketed by `batchId`, excluding the batch that
     carries the query row (DD-4, no id parsing, test with a malformed id proving
     it is irrelevant); `BatchEntry` carries the batch facts incl. `durationS`
     read before any drop; the owner row (`Assistant`, `AGENT`, `batch#<batchId>`,
     `ownsBatch`, `expandable`, `hasBody == false`, `copyable`) is emitted at the
     batch's first row; bucket-order assert; `make test_blocks` builds.
   - Deps: T4, T5.

8. **T8 — C++ projection: order, drops, placement (M)**
   - Acceptance: children emitted reasoning → content → cards, contiguous; the
     answer batch's content child dropped (DD-9); body-less content/reasoning
     dropped; an owner with no surviving child not emitted; the placement rule for
     interleaved plain rows implemented and pinned by `blocks-maxnotice`;
     `stepCount` provably unchanged by these drops (two config tests: default →
     −2 per hidden settled call, `hiddenTools = []` → unchanged).
   - Deps: T7.

9. **T9 — C++ model: row vocabulary, disclosure, consumer audit (M)**
   - Acceptance: `BlockDesc` gains `visible`, `bodySegment`, `ownsBatch`,
     `batchOwned`, `copyable`, `chips`, `failedCalls`; `expandable` is per row
     (owner true, segment false) so `setBlockExpanded`'s guard is real;
     `blockHasDisclosure` is descriptor-based and true for `ownsBatch`;
     `blockAt` answers the owner's `summary`, `copyable` and `blockText`; the
     `Assistant` consumers are audited and pinned (`blockIsAnswerRole` excludes
     owners; `lastAnswerBlock` copies through the owner only when the batch has
     text); the `blockCount` fixtures are updated to the restated contract.
   - Deps: T7.

10. **T10 — C++ fold: liveness, absolute pins, the visibility predicate (L)**
    - Acceptance: §7.5 implemented verbatim; `groupMinimal()` replaced by
      `groupOpen()` + `groupStyleIsMinimal()` (an opened minimal group draws
      maximal-style);
      `expanded()`/`blockExpanded()` read the predicate; `setBlockExpanded()`/
      `setExpanded()` store absolute pins and never drop them; the retired maps
      are gone; unit tests pin every cell of §4.5's matrix plus I-13 (a pin
      survives the default flipping) and B2's case (a re-opened settled batch
      shows its content with thinking and cards folded).
    - Deps: T7, T9.

11. **T11 — C++ view: the row loop obeys `visible` (M)**
    - Acceptance: the loop skips `!desc.visible` rows and holds no exception list;
      the indent predicate is `batchOwned`, one rail for thinking/content/cards;
      the prefetch path is kept (no per-row adapter probe); `lineCountEstimate`
      counts rows with `visible` (owner rows always, a closed batch's children
      not), so the first frame of a folded turn reserves the folded height.
    - Deps: T10.

12. **T12 — C++ view: the body segment (S)**
    - Acceptance: a segment draws through `renderBlockBody()` at the batch indent
      with no header measure, no hit box, no menu, no `expanded` (I-11); the pixel
      profile keeps mark-and-copy.
    - Deps: T11.

13. **T13 — C++ view: header composition, folded role word, FAIL, disclosure (M)**
    - Acceptance: `blockHeaderText(desc, draw, m, availUnits)` used by both paths;
      grid `-` / pixel `·` preserved; chips drop one token at a time, then the
      timing, never the label; both profiles pinned at 40 columns; the `Thinking`
      header carries neither count nor duration; `FAIL` from `failedCalls`; the
      owner draws a disclosure mark; ink per C8.
    - Deps: T9, T12.

14. **T14 — C++ view: minimal draws the live content with no owner on screen (S)**
    - Acceptance: with the group folded, the newest live batch's segment draws
      under the header (FR-5) and disappears when the turn closes; a test asserts
      the case explicitly ("the segment draws with no owner row drawn this
      frame"); `streaming-live-100x30.txt` shows the text.
    - Deps: T12.

15. **T15 — C++ view: the live edge is minimal-only, newest-only (S)**
    - Acceptance: the chips line (shared helper, DD-18) and the `Thinking` shadow
      line draw only in minimal style and only for the newest drawn group; a
      `Running` block in an older turn paints nothing; maximal draws neither.
    - Deps: T10.

16. **T16 — C++ view: anchors survive a batch folding in place (S)**
    - Acceptance: `test_streaming.cpp`'s hint test untouched and green; the
      group's anchor does not move when a batch folds mid-stream or when the live
      batch settles; a test pins the mid-stream case.
    - Deps: T11, T14.

### High (P1)

17. **T17 — D: replay parity (M)**
    - Acceptance: `buildTypedTurns`'s `ToolMessage` branch emits the content block
      with `durationMs`; replaying a finished turn reproduces the live grid row for
      row (the owner's duration included); `dub test` green.
    - Deps: T1, T7.

18. **T18 — C++: the answer batch suppresses its content (S)**
    - Acceptance: the batch the `Answer` names contributes no content child; a
      finished maximal turn shows `ANSWER` + text once; a golden pins it; R-3's
      trade-off recorded in this file.
    - Deps: T8, T6.

19. **T19 — Scene review and the new checks (M)**
    - Acceptance: a human reviews the four new captures and says they read well;
      every new check in §10.3 is implemented; `./test_blocks` reports its full
      count green; no golden changed without a reason.
    - Deps: T10–T18.

20. **T20 — The second mount stays identical (S)**
    - Acceptance: `inspector-agents-drilldown` regolded; a test compares the
      drill-down and the thread for the same turn (shape equality, not by eye).
    - Deps: T19.

21. **T21 — Documentation (M)**
    - Acceptance: `doc/tui_design.md` gains the modes chapter (glossary, the fold,
      §7.5's predicate, the live edge, pins, anchors); `doc/todo.md` carries the
      remainder; stale `file:line` anchors replaced by symbol names.
    - Deps: T19.

22. **T22 — Retired names and lying comments (S)**
    - Acceptance: `thinkingOpen`, `batchTools`, `groupOverrides`,
      `openWorkGroups`, `typedRowOpen_`, `kHiddenToolNames` gone and enforced by
      the grep gate; the comments that credit the store with behaviour it does not
      have (`test_blocks.cpp:1611`, `typed_thread_store.h:48-57`,
      `thread_store.cpp:652-661`) rewritten; `test_typed_thread_store.cpp`'s
      message matches the new `blockCount` contract.
    - Deps: T10, T13, T17.

### Medium (P2)

23. **T23 — Measured cost (S)**
    - Acceptance: the scaled-thread cost test covers `sync` + `blockExpanded` and
      holds its budget; NFR-9 passes with the new projection; if not, the pin maps
      move to `unordered_map` (measured, not guessed).
    - Deps: T11.

24. **T24 — Pin hygiene and "collapse all" (S)**
    - Acceptance: pins of evicted turns are pruned; a "collapse all" action clears
      both maps and is reachable from the keyboard; documented in C14.
    - Deps: T10.

25. **T25 — Console parity for the intra-batch order (S)**
    - Acceptance: the live tick emits reasoning before content; `dub test` green;
      no C++ behaviour depends on it.
    - Deps: T17.

### Low (P3)

26. **T26 — Grid copy of a body segment: decision, not code (S)**
    - Acceptance: recorded in `doc/tui_design.md` with the user's answer; no code
      unless the user asks; the GUI path (FR-16) already works.
    - Deps: T21.

---

## 12. Traceability

| Defect | Fixed by |
|--------|----------|
| D1 Thinking always folded | T7, T10 |
| D2 Tools never fold | T7, T10 |
| D3 Cards not part of the fold | T7, T11 |
| D4 Group draws open always | T10 |
| D5 Double `Thinking` line | T15 |
| D6 Bare `AGENT` row | T8, T19 |
| D7 Minimal hides the streamed text | T14 |
| D8 Three copies of the answer | T3, T4, T18 |
| D9 Live/replay reorder | T7, T17 |
| D10 Seeder batch ids | T5 |
| D11 Chips line under old groups | T15 |
| D12 Failed run never closes | T1, T2 |
| D13 Orphan resurrects a call | T1, T2 |
| D14 Dead state | T10, T22 |
| D15 Owner had no disclosure / every row expandable | T9, T13 |

| Requirement | Tasks |
|-------------|-------|
| FR-1 | T7, T9, T13 (disclosure, focus, keyboard) |
| FR-2 | T7, T17 |
| FR-3 | T10, T11 |
| FR-4 | T10, T13 |
| FR-5, FR-21 | T14, T10 |
| FR-6 | T10 |
| FR-7 | T10 (I-13) |
| FR-8, FR-9, FR-13 | T13 |
| FR-10 | T3, T4 |
| FR-11 | T1, T2 |
| FR-12 | T18 |
| FR-14 | T17 |
| FR-15 | T15 |
| FR-16, FR-22 | T9, T12 |
| FR-17 | T20 |
| FR-18 | T8 |
| FR-19 | T11 |
| FR-20 | T25 |
| FR-23 | T13, T15 |
| NFR-1 (determinism) | every shape task's regolden (§10.1) + T19 |
| NFR-2, NFR-3 | T7 (no id parsing), T10 (one predicate), T3/T4 (no tool names) |
| NFR-4 | T3 (one flag bit, version bump) |
| NFR-5, NFR-9 | T23 |
| NFR-6 (both profiles) | T13, T19 |
| NFR-8 (no new logging) | T22 (grep gate) |
| NFR-10 | every task's gate (§10.1) |
| `/c` resume (FR-11) | T1, T2 |

---

## 13. Evidence index (by symbol, with the line that was checked)

| Claim | Source |
|-------|--------|
| `responseBatchId` / `queryBatchId` / `contentBlockId` | `ui.d:632-646` |
| Live tick: content-then-reasoning, Pending calls, Running flip | `TypedStreamMessageUpdater::messageUpdate` / `streamMessageDone`, `ui.d:768-800` |
| Step walk: reasoning + tools, no content for `ToolMessage`; orphan result-only; Answer + Done | `emitStepTyped`, `package.d:1074-1157` |
| Replay: `Message` emits content, `ToolMessage` does not | `buildTypedTurns`, `package.d:118-165` |
| Only closes are stop and final answer | `runAgent`, `package.d:966-974` |
| `scope(exit)` flips the stream unconditionally | `agent/package.d:372-376` |
| Failure statuses exit without a close | `agent/package.d:612-657` |
| `hiddenTools` default + `isHiddenTool` dual axis | `config.d:637, 914-925` |
| Console hidden list | `ui.d:609` |
| Wire flags, `TuiBlock`, version | `tui_api.h:29, 648-653, 670-697` |
| `blockDisplayable` + name list | `typed_thread_store.cpp:11-18`, `typed_thread_store.h:57` |
| `stepCount` (reads the **store**, not the projection) | `typed_thread_store.cpp:167-196` |
| Flat row build, sentinels, `expandable = true`, estimate, stream bit | `syncTyped`, `thread_store.cpp:175-383` (expandable `:258`) |
| Fold derivation, pins, maximal-always | `blockExpanded`/`expanded`/`groupMinimal`/`setBlockExpanded`, `thread_store.cpp:460-705` |
| `streamHasText` / `streamHasThinking` (body-gated) | `thread_store.cpp:408-431` |
| Minimal is the factory default | `thread_store.h:194` |
| `thinkingOpen` dead | `ui_state.h:256`, `thread_store.cpp:904` |
| Row loop, prefetched rows, live edge, indent, rail, fold case | `renderThreadBody`, `views/thread.cpp:446-719` (prefetch `:474-482`) |
| `blockHasDisclosure` (kind-based today) | `views/blocks.cpp:161-166`, use `:308` |
| Disclosure vs role ink | `views/blocks.cpp:305-315` |
| Header composition, FAIL, body gating | `blockHeaderText`/`renderBlock`, `views/blocks.cpp:189-239, 259-262, 270, 299, 329-332` |
| Separator pins (`"YOU - 1.2 s"` / `"YOU · 1.2 s"`), `Thinking - 3.2 s`, helper signature | `test_blocks.cpp:264, 594-596, 618, 1008, 1305` |
| `blockCount` fixtures | `test_typed_thread_store.cpp:304, 327, 331` |
| `blockIsAnswerRole` / `lastAnswerBlock` | `views/palette.cpp:167-218` |
| Profile-correct text width | `ui::textUnits`, `ui/primitives.cpp:117-131`; `ui/text.h:64` |
| `BlockDesc` vocabulary | `model/block.h:111-145` |
| Entry / pins storage | `thread_store.h:92-93, 397-409, 434` |
| Seeder gives each call its own batch | `test/typed_seed.h:140-163` |
| Harness screen is 100x30 | `test_blocks.cpp:123-124` |
