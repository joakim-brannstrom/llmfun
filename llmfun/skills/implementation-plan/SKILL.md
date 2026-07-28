---
name: implementation-plan
description: >-
  Break down a use case, feature, or system design into individual code tasks
  in a structured, sensible order. Use when planning what to implement before
  writing code. Triggers on: implementation plan, plan implementation, break
  down, task breakdown, task plan, feature plan, design tasks, what to build,
  plan the work, task order.
version: 1.0.0
---

# Implementation Plan Skill

Break down a use case, feature, or system design into individual, verifiable
code tasks with a sensible execution order. This skill produces a plan of tasks
— it does **not** write implementation code. The `code-task` skill executes
each task from this plan.

## When to Use

Use this skill when:
- Planning how to implement a new feature or use case
- Breaking down a system design into executable code tasks
- Creating a structured task list before writing any code
- The user asks "what should I build first?" or "plan the implementation"

## Rules

- **Plan, don't code**: Produce tasks that describe what to build, not the code itself.
- **Write the plan to a file**: Always save the plan and inform the user.
- **Small, verifiable tasks**: Each task should be small enough to implement and verify in one pass.
- **Dependency order**: Order tasks so foundations come before dependents.
- **No significant code in tasks**: Tasks describe changes, not implementations.

## Workflow

Follow the planning protocol. See `references/workflow.md` for detailed steps.

1. **Understand Requirements** — Read the design/use case, identify scope, note constraints and dependencies.
2. **Survey the Codebase** — Read existing code at integration points, identify patterns, note what already exists.
3. **Decompose into Tasks** — Break the feature into small, focused, verifiable tasks.
4. **Order by Dependency** — Arrange tasks so each builds on previously completed ones.
5. **Write the Plan** — Document each task with description, files to change, and verification criteria.
6. **Save and Report** — Write the plan to a file and present the task summary to the user.

## Task Template

Each task in the plan follows this structure (see `references/output-format.md`):

```markdown
## Task: [Task Name]

### Description
[What this task accomplishes]

### Changes Made
- [File 1]: [Brief description of changes]
- [File 2]: [Brief description of changes]

### Verification
- [ ] Code compiles without errors
- [ ] All acceptance criteria met
- [ ] Edge cases handled
- [ ] Follows project conventions
- [ ] Tests added/updated

### Notes
[Any additional context or constraints]
```

## References

- Detailed workflow: `references/workflow.md`
- Task template and plan format: `references/output-format.md`
