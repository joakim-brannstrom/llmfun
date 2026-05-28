A structured protocol for an LLM agent to systematically identify, analyze, and fix bugs in code. Use this when encountering errors, unexpected behavior, or test failures.
# Debugging Strategy (LLM Agent)

**Important**: The output must be a fix task, not just a description. Code is produced by executing the fix.
**Important**: Write the plan to a file and inform the user.

## 1. Reproduce the Issue

- **Load the failing code**: Read the file(s) where the bug manifests.
- **Identify the symptom**: Note the exact error message, wrong output, or unexpected behavior.
- **Trace the execution path**: Follow the code flow from entry point to the failure.
- **Check inputs**: Identify what inputs or conditions trigger the bug.

## 2. Isolate the Problem

- **Narrow the scope**: Identify the smallest unit of code responsible (function, class, module).
- **Check recent changes**: If applicable, compare with the last working version.
- **Verify dependencies**: Ensure external dependencies are correctly imported and used.
- **Eliminate red herrings**: Rule out unrelated code that might distract from the root cause.

## 3. Analyze Root Cause

Ask yourself these questions:
- Is there a type mismatch or incorrect variable usage?
- Are all branches of conditionals handled correctly?
- Is there an off-by-one error in loops or array indexing?
- Is state being modified unexpectedly?
- Are resources (files, connections, memory) properly managed?
- Is there a race condition or timing issue?
- Is input validation missing or incorrect?

## 4. Formulate Fix Task

- **Define the fix**: Describe what needs to change to resolve the bug.
- **Identify affected files**: List all files that need modification.
- **Plan the fix**: Break the fix into small, executable steps if needed.
- **Define verification**: Specify how to verify the fix works.

## 5. Execute Fix

For each fix step:
- **Write the corrected code**: Provide the exact code that fixes the root cause.
- **Include context**: Show surrounding lines (3-5 before/after) for accurate placement.
- **Handle edge cases**: Ensure the fix doesn't break other cases.
- **Add safety checks**: If appropriate, add assertions or error handling to prevent recurrence.

## 6. Verify the Fix

- **Check syntax**: Ensure the fix compiles and is syntactically valid.
- **Trace the fixed path**: Follow the code flow again to verify the bug is resolved.
- **Check side effects**: Verify the fix doesn't introduce new bugs in related code.
- **Consider similar bugs**: Check if the same pattern exists elsewhere and should also be fixed.

## 7. Output Format

Produce output in this structure:

```markdown
## Bug Fix Task

### Issue
- **File**: [file_path]
- **Line**: [line_number]
- **Symptom**: [description of the error or wrong behavior]

### Root Cause
[1-2 sentence explanation of why the bug occurs]

### Fix Task
- **Task**: [Brief description of the fix]
- **Files to modify**: [list of files]
- **Acceptance criteria**: [how to verify the fix works]

### Code Produced
[Fixed code files - one file per code block]

### Verification
- [ ] Fix addresses the root cause
- [ ] No side effects in adjacent code
- [ ] Similar patterns checked for recurrence

### Notes
[Any other relevant findings or suggestions]
```

