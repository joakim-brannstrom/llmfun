# File Editing Pitfalls & Edge Cases

## Common Mistakes

### Using writeFile on existing files
`writeFile` replaces the entire file. If you only need to change a few lines, use `editFile`, `editFileByMarker`, or `searchAndReplace` instead. Accidental `writeFile` on an existing file destroys all content not included in the new content.

### Skipping readFile before applyDiff
Context lines in a diff must match the file exactly. If the file has changed since you last read it, your diff will fail silently or apply incorrectly. **Always read first.**

### Forgetting to verify after edit
An edit may look correct in your diff but fail at the tool level. Always verify:
- Executable files: run them
- Data files: re-read and validate structure
- Config files: re-read and check syntax

### Over-editing with editFile
`editFile` is for small, targeted changes. For large multi-line changes, `applyDiff` or `searchAndReplace` is more reliable and easier to review.

### Under-using editFile
For single-line replacements, `editFile` with mode "replace" is simpler than crafting a full diff. Don't over-engineer simple changes.

## Trimmed Equality Gotchas (searchAndReplace / searchAndReplaceAll)

### Search block must have at least one non-empty line
If all lines in your search content are empty or whitespace-only, the tool rejects it with an error. Include at least one substantive line in the search block.

### Trimmed equality prevents comment false matches
Searching for `"foo"` will NOT match `"// call fooBar()"` because trimmed equality requires `line.strip == searchLine.strip`. This prevents accidentally matching code inside comments or strings.

### Leading empty lines in search are skipped
The matching algorithm skips leading empty lines in the search block to find the anchor line. If your search content starts with blank lines, they are ignored during matching.

Example:
```
Search block:
    (empty line)
    (empty line)
    int x = 1;
    int y = 2;

The two leading empty lines are skipped. Matching starts with "int x = 1;".
```

### Empty lines in the middle are flexible
Empty lines within the search block don't need to match. This allows you to search for code blocks with variable spacing between lines.

## applyDiff Rules

- Must be a **valid unified diff** with `--- a/path` and `+++ b/path` headers
- Context lines (starting with space) **must match the file exactly**
- Always call `readFile` first to ensure context lines are accurate
- Mismatched context lines cause silent failures — verify after applying

## Dry-Run Pattern

Before applying any edit, use the dry-run variant to preview:

1. Call `*DryRun` to preview the change
2. Inspect the `preview` field to verify correctness
3. Check `matchedAt` to confirm the right location was found
4. Check `linesChanged` to understand the scope of the change
5. If satisfied, call the non-dry-run version to apply

Dry-run variants exist for: `editFileByMarkerDryRun`, `searchAndReplaceDryRun`, `applyDiffDryRun`.
`editFile` and `writeFile` have no dry-run variants.

## Marker Matching Details (editFileByMarker)

- **Case-sensitive substring** matching: finds the first line containing the marker
- Use unique, specific markers to avoid matching the wrong line
- When multiple lines contain the marker, only the **first match** is used
- Modes: `insert_before`, `insert_after`, `replace`, `remove`

## Tool-Specific Notes

### editFile (position-based)
- Modes: `replace`, `append`, `remove`
- Line numbers are 1-based
- No dry-run variant — verify after applying
- Risk: line numbers shift if file changes between read and edit

### searchAndReplace vs searchAndReplaceAll
- `searchAndReplace`: replaces only the **first** occurrence
- `searchAndReplaceAll`: replaces **all non-overlapping** occurrences
- Both use trimmed equality matching

### writeFile
- Creates the file (including parent directories) if it doesn't exist
- **Replaces entire content** if file exists — use only for new files or complete rewrites
- Never use for partial edits

### Indentation differences are tolerated
Since matching uses `strip`, searching for unindented code will match indented code. This is usually desired but can cause unexpected matches if indentation is semantically important.

### Multi-line search blocks require all non-empty lines to match in order
For a multi-line search block, each non-empty line must match a corresponding file line in sequence. If any non-empty line doesn't match, the entire block fails.

## First-Match Behavior

### editFileByMarker targets the first matching line
If multiple lines contain your marker string, only the **first** occurrence is edited. Use more specific markers to target the correct line.

### searchAndReplace only replaces the first occurrence
Use `searchAndReplaceAll` if you need to replace multiple occurrences. `searchAndReplace` stops after the first match.

### searchAndReplaceAll is non-overlapping
After replacing one occurrence, search resumes from the line after the replacement. This means replacements cannot overlap with previously replaced content.

## Marker-Based Editing Gotchas

### Case-sensitive matching
Marker matching is case-sensitive. Searching for `"Foo"` will not match `"foo"`. Use the exact casing from the file.

### Substring matching
The marker is matched as a substring. Searching for `"appMain"` will match `"void appMain()"`, `"// appMain implementation"`, etc. Use sufficiently unique markers.

### insert_before and insert_after keep the marker line
Unlike `replace` mode, `insert_before` and `insert_after` preserve the marker line. Content is added before or after it.

### remove mode requires empty content
When using `mode=remove`, the `content` parameter must be empty. Providing content with remove mode returns an error.

### Mode name formats
Marker-based tools accept both `snake_case` (e.g. `insert_before`) and `kebab-case` (e.g. `insert-before`). The original `editFile` tool supports only `replace`, `append`, and `remove` modes — it does NOT support `insert_before` or `insert_after`. Use `editFileByMarker` for those modes.

## Dry-Run Tools

### Dry-run tools never modify files
`*DryRun` tools return a preview of the modified content but do NOT write to disk. Always follow up with the non-dry-run version to actually apply the change.

### Preview shows full file content
The `preview` field in dry-run responses contains the complete modified file content, not just the changed lines.

### matchedAt is 1-based
The `matchedAt` field returns the 1-based line number where the match was found. Convert to 0-based if needed for other tools.

## Tool Selection Mistakes

### Using editFile for insert_before/insert_after
`editFile` does not support `insert_before` or `insert_after` modes. Use `editFileByMarker` for these operations.

### Using searchAndReplace for marker-based insertion
`searchAndReplace` replaces matched blocks. If you need to insert content before/after a marker without replacing it, use `editFileByMarker` with `insert_before` or `insert_after`.

### Using applyDiff for simple replacements
For replacing a known code block, `searchAndReplace` is simpler than crafting a unified diff. Use `applyDiff` for complex multi-hunk changes.

## Tool Selection Decision Tree

See `SKILL.md` for the full decision tree. This section highlights common mistakes when selecting tools.

## Verification Checklist

After every file edit:
1. [ ] Did I verify the result?
2. [ ] For code: did it execute successfully?
3. [ ] For data: is the format valid?
4. [ ] For config: does it parse correctly?
5. [ ] Did I handle any errors from the previous step?
6. [ ] Did I use the correct tool for the edit type?
