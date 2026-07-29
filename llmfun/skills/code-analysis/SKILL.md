---
name: code-analysis
description: >-
  Analyze existing source code and document findings before system design or
  feature development. Use when starting a new feature, modifying code,
  performing a code audit, or needing to understand an existing codebase.
  Triggers on keywords: code analysis, codebase analysis, analyze code,
  understand codebase, code audit, before design, system analysis.
version: 1.0.0
---

# Code Analysis Skill

## When to Use

Use this skill when the user:
- Wants to analyze an existing codebase before making changes
- Needs to understand the architecture and structure of a project
- Asks for a code audit, codebase review, or system analysis
- Is starting a new feature and needs context first

## Workflow

### 0. Define Analysis Scope

Clarify: **target** (full codebase / module / feature files), **purpose** (feature / bug fix / refactoring / audit), **depth** (surface / deep / focused).

#### Incremental Analysis (if `plan/code_analysis.md` exists)

1. Read old report; extract "File Integrity" MD5 hashes.
2. Scan current files with `listDirectory` (`recursive=1`).
3. Detect new, deleted, and modified files via `md5HashFile`.
4. Only re-analyze affected sections; carry forward unchanged content.

#### Large Codebase Strategy

1. Start with entry points, follow critical paths (2-3 user stories).
2. Read build config first. Sample strategically (first 20 lines).
3. Use `countLinesInFile` and `md5HashFile` to prioritize.

### 1-5. Core Analysis

1. **Map**: List files, categorize layers, note build config and dependencies.
2. **Architecture**: Identify pattern, map module dependencies, note boundaries.
3. **Data Flow**: Trace 2-3 key flows, document entities and storage.
4. **Patterns**: Design patterns, conventions, testing strategy, anti-patterns.
5. **Interfaces**: Public APIs, abstractions, configuration, error handling.

### 6-8. Quality, Security, Performance

6. **Quality**: Complexity hotspots (>500 lines, >4 nesting), duplication, debt.
7. **Security**: Auth, input validation, secrets, vulnerability patterns.
8. **Performance**: Bottlenecks, caching, concurrency, scalability.

### 9-12. Infrastructure, Domain, VCS, Extensions

9. **Deployment**: Containers, CI/CD, environments, monitoring.
10. **Domain**: Business rules, terminology, validation, workflows.
11. **VCS** (if git): Most modified files, stable modules, active areas.
12. **Extensions**: Natural modification points, plugin systems, risky areas.

### 13. Verify and Save

1. Check all items in `references/checklist.md`.
2. Write report to `plan/code_analysis.md`.
3. Call `loadFileToRAG("plan/code_analysis.md")`.
4. If in pipeline, call `pipelineOutput` with key findings.

## Output Format

Produce a structured markdown report at `plan/code_analysis.md`. See `references/output_format.md` for the complete template.

## References

- Full output format template: `references/output_format.md`
- Completeness checklist: `references/checklist.md`
- Utility scripts: `scripts/README.md`

## Utility Scripts

See `scripts/README.md` for full documentation. Quick reference:

| Script | Purpose |
|--------|---------|
| `check_braces.py` | Verify brace balance in source files |
| `count_loc.py` | Count LOC, language presets, brace analysis |
| `file_integrity.py` | Compute\/verify MD5 hashes |

