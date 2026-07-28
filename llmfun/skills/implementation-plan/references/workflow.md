# Implementation Plan Workflow — Detailed Steps

## Phase 1: Understand Requirements

- **Read the design/use case**: Understand what needs to be built at a high level.
- **Identify scope**: Note the boundaries of the feature — what is in scope and what is not.
- **Extract acceptance criteria**: Determine what "done" looks like for the overall feature.
- **Note constraints**: Identify technical constraints, performance requirements, or integration points.
- **Identify dependencies**: Note required imports, types, interfaces, and external services.

## Phase 2: Survey the Codebase

- **Read existing code**: Examine files at integration points to understand current state.
- **Identify patterns**: Note naming conventions, architecture patterns, error handling style.
- **Note what already exists**: Identify reusable code, existing types, or shared infrastructure.
- **Find gaps**: Determine what needs to be created vs. what needs to be modified.

## Phase 3: Decompose into Tasks

Break the feature into small, focused tasks. Each task should:

- **Be self-contained**: Complete enough to verify on its own.
- **Have a clear purpose**: One responsibility per task.
- **Be small enough**: Implementable in a single focused coding session.
- **Be verifiable**: Include clear acceptance criteria and verification steps.
- **List specific files**: Identify exactly which files will be created or modified.

**Good task sizes:**
- Create a new type/interface: 1 task
- Implement a single function/method: 1 task
- Add tests for a feature: 1 task
- Wire up integration points: 1 task

**Bad task sizes:**
- "Implement the entire feature" — too big
- "Fix a typo" — too small (inline in another task)

## Phase 4: Order by Dependency

Arrange tasks so each builds on previously completed ones:

1. **Foundation tasks first**: Types, interfaces, data structures
2. **Core logic next**: Business logic, algorithms, main functionality
3. **Integration tasks**: Wiring components together, API connections
4. **Testing tasks**: Unit tests, integration tests
5. **Polish tasks**: Error handling, edge cases, documentation

**Dependency rules:**
- Types before implementations that use them
- Interfaces before concrete implementations
- Utility functions before callers
- Core modules before dependent modules
- Tests after the code they test

## Phase 5: Write the Plan

For each task, document:

- **Task name**: Clear, action-oriented title
- **Description**: What this task accomplishes (not how)
- **Changes Made**: Specific files and what changes
- **Verification checklist**: Criteria for marking the task complete
- **Notes**: Any constraints, warnings, or context

## Phase 6: Save and Report

- **Write the plan to a file**: Save as markdown in an accessible location.
- **Present task summary**: Show the user the ordered task list.
- **Explain dependencies**: Note why tasks are ordered as they are.
- **Invite review**: Ask the user if the plan makes sense before execution.
