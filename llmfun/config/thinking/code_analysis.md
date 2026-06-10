## Code Analysis Thinking Template

A structured protocol for an LLM agent to analyze existing source code and document findings before system design. **Use this when** starting a new feature or modification and you need to understand the existing codebase first.

# Code Analysis Strategy (LLM Agent)
**Important**: The output must be a structured analysis document, not code or tasks..
**Important**: Continuously write your findings to `plan/code_analysis.md` while you are analysing.
**Important**: Write the analysis to `plan/code_analysis.md`.
**Important**: After saving, call `loadFileToRAG("plan/code_analysis.md")`.

---

## 0. Define Analysis Scope

Before diving in, clarify what is being analyzed:

- **Analysis target**: [Full codebase / specific module / feature-related files]
- **Purpose**: [New feature / bug fix / refactoring / security audit / general understanding]
- **Depth level**: [Surface mapping / deep analysis / focused on specific concern]

### Incremental Analysis (Existing Report)

If a previous analysis report exists at `plan/code_analysis.md`, perform an incremental update instead of a full re-analysis:

1. **Read the old report**: Load `plan/code_analysis.md` and extract the "File Integrity" section containing the file list with MD5 hashes.
2. **Scan current files**: Use `listFilesInDirectory` with `recursive=1` to get the complete current file tree.
3. **Detect changes**:
   - **New files**: Files present now but not in the old report.
   - **Deleted files**: Files in the old report but no longer on disk.
   - **Modified files**: Compute `md5HashFile` for files present in both and compare hashes.
4. **Scope the re-analysis**: Only re-analyze sections affected by changed, new, or deleted files. Carry forward unchanged sections from the old report.
5. **Focus areas**:
   - New files: Full analysis and integration into the report.
   - Modified files: Re-read and re-analyze only the changed content.
   - Deleted files: Update references and remove from the report.
   - Unchanged files: Retain previous analysis without re-reading.

**Important**: This incremental approach saves time and context window on subsequent runs. Always start by checking for an existing report before doing a full analysis.


### Large Codebase Strategy

For projects with many files, prioritize systematically:

1. **Start with entry points**: Read `main` files, CLI entry points, API route definitions.
2. **Follow the critical path**: Trace 2-3 key user stories from entry point to storage.
3. **Read build config first**: `dub.json`, `CMakeLists.txt`, `package.json` etc. reveal structure and dependencies.
4. **Sample strategically**: Read the first 20 lines of files to understand purpose before committing to full reads.
5. **Identify peripheral code**: Distinguish core business logic from utilities, tests, configs, and generated code.
6. **Use `countLinesInFile`** to quickly gauge file sizes and prioritize smaller, dense files.
7. **Use `md5HashFile`** to detect unchanged files when doing incremental analysis — skip re-reading files whose hash matches the previous report.


---

## 1. Map the Codebase

- **List all files and directories**: Use `listFilesInDirectory` with `recursive=1` to get the complete project structure.
- **Categorize by layer**: Group files into layers (e.g., presentation, business logic, data access, utilities).
- **Note build configuration**: Identify build system (dub, make, cmake, etc.) and key configuration.

### External Dependencies

- **Third-party libraries**: List all external packages and their purposes.
- **APIs and services**: Document external API integrations and service dependencies.
- **Databases and storage**: Note database engines, ORMs, and storage backends.
- **Version constraints**: Record pinned versions and compatibility requirements.

---

## 2. Analyze Architecture

- **Identify architectural pattern**: Determine if it's layered, MVC, hexagonal, microservices, etc.
- **Map module dependencies**: Document which modules depend on which (dependency graph).
- **Identify boundaries**: Note clear module boundaries and any boundary violations.

---

## 3. Analyze Data Flow

- **Trace key flows**: Follow 2-3 important user stories through the code (e.g., request → response).
- **Identify data models**: Document core entities, their relationships, and where they're defined.
- **Map storage layers**: Note databases, caches, file systems, and how data persists.
- **Document state management**: How is application state managed and shared?

---

## 4. Analyze Code Patterns

- **Identify design patterns**: Note recurring patterns (factory, observer, repository, etc.).
- **Document conventions**: Naming conventions, error handling style, logging approach.
- **Note testing strategy**: Test frameworks, coverage, test organization, mock patterns.
- **Identify anti-patterns**: Code smells, tight coupling, god classes, duplication hotspots.

---

## 5. Analyze Interfaces and Contracts

