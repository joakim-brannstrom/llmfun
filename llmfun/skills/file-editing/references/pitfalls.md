# File Editing Pitfalls & Edge Cases

## Common Mistakes

### Using writeFile on existing files
`writeFile` replaces the entire file. If you only need to change a few lines, use `editFile` instead. Accidental `writeFile` on an existing file destroys all content not included in the new content.

### Skipping readFile before applyDiff
Context lines in a diff must match the file (fuzzy by default). If the file has changed since you last read it, your diff will fail. **Always read first.**

### Forgetting to verify after edit
An edit may look correct in your diff but fail at the tool level. Always verify:
- Executable files: run them
- Data files: re-read and validate structure
- Config files: re-read and check syntax

### Successive byLine edits corrupt files silently
When making multiple byLine edits from a single readFile, the first edit shifts line numbers — subsequent edits target wrong content without any error. **Always re-read the file between byLine edits**, or better: **use byMarker or byContent** which search for content, not line numbers. If you must use byLine for successive edits, use the `verifyContent` parameter as a safety guard.

## byMarker + Multi-line Content (Auto-Count)

### Auto-derived count from content lines
When using `byMarker` targeting with `replace` mode and multi-line content, the tool auto-derives `count` from the number of content lines. If your replacement has 3 lines, it replaces 3 lines starting at the marker.

### If you want ONLY the marker line replaced
Set `count=1` explicitly when using multi-line content with `byMarker`:
```
editFile(path="file.txt", mode="replace", marker="the line", count=1,
         content="replacement line 1\nreplacement line 2")
```
This replaces only 1 line (the marker line) with your multi-line content.

### If the original block is larger than your replacement
If the original block has 10 lines but your replacement has 3, auto-count will only replace 3 lines. To replace the full original block, either:
- Set `count` explicitly to the original block size
- Use `byContent` targeting, which auto-derives count from the matched block size

## byContent Matching Rules

### Uses trimmed equality
`fileLine.strip == searchLine.strip`. Leading/trailing whitespace is ignored on both sides. This means searching for unindented code matches indented code.

### Empty lines in search and file are both skipped
Empty lines in your search block are skipped during matching, AND empty lines in the file are also skipped. This means blocks match regardless of blank line differences.

**Example that works**: File has `line1\n\nline3`, search is `line1\nline3`. Both the search blank line and the file blank line are skipped. Non-empty lines (`line1`, `line3`) match → success.

**Solution**: Either include the empty line in your search (`line1\n\nline3`), or search for just the non-empty lines you need.

### Full line matching (not substring)
Each non-empty search line must match a complete file line (after trimming). This prevents accidentally matching code inside comments or strings.

### Search block must have at least one non-empty line
If all lines in your search content are empty or whitespace-only, the tool rejects it with an error. Include at least one substantive line.

## replaceAll Constraints

### replaceAll with byLine is not supported
`replaceAll` only works with `byContent` and `byMarker` targeting. Using it with `byLine` returns an error.

### replaceAll with byMarker
Only replaces lines where the marker substring appears. Consider `byContent` for more precise multi-line targeting.

### replaceAll is non-overlapping
After replacing one occurrence, search resumes from the line after the replacement. Replacements cannot overlap with previously replaced content.

### matchIndex and replaceAll conflict
`replaceAll=true` with `matchIndex > 1` returns an error. Use either `replaceAll=true` (all occurrences) or `matchIndex=N` (single occurrence), not both.

## applyDiff Behavior

### Hunk header counts are advisory
The `@@ -oldStart,oldCount +newStart,newCount @@` counts are advisory. The tool counts actual body lines and uses those. Mismatches produce warnings (not errors) in the `warnings` array.

### Fuzzy context matching by default
Context lines (starting with ` `) use trimmed equality by default. Leading/trailing whitespace differences are tolerated. Set `fuzzy=false` for exact matching.

### Fuzzy matching does NOT change what's written
Only the matching is fuzzy — the file content written is always the `+` lines from the diff exactly as provided.

### Multi-hunk diffs preserve content between hunks
When applying multiple hunks, lines between hunks are preserved. Each hunk is applied independently.

### applyDiff no longer has strictCounts parameter
Hunk counts are always advisory; use `fuzzy` to control context matching strictness.

## Mode-Specific Gotchas

### remove mode requires empty content
When using `mode="remove"`, the `content` parameter must be empty. Providing content with remove mode returns an error.

### remove mode count defaults to 1
When using `byMarker` targeting with `remove` mode and no explicit `count`, only the marker line is removed (`count=1`). Set `count` explicitly to remove a larger block. The auto-count heuristic only applies to `replace` mode, not `remove`.

### insert_after is an alias for append
`insert_after` behaves identically to `append`. Prefer `append` for clarity.

