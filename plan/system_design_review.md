# Review — `plan/system_design.md` (response-owned batch fold)

Reviewer stance: system-design review, not a code review. The question asked is
"will this task list, executed as written by someone who reads only this
document, produce the behaviour the user approved in
`plan/maximal_investigation.md`?" — plus the usual dimension pass
(requirements, architecture, data, interfaces, cross-cutting, verification,
documentation).

## 0. Verdict

**Approve the architecture, reject the document as implementation-ready.**
The projection/derivation/view split is right, the diagnosis-to-task traceability
is the best I have seen in this tree, and the scope discipline (additive ABI, no
new `BlockKind`, no tool names in C++, goldens as the shape contract) is exactly
what this codebase needs. But five internal contradictions mean that an
implementer following the text literally will either produce a feature the user
cannot reach (the minimal content row, the re-opened settled batch) or a
half-visible affordance (the owner row), and the task order makes the stated
per-task gate unsatisfiable. None of those are architectural; all are fixable by
editing this document.

Evidence base for this review: `plan/system_design.md` (read in full),
`plan/maximal_investigation.md` (read in full), and spot verification of the
load-bearing code claims against the indexed source (`store/thread_store.cpp`,
`store/thread_store.h`, `views/blocks.cpp`, `views/blocks.h`, `views/thread.cpp`,
`views/palette.cpp`, `test_blocks.cpp`, `test_typed_thread_store.cpp`). Where I
say "the code does X" below I quote the indexed source, not memory.

---

## 1. Strengths (keep these; they are what makes the doc reviewable)

1. **A defect register with evidence, not adjectives** (§2.2, D1–D14). Every
   symptom the user reported is reduced to a `file:line` and a mechanism. That is
   what allows a reviewer to disagree with a *decision* instead of with a
   *feeling*.
2. **Design decisions carry their rejected alternative** (DD-1 … DD-15). DD-1's
   rejection of "`Thinking` owns the batch" (a response with calls and no
   reasoning has no owner) is the correct load-bearing argument for the whole
   design, and it is stated in one paragraph.
