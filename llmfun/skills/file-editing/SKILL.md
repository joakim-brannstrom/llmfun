---
name: file-editing
description: >-
  Guide for editing files correctly using the universal editing workflow.
  Ensures edits are applied safely with proper tool selection and verification.
  Use when modifying any file, applying changes, or writing new files.
  Triggers on: file editing, edit file, modify file, file change, apply diff,
  write file, file modification, code change, update file, replace file content,
  file-editing, edit-file.
version: 3.3.0
---

# File Editing

Structured protocol for editing files safely and correctly.

## When to Use

- Modifying an existing file (any type)
- Writing a new file
- Applying multi-line changes with diffs

## Core Principle

**Read → Choose → Apply → Verify.** Never skip reading before editing, never skip verification after.

## Universal Editing Workflow

1. **Read** the current state with `readFile`.
2. **Choose the right tool** (see Tool Selection below):
   - `writeFile` — new files or complete rewrites
   - `editFile` — targeted edits (line numbers, markers, or code blocks)
   - `applyDiff` — multi-line changes via unified diff
3. **Optional: Dry-run** — use `dryRun=true` to preview before applying.
   Available for: `editFile`, `applyDiff`. (`writeFile` has no dry-run option.)
4. **Apply** the edit.
5. **Verify immediately**: run executables, re-read others. If wrong, go to step 1.

## Tool Selection Decision Tree

```
File new?
  → YES: writeFile
  → NO: Continue...

Know exact line numbers?
  → YES: editFile(startLine, count)
  → NO: Continue...

Know a unique marker?
  → YES: editFile(marker) [+ count if replacing a block]
  → NO: Continue...

Know the exact code block?
  → YES: editFile(searchContent)
  → NO: Continue...

Have a pre-computed unified diff?
  → YES: applyDiff
  → NO: Re-read file, find a marker or code block, try again.
```

## Quick Tool Reference

### editFile — Unified file editing

- **Targeting** (exactly one required): `byLine` (startLine+count), `byMarker` (substring), `byContent` (code block).
- **Modes**: `replace`, `remove`, `append`, `insert_before`, `insert_after` (=append alias).
- **Options**: `dryRun`, `replaceAll` (byContent/byMarker only), `matchIndex` (Nth occurrence, 1-based; default 1; cannot be combined with replaceAll when > 1; ignored by byLine), `scopeStart`/`scopeEnd` (limit search range).
- **Auto-count**: byMarker+replace auto-derives count from content lines. Set `count` explicitly to override.
- **byContent**: trimmed equality; empty search lines skipped (but file empty lines still must match next non-empty search line); full-line match (not substring).
- **Returns**: JSON with `ok`, `matchedAt`, `matchedLines`, `linesChanged`, `operations`. On failure: `error` + `diagnostic`.
- **Full details**: `references/tool-reference.md`

### applyDiff — Unified diff patch

- **Parameters**: `path`, `diff` (unified diff), `dryRun` (default false), `fuzzy` (default true).
- Hunk header counts are advisory. Fuzzy matching uses trimmed equality.
- **Returns**: JSON with `ok`, `hunksApplied`, `linesChanged`, `warnings`.
- **Full details**: `references/tool-reference.md`

### writeFile — Write or create files

Creates file with parent directories. Replaces entire content if file exists — never use for partial edits.

## Matching & Dry-Run

- **byContent**: Trimmed equality; empty search lines skipped (file empty lines still match next non-empty search). See `references/tool-reference.md`.
- **byMarker**: Case-sensitive substring, first occurrence default; `matchIndex=N` for Nth.
- **applyDiff**: Fuzzy by default (`fuzzy=false` for exact).
- **Always dry-run first**, then apply. See `references/pitfalls.md` for edge cases.

See `references/patterns.md` for common usage patterns.

## References

- Full tool reference: `references/tool-reference.md`
- Common patterns: `references/patterns.md`
- Pitfalls and edge cases: `references/pitfalls.md`
