/// Targeting resolution and dispatch for the unified in-memory edit engine.
/// Breaks the former editFileUnifiedMemory monolith into per-method helpers.
module llm.tool_call.io.edit_dispatch;

import std.algorithm : any, canFind;
import std.array : appender, join;
import std.conv : text;
import std.exception : enforce;
import std.string : chomp, splitLines, strip;

import llm.tool_call.io : EditMode;
import llm.tool_call.io.diagnostic : appendScope;
import llm.tool_call.io.edit_engine : EditTarget, EditFileOutcome,
    editContentLines, editFileInMemory;
import llm.tool_call.io.search : CodeBlockRange, findMarkerLine, findNthMarkerLine,
    countMarkerOccurrences, findMultiLineMarkerLine, findNthMultiLineMarkerLine,
    countMultiLineMarkerOccurrences, findCodeBlock,
    findNthCodeBlock, countCodeBlockOccurrences, resolveScope, maxMarkerLines;

/// byLine targeting: validate startLine/count and apply the edit to the
/// resolved 1-based line range.
package EditFileOutcome executeByLine(string[] fileLines, EditMode mode, string content,
        long startLine, long count, bool replaceAll, string verifyContent = null) @safe {
    enforce(!replaceAll,
            "replaceAll is not supported with byLine targeting (there is no search pattern)");
    enforce(startLine >= 1, i"parameter startLine $(startLine) must be > 0".text);
    const isRangeMode = mode == EditMode.replace || mode == EditMode.remove;
    if (isRangeMode)
        enforce(count >= 1, i"parameter count must be >= 1 for byLine $(mode) mode. Specify how many lines to replace starting at startLine. Alternatively, use marker (byMarker) or searchContent (byContent) targeting to auto-detect the target range."
                .text);

    if (isRangeMode && verifyContent.length > 0) {
        string actualContent;
        if (startLine - 1 >= cast(long) fileLines.length) {
            // startLine is past end of file — the mismatch error below
            // will produce a clear diagnostic instead of an inverted range.
            actualContent = i"(line $(startLine) is beyond end of file at line $(fileLines.length))"
                .text;
        } else {
            const endIdxCheck = startLine - 1 + count > cast(long) fileLines.length
                ? cast(long) fileLines.length : startLine - 1 + count;
            if (startLine - 1 < endIdxCheck)
                actualContent = fileLines[startLine - 1 .. endIdxCheck].join("\n");
        }
        if (actualContent.strip != verifyContent.strip)
            throw new Exception(i"byLine verifyContent mismatch: expected target lines to match verifyContent, but found different content. Actual content:\n$(
                    actualContent)\n\nExpected verifyContent:\n$(verifyContent)\n\nSuggestion: re-read the file to get current line numbers, or use byMarker/byContent targeting which searches for content rather than relying on line numbers."
                    .text);
    }

    const startIdx = startLine - 1;
    const endIdx = isRangeMode ? startIdx + count : startIdx;
    const matchedLines = isRangeMode ? count : 1;
    auto res = editFileInMemory(fileLines, mode, content, EditTarget(startIdx,
            endIdx, startLine, matchedLines));
    return EditFileOutcome(res.lines, res.matched, res.linesChanged, 1, false, "");
}

