# editFile — Unified file editing

The single editing tool. Supports three targeting methods and five edit modes.

## Targeting methods (exactly one required)

| Method | Parameters | Use When |
|--------|-----------|----------|
| `byLine` | `startLine` (1-based) + `count` | You know exact line numbers (`count` only for replace/remove) |
| `byMarker` | `marker` (substring) | You know a unique string in the target line |
| `byContent` | `searchContent` (code block) | You know the exact code to replace |

## Modes

| Mode | Behavior |
|------|----------|
| `replace` | Replace targeted lines with `content` |
| `remove` | Delete targeted lines (`content` must be empty) |
| `append` | Keep target line, add `content` after it |
| `insert_before` | Add `content` before target line, keep target line |
| `insert_after` | Same as `append` (alias) |

## Options

| Option | Description |
|--------|-------------|
| `dryRun` | Preview the result without writing to disk |
| `matchIndex` | Which occurrence to target (1-based, default 1; values < 1 invalid). `matchIndex > 1` targets the Nth occurrence (byMarker/byContent only) and cannot be combined with `replaceAll`; ignored by byLine. If `matchIndex` exceeds the number of matches, the error reports the actual count (e.g. `matchIndex=3 but only 2 occurrences of marker 'X' were found`). |
| `scopeStart` / `scopeEnd` | Limit the byMarker/byContent search to a 1-based inclusive line range. Either or both may be given: `scopeStart` alone searches from that line to end of file; `scopeEnd` alone searches from line 1 to that line. The first line of a match (anchor) must be inside the range; the match may extend past `scopeEnd`. `scopeEnd` clamps to the file end. Ignored by byLine. `scopeStart` must be >= 1; if both are given, `scopeStart` must be <= `scopeEnd`. |

## Auto-count behavior (byMarker + replace mode)

- When replacing with **multi-line content** and no explicit `count`, the tool auto-derives `count` from the number of content lines.
- When replacing with **single-line content**, defaults to `count=1` (replace only the marker line).
- **Edge case**: If the original block is larger than your replacement, set `count` explicitly or use `byContent` targeting to replace the exact matched block.
- **`remove` mode**: `count` defaults to 1 (the marker line only). Set `count` explicitly to remove more lines.

## byContent behavior

- Auto-derives `count` from the matched block size.
- An explicit `count` overrides the auto-derived block size (not supported with `replaceAll`).
- Uses **trimmed equality** matching: `fileLine.strip == searchLine.strip`. Tolerates indentation differences.
- Empty search lines are **skipped** during matching (they don't consume file lines). However, empty lines in the **file** still need to match the next non-empty search line. If the file has `line1\n\nline3` and your search is `line1\nline3`, the match fails because file line 2 (empty) doesn't match `line3`.
- Full line matching (not substring) prevents comment false positives.

## Scope limiting (large files)

When you know roughly where the target is, pass `scopeStart`/`scopeEnd` (1-based,
inclusive) to restrict the byMarker/byContent search:

```json
editFile(path="big.c", mode="replace", marker="target",
         scopeStart=120, scopeEnd=180, content="new")
```

- Either or both bounds may be given (`scopeStart` alone = from that line to
  EOF; `scopeEnd` alone = from line 1 to that line).
- Only the **anchor** (first line of the match) must be inside the range; a
  byContent block may extend past `scopeEnd`. The same anchor rule applies to
  byMarker: with `count > 1` the replaced region may extend past `scopeEnd`.
- `scopeEnd` beyond the file is clamped to the file end. `scopeStart` is
  **not** clamped — if it exceeds the file length, the search range becomes
  empty and the search fails with "not found within scope". A marker/block
  outside the range reports `not found ... within scope [A, B]` (the message
  echoes the requested range) and the `diagnostic` includes a `scope` field
  with the **effective** clamped searched range.
- `scopeStart` must be >= 1; `scopeStart` > `scopeEnd` is a validation error.
- Ignored by `byLine` (byLine does not search). Works with `matchIndex`,
  `replaceAll`, and all 5 modes.

## Return format (JSON)

```json
{
  "ok": true,
  "matchedAt": 5,
  "matchedLines": 3,
  "linesChanged": -1,
  "operations": 1
}
```
When `dryRun=true`, also includes `"preview"` (the full modified file content).

## Error handling

- On failure, returns `"ok": false` with an `"error"` string and a `"diagnostic"` field.
- The diagnostic includes closest-match details when a marker or code block is not found.
- Read the `diagnostic` field to understand why the search failed and adjust your search.

## applyDiff — Unified diff patch application

Apply a pre-computed unified diff patch to a file.

**Parameters**:
- `path` — file path (relative to workarea)
- `diff` — unified diff text (with `--- a/path` and `+++ b/path` headers, then hunks)
- `dryRun` — preview without writing (default: false)
- `fuzzy` — fuzzy context matching (default: true, ignores leading/trailing whitespace)

**Auto-count**: Hunk header counts (`@@ -oldStart,oldCount +newStart,newCount @@`) are **advisory**. The tool counts actual body lines and uses those. Mismatches produce warnings, not errors.

**Fuzzy matching**: By default, context lines use trimmed equality (`line.strip`). Set `fuzzy=false` for exact character-by-character matching.

**Return format** (JSON):
```json
{
  "ok": true,
  "hunksApplied": 1,
  "linesChanged": 3,
  "warnings": []
}
```
Warnings list count mismatches (e.g., `"Hunk 1: declared 7 old lines, body has 8"`).
When `dryRun=true`, also includes `"preview"`.

## writeFile — Write or create files

- Creates the file (including parent directories) if it doesn't exist.
- **Replaces entire content** if file exists — use only for new files or complete rewrites.
- Never use for partial edits.
