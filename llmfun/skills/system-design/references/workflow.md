# System Design Workflow — Detailed Steps

Write each phase's results to `plan/design_notes.md` before moving to the
next phase.

## Phase 1: Clarify Requirements

- **Extract functional requirements**: List what the system must do, grouped by domain.
- **Extract non-functional requirements**: Define performance, scalability, availability, security targets.
- **Prioritize**: Classify requirements as Must/Should/Could/Won't (MoSCoW).
- **Identify constraints**: Document technical, budget, timeline, and existing system constraints.
- **Decompose into tasks**: Break each requirement into discrete, executable tasks with acceptance criteria.

## Phase 2: Design Architecture

- **Define boundaries**: Document system boundaries and external dependencies.
- **Evaluate architecture styles**: Consider monolith, microservices, serverless, or event-driven based on:
  - Expected scale and growth
  - Deployment complexity tolerance
  - Fault isolation needs
  - Existing infrastructure
- **Choose technologies**: Select technologies based on:
  - Compatibility with existing stack
  - Ecosystem maturity
  - Performance requirements
  - Licensing implications
- **Convert decisions into tasks**: Each architectural decision becomes a task.

## Phase 3: Design Components

- **Decompose by domain**: Split system into logical modules/bounded contexts.
- **Define responsibilities**: Assign single responsibility to each component.
- **Convert components into tasks**: Each component becomes a list of tasks:
  - "Task: Define interface for UserService"
  - "Task: Implement UserService with repository pattern"
  - "Task: Write unit tests for UserService"

## Phase 4: Design Data

- **Model data**: Define entity relationships and schema structure.
- **Convert data design into tasks**:
  - "Task: Create User schema with migration"
  - "Task: Implement data access layer"
  - "Task: Write integration tests for data layer"

## Phase 5: Design Interfaces

- **Define contracts**: Specify request/response formats, status codes, error handling.
- **Convert interface design into tasks**:
  - "Task: Define REST API endpoints with OpenAPI spec"
  - "Task: Implement authentication middleware"
  - "Task: Write API integration tests"

## Phase 6: Address Cross-Cutting Concerns

- **Security**: Specify threat modeling, encryption, secrets management approach.
- **Observability**: Define logging, metrics, and tracing strategy.
- **Resilience**: Specify circuit breakers, retries, fallbacks approach.
- **Convert into tasks**:
  - "Task: Implement input validation for all endpoints"
  - "Task: Add structured logging to all services"
  - "Task: Configure health check endpoints"

## Phase 7: Validate and Finalize Tasks

- **Check completeness**: Verify all requirements are addressed by tasks.
- **Order by dependency**: Arrange tasks so dependencies come first.
- **Assign priorities**: Mark tasks as P0 (critical), P1 (high), P2 (medium), P3 (low).
- **Define acceptance criteria**: Each task must have clear "done" criteria.
- **Finalize task list**: The output is a prioritized, ordered task list.

## Phase 8: Produce Output

Report design tasks using the output format template in `output-format.md`.

## Compression checkpoint (`requestCompression`)

`requestCompression` is the design's active checkpoint: it compresses on
your terms and re-injects your message-to-self afterwards — unlike forced
compression at ~90%, which summarizes without your control.

- Trigger points: when the harness injects the 80% `[SYSTEM NUDGE - NOT
  USER INPUT]`, and at every phase boundary in a long design — even below
  80%. Never mid-phase.
- Write the message-to-self as a briefing for a new instance with no memory
  of the session:

```
[Design handoff]
- Goal: design <system> for <use case>; output <path>
- Progress: phases done <list>; current phase
- Decisions so far: <decision + rationale, one-liners>
- Requirements captured: <Must/Should counts + key constraints>
- Open questions: <list>
- Task list so far: <count + key tasks>
- User decisions/constraints: <any>
- Next action: <first task of the current phase>
```

## Resume protocol (after a context compression)

0. If you requested the compression yourself, the re-injected handoff
   message is your first memory — read it, then continue below.
1. Re-read `plan/design_notes.md` first — it says where you were and what
   you decided.
2. Continue from the first unfinished phase; keep updating the file.
