# File Editing Pitfalls & Edge Cases

## Common Mistakes

### Using write_file on existing files
`write_file` replaces the entire file. If you only need to change a few lines, use `edit_file` instead. Accidental `write_file` on an existing file destroys all content not included in the new content.

### Skipping read_file before apply_diff
Context lines in a diff must match the file (fuzzy by default). If the file has changed since you last read it, your diff will fail. **Always read first.**

### Forgetting to verify after edit
An edit may look correct in your diff but fail at the tool level. Always verify:
- Executable files: run them
- Data files: re-read and validate structure
- Config files: re-read and check syntax

### Successive byLine edits corrupt files silently
When making multiple byLine edits from a single read_file, the first edit shifts line numbers — subsequent edits target wrong content without any error. **Always re-read the file between byLine edits**, or better: **use byMarker or byContent** which search for content, not line numbers. If you must use byLine for successive edits, use the `verifyContent` parameter as a safety guard.

## byMarker Count Rules (one auto-count rule)

### The single auto-count rule
With `byMarker` range modes (`replace`/`remove`) and no explicit `count`:
- Multi-line markers auto-derive `count` from the marker's line count (the result says `autoCountUsed=true` with a note).
- A single-line marker with multi-line replace content REQUIRES an explicit `count` — the edit is refused with an error telling you to pass `count=N` or use a multi-line marker to auto-size the target. The old auto-derive from the content line count is gone: it silently over-consumed lines (it once replaced 41 lines from the anchor).
- `remove` with a single-line marker auto-derives `count=1` (only the marker line is removed).
- `byContent` auto-derives `count` from the matched block size. Insert modes ignore `count`.

### If you want ONLY the marker line replaced
A single-line marker with multi-line content is refused, so pass `count=1` explicitly:
```
edit_file(path="file.txt", mode="replace", marker="the line", count=1,
         content="replacement line 1\nreplacement line 2")
```
This replaces only 1 line (the marker line) with your multi-line content.

### If the original block is larger than your replacement
Set `count` explicitly to the original block size, or use `byContent` targeting, which auto-derives count from the matched block size.

## Fragment Markers Are Refused (fragmentOk)

### byMarker replace/replaceAll refuses fragment markers
A marker matching as a strict substring INSIDE a longer line replaces the WHOLE line, destroying the text outside the match. To prevent silent destruction, `replace`/`replaceAll` with `byMarker` is refused when any matched line is strictly longer than its marker line (single-line marker: the matched line itself; multi-line marker: any verified marker line strictly contained in its file line). The error lists the affected 1-based line numbers with their full text. Pass a longer marker covering the full line(s), or `fragmentOk=true` to replace whole lines deliberately.

### fragmentOk constraints and reporting
`fragmentOk` is valid only with `byMarker` `replace`/`replaceAll` — providing it with `byLine`/`byContent` targeting, or with `remove`/insert modes, is an error. With `fragmentOk=true` the result includes `fragmentLinesReplaced` (count of matched lines strictly longer than their marker lines, summed across all occurrences with replaceAll) so the granularity stays visible in the success path. The guard fires identically under `dryRun=true`.

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
Line-granular: replaces each ENTIRE line containing the marker substring with the content lines (the fragment guard still applies — see "Fragment Markers Are Refused (fragmentOk)"). Consider `byContent` for more precise multi-line targeting.

### replaceAll is non-overlapping
After replacing one occurrence, search resumes from the line after the replacement. Replacements cannot overlap with previously replaced content.

### matchIndex and replaceAll conflict
`replaceAll=true` with `matchIndex > 1` returns an error. Use either `replaceAll=true` (all occurrences) or `matchIndex=N` (single occurrence), not both.

## Result-Report Fields Worth Checking

### replace/remove echo the consumed lines
Replace/remove edits that consume lines echo the exact pre-edit target lines in the `replaced` field (elided when more than 11 lines: first 5 + "… (N lines replaced; first 5 and last 5 shown)" marker + last 5, with `replacedTotal` and `replacedTruncated=true`). Check `replaced` against what you intended to consume — a fragment marker that replaced whole lines used to look exactly like a benign swap in the report.

### no-op edits are flagged
When the composed result is element-wise identical to the input lines, the result includes `noOp=true` and a note ("the edit produced no change (new content identical to the replaced text); if you intended a change, the content equals the search text"). A zero `linesChanged` is neither necessary nor sufficient for a no-op: replacing 2 lines with 2 DIFFERENT lines has `linesChanged == 0` but is a real change. If you get `noOp=true` but wanted a change, your content equals the search block (or the target lines).

### Insert modes keep the anchor — watch for anchorDuplicate
Insert modes (`append`/its alias `insert_after`, and `insert_before`) KEEP the targeted block and insert the content adjacent to it — do not repeat the targeted text in content. When the content's boundary line duplicates the kept anchor line/block, the result includes `anchorDuplicate=true` and a note telling you which boundary line to drop. The tool warns, never mutates: the anchor is not auto-stripped. Anchoring is at the FIRST occurrence when the block repeats.

## apply_diff Behavior

### Hunk header counts are advisory
The `@@ -oldStart,oldCount +newStart,newCount @@` counts are advisory. The tool counts actual body lines and uses those. Mismatches produce warnings (not errors) in the `warnings` array.

### Fuzzy context matching by default
Context lines (starting with ` `) use trimmed equality by default. Leading/trailing whitespace differences are tolerated. Set `fuzzy=false` for exact matching.

### Fuzzy matching does NOT change what's written
Only the matching is fuzzy — the file content written is always the `+` lines from the diff exactly as provided.

### Multi-hunk diffs preserve content between hunks
When applying multiple hunks, lines between hunks are preserved. Each hunk is applied independently.

### apply_diff no longer has strictCounts parameter
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

### Invisible-character-only mismatches
When a `byMarker`/`byContent` search (or a `verifyContent` check) fails and the diagnostic's closest match looks VISUALLY IDENTICAL to what you sent, do not retry identically: the contents likely differ only by invisible characters (NBSP, tabs, zero-width, control chars). The diagnostic/error now says "differ only by invisible characters" and renders both contents escaped (`\t` for tabs, `\uXXXX` for other invisibles; the plain space stays raw) — compare the escaped renderings, strip or retype the invisible characters, then retry.

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
edit_file(path="big.c", mode="replace", marker="target",
         scopeStart=120, scopeEnd=180, content="new")
```
Either or both bounds may be given. `scopeStart` alone searches from that line to EOF; `scopeEnd` alone searches from line 1 to that line.

### Scope constrains the anchor, not the whole match
Only the **first line** of a match must be inside the range. A byContent block whose anchor is in scope may extend past `scopeEnd`. The same applies to byMarker: with `count > 1` (or auto-count from multi-line markers), the replaced region may extend past `scopeEnd`. If you need the whole replaced region inside the range, widen `scopeEnd`.

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
