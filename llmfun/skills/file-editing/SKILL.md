---
name: file-editing
description: >
  Guide for editing files correctly using the universal editing workflow.
  Ensures edits are applied safely with proper tool selection and verification.
  Use when modifying any file, applying changes, or writing new files.
  Triggers on: file editing, edit file, modify file, file change, apply diff,
  write file, file modification, code change, update file, replace file content,
  file-editing, edit-file.
version: 1.0.0
---

# File Editing

Structured protocol for editing files safely and correctly.

## When to Use

- Modifying an existing file (any type)
- Writing a new file
- Applying multi-line changes with diffs
- Debugging file-related errors

## Core Principle

**Read → Choose → Apply → Verify.** Never skip reading the current state before editing, and never skip verification after applying changes.

## Universal Editing Workflow

1. **Read** the current state with `readFile`.
2. **Choose the right tool**:
   - `editFile` — single-line or small targeted changes
   - `applyDiff` — multi-line changes (additions, deletions, modifications)
   - `writeFile` — **new files or total rewrites only**; never use to edit an existing file
3. **Apply** the edit.
4. **Verify immediately**:
   - **Executable files**: run to confirm they work
   - **Non-executable files**: re-read with `readFile` to confirm correctness
   - If verification fails: go back to step 1

## applyDiff Rules

- Must be a **valid unified diff** with `--- a/path` and `+++ b/path` headers
- Context lines (starting with space) **must match the file exactly**
- Always call `readFile` first to ensure context lines are accurate
- Mismatched context lines cause silent failures — verify after applying

## References

- Detailed pitfalls and edge cases: `references/pitfalls.md`
