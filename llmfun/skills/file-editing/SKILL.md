---
name: file-editing
description: >
  Guide for editing files correctly using the universal editing workflow.
  Ensures edits are applied safely with proper tool selection and verification.
  Use when modifying any file, applying changes, or writing new files.
  Triggers on: file editing, edit file, modify file, file change, apply diff,
  write file, file modification, code change, update file, replace file content,
  file-editing, edit-file.
version: 2.0.0
---

# File Editing

Structured protocol for editing files safely and correctly.

## When to Use

- Modifying an existing file (any type)
- Writing a new file
- Applying multi-line changes with diffs
- Debugging file-related errors

## Core Principle

**Read → Choose → Apply → Verify.** Never skip reading before editing, never skip verification after.

## Universal Editing Workflow

1. **Read** the current state with `readFile`.
2. **Choose the right tool** (see Tool Reference):
   - `editFileByMarker` — find by marker text, insert/replace/remove (preferred)
   - `searchAndReplace` — replace a known code block by content (first match)
   - `searchAndReplaceAll` — replace all occurrences of a code block
   - `editFile` — position-based edits when you know exact line numbers
   - `applyDiff` — multi-line changes via unified diff
   - `writeFile` — new files or complete rewrites only; never for partial edits
3. **Optional: Dry-run** — use `*DryRun` variants to preview before applying.
   Available for: `editFileByMarker`, `searchAndReplace`, `applyDiff`.
4. **Apply** the edit.
5. **Verify immediately**: run executables, re-read others. If wrong, go to step 1.

## Tool Reference

| Tool | Use When |
|------|----------|
| `editFileByMarker` | Insert/replace/remove at a marker line (preferred) |
| `searchAndReplace` | Replace a known code block (first match) |
| `searchAndReplaceAll` | Replace all occurrences of a code block |
| `editFile` | You know exact line numbers (fallback) |
| `applyDiff` | Multi-line changes with mixed additions/deletions |
| `writeFile` | New files or complete rewrites |

Dry-run variants (`*DryRun`) exist for `editFileByMarker`, `searchAndReplace`, `applyDiff`.

## Tool Selection Decision Tree

```
File new?
  → YES: writeFile
  → NO: Continue...

Know a unique marker in the target line?
  → YES: editFileByMarker (modes: insert_before, insert_after, replace, remove)
  → NO: Continue...

Replacing a known code block?
  → YES: searchAndReplace (first) or searchAndReplaceAll (all)
  → NO: Continue...

Know exact line numbers?
  → YES: editFile (modes: replace, append, remove)
  → NO: Continue...

Multi-line with mixed additions/deletions?
  → YES: applyDiff
  → NO: Re-read file, find a marker or code block, try again. Last resort: applyDiff.
```

## Matching Behavior

- **Trimmed equality** (searchAndReplace): `fileLine.strip == searchLine.strip`. Tolerates indentation differences. Empty search lines are flexible. Full line match prevents false positives.
- **Marker matching** (editFileByMarker): Case-sensitive substring match. Only first occurrence is used. Use unique, specific markers.
- **applyDiff**: Context lines must match file exactly. Always `readFile` first.

## References

- Detailed pitfalls, gotchas, and edge cases: `references/pitfalls.md`
