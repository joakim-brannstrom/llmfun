# Debugging Output Format

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
