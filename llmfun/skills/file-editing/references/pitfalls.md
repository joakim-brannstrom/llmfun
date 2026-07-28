# File Editing Pitfalls & Edge Cases

## Common Mistakes

### Using writeFile on existing files
`writeFile` replaces the entire file. If you only need to change a few lines, use `editFile` or `applyDiff` instead. Accidental `writeFile` on an existing file destroys all content not included in the new content.

### Skipping readFile before applyDiff
Context lines in a diff must match the file exactly. If the file has changed since you last read it, your diff will fail silently or apply incorrectly. **Always read first.**

### Forgetting to verify after edit
An edit may look correct in your diff but fail at the tool level. Always verify:
- Executable files: run them
- Data files: re-read and validate structure
- Config files: re-read and check syntax

### Over-editing with editFile
`editFile` is for small, targeted changes. For large multi-line changes, `applyDiff` is more reliable and easier to review.

### Under-using editFile
For single-line replacements, `editFile` with mode "replace" is simpler than crafting a full diff. Don't over-engineer simple changes.

## Tool Selection Decision Tree

```
Is the file new (doesn't exist yet)?
  → YES: writeFile
  → NO: Continue...

How many lines are changing?
  → 1-3 lines: editFile (mode: replace)
  → Inserting after a specific line: editFile (mode: append)
  → Removing lines: editFile (mode: remove)
  → Many lines / mixed changes: applyDiff

Is it a complete rewrite?
  → YES: writeFile (acceptable for total rewrites)
  → NO: applyDiff
```

## Verification Checklist

After every file edit:
1. [ ] Did I verify the result?
2. [ ] For code: did it execute successfully?
3. [ ] For data: is the format valid?
4. [ ] For config: does it parse correctly?
5. [ ] Did I handle any errors from the previous step?
