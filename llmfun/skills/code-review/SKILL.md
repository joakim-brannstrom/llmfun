---
name: code-review
description: >-
  Language-agnostic code review for bugs, security issues, style violations,
  and logic errors. Use when the user asks for a code review, PR feedback,
  quality check, or wants to verify code correctness.
version: 1.0.0
---

# Code Review

## When to Use

Use this skill when the user:
- Asks for a code review or PR feedback
- Wants to check code quality, correctness, or security
- Mentions "review", "audit", "check", or "verify" in context of code
- Wants to find bugs, vulnerabilities, or style issues

## Rules

**Always check for:**
- **Syntax errors**: Unclosed brackets, missing semicolons, invalid operators, type mismatches
- **Security issues**: Hardcoded secrets, injection vectors (SQL, XSS, command), missing input validation
- **Resource leaks**: Unclosed file handles, network connections, database cursors, unreleased memory
- **Logic errors**: Uncovered branches, uninitialized variables, off-by-one errors, race conditions
- **Import issues**: Unused imports, missing dependencies, circular imports
- **Naming consistency**: Variables, functions, classes follow project conventions

**Severity classification:**
- **Critical**: Compilation failure, runtime crash, security vulnerability, data corruption
- **Important**: Logic error, performance bottleneck, missing error handling, resource leak
- **Minor**: Style violation, unused code, unclear naming, redundant logic

## Workflow

1. **Context Acquisition**
   - Read the entire file being reviewed
   - Read related modules, interfaces, types, and configuration files
   - Identify intent from naming, comments, docstrings, and call sites
   - Note existing conventions (naming, error handling, imports, structure)

2. **Static Analysis**
   - Validate syntax and structure
   - Audit imports for unused, missing, or circular dependencies
   - Verify naming compliance with project conventions
   - Check structural organization (public before private, helpers at bottom, grouped related code)

3. **Logic & Security Analysis**
   - Trace control flow: conditionals, loops, returns — ensure all branches covered
   - Verify data flow: follow variables from declaration to usage
   - Scan for security vulnerabilities (secrets, injection, missing validation)
   - Check resource management (proper close/release of handles)
   - Flag concurrency issues (race conditions, missing locks, improper async)

4. **Issue Reporting**
   - Classify each issue by severity (Critical / Important / Minor)
   - Generate fix-ready corrections for each issue
   - Provide exact replacement code with context (3-5 surrounding lines)
   - Include dependency changes (new imports, helpers, type changes) when needed
   - Add brief rationale for each fix

## Output Format

Present findings organized by severity:

```
### Critical
- **Line X**: Description of issue
  ```language
  // fix code with context
  ```
  Brief rationale

### Important
- ...

### Minor
```

## References

Language-specific checklists for deeper review guidance:

- **D**: `references/d-lang.md` — imports, concurrency attributes, type system, path handling, error patterns
- **Python**: `references/python.md` — type hints, error handling, resource management, concurrency, PEP 8
- **C++**: `references/cpp.md` — memory management, const correctness, modern C++, exception safety, templates
