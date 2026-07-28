---
name: code-task
description: >-
  Implement a single task from an implementation plan and produce working code.
  Use when executing a specific task defined in a higher-level plan, or when the
  user provides a task with a description, files to change, and acceptance criteria.
  Triggers on: code task, implement task, execute task, task implementation,
  implementation plan task.
version: 1.0.0
---

# Code Task Skill

Implement a single task from an implementation plan and produce working code.
A task describes **what** to build and **which files** to change, but contains
**no significant code**. Your job is to produce the actual implementation.

## When to Use

Use this skill when:
- Executing a specific task from a higher-level implementation plan
- The user provides a task with a description, files to change, and verification criteria
- You need a structured protocol for implementing code changes step by step

## Rules

- **One task at a time**: Fully complete one task before moving to the next.
- **Read before writing**: Always read the current state of a file before editing it.
- **Verify after every edit**: Re-read or run code after changes to catch errors early.
- **Follow existing patterns**: Match the style and conventions of the surrounding code.
- **No speculation**: If the task description is ambiguous, ask for clarification.

## Workflow

Follow the 8-phase protocol. See `references/workflow.md` for detailed steps.

1. **Analyze the Task** — Read description, scope, acceptance criteria, notes.
2. **Survey the Codebase** — Read existing files, find integration points, detect patterns.
3. **Plan the Implementation** — Determine order, separate new vs modified, plan tests.
4. **Implement — Foundation Layer** — Create files, define types, add imports, scaffold signatures.
5. **Implement — Core Logic** — Happy path, error handling, validation, wire connections.
6. **Implement — Tests** — Write unit tests, place correctly, run and verify.
7. **Verify Completion** — Compile check, trace execution, check edge cases, acceptance criteria.
8. **Produce Output** — Report completed task in the standard format.

## Output Format

Report the completed task using the structure in `references/output-format.md`.

## References

- Detailed workflow: `references/workflow.md`
- Output format template: `references/output-format.md`
