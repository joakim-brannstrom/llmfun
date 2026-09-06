# Spec-Driven Code Review — Detailed Protocol

Companion to SKILL.md step 3 (Spec Conformance) and step 6 (Empirical
Verification). Use when the code under review has design/spec/plan documents.

## 1. When this applies

- The repo (or a sibling `plan/` directory) carries design docs, an
  implementation plan, task files, or an appendix describing intent
  (e.g. `plan/phase1_system_design.md`, `plan/implementation_plan.md`,
  `plan/task_NN.md`).
- The user asks "does it implement X as described in Y" or "review against
  the plan".
- If no such documents exist, skip spec conformance; the rest of the review
  proceeds normally.

## 2. Extract review criteria from the documents

1. List every functional requirement (F1…Fn) and non-functional requirement
   (N1…Nn) the spec names, plus every architectural decision (D1…Dn) and
   recorded risk.
2. Note constraints the spec claims about the code (signatures, formats,
   defaults, error strings) — these become checkable assertions.
3. Note what the spec explicitly defers or rejects — do not report deferred
   items as missing.
4. Watch for spec revisions: a later section or a "finalized semantics" note
   may supersede an earlier table (e.g. a default changed by a later task).
   The most recent decision wins; report the deviation, not the stale table.

## 3. Build the requirement → evidence table

One row per requirement: status (OK / OK-with-deviation / MISSING / BUG),
evidence as `file:line`, and a one-line note. Example (Phase 1 dialogue review):

```
| F1 index evicted dialogue at checkpoint | OK | summary_agent.d:394-422 fires
  CompressionCheckpoint; dialogue_index.d:263 consumes evictedSummarized +
  evictedInPlace |
| F10 verbatim contract (no summaries)    | OK | dialogue_index.d:266-274 drops
  summary_turn_start and turnId==0 before grouping |
| F8 last-N-turns window                 | OK-with-deviation | maxTurnAge default 0
  (Task 11 finalization) vs spec's "default 200" |
```

- "OK-with-deviation" entries must name the decision that blessed the
  deviation (task number, user comment) — deliberate deviations are findings
  too, just not bugs.
- Verify each OK by reading the cited lines; never copy claims from the spec
  into the table without checking the code.

## 4. Empirical verification of suspected bugs

For a plausible logic bug that is cheap to reproduce, prove it before
reporting it. Procedure:

1. Write a minimal temporary probe — usually an inline `unittest` in the
   module under test, or a script against the public API.
2. Run it and capture the output; assert the behavior you believe is correct
   and watch which assertion fires.
3. Revert the probe (`git checkout -- <file>`) and confirm the tree is clean
   (`git status` shows no tracked modifications).
4. In the report, label the finding "confirmed by execution (probe: …)" with
   the observed result, or "inferred from code reading" if not probed.

Worked example (B1, cross-checkpoint turn split): the probe sent checkpoint 1
evicting only the user message of turn N (episode `tN_N`), drained, and the
query found it; checkpoint 2 evicted only the assistant message of the same
turn — after drain, the user piece returned 0 matches and only the assistant
piece remained. That observed replacement is what made the data-loss claim
verifiable, not just plausible.

Probe fixture validity: a failing probe may prove a guard works, not a bug.
The same probe first failed with session id `…-rev1` because `isValidId`
requires a 4-hex suffix (D12 format) — the checkpoint was correctly refused.
Check your own fixtures against the domain's constraints before concluding
the code is wrong.

## 5. Cross-event state checklist

Bugs that live between events are invisible in single-function control flow.
For state that persists across invocations (caches, databases, checkpoints,
cron jobs, idempotent endpoints), ask:

- What happens when the same key/topic/row is written twice with different
  content? (replace vs merge vs reject — is replace silent data loss?)
- Is the second write's metadata (timestamps, turn ranges) preserved, or
  does it clobber the first?
- Are readers guaranteed to see either the old or the new state, never a
  torn mix?
- Does deduplication key on the right identity (e.g. content hash vs
  topic+content)?
- What does a crash between two steps of one event leave behind?

## 6. Severity, trigger plausibility, and the policy split

- Classify severity, then state **trigger plausibility** explicitly:
  "normal operation" vs "config change" vs "rare edge". Two Important data
  losses can deserve different fates: one triggered by everyday turns, one
  only by an operator changing the embedding model.
- **Policy split**: technical severity is not the final word. A user may
  decide "the loss is the operator's problem — don't fix" (that happened to
  B2 in the Phase 1 review). Report the technical facts; leave the
  accept/reject call to the human.
- Structure the report so each finding can be accepted or rejected
  individually (numbered findings, one decision line each). This is what
  lets the user annotate the review (`user: …`) and turn accepted findings
  directly into a fix design + implementation plan.

## 7. Worked summary (Phase 1 review, 2026-09-06)

- Spec conformance: 12 requirements mapped to code evidence; 3 deliberate
  deviations recorded with their blessing decisions.
- Empirical probe: B1 confirmed by execution (replace semantics observed);
  probe reverted, tree clean.
- Cross-event analysis: the bug was the interaction of two separate
  compression checkpoints with remove-then-add — nothing wrong in any single
  function.
- Policy split: B2 rated Important technically, but the user's policy call
  (operator's responsibility) removed it from scope; the accepted findings
  (B1/B3/B4/B5) became a fix design (`review_fixes_system_design.md`) and an
  implementation plan (`review_fixes_implementation_plan.md`).
