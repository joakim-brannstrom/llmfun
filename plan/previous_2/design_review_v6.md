# Design review - Thread rendering v6 (plan/system_design.md)

Date: 2026-11-15
Reviewer: system-design reviewer pass
Reviewed: plan/system_design.md (v6 design, 2026-11-15) against its source spec plan/appendix_fixes.md,
the prior design plan/previous/system_design.md (v5), and companion notes plan/design_notes.md.
Code claims spot-verified in the primary RAG (tui/package.d, app_agent/package.d, slash_pipeline.d,
thread_store.cpp/.h, typed_thread_store.h, composer.cpp).

## Verdict: APPROVE WITH FIXES

The design is well-evidenced, correctly scoped, and internally consistent with v5. No redesign is
needed. The fixes below are specification tightenings (F-group clarifications, ABI versioning, and a
few unpinned edge rules) that should land as doc edits before the implementation-plan step.

## 1. What the design is

v6 amends the v5 typed-turn rendering per the post-v5 feedback (appendix items 4-8): query-first
turn anatomy (D1), a minimal-mode live edge (D2), a maximal-mode rebalance (D3), a reshaped tool card
with a D-side RESULT pair transport (D4), ordinal removal (D5), a real busy edge driving the composer
button (D6), drill-down inheritance scoping (D7), and count-unit pinning (D8). Eight tasks, P0-P3,
with a one-commit cutover per the v5 N1 rule.

## 2. Strengths

1. Root-cause discipline is exemplary. Every feedback item is traced to a verified code location
   (empty `uiAgentBusy()` body at tui/package.d ~1093; the Ok-glyph ASCII fallback as the "+" marker;
   the summarized body repeating "name("; the ordinal prefix at thread_store.cpp ~605/~717; the v5
   `defaultOpen = kind != Thinking` rule as the wall). Defects, reversed decisions, and rejected
   designs are separated, and the "got worse after the pipeline query fix" claim is explicitly
   debunked with golden evidence (which goldens were regenerated, which were not, and why the long
   turn renders identically). This is exactly what a design should do before proposing work.
2. Scope control is strong. D7's "fix the population, not the view" rule, the rejected-alternatives
   record (header restyle, fixed row budget, dropping the RESULT row, engine-side busy edge,
   phase-edge-derived button), and "no rollback is part of this design" keep the iteration tight.
3. The rollout honors the hard invariants: the ordinal removal (breaking wire change) rides the
   one-commit cutover (N1); the additive pair fields land first with both sides green and goldens
   unchanged; tasks 1 and 2 are honestly independent.
4. The testing strategy closes the exact gap that masked the shipped bug: the composer-busy golden is
   re-driven from the real D-side edge instead of poking the C++ flag (section 8, golden frames). This
   is the single most important test change and it is called out explicitly.
5. Boundaries carried through correctly: caps computed once at emission (I5) keep N2 and N4 true; the
   store never re-parses; the console formatter stays byte-identical (F9) with a parity golden.
6. Appendix-silent points are stated as explicit interpretations (I1-I6) instead of being silently
   assumed - good design hygiene that makes review and fix rounds cheap.
7. Real simplification captured: killing ordinals also kills v5's ordinal recomputation on compression
   (section 7), which is a genuine maintenance win, not just cosmetics.

## 3. Weaknesses and risks

### W1 (High) - Turn ordering after ordinal removal is unspecified for the live path
D5 says "ordering is turnId's job and session restore reads the history sequence", but the live
session path is not covered: turns arrive via tuiTurnUpsert, and the design never states what orders
them C-side once `TuiTurn.ordinal` and `GroupView.ordinal` die. The store evidently keeps turns in an
array (typed_thread_store.h: "loads turns (array order)"), so insertion/array order is the obvious
answer - but compression re-emission, out-of-order upsert delivery, or a re-attached emitter after a
session switch could expose a reordering that the ordinal used to mask. Also, "the ordinal leaves the
data model" must name `GroupView.ordinal` (thread_store.h) explicitly; task 4's "no ordinal references
remain (search-verified)" would catch it, but the task text only names TuiTurn/UiTurnData and the
"[N]" prefix - an implementer can pass the letter of the task while leaving the dead field.