- **Document public APIs**: List exposed endpoints, functions, or services.
- **Identify interfaces/abstract classes**: Note the abstraction layer and implementations.
- **Map configuration**: How is the system configured (env vars, config files, CLI args)?
- **Document error handling**: How are errors propagated and handled at boundaries?

---

## 6. Assess Code Quality

- **Complexity hotspots**: Identify the most complex files/functions. Use `countLinesInFile` and manual inspection of deeply nested functions to estimate cyclomatic complexity. Flag files over 500 lines or functions with deep nesting (>4 levels) as high-complexity candidates.
- **Duplication areas**: Note repeated code patterns that could be consolidated.
- **Technical debt**: Document known issues, TODOs, deprecated code, workarounds.
- **Documentation gaps**: Note missing or outdated documentation.

---

## 7. Security Analysis

- **Authentication and authorization**: How are users authenticated? What authorization model is used (RBAC, ABAC, etc.)? Where is auth logic located?
- **Input validation and sanitization**: Where and how is user input validated? Are there centralized validation patterns or ad-hoc checks?
- **Secrets management**: Are there hardcoded credentials, API keys, or tokens? How are secrets loaded (env vars, config files, secret managers)?
- **Known vulnerability patterns**: Look for SQL injection risks (string concatenation in queries), XSS (unsanitized output), path traversal, insecure deserialization, etc.
- **Sensitive data handling**: How are passwords, PII, and other sensitive data stored and transmitted?
- **Dependency security**: Note any dependencies with known vulnerabilities or outdated versions.

---

## 8. Performance Characteristics

- **Bottleneck identification**: Identify slow queries, blocking I/O calls, synchronous operations on hot paths, and N+1 query patterns.
- **Resource usage patterns**: Note memory-intensive operations, connection pool usage, file handle management, and CPU-heavy computations.
- **Caching strategies**: Document existing caching layers (in-memory, Redis, HTTP cache headers) and what is cached.
- **Concurrency model**: Identify the threading model, async patterns, event loop usage, and synchronization primitives.
- **Scalability constraints**: Note single-threaded bottlenecks, shared mutable state, and operations that don't scale horizontally.

---

## 9. Deployment and Infrastructure

- **Container setup**: Dockerfile, docker-compose, container orchestration (Kubernetes, Swarm).
- **CI/CD pipeline**: Build, test, and deploy configuration (GitHub Actions, GitLab CI, Jenkins, etc.).
- **Environment configurations**: Dev/staging/prod differences, environment-specific config files.
- **Service dependencies**: External services required at runtime (databases, message queues, caches).
- **Health checks and monitoring**: Liveness/readiness probes, logging aggregation, metrics endpoints.

---

## 10. Domain and Business Logic

- **Core business rules**: What are the critical business invariants? Where are they encoded (validation functions, domain objects, database constraints)?
- **Domain-specific terminology**: Document the language of the domain as used in code (entity names, operation names).
- **Validation rules**: Business-level validation beyond input sanitization (e.g., order total must be positive, user must be active).
- **Workflow state machines**: Document any state transitions, workflow engines, or process flows.

---

## 11. Version Control Context

If git is available, analyze recent activity:

- **Most modified files**: `git log --oneline --name-only` to find hot files.
- **Stable modules**: Files unchanged for extended periods (safe to depend on).
- **Active development areas**: Where the team is currently working.
- **Branch strategy**: Main branch protection, release branches, feature branch patterns.
- **Recent commit patterns**: What areas are getting attention? Any recurring fix patterns?

---

## 12. Identify Extension Points

- **Natural modification points**: Where should new features be added?
- **Plugin/hook systems**: Are there extension mechanisms already in place?
- **Configuration-driven behavior**: What can be changed without code modifications?
- **Risky areas**: Which parts are fragile, poorly tested, or complex to modify?

---

## 13. Verify Analysis Completeness

Before finalizing, confirm:

- [ ] Analysis scope is clearly defined
- [ ] All entry points identified
- [ ] Key data flows traced (at least 2-3)
- [ ] Module dependencies mapped
- [ ] External dependencies documented
- [ ] Security posture assessed
- [ ] Performance characteristics noted
- [ ] Extension points identified
- [ ] Risks and technical debt cataloged
- [ ] If incremental: Old report read and changes detected via `md5HashFile`
- [ ] If incremental: Only changed sections re-analyzed
- [ ] File integrity section updated with current MD5 hashes
- [ ] Output saved to `plan/code_analysis.md`
- [ ] Analysis loaded into RAG via `loadFileToRAG`


