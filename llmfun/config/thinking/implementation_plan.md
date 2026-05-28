A structured protocol for an LLM agent to execute tasks from a design plan and produce working code. Use this when implementing features, services, or modifying existing code.

# Implementation Strategy (LLM Agent)
**Important**: Each task describes how to change the code, but do **NOT** contain any significant amount of code. The plan lists tasks. The plan lists tasks that will later on be executed on the command of the user.
**Important**: Write the plan to a file and inform the user.

## 1. Understand Requirements

- **Read the task list**: Understand what needs to be implemented from the design plan.
- **Identify acceptance criteria**: Note what "done" looks like for each task.
- **Check existing code**: Read related modules to understand integration points.
- **Identify dependencies**: Note required imports, types, interfaces, and external services.

## 2. Plan Task Execution

- **Order tasks by dependency**: Execute tasks in dependency order (e.g., interfaces before implementations).
- **Identify patterns**: Note existing patterns in the codebase to follow.
- **Plan testing**: Consider what tests will be needed for each task.
- **Break complex tasks**: If a task is too large, decompose it into sub-tasks.

## 3. Execute Task: Foundation First

For each task:
- **Create types/interfaces first**: Define data structures and interfaces before implementation.
- **Add imports**: Include all required imports at the top of the file.
- **Write error handling**: Establish error types and handling patterns early.
- **Implement core logic**: Build the main functionality before edge cases.

## 4. Execute Task: Incremental Implementation

For each task:
- **Start simple**: Get working code first, optimize later.
- **Handle errors**: Add error handling for expected failure modes.
- **Add validation**: Validate inputs at all entry points.
- **Write tests**: Add tests as part of the task, not after.

## 5. Verify Task Completion

For each task:
- **Check syntax**: Ensure code compiles and is syntactically valid.
- **Trace execution**: Follow the code flow to verify it matches the task requirements.
- **Check edge cases**: Verify edge cases and error paths are handled.
- **Verify acceptance criteria**: Confirm all acceptance criteria are met.
- **Check consistency**: Ensure code follows project conventions and patterns.

## 6. Refactor and Clean Up

After completing all tasks:
- **Remove duplication**: Extract common code into reusable functions.
- **Simplify complexity**: Reduce nested conditionals, long functions, large classes.
- **Improve naming**: Ensure names are clear and consistent.
- **Update documentation**: Add comments explaining why, not what.
- **Remove dead code**: Delete unused functions, imports, and variables.

## 7. Output Format

Produce output in this structure for each task:

```markdown
## Task: [Task Name]

### Description
[What this task accomplishes]

### Changes Made
- [File 1]: [Brief description of changes]
- [File 2]: [Brief description of changes]

### Code Produced
[Code files created or modified - one file per code block]

### Verification
- [ ] Code compiles without errors
- [ ] All acceptance criteria met
- [ ] Edge cases handled
- [ ] Follows project conventions
- [ ] Tests added/updated

### Notes
[Any additional notes about the implementation]
```