3. **A single derived rule instead of stored state** (§4.1, NFR-2). "A row's
   openness is a derived read of one fact set plus the user's pin" is the right
   invariant for a projection, and I-8 ("no block status is ever written by the
   C++ side") is the right guard rail — it kills a whole class of the bugs the
   old D11 plan would have introduced.
4. **The fold has exactly one owner and the depth is bounded** (§4.5 mode
   matrix). Minimal and maximal differ in one boolean, and the doc says so out
   loud. That is a strong simplification relative to today's two unrelated fold
   models.
5. **Traceability in both directions** (§12): defect → task and FR → task. Rare
   and valuable; it is how I found the two traceability gaps in §7 below.
6. **Testable non-functional requirements.** "No tool-name knowledge in C++",
   "no new `BlockKind`", "additive ABI only", "goldens byte-exact" are checkable
   by a reviewer without a benchmark. NFR-8's ban on new logging is a
   deliberately austere choice and is honest about it.
7. **Honest risk register** (§9), including two items marked "needs sign-off"
   (R-3 DD-9's lost preamble, R-4 the `WORK · N steps` change) rather than
   silently deciding for the user. That is the right move on both.

---

## 2. Blocking issues (fix before `implementation-plan`)

### B1 — The minimal-mode content row cannot draw as specified (FR-5 is unreachable)

The doc states two gates that compose badly and never says how they compose:

- C7: "After an owner row: `batchOpen = desc.expanded`; a `batchOwned` row is
  skipped when `!batchOpen`."
- DD-10 / C11: in minimal the group is folded and "the fold exception list gains
  exactly one entry" — the newest live batch's **content** row.

Routing: in minimal the group is closed, so the owner row is not drawn (it is not
in the exception list), therefore `batchOpen` is never set, therefore the
`batchOwned` content segment is skipped by the nesting gate — every time. The
headline regression fix (D7/FR-5, "the streamed text is visible again") is
therefore unreachable as written.

Required change: state the evaluation order explicitly — fold exception first
(draw / skip), *then* the nesting bit, and define that a row admitted by the
content exception bypasses the nesting bit. Add a sentence to C7 and to §7.3,
plus the minimal assertion in T11 ("the segment draws with no owner row drawn
this frame").

### B2 — `rowOpen` for children hides a re-opened settled batch's contents

§7.5:

```
row.batchOwned ? (batchOpen(row.batch) && batchAutoOpen(row.batch))
```

`batchAutoOpen` is false for every settled batch. So a user who opens a finished
turn's group and then clicks a settled `AGENT` row gets `v AGENT · 4.1 s ·
[bash, grep]` with **nothing under it** — which is the opposite of the shape the
doc sells in §4.6 ("A batch re-opened by hand draws … its thinking folded, its
content drawn and its cards folded") and of the user's judged shape in
`maximal_investigation.md` §5.2. It also makes §9's own mitigation ("one rule
only") describe a rule that contradicts the table it is meant to explain.

Two fixes, both required:

- Drop the `&& batchAutoOpen` from the child formula: the child's *visibility* is
  `ownerOpen`; `batchAutoOpen` belongs only to the owner's default. Then the
  child's own `expanded` (thinking folded, card folded) is what "returns folded"
  means.
- Remove `&& batchAutoOpen` from the *owner's* resolved state as seen by
  children — otherwise pinning a live batch open (default true) and then letting
  it settle folds the children under an open owner.

Note the doc inherited this from the investigation, whose one-line summary rule
("a row is open iff its owner is open **and the batch is live**") contradicts its
own §5.2 table. The design's job was to resolve that, not to copy it.

Also: FR-3's last clause ("every child returns folded when the user re-opens it")
contradicts §4.6 ("its content drawn"). Pick one; I recommend "children return
folded, except the content segment, which draws because it has no fold of its
own".

### B3 — The body segment's visibility has no rule, and that omission breaks three cases

`bodySegment` rows have no header and no disclosure, so `expanded` is not a
meaningful fold for them. The doc never says what *does* decide their
visibility, and three requirements depend on it:

- FR-5 (minimal, until the turn closes) — see B1.
- §4.6 (re-opened settled batch: "content drawn") — see B2.
- DD-9's suppression (the answer batch contributes no content child) — this one
  *is* specified, which shows the author knew the segment needs an explicit rule
  and wrote only one of them.

Required change: one sentence plus one invariant — "a `bodySegment` draws iff its
owner row draws *and* its owner's state is open; when the content is visible
because of the minimal exception it is admitted by that exception, not by
`expanded`" — and I-11 to enforce it ("a body segment is never gated on its own
`expanded`").

### B4 — The owner row draws no disclosure mark, so the batch fold has no affordance

DD-13 makes the owner `kind = Assistant`. In `views/blocks.cpp` the chrome is
chosen by kind:

```cpp
if (blockHasDisclosure(block.kind)) { ... disclosure mark ... } else { ... role rule ... }
```

and `blockHasDisclosure()` is `WorkGroup || Thinking || ToolStep` only. A
synthetic `Assistant` owner therefore draws a **role rule** where every other
foldable row draws an open/closed mark: the row is clickable (`out.toggled =
activated && block.expandable`) but the user cannot see that it toggles, nor
whether it is open. That defeats FR-1/FR-6's whole interaction model.

Required change: extend the disclosure predicate to `ownsBatch` (rename it
kind-free, e.g. `blockHasDisclosure(const BlockDesc&)`), and say so in T6/T10's
acceptance. While there, pin DD-13's missing field: today the projection sets
`row.expandable = true` for *every* row (`thread_store.cpp`, the row build loop),
so §7.2's "`setBlockExpanded` answers false when the row is not `expandable`" is
a no-op guard until `expandable` becomes per-row. DD-13 lists `kind`, `key`,
`ownsBatch` but never `expandable = true` — an implementer following T5 literally
can produce a non-toggleable owner.

### B5 — The gate sequence contradicts itself: goldens are red from T5 to T15

- §10.1: "a golden diff is a gate, not a formality: every changed golden must be
  explained by the task that changed it".
- T5's acceptance: "the tree builds and `make test_blocks` runs (**its goldens
  move in T15**)".

T5 through T13 all change drawn shapes; T15 is the only golden task and it sits
after all of them. So tasks T5–T13 land with failing golden comparisons, which is
precisely what §10.1 forbids, and `test_blocks` cannot be a gate for nine
consecutive tasks.

Required change: (a) create the two target scenes (`blocks-maxlong`,
`blocks-maxlonglive`) as task **T5a before T5**, so the shape exists as a failing
test the projection work drives to green; (b) make regolden part of each task
that changes a shape ("the goldens this task moves are updated in this task, with
the one-line reason"), leaving T15 as *scene review by a human*, not as the
golden gate; (c) split T15 accordingly.

---

## 3. High-priority issues

### H1 — The synthetic `Assistant` row collides with the "copy the answer" path

`views/palette.cpp`:

```cpp
bool blockIsAnswerRole(const model::BlockDesc& block) {
    if (block.kind == model::BlockKind::Answer || block.kind == model::BlockKind::Assistant)
        return true;
    return block.label == defaultBlockLabel(BlockKind::Answer) ||
           block.label == defaultBlockLabel(BlockKind::Assistant);
}
```

`lastAnswerBlock()` walks the adapter's rows from the end to find "the newest
answer" for the copy row. A synthetic owner row is `kind Assistant`, label
`AGENT`, empty body → it qualifies. In a calls-only batch (the `blocks-maxlive`
shape) the owner is the last such row, so "copy the (partial) answer" can target
a row whose text is empty. `streamHasText()`/`streamHasAnswer()` use the same
kind test (they guard on a non-empty body, so they survive, but their semantics
now label a batch owner as an "assistant message").

The design's file list (§1 in-scope) and task list do not mention
`views/palette.cpp` or `streamHasText()` at all. Required change: add a
**consumer-audit** acceptance line to T6 — every `BlockKind::Assistant` site
(`views/palette.cpp` answer role + `lastAnswerBlock`, `store/thread_store.cpp`
`streamHasText`/`streamHasAnswer`, `blockAt`'s label switch, `views/blocks.cpp`
`blockInk`/`defaultBlockLabel`) must be re-audited against "Assistant is now also
the batch owner, with no body of its own", and `blockIsAnswerRole` must exclude
`ownsBatch` rows.

### H2 — DD-6's "drop the pin when it equals the default" re-introduces the bug it fixes

DD-6's thesis is "a pin must not be relative to a *moving* default". The write
rule then says: "a write that agrees with the current default drops the entry".
That is a relative encoding again, with a smaller blast radius:

- FR-7 says a pin survives the default changing under it. A pin equal to the
  default at write time is not stored, so it cannot survive.
- Concrete case: settled batch (default closed) → user opens then closes it
  (their last action is "closed") → the write equals the default → no entry →
  `/c` makes that batch the live tail again → it auto-opens, against the user's
  last explicit instruction.

Recommended change: store absolute pins unconditionally (`map<id,bool>`), and
prune only on `tuiThreadClear()` and on turn eviction (§6.4 currently leaves pins
of evicted turns behind for the session's lifetime). If map growth is the worry,
say so and bound it per turn, but do not make correctness depend on the default's
current value.

### H3 — DD-4 turns a private id format into an implicit ABI

DD-4: "a row is batch-owned iff its `batchId` has the response shape
`<turnId>#<n>`". That is a string parse of a format D owns, done in C++, with the
failure mode "silently treat responses as non-response" (no owner row at all).
It sits badly next to two of the doc's own principles — NFR-3 ("no tool-name
knowledge in C++": knowledge about *D's naming* is the same class of coupling)
and NFR-2 ("one source of truth per fact").

Cheaper and safer: one more additive flag bit, e.g.
`TuiBlockFlag_ResponseBatch = 16`, set by D at the same choke point as `Hidden`
(§7.6 already promises "every response's blocks share one `batchId`"; a flag makes
it a machine-readable promise instead of a parsed one). Polarity matters: absent
must mean "not a response batch", so a stale emitter yields today's shape (plain
rows, no owners) rather than a tray of bogus `AGENT` rows over the query. If the
team keeps DD-4 as written, then §7.6 must promise the *format* explicitly
(`<turnId>#<n>`, `n` numeric and ascending, `#q` the only non-numeric suffix) and
T5 must carry a test that a malformed id yields no owner.

### H4 — In maximal, the content folds at the *settle* moment, not at the turn's end

Minimal keeps the newest live batch's content "until the turn closes" (FR-5,
DD-10). Maximal gates the same row on `batchAutoOpen`, so the preamble disappears
the instant the response's last tool call returns — mid-stream, several rows of
text vanishing under the reader's eyes, and one frame later the next response's
text appears. Two modes, two different lifetimes for the same fact, and the
jumpier one is the one that is supposed to be the "more open" mode.

Recommended change: gate the content segment on "this batch is the newest batch
of a live turn" (not on `autoOpen`) in both modes, and let the fold-on-finish
rule apply only when the *turn* closes or a newer batch exists. That is one
rule, removes the flap, and makes FR-5/DD-10 the same statement in both modes.

### H5 — DD-12's "-2 steps" is probably -3, and the count now has two independent inputs

`stepCount()` weights a non-first prose row as 1 and a settled tool call as 2
(quoted in `typed_thread_store.cpp:167-196` per the design's own evidence).
Hiding `task_done` lowers the count by 2 per final response (DD-12 ✓), but DD-9
*also* drops the final response's content child, which was a weighted prose row
→ one more. The doc asserts "-2 per final response" and pins it in a golden, so
the golden will be born wrong and then "fixed" by whoever runs `--update`.

Required change: state the arithmetic for both drops, and add the check to §10.3
for the config where `hiddenTools` is empty (count unchanged) and the default
config (count reduced by 3) — that turns an accepted oddity into a tested
contract.

### H6 — §10.3 materially understates the test blast radius

It names two "untouched and must stay green" tests
(`test_streaming.cpp:660`, `test_blocks.cpp:1611`) and a regolden list. The
indexed tests say otherwise:

- `test_blocks.cpp` asserts `headerText(thinking, true) == "Thinking - 3.2 s"`
  ("the thinking disclosure says what it costs"). Moving the duration to the
  owner breaks it — and the doc never actually states that the `Thinking` child's
  `timingS` is retired (the investigation does, 5.1/Q3; the design only says "the
  `Thinking` header carries no count"). Without that sentence the duration will
  be drawn twice.
- `test_blocks.cpp` exercises row-level focus and copy by identity
  (`BlockId{1, hash("content#1"), BlockKind::Assistant}`, `rowOf(grid, "AGENT -
  2.0 s")`). Owner rows, body segments and the canonical reorder all move those
  row indices and identities.
- `blockHeaderText()`'s signature is `(BlockView, BlockDraw, bool textGrid)`;
  §7.4's pseudocode shows `headerText(block, draw, widthUnits)` — which *drops*
  `textGrid`, and `textGrid` is what picks `" - "` over `" · "` (pinned by
  `test_blocks.cpp`, "the grid separates with ASCII"). Feeding it a width only is
  a regression for `LC_ALL=C`.

Required change: (a) name the retirement of the Thinking row's duration in DD-8
and T10; (b) in T10's acceptance, state the new signature as
`(BlockView, BlockDraw, const ui::Metrics&)` (or width **plus** `textGrid`), so
the separator rule survives; (c) pin the 40-column header in **both** profiles
(the doc pins `AGENT · 3.2 s`, the grid will draw `AGENT - 3.2 s`); (d) replace
the "two untouched tests" claim with "the row-level focus/copy/timing checks in
`test_blocks` move; each one's new expectation is listed in the task that moves
it".

### H7 — Nothing verifies the D-side behaviour end to end

T1/FR-11 ("the turn closes on every run exit") is the requirement that makes the
whole fold's "live" bit trustworthy, and its verification is "`dub test` green
with updated turn-upsert counts". The investigation itself records that the
D-side claims were "read off the code and the existing unit tests, not reproduced
against a live model". Counting `turnUpsert` calls proves that a call was made,
not that the run loop reaches it on `networkFailure`.

Required change: add to T1 an end-to-end probe (one real run per exit path:
final answer, user stop, injected network failure, `agentStuckInLoop`) with the
observable being the *TUI's* shape ("no `v` after the failure; the batch reads
`> 3.2 s · [chips]`; `/c` re-opens the group"). Same for T14 (replay parity):
reload a finished session and compare the two grids the test already captures.
This is the cheapest place in the project to catch "the ledger leaked again".

### H8 — Interleaved plain rows: no placement rule, and the asserts cannot see it

C5 step 6 keeps "header, plain rows and owner+children in insertion order". If a
notice/vision row arrives *between* two rows of one response batch (the query
batch is emitted at turn start, but notices and vision blocks can land mid-turn,
per §4.3's own data-flow art), then "emit the owner at the batch's first row" and
"keep plain rows at their insertion position" produce two different orders, and
the choice is undefined. I-3 (children contiguous) and I-7 (bucket order equals
index order) both still hold in either interpretation, so no test would catch the
drift — but the golden would, once, silently.

Required change: one rule in C5 ("the batch block is emitted at the index of its
first row; rows of that batch later in the stream are consumed by the block; a
plain row that lay between them is emitted after the block") plus the golden that
pins it (a scene with a notice mid-response).

---

## 4. Medium issues

| # | Issue | Action |
|---|-------|--------|
| M1 | `batchIsNewest` is used in §7.5 but never defined (per-turn or per-thread?). With a stale `Running` turn this decides whether an old batch auto-opens. | Define as "the newest batch of the newest live turn" and add it to §6.5 as an invariant. |
| M2 | NFR-5 is qualitative and unmeasured. `syncTyped` re-buckets **every** row of **every** turn on every version bump, and the bump rides the streaming tick; the per-frame fold read becomes a string-keyed `std::map` probe per row. | Extend the existing scaled-thread cost test (`test_thread_window` already sizes the same work against a thread ten times longer) with a `blockExpanded` + sync cost check, and name it in NFR-5. Consider `unordered_map` (or caching the resolved open bit on the row) if the test shows the probe. |
| M3 | Pins of evicted turns are never pruned (`tuiThreadClear()` only). Session-long pins with no "reset folds" affordance. | Prune in the eviction path; add a "collapse all" / documented escape hatch in C14. |
| M4 | The ink spec was lost between investigation and design: label in `roleAgent`, chips quiet (`textMid`), `FAIL` in the failure ink. §8 only says "ink runs are compared". | Add a three-line ink table to C8; otherwise the implementer picks tokens and the goldens freeze the pick. |
| M5 | `Thinking`'s duration retirement is unstated (see H6a). | State it in DD-8/T10. |
| M6 | `blockCount`'s contract changes ("row 0 is the header; the count is no longer 1 + displayed wire blocks") without an audit of its readers — `test_typed_thread_store.cpp` asserts `blockCount(0) == 2` and describes the old contract in its message, `views/thread.cpp` prefetches per group. | Restate the contract in §7.2 and add the affected fixtures to T6's acceptance. |
| M7 | `blockHeaderText` needs `ui::Metrics`, not just a width (H6c). | Fix the interface in §7.4 and §7.2. |
| M8 | No effort sizing. T15 bundles scene creation, regolden and ~14 new checks, and is the dependency of T16 and T18; T5/T7 are each 5–7 acceptance criteria. | Add sizes (S/M/L) and split T15 (see B5); note that T15 as written cannot be "done" until T16/T18 are also done (mutual dependency smell). |
| M9 | No rollback story for a 22-task shape change; if the fold reads badly in use, the only escape is `git revert`. | Add a config toggle (default on) for one release, or state explicitly why not (the two modes already give an escape). |

---

## 5. Low / nits

- **L1 — Two modes, one boolean.** §4.5 says they differ in exactly one thing. A
  maintained second mode plus a full second golden family is a permanent cost.
  Either state why minimal is retained (habit? config? muscle memory?), or
  consider one preference ("expand live work: always / never"). At minimum, say
  which mode is the default and that both profiles stay pinned.
- **L2 — No glossary, no legend.** "owner", "batch", "child", "segment", "live
  edge", "settled", "pin" are all load-bearing and undefined; the art uses `v` /
  `>` fold marks with no legend. And the doc's authority rests on `file:line`
  anchors that will rot on the first refactor — anchor on symbols
  (`ThreadStoreAdapter::syncTyped`) and keep the line numbers as evidence only.
- **L3 — Traceability gaps.** FR-16 (copy/selection) maps to T9 only, though C10
  and T6 own `copyable`/`blockText`; no FR covers the `/c`-resume fold; FR-19
  (Could) is a task (T20) while FR-13 (Should) is folded into T10. Fine, but the
  table reads as if coverage is complete when it is approximate.
- **L4 — Naming collisions in the pseudocode.** `batchOpen` means both "the
  owner's resolved state" (§7.5) and "the view's one bit" (C7);
  `groupStyleMinimal(g)` takes a parameter its own definition ignores. Rename to
  `ownerOpen` / `drawnBatchOpen` in the pseudocode.
- **L5 — Mixed-binary skew is described in one direction only.** New C++ + old D
  → `task_done` reappears (stated). Add old C++ + new D (flag bit ignored → same
  shape, fine) so the table is symmetric.
- **L6 — Formatting drift inside the doc**: `WORK - N steps` vs `WORK · N steps`;
  `[bash, grep]` (art) vs `[bash] [grep]` (shipped code, see H-section below);
  `AGENT` with and without a duration in adjacent examples. In a document whose
  contract *is* byte-exact goldens, this is not cosmetic.

---

## 6. Requirement gaps (things the user agreed that this doc no longer states)

These are fidelity losses against `maximal_investigation.md`, and each one is a
place where the implementer must guess:

| Missing | Where it came from | Fix |
|---------|--------------------|-----|
| **Chip string construction** — `[a] [b]` vs `[bash, grep]`, de-duplication, and the truncation rule (`+N` past three) | investigation 7.3.3 states `[a] [b]` **and** `+N` past three; every art block in both docs draws `[bash, grep]`; the shipped live chips line composes `"[" + d.summary + "]"` per call (`views/thread.cpp`) | Add one FR: "the owner's chips are composed by *the same* helper as the minimal chips line (one notation, both modes), with the fold-to-`+N` rule; pinned at 3 chips and at 40 columns." Right now minimal and maximal would render chips in two different notations. |
| **Content visible while streaming** — an explicit statement that content does not vanish at the settle boundary | investigation 5.3 ("the content is what the user reads live") + FR-5 | See H4; state it once for both modes. |
| **Owner affordance** — disclosure mark, keyboard/Space/Enter, focus identity, menu | investigation 5.1 + `views/blocks.h` ("the disclosure mark, the focus ring and the copy control") | See B4; add to FR-1 and T6/T10. |
| **Ink** — `roleAgent` label, quiet chips, failure ink | investigation 5.1 | M4. |
| **`Thinking` row loses its duration** | investigation 5.1 / Q3 | M5. |
| **`+N` chip budget at 40 / 100 columns** | investigation 7.4.3's fitting rule | Fold into the chips FR; FR-13 currently says only "chips drop before the time", so a 4-chip row loses *all* chips at 40 columns when `[bash, +3]` would fit. Consider dropping chips one at a time from the right. |

Also worth adding as requirements rather than comments:

- **FR-21 (Should):** the fold never hides text the user is currently reading —
  i.e. a *content* segment does not disappear while its batch is the newest of a
  live turn (this is H4 stated as a requirement).
- **FR-22 (Should):** a batch's fold state is reachable by keyboard, and the
  owner row is the copy target for its content.
- **NFR-9:** the per-frame cost of `blockExpanded` on a 10x thread does not grow
  (measured, see M2).
- **NFR-10:** the full suite green at every task boundary (see B5).

---

## 7. Architecture, data and interface review (what is right, and the one gap)

**Right.** The three-way ownership (§4.1) is the correct seam: D owns facts, the
adapter owns shape, the view owns pixels. It is also the seam that makes NFR-3
enforceable. `BatchEntry` as a sibling of the group's rows (§6.2) keeps the
per-frame read a field read, which is the right answer to a TUI that redraws
every frame. Keying pins by wire id (`6.3`) rather than row index is correct and
correctly justified (the row list churns while a response streams).

**The gap: alternatives were evaluated for the owner's *identity* but not for the
owner's *existence*.** DD-13 makes the owner "the first row the C++ side
invents", and then R-2/I-6 exist only to manage the consequences (namespace
collision, hash collision, uniqueness assert, pin key). The alternative — D
emits one extra `TuiBlock` per response (`kind Prose`/`Assistant`,
`id = "batch#<batchId>"`, empty body, batch id set) — costs one upsert per
response on a path that already upserts three, and buys: no invented namespace, no
collision class, no `batch#` parse, replay parity for free, and an owner whose
existence is D's fact rather than a C++ derivation. The cost is the opposite
trade-off — a row on the wire that draws nothing when the batch has no surviving
child (DD-15 stays either way), and one more place where D must stay silent about
display.

I am not saying the doc's choice is wrong. I am saying the doc must record the
alternative and why it lost, because R-2's mitigation is three quarters of the
complexity in §6.3, §6.5 and T5 — and because "the first row C++ invents" is a
precedent that will be cited by the next feature.

**Second architecture note: the fold lives in the projection, but the
*visibility* decisions live in two places** (the adapter's `expanded` and the
view's exception list, §C7 + DD-10 + §7.3). That split is what produced B1 and
B3. If the fold is to be "one rule", the exception list should move into the
adapter (`BlockDesc.visible` or `bodySegment` + an adapter-computed
`admittedByException`), so a single function answers "does this row draw, and is
its body open". Two booleans, one owner.

---

## 8. Task-list review (order, granularity, gates)

| Task | Verdict |
|------|---------|
| T1 (run close) | Correct, first, and independent — good. Weak acceptance (H7): add the real-run probes and the `/c` shape check. |
| T2/T3 (hidden flag) | Correct pairing; the "recording-sink test walks all six paths" idea is excellent. Add the seventh path risk note: a *new* upsert site written later. Consider making the helper the only way to build tool flags (a type, not a convention). |
| T4 (seeder) | Correct, and correctly before the fold (D10). Add: a seeded *notice inside a response batch* for H8, and a `+N` chips scene. |
| T5 (projection + owner) | Too much in one task (six acceptance clauses, three of them with independent failure modes: bucketing, dropping, ordering). Split into T5a (bucket + `BatchEntry` + owner emission, DD-4/DD-15) and T5b (canonical order + body-less drop + answer-batch drop, DD-5/DD-9/FR-18). |
| T6 (row vocabulary) | Add the consumer audit (H1) and the `expandable` polarity (B4); then it is a real task rather than a struct edit. |
| T7 (fold) | The most important task and the one with the wrong formula (B2). Rewrite §7.5 before this task exists, and make its acceptance *the mode matrix's* cells, not prose. |
| T8 (nesting) | Add the exception/nesting interaction (B1) and the placement rule (H8). |
| T9 (segment) | Add I-11 (B3). |
| T10 (header) | Add: signature keeps `textGrid`/`Metrics` (H6b), duration leaves `Thinking` (M5), chips notation decided (chips FR), both 40-column profiles pinned, and `blockHasDisclosure` (B4). |
| T11 (minimal content) | The headline fix; it cannot pass until B1 is fixed. |
| T12 (live edge) | Fine. Consider framing it as "the live edge is a *derived* fact too" (minimal && newest && (running || thinking-without-content)), so it stops being two hand-written sites. |
| T13 (anchors) | Good that it exists; extend to "the anchor does not move when a *batch* folds in place mid-stream" (the H4 jump is the case that will be reported as a bug). |
| T14 (replay parity) | Correct and late. Move the optional console-order work (T21) out of its acceptance; a task should not have an "optional or explicitly deferred" clause. |
| T15 (goldens) | Split (B5). As written it is a milestone, not a task, and it cannot close before T16/T18. |
| T16 (second mount) | Good. Add "no shape divergence" as a *test*, not a comparison by eye. |
| T17 (answer suppression) | Belongs *before* T15 (it changes a shape T15 would otherwise freeze). |
| T18 (docs) | Fine, but the doc chapter should be written *from this file*, and this file's `file:line` anchors re-checked by symbol. |
| T19 (dead state) | Overlaps T7/T6/T10/T14's acceptance (each already retires its own state). Either drop it or make it purely "comments that lie + a grep gate for the retired names". |
| T20–T22 | Correctly deprioritized. T22 (grid copy) is a decision, not a task — record it in the doc and close it. |

Missing tasks:

- **T5a** target scenes before the projection (B5).
- **T6b** consumer audit of `BlockKind::Assistant` (H1).
- **T1b** end-to-end run-exit probe incl. `/c` resume (H7).
- **Tn** the "pin reset / collapse all" affordance (M3), if accepted.

---

## 9. Documentation-quality review of the document itself

Good: consistent section numbering, decisions with rationale, invariants with
enforcement, evidence index (§13) that a reviewer can sample, and a defect
register that makes the FR set auditable. The doc is also honest in the two
places where honesty costs something (R-3, R-5) and does not hide the accepted
regression in R-5 ("the last text folds away").

Weak: (a) it duplicates the investigation almost verbatim in §5 and §7, so the
two can drift and the next reader will not know which is authoritative — state
"this file is authoritative for tasks; the investigation is authoritative for
observed behaviour" and delete the repeats; (b) the `file:line` density reads as
proof but rots — anchor by symbol; (c) no glossary and no legend for the art;
(d) the "Status: design complete, no code written. Every `file:line` below was
re-verified" line is a promise the document cannot keep (I found §7.3's "the view
asks `blockAt` once per row per frame" inaccurate — the view *prefetches* the
rows per group per frame — and the `blockCount` contract in §7.2 no longer
matches the header comment it cites); replace it with "verified at HEAD
`a1e7743`" and a date; (e) §12's tables imply full coverage; mark approximate
rows.

---

## 10. Required changes before this goes to `implementation-plan`

Blocking (B):

1. State the evaluation order of the fold exception vs the nesting bit, and make
   the minimal content row reachable (B1).
2. Fix `rowOpen` for children (drop `&& batchAutoOpen`) and resolve FR-3 vs §4.6
   for the content segment (B2).
3. Add the body-segment visibility rule + invariant (B3).
4. Extend the disclosure predicate to the owner; set `expandable` per row and
   say so in DD-13/T5 (B4).
5. Create the target scenes before the projection and regolden inside each
   shape-changing task; restate the gate in §10.1 (B5).

High (H): the `Assistant` consumer audit (H1); absolute pins without the
drop-if-equal rule and the eviction prune (H2); the batch-marker flag or a
written `batchId` format promise (H3); one content lifetime for both modes (H4);
the step-count arithmetic for both drops (H5); the true test blast radius,
`textGrid`/`Metrics` in the header signature, the Thinking duration, and the
40-column pin in both profiles (H6); end-to-end run-exit and replay probes (H7);
the interleaved-row placement rule (H8); plus the chips notation/truncation FR
and the ink table (§6, M4).

Nice to have (M/L): measured cost gate, pin pruning + reset affordance, effort
sizing, rollback toggle, glossary/legend, pseudocode naming, skipped-alternative
entry for "owner row on the wire".

## 11. Sign-off

- **Architecture and scope: approved.** The response-owned fold, the derived
  liveness, the wire `Hidden` flag and the additive-ABI posture are the right
  calls, and the rejection of "fabricate `Cancelled`" is the single best decision
  in the document.
- **Document: needs one revision round** addressing B1–B5 and H1–H8. After that I
  would expect `implementation-plan` to produce clean code tasks; as written it
  would produce five tasks that "pass" their acceptance while the feature is
  unreachable.
- **Two user decisions are genuinely open and correctly flagged**: R-3 (the
  final response's differing preamble is suppressed) and R-4/R-5 (what a
  finished turn hides). Get them signed before T17/T15 freeze them into goldens.
