---
name: file-editing
description: >-
  Guide for editing files correctly using the universal editing workflow.
  Ensures edits are applied safely with proper tool selection and verification.
  Use when modifying any file, applying changes, or writing new files.
  Triggers on: file editing, edit file, modify file, file change, write file,
  file modification, code change, update file, replace file content,
  file-editing, edit-file.
version: 3.5.0
---

# File Editing

Structured protocol for editing files safely and correctly.

## When to Use

- Modifying an existing file (any type)
- Writing a new file

## Core Principle

**Read → Choose → Apply → Verify.** Never skip reading before editing, never skip verification after.

## Universal Editing Workflow

1. **Read** the current state with `read_file`.
2. **Choose the right tool** (see Tool Selection below):
   - `write_file` — new files or complete rewrites
   - `edit_file` — targeted edits (line numbers, markers, or code blocks)
3. **Optional: Dry-run** — use `dryRun=true` to preview before applying.
   Available for `edit_file` only. (`write_file` has no dry-run option.)
4. **Apply** the edit.
5. **Verify immediately**: run executables, re-read others. If wrong, go to step 1.

## Tool Selection Decision Tree

File new?
  → YES: write_file
  → NO: Continue...

Know a unique marker (single or multi-line)?
  → YES: edit_file(marker) [+ count if replacing a block]
  → NO: Continue...

Know the exact code block?
  → YES: edit_file(searchContent)
  → NO: Continue...

Know exact line numbers?
  → YES: edit_file(startLine, count)
         ⚠ Re-read the file between successive byLine edits — line numbers shift!
  → NO: Re-read file, find a marker or code block, try again.

## Quick Tool Reference

### edit_file — Unified file editing

- **Targeting** (exactly one required): `byLine` (startLine+count), `byMarker` (substring, single or multi-line up to 20 lines), `byContent` (code block).
- **Modes**: `replace`, `remove`, `append`, `insert_before`, `insert_after` (=append alias).
- **Options**: `dryRun`, `replaceAll` (byContent/byMarker only), `matchIndex` (Nth occurrence, 1-based; default 1; cannot be combined with replaceAll when > 1; ignored by byLine), `scopeStart`/`scopeEnd` (limit search range), `verifyContent` (byLine guard — verify target lines before editing).
- **Auto-count**: byMarker+replace auto-derives count from content lines. Set `count` explicitly to override. Multi-line markers use marker line count instead.
- **byContent**: trimmed equality; empty lines skipped in both search and file; full-line match (not substring).
- **Returns**: JSON with `ok`, `matchedAt`, `matchedLines`, `linesChanged`, `operations`, `lineShift`, `autoCountUsed`/`note` (when auto-count active). On failure: `error` + `diagnostic`.
- **Full details**: `references/tool-reference.md`

### write_file — Write or create files

Creates file with parent directories. Replaces entire content if file exists — never use for partial edits.

## Matching & Dry-Run

- **byContent**: Trimmed equality; empty lines skipped in both search and file; full-line match (not substring).
- **Always dry-run first**, then apply. See `references/pitfalls.md` for edge cases.

See `references/patterns.md` for common usage patterns.

## References

- Full tool reference: `references/tool-reference.md`
- Common patterns: `references/patterns.md`
- Pitfalls and edge cases: `references/pitfalls.md`
