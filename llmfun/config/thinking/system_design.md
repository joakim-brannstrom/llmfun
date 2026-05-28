A structured protocol for an LLM agent to design software systems by breaking down requirements into architectural decisions and executable tasks. Use this when planning new components, services, or system architectures.

# System Design Strategy (LLM Agent)
**Important**: The output must be a list of tasks, not code. Code is produced by executing tasks.
**Important**: Write the plan to a file and inform the user.

## 1. Clarify Requirements

- **Extract functional requirements**: List what the system must do, grouped by domain.
- **Extract non-functional requirements**: Define performance, scalability, availability, security targets.
- **Prioritize**: Classify requirements as Must/Should/Could/Won't (MoSCoW).
- **Identify constraints**: Document technical, budget, timeline, and existing system constraints.
- **Decompose into tasks**: Break each requirement into discrete, executable tasks with acceptance criteria.

## 2. Design Architecture

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
- **Convert decisions into tasks**: Each architectural decision becomes a task (e.g., "Task: Evaluate monolith vs microservices for X component").

## 3. Design Components

- **Decompose by domain**: Split system into logical modules/bounded contexts.
- **Define responsibilities**: Assign single responsibility to each component.
- **Convert components into tasks**: Each component becomes a list of tasks:
  - "Task: Define interface for UserService"
  - "Task: Implement UserService with repository pattern"
  - "Task: Write unit tests for UserService"

## 4. Design Data

- **Model data**: Define entity relationships and schema structure.
- **Convert data design into tasks**:
  - "Task: Create User schema with migration"
  - "Task: Implement data access layer"
  - "Task: Write integration tests for data layer"

## 5. Design Interfaces

- **Define contracts**: Specify request/response formats, status codes, error handling.
- **Convert interface design into tasks**:
  - "Task: Define REST API endpoints with OpenAPI spec"
  - "Task: Implement authentication middleware"
  - "Task: Write API integration tests"

## 6. Address Cross-Cutting Concerns

- **Security**: Specify threat modeling, encryption, secrets management approach.
- **Observability**: Define logging, metrics, and tracing strategy.
- **Resilience**: Specify circuit breakers, retries, fallbacks approach.
- **Convert into tasks**:
  - "Task: Implement input validation for all endpoints"
  - "Task: Add structured logging to all services"
  - "Task: Configure health check endpoints"

## 7. Validate and Finalize Tasks

- **Check completeness**: Verify all requirements are addressed by tasks.
- **Order by dependency**: Arrange tasks so dependencies come first (e.g., "Define interface" before "Implement interface").
- **Assign priorities**: Mark tasks as P0 (critical), P1 (high), P2 (medium), P3 (low).
- **Define acceptance criteria**: Each task must have clear "done" criteria (e.g., "Passes linting", "Has 80% test coverage").
- **Finalize task list**: The output is a prioritized, ordered task list.

## 8. Output Format

Produce output in this structure:

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