/// byMarker targeting: auto-count heuristic, replaceAll with scope pre-copy.
/// Supports both single-line markers (substring match on one line) and
/// multi-line markers (first line as anchor, subsequent lines verified as
/// substrings of consecutive file lines).
package EditFileOutcome executeByMarker(string[] fileLines, EditMode mode, string content,
        const(string[]) contentLines, long contentLineCount, string marker, long count, bool replaceAll, long matchIndex,
        size_t scopeBegin, size_t scopeEndExclusive, long scopeStart, long scopeEnd) @safe {
    const isMultiLine = marker.canFind("\n");
    string[] markerLines;
    if (isMultiLine) {
        markerLines = marker.splitLines;
        enforce(markerLines.length <= maxMarkerLines, i"marker must not exceed $(maxMarkerLines) lines (got $(
                markerLines.length))".text);
    }
    const isRangeMode = mode == EditMode.replace || mode == EditMode.remove;
    long targetCount = count;
    bool autoCount = false;
    if (isRangeMode && targetCount < 1) {
        if (isMultiLine) {
            targetCount = cast(long) markerLines.length; // auto-count from marker lines
            autoCount = true;
        } else if (mode == EditMode.replace && contentLineCount > 1) {
            targetCount = contentLineCount; // auto-count heuristic
            autoCount = true;
        } else {
            targetCount = 1;
        }
    }
    if (!isRangeMode)
        targetCount = 1;

    string note;
    if (autoCount) {
        if (isMultiLine)
            note = i"count was auto-derived as $(targetCount) from marker line count".text;
        else
            note = i"count was auto-derived as $(targetCount) from content line count".text;
    }

    const scopeActive = scopeStart != -1 || scopeEnd != -1;
    // Truncate multi-line markers in error messages for readability.
    const markerDisplay = isMultiLine ? i"'$(markerLines[0])' (+$(markerLines.length - 1) more lines)".text
        : i"'$(marker)'".text;
    const markerNotFound = scopeActive ? appendScope(i"marker $(markerDisplay) not found in file".text,
            scopeStart, scopeEnd) : i"marker $(markerDisplay) not found in file".text;

    if (replaceAll) {
        // Replace every in-scope occurrence; each match replaces
        // targetCount lines. Lines outside the scope are preserved.
        auto app = appender!(string[])();
        // Preserve lines before the scope start (search begins at scopeBegin).
        if (scopeBegin < fileLines.length)
            app.put(fileLines[0 .. scopeBegin]);
        size_t pos = scopeBegin;
        long ops = 0;
        EditTarget firstMatch;
        bool first = true;
        while (pos < scopeEndExclusive && pos < fileLines.length) {
            long idx;
            if (isMultiLine) {
                idx = findMultiLineMarkerLine(fileLines, markerLines, pos, scopeEndExclusive);
            } else {
                idx = findMarkerLine(fileLines, marker, pos, scopeEndExclusive);
            }
            if (idx < 0)
                break;
            const absEnd = idx + targetCount;
            enforce(absEnd <= cast(long) fileLines.length, i"marker at line $(idx + 1) with count $(
                    targetCount) exceeds file length".text);
            app.put(fileLines[pos .. cast(size_t) idx]);
            app.put(contentLines);
            if (first) {
                firstMatch = EditTarget(idx, absEnd, idx + 1, targetCount);
                first = false;
            }
            pos = cast(size_t) absEnd;
            ops++;
        }
        if (ops == 0)
            throw new Exception(markerNotFound);
        app.put(fileLines[pos .. $]);
        return EditFileOutcome(app[], firstMatch,
                cast(long) app[].length - cast(long) fileLines.length, ops, autoCount, note);
    }

    long markerIdx;
    if (matchIndex > 1) {
        if (isMultiLine) {
            markerIdx = findNthMultiLineMarkerLine(fileLines, markerLines,
                    matchIndex, scopeBegin, scopeEndExclusive);
        } else {
            markerIdx = findNthMarkerLine(fileLines, marker, matchIndex,
                    scopeBegin, scopeEndExclusive);
        }
        if (markerIdx < 0) {
            const occ = isMultiLine ? countMultiLineMarkerOccurrences(fileLines,
                    markerLines, scopeBegin, scopeEndExclusive) : countMarkerOccurrences(fileLines,
                    marker, scopeBegin, scopeEndExclusive);
            const occWord = occ == 1 ? "occurrence" : "occurrences";
            const verbWord = occ == 1 ? "was" : "were";
            throw new Exception(appendScope(i"matchIndex=$(matchIndex) but only $(occ) $(occWord) of marker $(
                    markerDisplay) $(verbWord) found".text, scopeStart, scopeEnd));
        }
    } else {
        if (isMultiLine) {
            markerIdx = findMultiLineMarkerLine(fileLines, markerLines,
                    scopeBegin, scopeEndExclusive);
        } else {
            markerIdx = findMarkerLine(fileLines, marker, scopeBegin, scopeEndExclusive);
        }
        if (markerIdx < 0)
            throw new Exception(markerNotFound);
    }
    const absEnd = markerIdx + targetCount;
    enforce(absEnd <= cast(long) fileLines.length, i"marker at line $(markerIdx + 1) with count $(
            targetCount) exceeds file length".text);
    auto res = editFileInMemory(fileLines, mode, content, EditTarget(markerIdx,
            isRangeMode ? absEnd : markerIdx, markerIdx + 1, isRangeMode ? targetCount : 1));
    return EditFileOutcome(res.lines, res.matched, res.linesChanged, 1, autoCount, note);
}

