# Code Review Workflow — Detailed Steps

> **Context compression risk**: The review session may be compressed one or
> more times, causing details held only in the context window to be lost. You
> must continuously externalize findings to the state file `review_notes.md`.
> Never assume the context will retain intermediate notes, evidence, or
> findings. Write the step's output after every step and re-read the file to
> recover context after a compression.

## The state file (`review_notes.md`)

Location: in the directory of the final review output (e.g. next to
`review.md`, or `plan/review_notes.md` when the output goes to `plan/`).

Sections (update the relevant section after every step):

1. **Progress checklist** — the 8 workflow steps as `[ ]`/`[x]` items. For a
   large review, add one sub-item per file/module ("`[x] context.d` done").
   This is the resume point after a compression.
2. **Scope & criteria** — what is under review, the review-output path, the
   spec/design/plan documents, and the extracted requirement/decision list
   (F/N/D letters as named in the specs).
3. **Evidence log** — verified anchors (`file.d:123-145` + one-line content
   note) and facts learned while reading. Anchors let later steps re-read
   ~20 lines instead of whole files.
4. **Findings** — every finding the moment it is found (never accumulate in
   context): severity, trigger plausibility, file:line evidence, fix
   suggestion, and the label `confirmed by execution` or `inferred from code
   reading`.
5. **Probe log** — each temporary probe: what was written, run result,
   observed output, and that it was reverted.

## Step details (each step ends with a `review_notes.md` update)

### 1. Scope & Criteria
- Confirm the review target, the spec documents, and the output path; ask
  the user when unclear.
- Run the project's build/test suite once as a baseline (when feasible and
  cheap): redirect the output to a log file, record the result (pass/fail,
  module counts) in the Evidence log. This turns "does it build" into a
  verified fact — no amount of reading proves it, and inline compiler
  output can blow the context window.
- Read the spec/design/plan docs and extract the requirement and decision
  list into Scope & criteria. Never keep the criteria only in context.
- Large scope: order areas by risk — security-sensitive paths, concurrency,
  and persistence/data-loss paths first — or ask the user to prioritize.

### 2. Context Acquisition (128k-aware)
- Branch/PR/commit-range review: run `git diff <base>...HEAD --stat` first
  and list the changed files in the progress checklist; review changed
  files/hunks before anything else. Unchanged code is context, not the
  primary read set.
- Read one file at a time, in chunks sized to the environment's file-read
  cap; after EACH chunk, append a one-line summary + anchors to the
  Evidence log before reading the next chunk. Several small files may be
  read in parallel.
- Raw file content in context at any moment should stay ≤ ~15-20% of the
  window (at 128k ≈ 1 large file / ~20k tokens; larger windows may keep
  whole modules); the evidence log records anchors + summaries for
  everything you drop.
- Check off progress per file (and per chunk range for large files).

### 3. Spec Conformance
- Build the requirement → evidence table (format and example:
  `references/spec-review.md` §3). Mark each requirement OK /
  OK-with-deviation / MISSING / BUG only after checking the cited code.
- Record the table to the Findings section of `review_notes.md`.

### 4. Static Analysis
- Validate syntax and structure; audit imports (unused, missing, circular);
  verify naming compliance; check structural organization.
- Record every finding to `review_notes.md` as found.

### 5. Logic & Security Analysis
- Trace control flow (conditionals, loops, returns — all branches covered);
  verify data flow (variables from declaration to usage); trace state across
  repeated events (idempotency, replace-vs-merge, cross-call interactions —
  `references/spec-review.md` §5); scan for security vulnerabilities; check
  resource management; flag concurrency issues.
- Record every finding to `review_notes.md` as found.

### 6. Empirical Verification
- Suspected bugs cheap to reproduce: write a temporary probe (unittest/
  script), run it, observe the actual behavior, revert it, confirm the tree
  is clean (`references/spec-review.md` §4). Prefer the smallest runnable
  probe (a single unittest where the toolchain allows); full builds only
  when the probe needs them.