---

## 14. Output Format

Produce output in this structure:

```markdown
# Code Analysis Report

## Project Overview
- **Language**: [Primary language and version]
- **Build System**: [Build tool and configuration]
- **Architecture Pattern**: [Identified pattern]
- **Entry Points**: [List of main entry points]

## Codebase Metrics
- **Total Files**: [count]
- **Total Lines**: [approximate count]
- **Languages**: [breakdown if polyglot]
- **Test Coverage**: [estimated: Low/Medium/High]
- **Average Function Length**: [estimated lines]
- **Max Nesting Depth**: [estimated levels]

## Directory Structure
```
[Tree view of important directories and files]
```

## Module Dependencies
```
[Dependency graph showing module relationships]
```

## File Index
| File | Responsibility | Complexity | Test Coverage |
|------|---------------|------------|---------------|
| [path] | [what it does] | [Low/Med/High] | [Low/Med/High] |

## File Integrity

This section enables incremental analysis on subsequent runs. Compute `md5HashFile` for each source file and record the hash here.

| File | MD5 Hash |
|------|----------|
| [path] | [md5hex] |

**Note**: On re-analysis, compare current `md5HashFile` results against this table to identify new, modified, and deleted files. Only re-analyze sections affected by changes.

## Key Components


### Component 1: [Name]
- **Location**: [File path]
- **Responsibility**: [What it does]
- **Dependencies**: [What it depends on]
- **Dependents**: [What depends on it]
- **Complexity**: [Low/Medium/High]

### Component 2: [Name]
[same structure]

## Data Flow

### Flow 1: [Description, e.g., "User Authentication"]
1. [Step 1: entry point]
2. [Step 2: processing]
3. [Step 3: data access]
4. [Step 4: response]

## Data Models
- **[Entity 1]**: [Fields and relationships]
- **[Entity 2]**: [Fields and relationships]

## Code Patterns & Conventions
- **Error Handling**: [Pattern used]
- **Logging**: [Approach]
- **Testing**: [Framework and strategy]
- **Naming**: [Conventions]

## Security Posture
- **Authentication**: [Mechanism and location]
- **Input Validation**: [Approach and coverage]
- **Secrets Management**: [How secrets are handled]
- **Vulnerability Risks**: [Identified patterns and severity]

## Performance Characteristics
- **Known Bottlenecks**: [Identified slow paths]
- **Caching**: [Existing strategies]
- **Concurrency Model**: [Threading/async approach]
- **Scalability Concerns**: [Limitations]

## Deployment & Infrastructure
- **Container Setup**: [Docker/orchestration]
- **CI/CD**: [Pipeline tools]
- **Environment Config**: [Dev/staging/prod differences]

## Domain & Business Logic
- **Core Rules**: [Key business invariants]
- **Validation**: [Business-level constraints]
- **Workflows**: [State machines/process flows]

## Recent Changes (if git available)
- **Most modified files**: [Files with recent activity]
- **Stable modules**: [Long-unchanged files]
- **Active development areas**: [Where work is happening]

## Extension Points
1. **[Point 1]**: [Where and how to extend]
2. **[Point 2]**: [Where and how to extend]

## Risks & Technical Debt
- **[Risk 1]**: [Description and impact]
- **[Risk 2]**: [Description and impact]

## Design Constraints from Analysis
- **MUST**: [Patterns/conventions that must be preserved]
- **SHOULD**: [Recommended approaches for new code]
- **AVOID**: [Areas or patterns to steer clear of]

## Analysis Index (for downstream pipeline)
- **High-Risk Files**: [file paths with risk level]
- **Extension-Ready Modules**: [modules with low coupling, good test coverage]
- **Required Preserves**: [patterns/conventions that must not break]
- **Blocked Areas**: [areas requiring refactoring before modification]

## Recommendations for System Design
- [Specific suggestions for upcoming design phase]
```

---

## Notes for Downstream Pipeline

After producing the analysis:

1. **Save the file**: Write the report to `plan/code_analysis.md`.
2. **Load into RAG**: Call `loadFileToRAG("plan/code_analysis.md")` so system_design can query it.
3. **Pass to pipeline**: If running in a pipeline context, call `pipelineOutput` with a summary of key findings.
4. **Inform the user**: Confirm the analysis is complete and point them to the saved file.
