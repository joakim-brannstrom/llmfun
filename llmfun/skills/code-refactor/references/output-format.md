# Code Refactor Output Format

Produce output in this structure:

```markdown
## Refactoring Tasks

### Task 1: [Task Name]
- **Files**: [list of files]
- **Acceptance**: [What verifies this task is done correctly]

### Task 2: [Task Name]
- **Files**: [list of files]
- **Acceptance**: [What verifies this task is done correctly]

### Task 3: [Task Name]
- **Files**: [list of files]
- **Acceptance**: [What verifies this task is done correctly]

## Code Produced
[Refactored code files - one file per code block]

## Verification
- [ ] Public interfaces unchanged
- [ ] All code paths still execute correctly
- [ ] Edge cases handled properly
- [ ] No new dependencies introduced
- [ ] All tests pass
```

## Example

```markdown
## Refactoring Tasks

### Task 1: Extract duplicate validation logic
- **Files**: auth.py, utils.py
- **Acceptance**: Validation logic extracted to utils, all callers updated, tests pass

### Task 2: Rename unclear variable names
- **Files**: parser.py
- **Acceptance**: All references updated, code compiles, tests pass

### Task 3: Simplify nested conditionals
- **Files**: processor.py
- **Acceptance**: Logic simplified, behavior preserved, tests pass

## Code Produced
[Refactored code files - one file per code block]

## Verification
- [x] Public interfaces unchanged
- [x] All code paths still execute correctly
- [x] Edge cases handled properly
- [x] No new dependencies introduced
- [x] All tests pass
```
