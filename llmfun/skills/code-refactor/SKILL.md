---
name: code-refactor
description: >-
  Improve code structure while preserving behavior. Use when code is duplicated,
  overly complex, poorly organized, or violates project conventions. Triggers on:
  refactor, refactoring, code cleanup, improve code structure, simplify code,
  extract function, rename, reorganize, code smells, reduce complexity,
  code-refactor, refactor-code.
version: 1.0.0
---

# Code Refactor Skill

Improve code structure while preserving behavior. The output must be refactoring
tasks with actual code, not just descriptions.

## When to Use

Use this skill when:
- Code has duplication, long functions, large classes, or deep nesting
- Naming is unclear or inconsistent
- Code structure is poor (logic in wrong module, misplaced helpers)
- Complexity is high (too many branches, parameters, responsibilities)
- Code violates project conventions or patterns

## Core Principle

**Preserve behavior while improving structure.** Every refactoring step must
maintain identical external behavior. Tests should pass before and after.

## Refactoring Types

| Type | Action | When to Use |
|------|--------|-------------|
| **Extract** | Move code to functions, classes, or modules | Duplication, long functions, misplaced logic |
| **Rename** | Improve names for clarity and consistency | Unclear variables, functions, classes |
| **Simplify** | Remove dead code, combine conditions, flatten nesting | Complex logic, unused code, deep nesting |
| **Organize** | Reorder code, group related code into modules | Poor structure, scattered related code |

## Rules

- **Write the plan to a file**: Document refactoring tasks and inform the user.
- **Preserve behavior**: Public interfaces must remain unchanged.
- **Incremental steps**: Each task should be small and verifiable on its own.
- **Verify after each task**: Run tests and check behavior after every change.
- **One concern per task**: Each task addresses one specific improvement.

## Workflow

Follow the protocol. See `references/workflow.md` for detailed steps.

1. **Identify Targets** — Scan for code smells, check naming, analyze structure, assess complexity, check consistency.
2. **Plan Tasks** — Define scope, preserve behavior, break into tasks, check dependencies, order by impact.
3. **Execute Tasks** — Apply extract, rename, simplify, and organize refactoring types.
4. **Verify Preservation** — Check interfaces, trace execution, check edge cases, run tests.
5. **Produce Output** — Report refactoring tasks in the standard format.

## Output Format

Report refactoring tasks using the structure in `references/output-format.md`.

## References

- Detailed workflow: `references/workflow.md`
- Output format template: `references/output-format.md`
