# Implementation Plan Output Format

## Overview File

The overview is saved to `plan/implementation_plan.md`. Start it with:

```markdown
# Implementation Plan: [Feature/Use Case Name]

## Overview
[Brief description of what this plan implements]

## Task Order
Tasks should be executed in the order listed. Each task depends on the
tasks before it. Tasks can be executed by the `code-task` skill — point it
at the task file.

1. task_01.md — [Task 1 name]
2. task_02.md — [Task 2 name]

## Cross-cutting Concerns
[Anything more than one task depends on: shared types, conventions, test
setup, error handling style. Keep items terse — if a specific task depends
on one, restate it in that task's Notes.]
```

## Task Template

Each task is written to its own file.
Each task file follows this structure:

```markdown
## Task [Number]: [Task Name]

### Description
[What this task accomplishes — concise, focused description]

### Changes Made
- [File 1]: [Brief description of changes — create, modify, or add]
- [File 2]: [Brief description of changes]

### Verification
- [ ] Code compiles without errors
- [ ] All acceptance criteria met
- [ ] Edge cases handled
- [ ] Follows project conventions
- [ ] Tests added/updated

### Notes
[Any additional context, constraints, or warnings]
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
tasks before it. Tasks can be executed by the `code-task` skill — point it
at the task file.

1. task_01.md — Create User Model

## Cross-cutting Concerns
All models follow the patterns in models/. Passwords are hashed with bcrypt.
```

In `task_01.md`:
```markdown
## Task 1: Create User Model

### Description
Define the User data structure and database schema for authentication.

### Changes Made
- models/user.d: Create User struct with id, email, passwordHash fields
- models/user_test.d: Add unit tests for User model

### Verification
- [ ] Code compiles without errors
- [ ] All acceptance criteria met
- [ ] Edge cases handled
- [ ] Follows project conventions
- [ ] Tests added/updated

### Notes
Use bcrypt for password hashing. Follow existing model patterns.
```
