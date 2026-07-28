---
name: system-design
description: >-
  Design software systems by breaking down requirements into architectural
  decisions and executable tasks. Use when planning new components, services,
  or system architectures. Triggers on: system design, design system,
  architecture, architectural decisions, plan component, plan service,
  system architecture, design tasks, break down requirements.
version: 1.0.0
---

# System Design Skill

Design software systems by breaking down requirements into architectural decisions
and executable tasks. The output is a **list of tasks, not code**.

## When to Use

Use this skill when:
- Planning new components, services, or system architectures
- Breaking down requirements into design decisions and tasks
- Evaluating architecture styles or technology choices
- Designing data models, interfaces, or cross-cutting concerns

## Core Principle

**Design first, code later.** Produce a prioritized, ordered task list that
describes what to build. Each task has acceptance criteria but no implementation
code. Pseudocode is acceptable if it clarifies the design or specification.

**Pipeline**: `system-design` → `implementation-plan` → `code-task`

1. **system-design** (this skill): High-level design tasks with architectural decisions
2. **implementation-plan**: Breaks design tasks into detailed, verifiable code tasks
3. **code-task**: Executes each code task and produces actual implementation

## Design Areas

| Area | Focus | Example Tasks |
|------|-------|---------------|
| **Requirements** | Functional, non-functional, constraints, MoSCoW prioritization | "Define performance targets", "Document constraints" |
| **Architecture** | Boundaries, style (monolith/microservices), technology choices | "Evaluate monolith vs microservices", "Choose database" |
| **Components** | Domain decomposition, responsibilities, interfaces | "Define UserService interface", "Implement with repository pattern" |
| **Data** | Entity relationships, schema, migrations, access layer | "Create User schema", "Implement data access layer" |
| **Interfaces** | Contracts, request/response formats, error handling | "Define REST API endpoints", "Implement auth middleware" |
| **Cross-Cutting** | Security, observability, resilience | "Add structured logging", "Configure health checks" |

## Rules

- **Write the plan to a file**: Always save the task list and inform the user.
- **No implementation code**: Tasks describe design decisions and specifications. The `implementation-plan` skill breaks these into code tasks.
- **Pseudocode is OK**: If pseudocode clarifies the design or specification, include it. Full implementation code is not.
- **Dependency order**: Order tasks so foundations come before dependents.
- **Prioritize**: Mark tasks as P0 (critical), P1 (high), P2 (medium), P3 (low).
- **Acceptance criteria**: Each task must have clear "done" criteria.

## Workflow

Follow the protocol. See `references/workflow.md` for detailed steps.

1. **Clarify Requirements** — Extract functional/non-functional requirements, prioritize, identify constraints.
2. **Design Architecture** — Define boundaries, evaluate styles, choose technologies.
3. **Design Components** — Decompose by domain, define responsibilities.
4. **Design Data** — Model data, define schema and access patterns.
5. **Design Interfaces** — Define contracts, specify formats and error handling.
6. **Address Cross-Cutting** — Security, observability, resilience.
7. **Validate and Finalize** — Check completeness, order by dependency, assign priorities, define acceptance criteria.
8. **Produce Output** — Report prioritized task list in the standard format.

## Output Format

Report design tasks using the structure in `references/output-format.md`.

## References

- Detailed workflow: `references/workflow.md`
- Output format template: `references/output-format.md`