/// byContent targeting: block search, replaceAll loop, matchIndex, insert anchor.
package EditFileOutcome executeByContent(string[] fileLines, EditMode mode, string content,
        const(string[]) contentLines, string searchContent, long count, bool replaceAll, long matchIndex,
        size_t scopeBegin, size_t scopeEndExclusive, long scopeStart, long scopeEnd) @safe {
    // Reaching here means searchContent was non-empty (required by targeting
    // resolution above), so no empty-check is needed.
    auto searchLines = searchContent.chomp.splitLines;
    enforce(searchLines.any!(l => l.strip.length > 0),
            "searchContent must contain at least one non-empty line (all lines are empty or whitespace)");

    const isRangeMode = mode == EditMode.replace || mode == EditMode.remove;
    long targetCount = count;
    bool autoCount = false;
    if (isRangeMode && targetCount < 1)
        targetCount = -1; // filled from the matched block size below

    const scopeActive = scopeStart != -1 || scopeEnd != -1;
    const blockNotFound = scopeActive ? appendScope("search block not found in file",
            scopeStart, scopeEnd) : "search block not found in file";

    if (replaceAll) {
        // Explicit count would silently change nothing, so reject it instead.
        enforce(count < 1, "explicit count is not supported with replaceAll and byContent; each occurrence replaces the full matched block (omit count, or use byMarker with count for fixed-size blocks)");
        // Replace every in-scope occurrence with the full matched block.
        auto app = appender!(string[])();
        // Preserve lines before the scope start (search begins at scopeBegin).
        if (scopeBegin < fileLines.length)
            app.put(fileLines[0 .. scopeBegin]);
        size_t pos = scopeBegin;
        long ops = 0;
        EditTarget firstMatch;
        bool first = true;
        while (pos < scopeEndExclusive && pos < fileLines.length) {
            auto r = findCodeBlock(fileLines, searchLines, pos, scopeEndExclusive);
            if (!r.found)
                break;
            const absStart = r.start;
            const absEnd = r.end;
            enforce(absEnd > pos, "findCodeBlock returned zero-length match");
            app.put(fileLines[pos .. absStart]);
            app.put(contentLines);
            if (first) {
                firstMatch = EditTarget(cast(long) absStart, cast(long) absEnd,
                        cast(long) absStart + 1, cast(long)(absEnd - absStart));
                first = false;
            }
            pos = absEnd;
            ops++;
        }
        if (ops == 0)
            throw new Exception(blockNotFound);
        app.put(fileLines[pos .. $]);
        return EditFileOutcome(app[], firstMatch, cast(long) app[].length - cast(long) fileLines.length,
                ops, true, "count auto-derived from matched block size (replaceAll)");
    }

    CodeBlockRange range;
    if (matchIndex > 1) {
        range = findNthCodeBlock(fileLines, searchLines, matchIndex,
                scopeBegin, scopeEndExclusive);
        if (!range.found) {
            const occ = countCodeBlockOccurrences(fileLines, searchLines,
                    scopeBegin, scopeEndExclusive);
            const occWord = occ == 1 ? "occurrence" : "occurrences";
            const verbWord = occ == 1 ? "was" : "were";
            throw new Exception(appendScope(i"matchIndex=$(matchIndex) but only $(occ) $(occWord) of the search block $(verbWord) found".text,
                    scopeStart, scopeEnd));
        }
    } else {
        range = findCodeBlock(fileLines, searchLines, scopeBegin, scopeEndExclusive);
        if (!range.found)
            throw new Exception(blockNotFound);
    }
    if (targetCount < 1) {
        targetCount = cast(long) range.end - cast(long) range.start;
        autoCount = true;
    }

    string note;
    if (autoCount)
        note = i"count was auto-derived as $(targetCount) from matched block size".text;
    const absStart = cast(long) range.start;
    long absEnd;
    if (isRangeMode) {
        absEnd = absStart + targetCount;
        enforce(absEnd <= cast(long) fileLines.length, i"byContent target at line $(range.start + 1) with count $(
                targetCount) exceeds file length".text);
    } else {
        // Insert modes ignore count: anchor comes from the matched block only.
        absEnd = cast(long) range.end;
    }
    // append/insert_after anchor on the LAST line of the block; insert_before
    // anchors on the first line (content before the block).
    const insertAnchor = isRangeMode || mode == EditMode.insert_before ? absStart : absEnd - 1;
    auto res = editFileInMemory(fileLines, mode, content, EditTarget(insertAnchor, isRangeMode
            ? absEnd : insertAnchor, absStart + 1, isRangeMode ? targetCount
            : cast(long)(range.end - range.start)));
    return EditFileOutcome(res.lines, res.matched, res.linesChanged, 1, autoCount, note);
}