### W2 (High) - ABI version discipline across the two wire-changing commits is ambiguous
Two commits change the layout-pinned structs: task 2 (additive pair fields, "bump TUI_API_VERSION") and
task 4 (ordinal removal, "offsets re-pinned" - no bump mentioned). If the version identifies a layout,
each layout change needs its own bump (v6 additive, then v7 at cutover); if one bump is intended to
cover both, the intermediate commit's version lies about its layout, which is exactly the failure mode
N1's per-commit agreement exists to prevent. The design must pick one and say so.

### W3 (High) - Busy edge: concurrency assumptions and entry-point completeness unpinned
Three related gaps in D6:
- The ready flag is a plain bool with non-refcounted exception-safe brackets. If any two runs can
  overlap (e.g. compression while an agent run is in flight), a nested bracket pair restores Send while
  the outer run is still live - recreating the bug in a rarer form. The design asserts "a run is in
  flight" (singular) but never states the at-most-one-run assumption or a nesting rule.
- Entry-point completeness is asserted ("every turn entry point") but only four are named (userQuery,
  doCompress, /plan, /code). The shipped bug was precisely a missed path; the design should require a
  search-verified enumeration of all busy()/runInFlight-raising paths (one-shot mode, drag&drop-triggered
  runs, session-resume paths) before task 1 closes. (Project lesson on record: a truncated grep once hid
  package.d:1434 and produced a wrong "pipeline-only" conclusion.)
- Session switch mid-run keeps the flag busy (section 7), but nothing says what a Stop click in the
  newly-shown session targets. If exactly one run can exist globally this is fine - say so.

### W4 (Medium) - D2/D3 leave three render rules ambiguous
- Pending chips: v5 rendered dim chips for Pending calls; D2's rule as written ("one chip per running
  call") excludes Pending, so during streaming no chip renders for a streamed-but-unstarted call. That
  is a behavior change that is nowhere stated as intended (the "What dies" list does not mention it).
- Fold-on-finish vs mid-stream manual open: D3 says the group folds to its header when the turn
  finishes, and separately that manual overrides beat mode defaults. If the user opened the group
  mid-stream (a manual open), does it fold anyway at finish, or does the override survive? The two
  rules collide and the precedence is unstated.
- D2 is titled "Minimal-mode live edge" but its mechanics read mode-general, and the appendix's maximal
  sketch shows the chips line ("same as minimal"). Maximal's own bullet list never restates the live
  edge. State once, mode-generally: the live edge exists in both modes; modes differ only in fold
  defaults (F3 already says this - make D2 agree with its own heading).

### W5 (Medium) - D4 transport details a renderer will trip over
- Value truncation at 200 characters must be UTF-8 codepoint-safe (D strings are UTF-8; a byte cut can
  split a codepoint and hand the C++ renderer invalid UTF-8). Unspecified.
- "Sorted by key" - state byte-wise comparison for determinism; and the duplicate-key policy after parse
  (last-wins?) is unstated.
- The pair set must merge atomically (all four fields replaced together) on upsert; the design implies it
  ("part of the merged tool block") but the store test list does not pin it.
- The 200-char/100-pair caps are RESULT-only, but D4's body sentence ("arguments only, one key=value pair
  per line") sits next to them with no cap statement; the existing args summarizer keeps its own
  (1000-char per-value) cap. One line prevents an implementer from applying 200 chars to args.

### W6 (Low) - "What dies" wording can kill the wrong path
"the packed one-line RESULT blob" (C-side) reads as if the packed rendering dies - but resultRaw's packed
fallback for arrays/non-JSON/zero-pair objects is explicitly kept (D4). Clarify: the JSON-object blob
path dies; the raw fallback stays.

### W7 (Low) - Cancelled tool card header shape unspecified
Failures get a textual FAIL (red); Cancelled gets nothing stated. Silence is defensible (the user asked
for the stop), but N3's honesty rules and the interrupted-turn replay path make this a golden-visible
shape - record the decision ("header-only, no marker") rather than leaving it implicit.

### W8 (Low) - Documentation quality nits
- Section numbering breaks: the doc runs 1-9, then task lists, then "## 11. Out of scope" - there is no
  section 10 (v5 had "## 10. Task list"). Restore a "## 10. Task list" header or renumber.
- Section 9 (Rollout) duplicates the task list nearly one-to-one; keep it as the commit-shape summary and
  drop the repeated acceptance text, or merge.
