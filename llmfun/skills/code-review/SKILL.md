---
name: code-review
description: >-
  Language-agnostic code review: bugs, security
  issues, style violations, and logic errors — including reviewing an
  implementation against its design/spec/plan. Use on code review, PR
  feedback, or correctness checks. Triggers on: code review, PR feedback,
  review, audit, check code, verify code, correctness.
version: 1.1.0
---

# Code Review

## When to Use

Use this skill when asked for a code review, PR feedback, or a
correctness/security check — including reviewing an implementation against
its design/spec/plan.

## Rules

**Always check for:**
- **Syntax errors**: Unclosed brackets, missing semicolons, invalid operators, type mismatches
- **Security issues**: Hardcoded secrets, injection vectors (SQL, XSS, command), missing input validation
- **Resource leaks**: Unclosed handles, connections, cursors, unreleased memory
- **Logic errors**: Uncovered branches, uninitialized variables, off-by-one errors, race conditions
- **Import issues**: Unused imports, missing dependencies, circular imports
- **Naming consistency**: Follows project conventions

**Severity classification:**
- **Critical**: Build failure, crash, security vulnerability, data corruption
- **Important**: Logic error, performance bottleneck, missing error handling, resource leak
- **Minor**: Style violation, unused code, unclear naming, redundant logic

**Externalize continuously**: After every workflow step, write its output to
the state file `review_notes.md` (progress checklist, scope, evidence,
findings, probe log) — never keep findings only in context. Checkpoint long
reviews with `requestCompression` at natural boundaries (template:
workflow.md); after compression, resume from the re-injected handoff +
`review_notes.md` and the specs — if compression hit without your handoff,
re-read `review_notes.md` to recover.

**Mind the context budget**: baseline first — run the project's build/test
once, output to a log (grep/head only, never inline). Branch/PR reviews:
diff stat first, changed files first. Keep the raw-code working set
~15-20% of the window (128k ≈ 1 large file; bigger windows may hold whole
modules); summarize dropped files to the state file.

## Workflow

Follow the review protocol. See `references/workflow.md` for detailed steps.

1. **Scope & Criteria** — Confirm scope; record spec docs + requirement list to `review_notes.md`.
2. **Context Acquisition** — Read files in anchored chunks; record anchors + facts per chunk to `review_notes.md`.
3. **Spec Conformance** — Map each requirement/decision to code evidence (requirement → file:line); record the table to `review_notes.md`.
4. **Static Analysis** — Validate syntax, imports, naming, structure; record findings as found.
5. **Logic & Security Analysis** — Control/data flow, cross-event state, security, resources, concurrency; record findings as found.
6. **Empirical Verification** — Probes: write, run, observe, revert; log each to `review_notes.md`.
7. **Write the Review** — Assemble the final report from `review_notes.md`.
8. **Cleanup** — Revert any probes, confirm the tree is clean, deliver the review.

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

For spec-driven reviews, prepend a requirement → evidence table; keep
findings individually acceptable/rejectable.

## References

- **D**: `references/d-lang.md` — imports, concurrency, type system, paths, errors
- **Python**: `references/python.md` — types, errors, resources, concurrency, PEP 8
- **C++**: `references/cpp.md` — memory, const, modern C++, exceptions, templates
- Spec-driven: `references/spec-review.md` — extraction, evidence mapping, probes, cross-event state, policy split
- Detailed workflow: `references/workflow.md` — steps, state-file format, resume protocol
