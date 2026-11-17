# Design notes (working record) - maximal-mode fold

Source: ./plan/maximal_investigation.md (HEAD a1e7743, branch update-ui).
All file:line claims re-verified against ./llmfun at that HEAD.

**Revision 2 is the current document** (`plan/system_design.md`, rev 2). It
answers the design review of rev 1: B1-B5 blocking, H1-H8 high, the M/L list and
the requirement gaps. The change log is §0.2 of the document; the two review
points not accepted (H5 step-count arithmetic, H4 single content lifetime) are
argued with evidence in §0.3.

## Load-bearing model after rev 2

- Owner per LLM response (`AGENT`), id `batch#<batchId>`, children = thinking,
  content segment, cards, canonical order, contiguous.
- **One visibility predicate** resolved in `blockAt()` (the view never keeps an
  exception list):
  `groupOpen = pin ?? (mode maximal && turnLive)`;
  `styleIsMinimal = mode minimal && !groupOpen` (an opened minimal group draws
  maximal-style);
  `ownerVisible = groupOpen`; `ownerOpen = pin ?? batchLive`;
  `childVisible = ownerVisible && ownerOpen`; `childOpen = pin ?? batchLive`;
  `segmentVisible = (ownerVisible && ownerOpen) || (styleIsMinimal && turnLive && isNewest)`;
  `plainVisible = groupOpen`.
- Pins absolute and always stored (never dropped when equal to the default);
  pruned on eviction/clear.
- Displayability = `TuiBlockFlag_Hidden` from `isHiddenTool` at one D-side helper;
  **no batchId parsing** - a response batch is any batch that is not the query
  batch.
- Turn closes at the source on every run exit; `/c` reopens; no fabricated
  `Cancelled`; orphan clears its live call.
- Owner carries duration; the `Thinking` row carries neither duration nor count.
- Chips `[bash] [grep]`, `+N` past three, one helper for both modes.

## Task list (rev 2, 26 tasks)

P0 T1 run close, T2 run-exit probes, T3 hidden flag (D), T4 flag read (C++),
T5 seeder, T6 scenes first, T7 buckets+owner, T8 order+drops, T9 vocabulary +
disclosure + Assistant consumer audit, T10 fold/pins/visibility, T11 row loop,
T12 body segment, T13 header, T14 minimal content, T15 live edge, T16 anchors.
P1 T17 replay parity, T18 answer-batch suppression, T19 scene review + checks,
T20 drill-down, T21 docs, T22 retired names + grep gate.
P2 T23 measured cost, T24 pin hygiene + collapse all, T25 console order.
P3 T26 grid-copy decision.

Full acceptance criteria and dependencies: plan/system_design.md §11.
