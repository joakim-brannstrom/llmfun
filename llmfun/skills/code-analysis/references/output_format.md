# Code Analysis Report Output Format

Use this template when producing the analysis report at `plan/code_analysis.md`.

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
