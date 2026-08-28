# Implementation Plan Workflow — Detailed Steps

All plan files shall be saved in the directory `plan/` (relative to the
project root).

## Phase 1: Understand Requirements

- **Read the design/use case**: Understand what needs to be built at a high level.
- **Identify scope**: Note the boundaries of the feature — what is in scope and what is not.
- **Extract acceptance criteria**: Determine what "done" looks like for the overall feature.
- **Note constraints**: Identify technical constraints, performance requirements, or integration points.
- **Identify dependencies**: Note required imports, types, interfaces, and external services.

## Phase 2: Survey the Codebase

- **Read existing code**: Examine files at integration points to understand current state.
- **Identify patterns**: Note naming conventions, architecture patterns, error handling style.
- **Note what already exists**: Identify reusable code, existing types, or shared infrastructure.
- **Find gaps**: Determine what needs to be created vs. what needs to be modified.
- **Capture anchor candidates**: while reading, record the line numbers of
  integration points and pattern exemplars (`file.d:123-145`); reuse them as
  anchors in Phases 4–5 instead of re-reading files.

## Phase 3: Decompose into Tasks

Break the feature into small, focused tasks. Each task should:

- **Be self-contained**: Complete enough to verify on its own.
- **Have a clear purpose**: One responsibility per task.
- **Be small enough**: Implementable in a single focused coding session.
- **Be verifiable**: Include clear acceptance criteria and verification steps.
- **List specific files**: Identify exactly which files will be created or modified.

**Task size budget** — check every task against it; over any budget → split:

- **Bytes**: task file ≤ ~8 KB (~2.5k tokens) target, ~10 KB hard ceiling.
- **Deliverables**: 1 primary deliverable — one new module OR one surgical
  change, not both at scale.
- **Source opened**: ≤ 5 files, read via the task's line anchors (targeted
  ranges, ≤ ~15 KB total). A task that forces a whole-file read of a
  > 500-line file has insufficient anchors — fix the plan.
- **Verification items**: ≤ 8 (aim ≤ 5). Each item costs ≥ 1 build/test
  cycle.
- **Hard constraints**: ≤ ~15 separate constraints in Changes Made.
- **Open decisions**: ≤ 1; it must name a placeholder constant and the task
  that finalizes it.

**Many open unknowns**: if the design leaves more than one unknown open,
add a read-only spike task first (`task_00.md`): investigate the codebase
and resolve the unknowns without code changes, writing the answers where
the tasks consume them; re-run Phase 6 before execution.

Still true: "implement the entire feature" is too big; "fix a typo" is too
small (inline it in another task).

**Split patterns** for over-budget tasks:
- Component with a lifecycle + a query/consume path → one task each; the
  second task carries the first's exact call signature verbatim, so its
  executor never opens the first task's source.
- Producer/worker half + consumer half → separate tasks.
- Code + full test matrix → code task (smoke tests only) + follow-up test
  tasks (exact-match, regression guards, end-to-end).
- Refactor → seam/extraction task, then caller-migration task.
- A task file over the byte budget is a split trigger by itself, even if the
  shape budget looks fine.

Write each task to `task_NN.md` (zero-padded, e.g. `task_01.md`) as soon as
its scope and details are decided. The task will be revised later, but
capturing the decomposition early is more important than getting it perfect.

## Phase 4: Order by Dependency

Arrange tasks so each builds on previously completed ones:

1. **Foundation tasks first**: Types, interfaces, data structures
2. **Core logic next**: Business logic, algorithms, main functionality
3. **Integration tasks**: Wiring components together, API connections
4. **Testing tasks**: Unit tests, integration tests
5. **Polish tasks**: Error handling, edge cases, documentation

**Dependency rules:**
- Types before implementations that use them
- Interfaces before concrete implementations
- Utility functions before callers
- Core modules before dependent modules
- Tests after the code they test

## Phase 5: Write the Plan

Write each task to its own file (`task_NN.md`). Each task documents:

- **Task name**: Clear, action-oriented title
- **Description**: What this task accomplishes (not how)
- **Changes Made**: Specific files and what changes
- **Verification checklist**: Criteria for marking the task complete
- **Notes**: Any constraints, warnings, or context

Write the implementation overview that ties all tasks together to
`plan/implementation_plan.md`. It contains:

- **Overview**: What the plan as a whole implements
- **Task Order**: An ordered list of the task files (e.g. `1. task_01.md — Create User Model`)
- **Cross-cutting Concerns**: Anything more than one task depends on — shared types, conventions, test setup, error handling style
- **Standing Executor Rules**: the standing rules every task runs under —
  build/test commands, regression command, stop-and-re-plan guard, anchor
  verification discipline, decision-letter table, global do-not-reintroduce
  warnings (see below)

The executor reads `implementation_plan.md` first and then the task file,
keeping both in context — conventions stated in the overview need no
restatement in task footers. Keep cross-cutting items terse.

**Line anchors (execution efficiency)** — the biggest token saving at
execution time:

- Every task that edits existing code carries anchors for every edit site
  and every pattern file it imitates, as `path/file.d:123-145`. Verify each
  anchor against the source during Phase 2.
- Anchors let the executor read ~20 lines instead of an 1800-line file.
- A task that edits a > 300-line file with zero line anchors is a defect.
- Pattern references ("follow the pattern in X") must name file and lines.
- The master anchor list lives in the overview or `plan/anchors.md`; each
  task file restates only the anchors it uses.
- Anchors go stale as earlier tasks in the same plan shift lines. The
  overview's Standing Executor Rules tell the executor: verify anchor
  content before each edit (readFile, compare against the expected text);
  if stale, re-anchor by content (grep) and proceed — never guess line
  numbers.

**Self-containment (reading efficiency)** — the executor reads the overview
first, then the task file; together the two must suffice:

- Restate, don't reference: any constant, format, error string, or decision
  the task depends on appears in the overview or the task file. Pointers to
  the design doc are allowed only when the inline text is a complete
  restatement, keeping the design doc optional to read.
- Decision references (D5, F10, N3, …) are defined once in the overview's
  decision-letter table; a task file may add task-specific definitions in
  its footer.
- Cross-task contracts: when a task calls an earlier task's API, write the
  exact call signature inline so the executor never opens the earlier
  task's source.
- Verbatim contract artifacts (signatures, struct definitions, format
  strings, render templates, exact result strings, test scaffolding) are
  cheap for the executor to copy; prose that forces inference ("like the
  existing one") is expensive and ambiguous — prefer the artifact.

**Standing executor rules (write once, in the overview — the executor reads
it before every task):**

- Build/test: the repo's build/test commands, plus the regression command
  (the full standard test suite) that every task's Verification must pass.
  Run builds with output redirected to a log file and inspect with
  grep/head — never capture full compiler output inline (a single failed
  compile in a large D/C++ project can exceed the executor's entire context
  window).
- Stop-and-re-plan guard: if a task's actual work exceeds its read set or
  size budget, the executor stops and reports it for a Phase 6 re-plan —
  never pushes through an overgrown task.
- Anchors: verify anchor content before each edit (readFile); if stale,
  re-anchor by content (grep) — never guess line numbers.
- Decision letters: one-line definitions (D6, F10, N3, …) so no task file
  needs the design doc.
- Global "do not reintroduce X" warnings after a design revision.

Per-task items stay in the task file's Notes: documented holes ("accepted,
do not fix here") and task-specific open decisions with their placeholder
constant and finalizing task.

## Phase 6: Revise

Revise the tasks, their content, and their order to ensure that the plan is
consistent and logical. Revision may add, split, merge, or delete task files.
If the order changed before execution, renumber the task files and update the
ordered list in `implementation_plan.md` (mid-execution splits follow the
rule below — no renumbering).

Check size budgets during revision (measure with `wc -c`; the budgets are a
hard gate, not a guideline):

- Every task file ≤ ~8 KB (hard ceiling ~10 KB) and within the shape budget
  (Phase 3) — split the rest.
- The overview ≤ ~10 KB — move overflow into the task files that use it or
  into companion files (`plan/anchors.md`).
- No task edits a > 300-line file without line anchors.
- Every decision reference resolves from the overview + task file (read both
  cold, without the design doc, to check).
- **Design-vs-code conflicts**: when the design doc and the existing code
  disagree, do not resolve silently — record the conflict in the task's
  Notes with the conservative interpretation chosen, so the user can
  override at review.
- **Mid-execution splits**: if a task turns out too big during execution,
  insert `task_NNa.md` right after it (never renumber already-completed
  tasks), update the ordered list in `implementation_plan.md`, and re-run
  this size-budget gate on the new file.

## Phase 7: Save and Report

- **Write the files**: Ensure the overview and all task files are present in `plan/`.
- **Present task summary**: Show the user the ordered task list.
- **Explain dependencies**: Note why tasks are ordered as they are.
- **Invite review**: Ask the user if the plan makes sense before execution.
