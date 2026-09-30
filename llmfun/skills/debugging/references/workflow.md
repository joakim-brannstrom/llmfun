# Debugging Workflow — Detailed Steps

**Context compression risk**: A debugging session may be compressed one or more
times, causing details held only in the context window to be lost — the exact
symptom, the evidence collected, the hypotheses already eliminated, the fix
plan, the fix steps already applied. You must continuously externalize findings
to the state file `debug_notes.md`. Never assume the context will retain
intermediate notes or dead ends. Write the phase's output after every phase and
re-read the file to recover context after a compression.

## The state file (`debug_notes.md`)

Location: workarea root (`debug_notes.md`).

One section per bug ("one bug at a time" — start a new `## Bug N: <symptom>`
section when the next bug is taken up). Within a bug section, update the
relevant subsection after every phase:

1. **Progress checklist** — the 6 workflow phases as `[ ]`/`[x]` items. This is
   the resume point after a compression; an interrupted hypothesis round resumes
   from the Hypotheses section's last entry.
2. **Symptom & reproduction** — the exact error message or wrong output
   (verbatim), the environment, the repro command, the trigger conditions, and
   whether it reproduces reliably.
3. **Evidence log** — verified anchors (`file.d:123-145` + one-line content
   note) and facts learned while reading or tracing. Anchors let later phases
   re-read ~20 lines instead of whole files.
4. **Hypotheses** — one entry per loop round: the hypothesis, its stated
   prediction, status (`eliminated` / `surviving` / `confirmed`), the test or
   evidence that decided it, and one line of reasoning. Eliminated hypotheses
   MUST be recorded — after a compression they are the cheapest thing to
   accidentally re-test, wasting a whole cycle.
5. **Root cause** — the confirmed cause and the reasoning chain that
   established it.
6. **Fix plan** — files to modify, the ordered fix steps, the verification
   criteria.
7. **Fix log** — each applied step (file + what changed), so an interrupted fix
   is resumable.

## Step details (each phase ends with a `debug_notes.md` update)

### Phase 1: Reproduce the Issue

- **Load the failing code**: Read the file(s) where the bug manifests.
- **Identify the symptom**: Note the exact error message, wrong output, or
  unexpected behavior — verbatim.
- **Trace the execution path**: Follow the code flow from entry point to the
  failure.
- **Check inputs**: Identify what inputs or conditions trigger the bug.
- **Minimize the failing example**: Reduce the bug to the smallest input and
  code that still triggers it — a small, deterministic repro makes every later
  step cheaper; use it also when the hypothesis loop spins (see Phase 2).
- **Create the state file now**: Write the Progress checklist and the Symptom &
  reproduction section (exact error text, repro command). Do not keep them
  only in context.

### Phase 2: Isolate & Find the Root Cause (the hypothesis loop)

Loop until a hypothesis is confirmed — isolating and analyzing are not
one-pass steps. Each round:

1. **Review the evidence**: the state file's Evidence log plus what the last
   test taught you. Re-entry is evidence-first, never a fresh re-read of the
   same code.
2. **Form ONE hypothesis**: specific and falsifiable. The questions below are
   the hypothesis generators:
   - Is there a type mismatch or incorrect variable usage?
   - Are all branches of conditionals handled correctly?
   - Is there an off-by-one error in loops or array indexing?
   - Is state being modified unexpectedly?
   - Are resources (files, connections, memory) properly managed?
   - Is there a race condition or timing issue?
   - Is input validation missing or incorrect?
3. **State the prediction**: if this hypothesis is true, <specific observation>
   should hold. Write the prediction into the Hypotheses entry before testing —
   a test without a prediction cannot be told from a vacuous one.
4. **Test it cheaply** — escalate only as needed: read/trace the code path
   first (free), then a temporary probe or log, then a run with modified
   input, then heavier runs (bisect the change window). Prefer a test that
   discriminates between the surviving hypotheses; one test per hypothesis.
5. **Record the result** in the Hypotheses entry: `eliminated` (with what the
   test taught you — this is the new information the next round starts from)
   or `confirmed` (root cause + reasoning chain, before planning the fix).

Loop rules:
- **Eliminated → next round** with what you learned. Never re-test an
  eliminated hypothesis without new evidence.
