/// Marker and code-block search primitives for the file-editing tools.
/// All functions are package-private and @safe.
module llm.tool_call.io.search;

import std.algorithm : canFind;
import std.conv : text;
import std.exception : enforce;
import std.string : strip;

/// Effective 0-based half-open search window [begin, endExclusive), clamped
/// to the file length.
package struct ScopeWindow {
    size_t begin;
    size_t endExclusive;
}

/// Validate scope bounds and clamp the search window to the file length.
/// Throws on invalid bounds; -1 means "not provided".
package ScopeWindow resolveScope(long scopeStart, long scopeEnd, size_t fileLength) @safe {
    const hasScopeStart = scopeStart != -1;
    const hasScopeEnd = scopeEnd != -1;
    if (hasScopeStart && scopeStart < 1)
        throw new Exception(i"parameter scopeStart $(scopeStart) must be >= 1".text);
    if (hasScopeEnd && scopeEnd < 1)
        throw new Exception(i"parameter scopeEnd $(scopeEnd) must be >= 1".text);
    if (hasScopeStart && hasScopeEnd && scopeStart > scopeEnd)
        throw new Exception(i"parameter scopeStart ($(scopeStart)) must be <= scopeEnd ($(scopeEnd))"
                .text);
    size_t begin = 0;
    size_t endExclusive = fileLength;
    if (hasScopeStart || hasScopeEnd) {
        begin = hasScopeStart ? cast(size_t)(scopeStart - 1) : 0;
        endExclusive = hasScopeEnd ? cast(size_t) scopeEnd : fileLength;
        if (endExclusive > fileLength)
            endExclusive = fileLength;
    }
    return ScopeWindow(begin, endExclusive);
}
/**
 * Perform a case-sensitive substring search across file lines.
 * Returns the 0-based index of the first line containing the marker,
 * or -1 if not found.
 *
 * Parameters:
 *     lines:        Array of file lines to search
 *     marker:       Substring to search for (case-sensitive)
 *     start:        First line index to search from (0-based, default 0)
 *     endExclusive: Last line index to search up to (0-based, exclusive;
 *                   default size_t.max = search to end of file)
 *
 * Returns:
 *     0-based index of the first matching line in [start, endExclusive),
 *     or -1 if not found
 */
package long findMarkerLine(string[] lines, string marker, size_t start = 0,
        size_t endExclusive = size_t.max) @safe {
    enforce(marker.length > 0, "marker must not be empty");
    if (start >= lines.length)
        return -1;
    const endIdx = endExclusive < lines.length ? endExclusive : lines.length;
    if (start >= endIdx)
        return -1;
    foreach (i; start .. endIdx) {
        if (lines[i].canFind(marker)) {
            return cast(long) i;
        }
    }
    return -1;
}

/// Find the Nth (1-based) occurrence of `marker` in `lines`.
///
/// Parameters:
///     start:        First line index to search from (0-based, default 0)
///     endExclusive: Last line index to search up to (0-based, exclusive;
///                   default size_t.max = search to end of file)
///
/// Returns the 0-based index of the Nth matching line, or -1 if fewer than
/// `nth` occurrences exist within the range.
package long findNthMarkerLine(string[] lines, string marker, long nth,
        size_t start = 0, size_t endExclusive = size_t.max) @safe {
    enforce(marker.length > 0, "marker must not be empty");
    enforce(nth >= 1, "nth must be >= 1");
    long found = 0;
    size_t pos = start;
    while (pos < endExclusive && pos < lines.length) {
        const idx = findMarkerLine(lines, marker, pos, endExclusive);
        if (idx < 0)
            return -1;
        found++;
        if (found == nth)
            return idx;
        pos = cast(size_t) idx + 1;
    }
    return -1;
}

/// Count occurrences of `marker` in `lines` (each line counted once).
///
/// Parameters:
///     start:        First line index to search from (0-based, default 0)
///     endExclusive: Last line index to search up to (0-based, exclusive;
///                   default size_t.max = search to end of file)
package long countMarkerOccurrences(string[] lines, string marker,
        size_t start = 0, size_t endExclusive = size_t.max) @safe {
    long n = 0;
    size_t pos = start;
    while (pos < endExclusive && pos < lines.length) {
        const idx = findMarkerLine(lines, marker, pos, endExclusive);
        if (idx < 0)
            break;
        n++;
        pos = cast(size_t) idx + 1;
    }
    return n;
}

/**
 * Range representing a matched code block within a file.
 *
 * Fields:
 *     start: 0-based start index in the file lines (inclusive)
 *     end:   0-based end index in the file lines (exclusive)
 *     found: true if a matching code block was found
 */
package struct CodeBlockRange {
    size_t start;
    size_t end;
    bool found;
}