- The appendix's open questions 6.1 (glyph) and 6.2 (ordinal feature) are silently closed by D4/D5; the
  design's Notes should record them as closed (6.4 is recorded; 6.1/6.2 are not).

### W9 (Low) - F5 MoSCoW label conflicts with its own cutover ride
F5 is "Should" but the ordinal removal is a breaking change baked into the Must cutover (task 4). Once
task 4 lands, F5 is not optional. Either relabel Must (rides cutover) or state the descope consequence
(fields stay on the wire, unset and unrendered - which contradicts "the ordinal leaves the data model").

### W10 (Low) - Test-strategy gaps
- No cap-boundary tests: exactly 100 pairs (moreCount = 0) and 101 (moreCount = 1) pin the cap semantics;
  same for a value at exactly 200 chars.
- No UTF-8 truncation test (multi-byte character at the cut).
- No test pinning the W4a (Pending chip) and W4b (override vs fold-on-finish) decisions once made.
- The maximal UX consequence - a passive maximal watcher has the open live tail fold at completion, and
  re-expand shows the reduced finished shape - is deliberate per the appendix, but worth one recorded
  "accepted consequence" line so a future fix round does not re-litigate it as a bug.

## 4. Actionable suggestions (fix list)

1. (W1) Add one data-model paragraph: turns order by the store's turn-array insertion order (the existing
   behavior), replay order comes from the history sequence; name `GroupView.ordinal` in task 4's removal
   list; add a store test pinning turn order across a compression re-emission and an emitter re-attach.
2. (W2) Pick and state the version story: either bump TUI_API_VERSION at each layout-changing commit
   (recommended: additive task 2 -> v6, cutover task 4 -> v7) or explicitly document the single-bump +
   intermediate-layout decision. Update task 4's acceptance to include the version action.
3. (W3) In D6/section 7: state the single-run-at-a-time assumption (or make the bracket a nesting counter);
   add a task-1 pre-step "enumerate all busy()/runInFlight-raising paths, search-verified" and list the
   verified set in the acceptance; state what Stop targets after a session switch mid-run.
4. (W4) Three one-line rule fixes: Pending calls render no chip (state it); a mid-stream manual open does
   (or does not) survive the finish fold - pick and pin; retitle D2 "Live edge (both modes)" or restate its
   first sentence mode-generally.
5. (W5) D4 additions: codepoint-safe truncation; byte-wise key sort; duplicate-key last-wins; atomic pair-set
   replacement on merge (with a store test); one line stating args keep the existing summarizer and its cap.
6. (W6) Reword the "What dies" C-side bullet to "the JSON-object RESULT blob path (the packed raw fallback
   stays)".
7. (W7) Add "Cancelled renders header-only, no marker" as a recorded decision in D4/section 7.
8. (W8) Restore "## 10. Task list"; trim section 9 to the commit shape; record 6.1/6.2 as closed in Notes.
9. (W9) Relabel F5 as "Must (rides the cutover commit)" or state the descope consequence.
10. (W10) Add the boundary and UTF-8 tests to section 8's D-side emission test list; add the two W4 pinning
    tests after those decisions; add the accepted-consequence line for the maximal fold-on-finish.

Items 1-3 are the blocking set for the implementation-plan step; 4-10 are doc edits that can ride the same
revision. Nothing here reopens D1-D8's substance.

## 5. Verification notes

- `void uiAgentBusy() {}` empty body and `uiAgentReady()` -> setReadyStatus(true): confirmed (tui/package.d).
- userQuery bracket (runInFlight true/false, uiMsg.ready() around the run): confirmed (app_agent/package.d ~1453-1473).
- /plan and /code handlers call the pipelines with no busy()/ready() bracket: confirmed (slash_pipeline.d 17-50).
- Ordinal prefix on the typed query row and GroupView.ordinal: confirmed (thread_store.cpp ~586-612, ~709-724;
  thread_store.h GroupView).
- Composer button flips on a busy input derived from the ready state: confirmed (composer.cpp; test_composer.cpp).
- Step-count weights and the turn-opening-Prose rule: confirmed (typed_thread_store.h).
- The design's companion notes (design_notes.md) are current and match the design doc (I1-I6, task shape, deps).
