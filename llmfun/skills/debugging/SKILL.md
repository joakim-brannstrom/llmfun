---
name: debugging
description: >-
  Systematically identify, analyze, and fix bugs in code. Use when encountering
  errors, unexpected behavior, or test failures. Triggers on: debugging, debug,
  bug fix, fix bug, error, unexpected behavior, test failure, troubleshoot,
  root cause, something is broken.
version: 1.0.0
---

# Debugging Skill

Systematically identify, analyze, and fix bugs in code. The output must be a
fix task with actual code, not just a description.

## When to Use

Use this skill when:
- Encountering errors, crashes, or exceptions in code
- Observing unexpected behavior or wrong output
- Tests are failing and need diagnosis
- The user reports a bug or asks to troubleshoot an issue

## Rules

- **Produce fixes, not descriptions**: The output must include actual code changes.
- **Write the plan to a file**: Document the debugging process and inform the user.
- **One bug at a time**: Fully resolve one issue before moving to the next.
- **Verify after every fix**: Re-read or run code after changes to confirm the fix.
- **Check for similar bugs**: After fixing, scan for the same pattern elsewhere.

## Workflow

Follow the 7-phase protocol. See `references/workflow.md` for detailed steps.

1. **Reproduce the Issue** — Load failing code, identify symptom, trace execution, check inputs.
2. **Isolate the Problem** — Narrow scope, check recent changes, verify dependencies, eliminate red herrings.
3. **Analyze Root Cause** — Ask targeted questions about types, conditionals, state, resources, timing.
4. **Formulate Fix Task** — Define the fix, identify files, plan steps, define verification.
5. **Execute Fix** — Write corrected code, include context, handle edge cases, add safety checks.
6. **Verify the Fix** — Check syntax, trace fixed path, check side effects, find similar bugs.
7. **Produce Output** — Report the bug fix in the standard format.

## Output Format

Report the bug fix using the structure in `references/output-format.md`.

## References

- Detailed workflow: `references/workflow.md`
- Output format template: `references/output-format.md`
