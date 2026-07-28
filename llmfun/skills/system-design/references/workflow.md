# System Design Workflow — Detailed Steps

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
