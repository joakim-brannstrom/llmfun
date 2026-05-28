A structured protocol for an LLM agent to analyze code and produce structured, actionable fix instructions. **Use this when** reviewing code for bugs, security issues, style violations, or improvements.
# Code Review Strategy (LLM Agent)

## 1. Context Acquisition

- **Load Target File**: Read the entire file being reviewed.
- **Load Dependencies**: Read related modules, interfaces, types, and configuration files.
- **Identify Intent**: Determine the code's purpose from naming, comments, docstrings, and call sites.
- **Establish Patterns**: Note existing conventions (naming, error handling, imports, structure) to ensure fixes align.

## 2. Static Analysis

- **Syntax Validation**: Detect unclosed brackets, missing semicolons, invalid operators, type errors.
- **Import Audit**: Flag unused imports, missing dependencies, circular imports.
- **Naming Compliance**: Verify variable, function, class names match project conventions.
- **Structural Check**: Ensure logical organization (public before private, helpers at bottom, grouped related code).

## 3. Logic & Security Analysis

- **Control Flow Tracing**: Trace conditionals, loops, returns to ensure all branches are covered.
- **Data Flow Verification**: Follow variables from declaration to usage; check for uninitialized variables, type mismatches.
- **Security Scan**:
  - Detect hardcoded secrets, credentials, API keys.
  - Identify SQL injection, XSS, command injection vectors.
  - Check for missing input validation or sanitization.
- **Resource Management**: Verify file handles, network connections, database cursors, and memory are properly closed/released.
- **Concurrency Safety**: Flag race conditions, missing locks, improper async patterns.

## 4. Issue Identification & Severity Classification

For every problem, assign a severity and generate a fix-ready correction:

- **Critical**: Compilation failure, runtime crash, security vulnerability, data corruption.
- **Important**: Logic error, performance bottleneck, missing error handling, resource leak.
- **Minor**: Style violation, unused code, unclear naming, redundant logic.

## 5. Correction Generation

For each issue, produce a complete, context-aware fix:

- **Provide Exact Code**: Write the corrected code snippet that replaces the problematic section.
- **Include Context**: Show enough surrounding code (3-5 lines before/after) for accurate placement.
- **Handle Dependencies**: If the fix requires new imports, helper functions, or type changes, include them.
- **Explain Briefly**: Add a one-line rationale for the fix (e.g., "Prevents null pointer dereference on line 42").

## 6. Output Format

Produce output in this strict, machine-parseable structure:

```markdown
# Code Review Report

## Summary
- **File**: [file_path]
- **Issues**: [total] (Critical: [n], Important: [n], Minor: [n])

## Critical Issues

### [Short Title]
- **Line**: [line_number]
- **Problem**: [1-2 sentence description]
- **Fix**:
  ```[language]
  [exact corrected code]
  ```
- **Rationale**: [1 sentence explanation]

## Important Issues

### [Short Title]
- **Line**: [line_number]
- **Problem**: [1-2 sentence description]
- **Fix**:
  ```[language]
  [exact corrected code]
  ```
- **Rationale**: [1 sentence explanation]

## Minor Issues

### [Short Title]
- **Line**: [line_number]
- **Problem**: [1-2 sentence description]
- **Suggestion**: [description or code snippet]

## Positive Feedback
- [Brief note on correct patterns, if any]
```

## 7. Scenario-Specific Rules

- **Bug Fixes**: Address root cause, not symptoms. Check for similar bugs in the same file. Provide test case suggestions if possible.
- **New Features**: Verify completeness against requirements. Check API consistency with existing patterns. Ensure graceful degradation.
- **Refactoring**: Confirm behavioral equivalence. Preserve public interfaces. Update related callers if signatures change.
- **Security**: If uncertain, provide the most secure default and flag for verification. Never suggest insecure workarounds.

## 8. Self-Verification Checklist

Before outputting:
- [ ] Every fix is syntactically valid in the target language.
- [ ] Fixes fit within the surrounding context (no breaking changes to adjacent code).
- [ ] All imports and dependencies required by fixes are included.
- [ ] No issues were skipped or downgraded without justification.
- [ ] Line numbers are accurate and match the loaded file.
- [ ] Output follows the exact structure defined in Section 6.

## 9. Error Handling for Ambiguity

- **Unclear Intent**: If the code's purpose is ambiguous, note it in the Problem field and suggest the most likely interpretation.
- **Conflicting Patterns**: If multiple valid approaches exist, choose the one matching the established baseline and note alternatives.
- **Insufficient Context**: If dependencies are missing, list them explicitly and provide the fix assuming standard implementations.