/**
 * Find a code block in file lines using trimmed equality matching.
 *
 * Matching algorithm:
 * 1. Skip leading empty lines in searchLines to find the anchor line.
 * 2. Anchor line matches when fileLine.strip == searchLine.strip (trimmed equality).
 * 3. Subsequent non-empty search lines must match corresponding file lines via trimmed equality.
 * 4. Empty search lines are flexible — they are skipped and do not consume file lines.
 *
 * Parameters:
 *     fileLines:    Array of file lines to search in
 *     searchLines:  Array of search lines to match (the code block to find)
 *     start:        First file line index to search from (0-based, default 0)
 *     endExclusive: Last line index the match may START at (0-based, exclusive;
 *                   default size_t.max = search to end of file). Only the
 *                   anchor (first line of the match) is constrained; the
 *                   matched block may extend past `endExclusive`.
 *
 * Returns:
 *     CodeBlockRange with absolute start/end indices if found, or found=false if not found
 *     (an empty or all-whitespace searchLines also yields found=false)
 */
package CodeBlockRange findCodeBlock(const(char[])[] fileLines,
        const(char[])[] searchLines, size_t start = 0, size_t endExclusive = size_t.max) @safe {
    if (searchLines.length == 0) {
        return CodeBlockRange(0, 0, false);
    }

    // Find anchor (first non-empty line in searchLines after stripping)
    size_t anchorIdx = 0;
    while (anchorIdx < searchLines.length && searchLines[anchorIdx].strip.length == 0) {
        anchorIdx++;
    }

    if (anchorIdx >= searchLines.length) {
        return CodeBlockRange(0, 0, false);
    }

    auto anchorStripped = searchLines[anchorIdx].strip;

    // Search for anchor in fileLines starting at `start`
    const endIdx = endExclusive < fileLines.length ? endExclusive : fileLines.length;
    if (start >= endIdx)
        return CodeBlockRange(0, 0, false);
    foreach (i; start .. endIdx) {
        if (fileLines[i].strip != anchorStripped) {
            continue;
        }
        // Found anchor at position i — try to match remaining search lines
        size_t filePos = i + 1;
        bool allMatch = true;

        foreach (searchIdx; anchorIdx + 1 .. searchLines.length) {
            auto searchStripped = searchLines[searchIdx].strip;
            if (searchStripped.length == 0) {
                // Empty search line is flexible — skip without consuming file lines
                continue;
            }

            if (filePos >= fileLines.length) {
                allMatch = false;
                break;
            }

            if (fileLines[filePos].strip != searchStripped) {
                allMatch = false;
                break;
            }

            filePos++;
        }

        if (allMatch) {
            return CodeBlockRange(i, filePos, true);
        }
    }

    return CodeBlockRange(0, 0, false);
}

/// Find the Nth (1-based) non-overlapping occurrence of a code block in
/// `fileLines`.
///
/// Parameters:
///     start:        First file line index to search from (0-based, default 0)
///     endExclusive: Last line index the match may START at (0-based, exclusive;
///                   default size_t.max = search to end of file). Only the
///                   anchor (first line of the match) is constrained.
///
/// Returns the matched range with absolute indices, or found=false if fewer
/// than `nth` occurrences exist within the range.
package CodeBlockRange findNthCodeBlock(const(char[])[] fileLines,
        const(char[])[] searchLines, long nth, size_t start = 0, size_t endExclusive = size_t.max) @safe {
    enforce(nth >= 1, "nth must be >= 1");
    size_t pos = start;
    long found = 0;
    while (pos < fileLines.length) {
        auto r = findCodeBlock(fileLines, searchLines, pos, endExclusive);
        if (!r.found)
            return CodeBlockRange(0, 0, false);
        if (r.end <= pos)
            return CodeBlockRange(0, 0, false); // defensive: no progress
        found++;
        if (found == nth)
            return r;
        pos = r.end;
    }
    return CodeBlockRange(0, 0, false);
}

/// Count non-overlapping occurrences of a code block in `fileLines`.
///
/// Parameters:
///     start:        First file line index to search from (0-based, default 0)
///     endExclusive: Last line index the match may START at (0-based, exclusive;
///                   default size_t.max = search to end of file). Only the
///                   anchor (first line of the match) is constrained.
package long countCodeBlockOccurrences(const(char[])[] fileLines,
        const(char[])[] searchLines, size_t start = 0, size_t endExclusive = size_t.max) @safe {
    long n = 0;
    size_t pos = start;
    while (pos < fileLines.length) {
        auto r = findCodeBlock(fileLines, searchLines, pos, endExclusive);
        if (!r.found)
            break;
        if (r.end <= pos)
            break; // defensive: no progress
        n++;
        pos = r.end;
    }
    return n;
}