/// Validate exactly one targeting method and replaceAll/matchIndex combos.
package void resolveTargeting(long startLine, string marker, string searchContent,
        bool replaceAll, long matchIndex) @safe {
    // -1 is the "not provided" sentinel (the params struct defaults to it);
    // an explicit 0 or negative startLine is a validation error, not "absent".
    const hasStartLine = startLine != -1;
    const hasMarker = marker.length > 0;
    const hasSearch = searchContent.length > 0;
    const provided = (hasStartLine ? 1 : 0) + (hasMarker ? 1 : 0) + (hasSearch ? 1 : 0);
    if (provided == 0)
        throw new Exception("no targeting method specified. Provide exactly one of: startLine (byLine), marker (byMarker), or searchContent (byContent).");
    if (provided > 1) {
        string[] names;
        if (hasStartLine)
            names ~= "startLine";
        if (hasMarker)
            names ~= "marker";
        if (hasSearch)
            names ~= "searchContent";
        throw new Exception(i"ambiguous targeting: multiple targeting parameters provided ($(
                names.join(", "))). Use exactly one of: startLine (byLine), marker (byMarker), or searchContent (byContent)."
                .text);
    }
    // Checked after targeting resolution so byLine-specific errors (e.g.
    // "replaceAll is not supported with byLine targeting") take precedence;
    // matchIndex is documented as ignored by byLine.
    if (replaceAll && matchIndex > 1 && !hasStartLine)
        throw new Exception("matchIndex > 1 cannot be combined with replaceAll; replaceAll already targets every occurrence (omit matchIndex to replace all, or use matchIndex=N to target a single occurrence)");
}

/// Resolve the target and apply the edit entirely in memory. Single entry
/// point for the unified editFile tool: supports byLine/byMarker/byContent,
/// auto-count, replaceAll, matchIndex, scope, and all 5 edit modes.
///
/// Throws:
///     Exception on invalid targeting, failed searches, or invalid ranges;
///     the unified editFile tool converts exceptions into diagnostic JSON.
package EditFileOutcome editFileUnifiedMemory(string[] fileLines, EditMode mode, string content,
        long startLine = -1, long count = 0, string marker = null, string searchContent = null,
        bool replaceAll = false, long matchIndex = 1, long scopeStart = -1,
        long scopeEnd = -1, string verifyContent = null) @safe {
    enforce(matchIndex >= 1, i"parameter matchIndex $(matchIndex) must be >= 1".text);
    enforce(!(mode == EditMode.remove && content.length > 0),
            "parameter mode is 'remove' but content is not empty");
    if (replaceAll && (mode == EditMode.append || mode == EditMode.insert_after
            || mode == EditMode.insert_before))
        throw new Exception("replaceAll cannot be used with insert modes; use replace or remove");
    // Scope is validated even when byLine would ignore it, so bad
    // parameters are always rejected.
    const scopeWindow = resolveScope(scopeStart, scopeEnd, fileLines.length);
    const contentLines = editContentLines(content);
    const contentLineCount = cast(long) contentLines.length;
    const hasStartLine = startLine != -1;
    const hasMarker = marker.length > 0;
    resolveTargeting(startLine, marker, searchContent, replaceAll, matchIndex);
    if (hasStartLine)
        return executeByLine(fileLines, mode, content, startLine, count,
                replaceAll, verifyContent);
    if (hasMarker)
        return executeByMarker(fileLines, mode, content, contentLines, contentLineCount, marker, count, replaceAll,
                matchIndex, scopeWindow.begin, scopeWindow.endExclusive, scopeStart, scopeEnd);
    // Reaching here means searchContent was non-empty (required by targeting
    // resolution above), so no empty-check is needed.
    return executeByContent(fileLines, mode, content, contentLines, searchContent, count, replaceAll,
            matchIndex, scopeWindow.begin, scopeWindow.endExclusive, scopeStart, scopeEnd);
}
