# Implementation Plan Output Format

## Plan Header

Start the plan with a header:

```markdown
# Implementation Plan: [Feature/Use Case Name]

## Overview
[Brief description of what this plan implements]

## Task Order
Tasks should be executed in the order listed. Each task depends on the
tasks before it. Tasks can be executed by the `code-task` skill.
```

## Task Template

Each task in the plan follows this structure:

```markdown
## Task: [Task Name]

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

## Example Plan

```markdown
# Implementation Plan: User Authentication

## Overview
Add user authentication with JWT tokens to the API. Includes user model,
login endpoint, token generation, and middleware.

## Task Order
Tasks should be executed in the order listed. Each task depends on the
tasks before it. Tasks can be executed by the `code-task` skill.

## Task: Create User Model

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