- **Spinning? Stop rule**: if rounds stop producing new information
  (hypotheses eliminated without narrowing anything), change tack: get better
  data (bigger repro, logs, dumps, runtime state, ask the user what they
  observe), or reduce the failing example until it is small and
  deterministic. Report what is known and what is blocked rather than looping.
- **Confirmed → exit to Phase 3.** Multiple confirmed causes or a fix that
  reveals the next bug: "one bug at a time" — start a new `## Bug N:` section
  and re-enter the loop there.
- Each round is a natural `requestCompression` boundary (see the compression
  checkpoint below).

### Phase 3: Formulate Fix Task

- **Define the fix**: Describe what needs to change to resolve the bug.
- **Identify affected files**: List all files that need modification.
- **Plan the fix**: Break the fix into small, executable steps if needed.
- **Define verification**: Specify how to verify the fix works.
- **Write the fix plan to the state file**: Files, ordered steps, and
  verification criteria go into the Fix plan section before any code is touched.

### Phase 4: Execute Fix

For each fix step:
- **Write the corrected code**: Provide the exact code that fixes the root cause.
- **Include context**: Show surrounding lines (3-5 before/after) for accurate placement.
- **Handle edge cases**: Ensure the fix doesn't break other cases.
- **Add safety checks**: If appropriate, add assertions or error handling to prevent recurrence.
- **Log each applied step**: After EVERY applied step, add file + what changed
  to the Fix log before starting the next — an interrupted session must be
  resumable mid-fix.

### Phase 5: Verify the Fix

- **Re-read the state file first**: If the session may have been compressed,
  recover the symptom, root cause, and fix plan from `debug_notes.md` before
  verifying from memory.
- **Check syntax**: Ensure the fix compiles and is syntactically valid.
- **Trace the fixed path**: Follow the code flow again to verify the bug is resolved.
- **Check side effects**: Verify the fix doesn't introduce new bugs in related code.
- **Consider similar bugs**: Check if the same pattern exists elsewhere and should also be fixed.
- **Record the result**: Verification command + result in the state file; only
  a verified fix may be reported.

### Phase 6: Produce Output

Report the bug fix using the output format template in `output-format.md`,
assembled from the state file; mention the `debug_notes.md` path in the
report's Notes.

## Compression checkpoint (`requestCompression`)

`requestCompression` is the debugging session's active checkpoint: it compresses
on your terms and re-injects your message-to-self afterwards — unlike the
forced compression at ~90%, which summarizes without your control and can
garble in-context evidence.

- Trigger points:
  - **80% `[SYSTEM NUDGE - NOT USER INPUT]`**: act NOW — do not finish the
    current phase first. Write down what you have NOW (`debug_notes.md` up to
    here + current-step notes) and call `requestCompression` immediately —
    a self-requested compression mid-fix is always better than the forced
    one at ~90%, which summarizes without your control.
  - At any natural debugging boundary in a long session (after a phase,
    after a failed repro attempt, after a hypothesis is eliminated) — even
    below 80%: update the state file and checkpoint, so the nudge-time
    write stays small.
- Never call `requestCompression` with unsaved state — write `debug_notes.md`
  first; the handoff carries only the summary.
- Write the message-to-self as a briefing for a new instance with no memory of
  the session:

```
[Debug handoff]
- Goal: fix <bug/symptom> in <target>; output <fix report>
- Progress: phases done <list>; current phase
- Bug: <symptom one-liner>; repro <command>
- Root cause: <confirmed cause | none yet — surviving hypotheses + one-liners>
- Fix state: <steps applied; verification pending>
- State file: `debug_notes.md` (checklist, symptom, evidence, hypotheses, fix plan, fix log)
- User decisions/constraints: <any>
- Next action: <first unchecked step of the current phase>
```

## Resume protocol (after a context compression)

0. If you requested the compression yourself, the re-injected handoff message
   is your first memory — read it, then continue below.
1. Re-read `debug_notes.md` first: the Progress checklist says where you were;
   the Evidence log, Hypotheses, and Fix log say what you already ruled out and
   already changed.
2. Continue from the first unchecked phase. Never re-test an eliminated
   hypothesis or re-apply an applied fix step without new evidence; keep
   updating the file.
