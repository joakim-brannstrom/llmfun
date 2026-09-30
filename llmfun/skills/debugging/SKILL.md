---
name: debugging
description: >-
  Systematically identify, analyze, and fix bugs in code. Use when encountering
  errors, unexpected behavior, or test failures. Triggers on: debugging, debug,
  bug fix, fix bug, error, unexpected behavior, test failure, troubleshoot,
  root cause, something is broken.
version: 1.2.0
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
- **Externalize continuously**: After every phase, append its findings (symptom,
  evidence, hypotheses eliminated or confirmed, root cause, fix steps applied)
  to the state file `debug_notes.md` — never keep findings only in context.
  On the 80% nudge do not finish the phase first: write down what you have NOW
  and call `requestCompression` immediately (handoff template:
  `references/workflow.md`) — self-requested mid-task beats forced ~90%. Also
  at boundaries; resume from the re-injected handoff + `debug_notes.md`
  (no handoff → re-read the file).
- **One bug at a time**: Fully resolve one issue before moving to the next.
- **Verify after every fix**: Re-read or run code after changes to confirm the fix.
- **Check for similar bugs**: After fixing, scan for the same pattern elsewhere.
- **Loop, don't shotgun**: One hypothesis per round with a stated prediction and
  one cheap test; a failed test feeds the next round — record what it taught
  you. Spinning without new information means get better data, reduce the
  failing example, or ask the user.

## Workflow

Follow the 6-phase protocol. See `references/workflow.md` for detailed steps.

1. **Reproduce the Issue** — Load failing code, identify symptom, trace execution, check inputs, minimize the failing example. Create `debug_notes.md` now.
2. **Isolate & Find the Root Cause** — Run the hypothesis loop: review the evidence, form one hypothesis, state its prediction, test it cheaply, record the result. Eliminated → next round with what you learned; confirmed → write the root cause; no new information → get better data or reduce the example.
3. **Formulate Fix Task** — Define the fix, identify files, plan steps, define verification. Write the fix plan to the state file before touching code.
4. **Execute Fix** — Write corrected code, include context, handle edge cases, add safety checks. Log each applied step to the state file.
5. **Verify the Fix** — Check syntax, trace fixed path, check side effects, find similar bugs. Record the verification result.
6. **Produce Output** — Report the bug fix in the standard format; mention the state file path in the report's Notes.

## Output Format

Report the bug fix using the structure in `references/output-format.md`.

## References

- Detailed workflow and handoff template: `references/workflow.md`
- Output format template: `references/output-format.md`