- A failing probe may prove a guard works — check probe fixtures before
  assuming a bug.
- Label each finding confirmed/inferred and log every probe.

### 7. Write the Review
- Assemble the final report from `review_notes.md`: the spec-conformance
  table (if spec-driven), then findings by severity, each with trigger
  plausibility, file:line evidence, fix-ready corrections (replacement code
  with 3-5 lines of context, dependency changes, brief rationale).
- Keep technical severity separate from operator policy decisions; structure
  findings so the user can accept or reject each one
  (`references/spec-review.md` §6).
- The final report is self-contained (standalone severity, evidence, and
  fixes) — the user acts on it without the state file; accepted findings
  typically feed a fix design/implementation plan, so keep findings
  numbered and decision-ready.
- `review_notes.md` may be kept or removed afterwards.

### 8. Cleanup
- `git status` shows no leftover probe modifications; remove temp files;
  deliver the report path to the user.

## Resume protocol (after a context compression)

0. If you requested the compression yourself, the re-injected handoff
   message is your first memory — read it, then continue below.
1. Re-read `review_notes.md` first — the progress checklist says where you
   were; the findings and evidence sections say what you already know.
2. Re-read only the spec documents listed under Scope & criteria.
3. Re-read only the anchor ranges recorded in the Evidence log — do not
   re-read whole files.
4. Continue from the first unchecked step; keep updating the file.

## Compression checkpoint (`requestCompression`)

`requestCompression` is the review's active checkpoint: it compresses on
your terms and re-injects your message-to-self afterwards — unlike the
forced compression at ~90%, which summarizes without your control and can
garble in-context evidence.

- Trigger points: when the harness injects the 80% `[SYSTEM NUDGE - NOT
  USER INPUT]`, and at any natural review boundary in a long review (after
  a step, after each file) — even below 80%. Never mid-file or
  mid-analysis.
- Write the message-to-self as a briefing for a new instance with no memory
  of the session:

```
[Review handoff]
- Goal: review <target> against <spec docs>; output <path>
- Progress: steps done <list>; current step; files done <list>; next <list>
- Key evidence (anchors): <file:line — one-liners>
- Findings so far: <severity + one-liner each>
- Probe state: <probes run/reverted>
- User decisions/constraints: <any>
- Next action: <first unchecked step or next file chunk>
```

- After the compression the harness re-injects the handoff; then still
  re-read `review_notes.md` for the full evidence and continue from the
  checklist. Handoff = narrative briefing; state file = complete record.
- If compression fires without your handoff (forced at ~90%), fall back to
  the resume protocol above.

## Context budget (adaptive — 128k, 512k, or 1M)

- Calibrate the working set to the window, not to a fixed number. Rule:
  hold at most ~15-20% of the window in raw source at once. At 128k that is
  ~1 large file (~20k tokens); at 512k-1M you may keep several whole
  modules in context — do, because cross-file analysis (call tracing,
  interaction bugs) improves with whole files in view.
- The file-read cap is a tool constraint, not a context one: read files in
  read-cap-sized chunks regardless, but at large windows keep the chunks in
  context instead of summarizing them away. Summarize to the Evidence log
  only to bound the working set (always at 128k; for anything you drop at
  any size).
- Execution output (build/test logs, compiler errors): redirect to a log
  file and inspect with grep/head at every size — inlining is wasted cost
  and attention, not a window-size question.
- Split the review into rounds (or ask the user to prioritize) only when
  the scope genuinely cannot fit the working set — at 128k that can happen
  fast; at 1M almost never.

## Step budgets

- One step = one review area (one file, or one analysis pass). If a step
  exceeds its budget, split it and extend the progress checklist instead of
  pushing through.
- A large review proceeds file-by-file: steps 2-5 repeat per file with
  per-file checklist items, so a compression at any point loses at most the
  current file's unrecorded notes.
