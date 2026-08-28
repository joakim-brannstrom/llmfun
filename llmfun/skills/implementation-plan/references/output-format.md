# Implementation Plan Output Format

## Overview File

The overview is saved to `plan/implementation_plan.md`. Start it with:

```markdown
# Implementation Plan: [Feature/Use Case Name]

## Overview
[Brief description of what this plan implements]

## Task Order
Tasks should be executed in the order listed. Each task depends on the
tasks before it. The executor (e.g. the `code-task` skill) reads this
overview first, then the task file.

1. task_01.md — [Task 1 name]
2. task_02.md — [Task 2 name]

## Cross-cutting Concerns
[Anything more than one task depends on: shared types, conventions, test
setup, error handling style. Keep items terse — the executor reads this
overview first, so no restatement in task footers is needed.]

## Standing Executor Rules
[The executor reads these before every task:]
- Build/test: `[build cmd]`, `[test cmd]` — redirect output to a log file;
  inspect with grep/head, never inline full compiler output.
- Regression: every task's Verification ends with `[full test suite cmd]`
  passing.
- Stop-and-re-plan: if a task's actual work exceeds its read set or size
  budget, stop and report for a re-plan — never push through.
- Anchors: verify anchor content before each edit (readFile); if stale,
  re-anchor by content (grep) — never guess line numbers.
- Decisions: `D6: …`, `F10: …` — one-line definitions.
- Open issues: [design-vs-code conflicts and the conservative
  interpretation used; only if any]
```

## Size Budgets

The executing agent has a finite context window (assume ~128k tokens).
Overviews and task files are written to keep both the read side and the
execution side inside it:

- **Task file**: ≤ ~8 KB (~2.5k tokens) target, ~10 KB hard ceiling; plus
  the shape budget in workflow.md Phase 3 (1 deliverable, ≤ 5 source files
  via line anchors, ≤ 8 verification items, ≤ ~15 hard constraints, ≤ 1 open
  decision).
- **Overview**: ≤ ~10 KB. The plan is multi-file by design: overview + one
  file per task + optional companion files. If the anchor table or shared
  context overflows the overview, split it into `plan/anchors.md` (or
  similar) and link it from the overview. The overview never absorbs task
  detail; task detail never leaves the task file.
- **Reading order**: the executor reads `implementation_plan.md` first and
  then the task file, keeping both in context.
- **Per-task read set**: the executor should be able to complete a task by
  reading the overview + the task file + the anchored line ranges + the
  files it creates. If completing the task requires more, the task is too
  big — split it.

## Task Template

Each task is written to its own file.
Each task file follows this structure:

```markdown
## Task [Number]: [Task Name]

Priority: [P0/P1/P2]. Dependencies: [Task N, …].

### Description
[What this task accomplishes — 1-3 lines, one deliverable]

### Changes Made
- path/file.d (edit at 123-145): [change]
- path/new.d (new module): [what it contains]

[Verbatim contract artifacts as code blocks: exact signatures this task
defines or calls, struct definitions, format strings, exact error/result
strings, test scaffolding the unittests need. Restated in every task that
uses them — a little duplication is acceptable; the overview carries only
standing rules.]

### Verification
- [ ] [concrete items, ≤ 8; each costs ≥ 1 build/test cycle]
- [ ] Regression: the overview's regression command passes

### Notes
[Constraints; documented holes ("accepted, do not fix here"); task-specific
do-not-reintroduce warnings (global ones live in the overview); open
decisions with the placeholder constant and the task that finalizes it]

Plan pointers (task-specific only — standing rules live in the overview):
- [Decisions/conventions this task uses that are not in the overview, each
  one line; open decisions name the placeholder constant and the task that
  finalizes it; write "None" if the overview covers everything]
- Anchors above were verified at plan time; the executor re-verifies before
  each edit per the Standing Executor Rules.
```

The filename is `task_NN.md`, with the number zero-padded (e.g. `task_01.md`,
`task_12.md`) so that lexicographic sort matches numeric order.

## Example Plan

`plan/implementation_plan.md`:
```markdown
# Implementation Plan: User Authentication

## Overview
Add user authentication with JWT tokens to the API. Includes user model,
login endpoint, token generation, and middleware.

## Task Order
Tasks should be executed in the order listed. Each task depends on the
tasks before it. The executor (e.g. the `code-task` skill) reads this
overview first, then the task file.

1. task_01.md — Create User Model

## Cross-cutting Concerns
All models follow the patterns in models/. Passwords are hashed with bcrypt.

## Standing Executor Rules
- Build/test: `dub build`, `dub test` — redirect output to a log file;
  inspect with grep/head.
- Regression: `dub test` must pass for every task.
- Stop-and-re-plan if a task outgrows its read set or size budget.
- Verify anchor content before each edit; re-anchor by grep if stale.
```

In `task_01.md`:
```markdown
## Task 1: Create User Model

Priority: P0. Dependencies: none.

### Description
Define the User data structure and repository access for authentication.

### Changes Made
- models/user.d (new): User struct + repository
- models/auth.d (edit at 42-58): register the model on the existing registry

```d
// Verbatim contract artifact — the exact type Task 2 will call:
struct User { long id; string email; string passwordHash; }
```

### Verification
- [ ] Code compiles (`dub build`) without errors
- [ ] Unittest: create/read a User via the repository (temp dir, no network)
- [ ] Regression: `dub test` (the overview's regression command) passes

### Notes
- Passwords are bcrypt-hashed (pattern: models/user_test.d:30-41).
- No cascade delete for sessions — documented hole, do not fix here.

Plan pointers (task-specific only — standing rules live in the overview):
- None — the overview covers this task's conventions.
```
