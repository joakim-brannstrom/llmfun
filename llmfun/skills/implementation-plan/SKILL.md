---
name: implementation-plan
description: >-
  Break down a use case, feature, or system design into individual code tasks
  in a structured, sensible order. Use when planning what to implement before
  writing code. Triggers on: implementation plan, plan implementation, break
  down, task breakdown, task plan, feature plan, design tasks, what to build,
  plan the work, task order.
version: 1.3.0
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

- **Plan, don't code**: Tasks describe what to build. They carry contract
  code — exact signatures, structs, formats, error strings, test scaffolds —
  verbatim; never the implementation code they describe.
- **Write the plan to files**: Save the overview and each task file to `plan/` and inform the user.
- **Budget task size**: ≤ ~8 KB per task file (hard ceiling ~10 KB), ≤ 1
  deliverable, ≤ 5 source files opened via line anchors, ≤ 8 verification
  items. Over any budget → split (workflow.md, Phase 3).
- **Anchor and restate**: Every edit site gets a verified line anchor
  (`file.d:123-145`). The executor reads `implementation_plan.md` first,
  then the task file — between the two, every constant, format, error
  string, call signature, and decision the task needs must appear.
- **Budget the overview**: `implementation_plan.md` ≤ ~10 KB. The executor
  reads it before every task, so it carries the standing executor rules
  (build/test commands, regression command, stop-and-re-plan guard, anchor
  verification, decision letters); split other overflow into companion
  files (e.g. `plan/anchors.md`).
- **Dependency order**: Order tasks so foundations come before dependents.

## Workflow

Follow the planning protocol. See `references/workflow.md` for detailed steps.

1. **Understand Requirements** — Read the design/use case, identify scope, note constraints and dependencies.
2. **Survey the Codebase** — Read existing code at integration points, identify patterns, note what already exists.
3. **Decompose into Tasks** — Break the feature into small, focused, verifiable tasks. Draft each task file as you go.
4. **Order by Dependency** — Arrange tasks so each builds on previously completed ones.
5. **Write the Overview** — Finalize each task file and write the overview to `plan/implementation_plan.md`: task order, standing executor rules, cross-cutting concerns, and everything that is not the details of the individual tasks.
6. **Revise** — Ensure the tasks, their content, and their order are consistent and logical; check size budgets (measure files, split over-budget tasks); renumber task files if the order changed.
7. **Save and Report** — Write the overview and task files to `plan/` and present the task summary to the user.

## Task Template

Each task file (`task_NN.md`) is size-budgeted and, together with the
overview, self-contained — exact structure, verbatim contract artifacts,
line anchors, and the task-specific Plan pointers footer are defined in
`references/output-format.md`.

## References

- Detailed workflow: `references/workflow.md`
- Task template and plan format: `references/output-format.md`
