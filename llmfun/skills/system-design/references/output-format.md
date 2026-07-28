# System Design Output Format

Produce output in this structure:

```markdown
# System Design Tasks

## Critical (P0)
1. **Task**: [Task name]
   - **Acceptance**: [Clear "done" criteria]
   - **Dependencies**: [List of task numbers or "None"]

2. **Task**: [Task name]
   - **Acceptance**: [Clear "done" criteria]
   - **Dependencies**: [List of task numbers]

## High (P1)
3. **Task**: [Task name]
   - **Acceptance**: [Clear "done" criteria]
   - **Dependencies**: [List of task numbers]

## Medium (P2)
4. **Task**: [Task name]
   - **Acceptance**: [Clear "done" criteria]
   - **Dependencies**: [List of task numbers]

## Low (P3)
5. **Task**: [Task name]
   - **Acceptance**: [Clear "done" criteria]
   - **Dependencies**: [List of task numbers]

## Notes
- Execute tasks in order
- Each task produces code, tests, or documentation
- Do not include code in task descriptions
```

## Example

```markdown
# System Design Tasks

## Critical (P0)
1. **Task**: Define User schema and migration
   - **Acceptance**: Schema defined, migration script created
   - **Dependencies**: None

2. **Task**: Implement authentication middleware
   - **Acceptance**: Middleware handles JWT validation, passes unit tests
   - **Dependencies**: Task 1 (User schema)

## High (P1)
3. **Task**: Create REST endpoints for /users
   - **Acceptance**: CRUD endpoints implemented, OpenAPI spec generated
   - **Dependencies**: Task 1, Task 2

4. **Task**: Add input validation for all endpoints
   - **Acceptance**: All inputs validated, error responses standardized
   - **Dependencies**: Task 3

## Medium (P2)
5. **Task**: Implement caching layer
   - **Acceptance**: Cache middleware added, cache invalidation works
   - **Dependencies**: Task 3

## Notes
- Execute tasks in order
- Each task produces code, tests, or documentation
- Do not include code in task descriptions
```