### Content is preserved exactly
The content you provide is written to the file exactly as given. No extra whitespace or indentation is added.

### Empty content in replace mode
Passing `content=""` for `replace` mode effectively deletes the targeted lines (same as `remove` but without the empty-content requirement).

## Diagnostic Error Messages

### Read the diagnostic field on failure
When a `byMarker` or `byContent` search fails, the error includes a `diagnostic` field with:
- Which lines were searched
- The closest matching line (if any)
- Details about what didn't match

Use this information to adjust your search — the closest match often reveals a typo or whitespace difference.

### Ambiguous targeting error
If you provide multiple targeting methods (e.g., both `startLine` and `marker`), the tool returns an error. Provide exactly one targeting method.

### Missing targeting error
If you provide no targeting method, the tool returns an error. Provide exactly one of: `startLine`, `marker`, or `searchContent`.

## Edge Cases

### Empty files
- `byLine` on empty file: error (no lines exist)
- `byMarker` on empty file: error (marker not found)
- `byContent` on empty file: error (block not found)

### Single-line files
All targeting methods work on single-line files. Verify `matchedLines` in the return value.

### Duplicate markers
When multiple lines contain your marker string, only the **first** occurrence is targeted by default. Use more specific markers or `matchIndex` to target later occurrences.

### Trailing newline preservation
The file's trailing-newline state is preserved. If the original file ended with a newline, the edited file will too.

## Dry-Run Pattern

### Dry-run never modifies files
`dryRun=true` returns a preview of the modified content but does NOT write to disk. Always follow up without `dryRun` to actually apply the change.

### Preview shows full file content
The `preview` field in dry-run responses contains the complete modified file content, not just the changed lines.

### matchedAt is 1-based
The `matchedAt` field returns the 1-based line number where the match was found.

## Nth-Occurrence Targeting (matchIndex)

### byMarker targets the Nth matching line with matchIndex
By default only the **first** line containing your marker is edited. Pass `matchIndex=N` (1-based) to target the Nth matching line instead. If `matchIndex` exceeds the number of matching lines the error reports the actual count, e.g. `matchIndex=3 but only 2 occurrences of marker 'X' were found`. Occurrences are counted **per line**: a marker appearing twice on one line counts once. `matchIndex > 1` cannot be combined with `replaceAll`. Use more specific markers to reduce ambiguity.

### byContent targets the Nth matching block with matchIndex
By default only the **first** matching block is replaced. Pass `matchIndex=N` (1-based) to target the Nth non-overlapping block instead. If `matchIndex` exceeds the number of matching blocks the error reports the actual count, e.g. `matchIndex=3 but only 2 occurrences of the search block were found`. Use `replaceAll=true` (with default `matchIndex=1`) to replace all occurrences.

## Scope Limiting (scopeStart / scopeEnd)

### Use scope for large files when you know the approximate location
Pass `scopeStart`/`scopeEnd` (1-based, inclusive) to restrict the byMarker/byContent search:
```json
editFile(path="big.c", mode="replace", marker="target",
         scopeStart=120, scopeEnd=180, content="new")
```
Either or both bounds may be given. `scopeStart` alone searches from that line to EOF; `scopeEnd` alone searches from line 1 to that line.

### Scope constrains the anchor, not the whole match
Only the **first line** of a match must be inside the range. A byContent block whose anchor is in scope may extend past `scopeEnd`. The same applies to byMarker: with `count > 1` (or auto-count from multi-line content), the replaced region may extend past `scopeEnd`. If you need the whole replaced region inside the range, widen `scopeEnd`.

### Not-found errors mention the scope
When a marker/block is outside the scope, the error says e.g. `marker 'X' not found in file within scope [10, 20]` and the `diagnostic` includes a `scope` field (`start`/`end`). The error message echoes the range you requested; the `diagnostic.scope` field reports the **effective** (clamped to the file) range. If you didn't intend a scope, you passed the wrong `scopeStart`/`scopeEnd` — widen or remove it.

### Invalid scope values are rejected
`scopeStart` must be >= 1, `scopeEnd` must be >= 1, and `scopeStart` must be <= `scopeEnd`. `scopeEnd` beyond the file is clamped to the file end (no error). `scopeStart` is **not** clamped — if it exceeds the file length, the search range becomes empty and the search fails.

### Scope is ignored by byLine
`byLine` does not search, so `scopeStart`/`scopeEnd` have no effect with it. A *valid* scope is silently ignored; an *invalid* one still errors.

### Scope combines with matchIndex and replaceAll
`matchIndex` counts occurrences **within the scope**; `replaceAll` replaces only in-scope occurrences and preserves lines outside the scope. Out-of-scope matches are never touched.

## Atomicity Guarantee

All edits are applied in memory first. The file is written only if the entire edit succeeds. Partial edits never occur.
