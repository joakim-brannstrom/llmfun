/// Targeting resolution and dispatch for the unified in-memory edit engine.
module llm.tool_call.io.edit_dispatch;

import std.algorithm : any, canFind, min;
import std.array : appender, join;
import std.conv : text;
import std.exception : enforce;
import std.string : chomp, splitLines, strip;

import llm.tool_call.io : EditMode;
import llm.tool_call.io.diagnostic : appendScope, invisibleCharMismatchNote;
import llm.tool_call.io.edit_engine : EditTarget, EditFileOutcome,
    editContentLines, editFileInMemory, isNoOpEdit;
import llm.tool_call.io.search : CodeBlockRange, findMarkerLine, findNthMarkerLine,
    countMarkerOccurrences, findMultiLineMarkerLine, findNthMultiLineMarkerLine,
    countMultiLineMarkerOccurrences, findCodeBlock,
    findNthCodeBlock, countCodeBlockOccurrences, resolveScope, maxMarkerLines;

version (unittest) {
    // Test-only: the placement unittests' assert messages use `to!string`.
    import std.conv : to;
}

private string[] normalizeSearchLines(const(string) block) @safe {
    return block.chomp.splitLines;
}

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
        if (actualContent.strip != verifyContent.strip) {
            auto msg = i"byLine verifyContent mismatch: expected target lines to match verifyContent, but found different content. Actual content:\n$(
                    actualContent)\n\nExpected verifyContent:\n$(verifyContent)\n\nSuggestion: re-read the file to get current line numbers, or use byMarker/byContent targeting which searches for content rather than relying on line numbers."
                .text;
            msg ~= invisibleCharMismatchNote(verifyContent, actualContent);
            throw new Exception(msg);
        }
    }

    const startIdx = startLine - 1;
    const endIdx = isRangeMode ? startIdx + count : startIdx;
    const matchedLines = isRangeMode ? count : 1;
    auto res = editFileInMemory(fileLines, mode, content, EditTarget(startIdx,
            endIdx, startLine, matchedLines));
    string[] replaced = isRangeMode ? fileLines[startIdx .. endIdx].dup : [];
    return EditFileOutcome(res.lines, res.matched, res.linesChanged, 1, false,
            "", 0, replaced, cast(long) replaced.length, isRangeMode
            && isNoOpEdit(fileLines, res.lines));
}

/// One fragment match: a file line verified to contain a marker line yet
/// strictly longer than it (1-based line number + full text). Replacing such a
/// line wholesale destroys the text outside the marker match.
package struct FragmentLine {
    /// 1-based file line number of the offending line
    long lineNum;
    /// Full text of the offending line
    string text;
}

/// Find the lines of one byMarker match that were verified to contain a
/// marker line yet are strictly longer than it. matchLines are the file lines
/// of the match, verifiedMarkerLines the marker lines they were verified
/// against (same length: for a single-line marker that is the matched line
/// itself; for a multi-line marker the first min(replacedCount,
/// markerLines.length) marker lines starting at the anchor). Substring
/// verification guarantees each marker line is a substring of the
/// corresponding match line, so a difference is exactly a strict lengthening.
/// firstLineNum is the 1-based number of the match's first line. Returns the
/// offending lines in match order.
package FragmentLine[] findFragmentLines(string[] matchLines,
        string[] verifiedMarkerLines, long firstLineNum) @safe {
    // TODO: is this an in-contract and thus a validation that should be done
    // only during testing, an invariant, or is this a sanitizer check of user
    // input? Isn't it better to return a SumType instead of an exception with a magic string?
    enforce(matchLines.length == verifiedMarkerLines.length,
            i"findFragmentLines: $(matchLines.length) match lines but $(verifiedMarkerLines.length) verified marker lines"
                .text);
    FragmentLine[] fragments;
    // TODO: rewrite using ranges. This should be possible to replace with enumerate, filter, array
    foreach (i, l; matchLines)
        if (l.length > verifiedMarkerLines[i].length)
            fragments ~= FragmentLine(firstLineNum + cast(long) i, l);
    return fragments;
}

/// byMarker fragment-guard refusal message: the marker matched as a strict
/// substring inside longer file line(s), so a whole-line replace would destroy
/// the text outside the marker match. The whole edit is refused before any
/// change.
package string fragmentGuardError(string markerDisplay, const(FragmentLine)[] fragments) @safe {
    string affected;
    foreach (i, f; fragments)
        affected ~= (i > 0 ? "; " : "") ~ i"line $(f.lineNum): '$(f.text)'".text;
    return i"marker $(markerDisplay) matched as a strict substring inside longer line(s); the edit was refused before any change. Matched lines longer than the marker: $(
            affected). Pass a longer marker that covers the full line(s), or pass fragmentOk=true to replace whole lines deliberately."
        .text;
}

/// Task 1 fragment guard shared by the replaceAll loop and the
/// single-occurrence path: refuse replace-mode edits whose matched line(s) are
/// strictly longer than the marker lines they were verified to contain (a
/// whole-line replace would destroy the text outside the marker match), before
/// any mutation, unless the caller acknowledged it with fragmentOk. verified
/// lines are the marker lines the match was verified against, already truncated
/// to the lines actually being replaced; firstLineNum is the 1-based number of
/// the match's first line. Returns the fragment count for the outcome echo (0
/// when refused).
package long checkFragmentGuard(string[] matchLines, string[] verifiedLines,
        long firstLineNum, string markerDisplay, bool fragmentOk) @safe {
    const frags = findFragmentLines(matchLines, verifiedLines, firstLineNum);
    if (frags.length > 0 && !fragmentOk)
        throw new Exception(fragmentGuardError(markerDisplay, frags));
    return cast(long) frags.length;
}

/// byMarker targeting: search, auto-count, replaceAll with scope. Supports
/// single-line markers (substring match on one line) and multi-line markers
/// (first line as anchor, subsequent lines verified as substrings of
/// consecutive file lines).
package EditFileOutcome executeByMarker(string[] fileLines, EditMode mode, string content, string[] contentLines,
        long contentLineCount, string marker, long count, bool replaceAll, long matchIndex, size_t scopeBegin,
        size_t scopeEndExclusive, long scopeStart, long scopeEnd, bool fragmentOk = false) @safe {
    const isMultiLine = marker.canFind("\n");
    string[] markerLines;
    if (isMultiLine) {
        markerLines = normalizeSearchLines(marker);
        enforce(markerLines.length <= maxMarkerLines, i"marker must not exceed $(maxMarkerLines) lines (got $(
                markerLines.length))".text);
    }
    const isRangeMode = mode == EditMode.replace || mode == EditMode.remove;
    long targetCount = count;
    bool autoCount = false;
    if (isRangeMode && targetCount < 1) {
        if (isMultiLine) {
            targetCount = cast(long) markerLines.length;
            autoCount = true;
        } else if (mode == EditMode.replace && contentLineCount > 1) {
            enforce(false, i"marker matches 1 line but content has $(contentLineCount) lines; pass count=$(
                    contentLineCount) to replace $(contentLineCount) lines starting at the marker, or use a multi-line marker to auto-size the target"
                    .text);
        } else {
            targetCount = 1;
        }
    }
    if (!isRangeMode)
        targetCount = 1;

    string note;
    if (autoCount)
        note = i"count was auto-derived as $(targetCount) from marker line count".text;

    const scopeActive = scopeStart != -1 || scopeEnd != -1;
    // Truncate multi-line markers in error messages for readability.
    const markerDisplay = isMultiLine ? i"'$(markerLines[0])' (+$(markerLines.length - 1) more lines)".text
        : i"'$(marker)'".text;
    const markerNotFound = scopeActive ? appendScope(i"marker $(markerDisplay) not found in file".text,
            scopeStart, scopeEnd) : i"marker $(markerDisplay) not found in file".text;

    if (replaceAll) {
        auto app = appender!(string[])();
        if (scopeBegin < fileLines.length)
            app.put(fileLines[0 .. scopeBegin]);
        size_t pos = scopeBegin;
        long ops = 0;
        // matched lines strictly longer than their marker lines, SUMMED across
        // occurrences for the success JSON
        long fragLong = 0;
        auto repl = appender!(string[])();
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
            // A marker line matching as a strict substring inside a longer
            // file line replaces the WHOLE line, refuse the whole edit on the
            // first offending match, before any mutation, unless the caller
            // acknowledged it with fragmentOk. Only the replaced lines
            // verified to contain a marker line count.
            if (mode == EditMode.replace) {
                const verifiedOverlap = min(cast(size_t) targetCount,
                        isMultiLine ? markerLines.length : 1);
                fragLong += checkFragmentGuard(fileLines[idx .. idx + verifiedOverlap], isMultiLine
                        ? markerLines[0 .. verifiedOverlap] : [marker], idx + 1,
                        markerDisplay, fragmentOk);
            }
            app.put(fileLines[pos .. cast(size_t) idx]);
            app.put(contentLines);
            repl.put(fileLines[idx .. absEnd]);
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
                cast(long) app[].length - cast(long) fileLines.length, ops,
                autoCount, note, fragLong, repl[], cast(long) repl[].length,
                isRangeMode && isNoOpEdit(fileLines, app[]));
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
    // fragment guard, single-occurrence path (shared with the
    // replaceAll loop via checkFragmentGuard above): refuse replace-mode edits
    // whose matched line(s) are strictly longer than their marker lines,
    // before any mutation.
    long fragLong = 0;
    if (mode == EditMode.replace) {
        const verifiedOverlap = min(cast(size_t) targetCount, isMultiLine ? markerLines.length : 1);
        fragLong = checkFragmentGuard(fileLines[markerIdx .. markerIdx + verifiedOverlap], isMultiLine
                ? markerLines[0 .. verifiedOverlap] : [marker], markerIdx + 1,
                markerDisplay, fragmentOk);
    }
    auto res = editFileInMemory(fileLines, mode, content, EditTarget(markerIdx,
            isRangeMode ? absEnd : markerIdx, markerIdx + 1, isRangeMode ? targetCount : 1));
    string[] replaced = isRangeMode ? fileLines[markerIdx .. absEnd].dup : [];

    // Insert modes keep the anchor marker line: warn when the content's
    // boundary line duplicates it (trimmed equality on both sides, matching
    // the targeting semantics). append is insert_after's alias, so it warns
    // the same way.
    bool anchorDuplicate = false;
    if (mode == EditMode.insert_after || mode == EditMode.append) {
        anchorDuplicate = contentLines.length > 0
            && contentLines[0].strip == fileLines[markerIdx].strip;
    } else if (mode == EditMode.insert_before) {
        anchorDuplicate = contentLines.length > 0
            && contentLines[$ - 1].strip == fileLines[markerIdx].strip;
    }
    return EditFileOutcome(res.lines, res.matched, res.linesChanged, 1, autoCount,
            note, fragLong, replaced, cast(long) replaced.length, isRangeMode
            && isNoOpEdit(fileLines, res.lines), anchorDuplicate);
}

/// byContent targeting: block search, replaceAll loop, matchIndex, insert anchor.
package EditFileOutcome executeByContent(string[] fileLines, EditMode mode, string content,
        const(string[]) contentLines, string searchContent, long count, bool replaceAll, long matchIndex,
        size_t scopeBegin, size_t scopeEndExclusive, long scopeStart, long scopeEnd) @safe {
    // Reaching here means searchContent was non-empty (required by targeting
    // resolution above), so no empty-check is needed.
    auto searchLines = normalizeSearchLines(searchContent);
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
        auto app = appender!(string[])();
        if (scopeBegin < fileLines.length)
            app.put(fileLines[0 .. scopeBegin]);
        size_t pos = scopeBegin;
        long ops = 0;
        auto repl = appender!(string[])();
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
            repl.put(fileLines[absStart .. absEnd]);
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
                ops, true, "count auto-derived from matched block size (replaceAll)", 0, repl[],
                cast(long) repl[].length, isRangeMode && isNoOpEdit(fileLines, app[]));
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

    // Insert modes keep the anchored line/block: warn when the content's
    // boundary line duplicates it (trimmed equality on both sides, matching
    // the targeting semantics). append is insert_after's alias, so it warns
    // the same way.
    bool anchorDuplicate = false;
    if (mode == EditMode.insert_after || mode == EditMode.append) {
        anchorDuplicate = contentLines.length > 0
            && contentLines[0].strip == fileLines[absEnd - 1].strip;
    } else if (mode == EditMode.insert_before) {
        anchorDuplicate = contentLines.length > 0
            && contentLines[$ - 1].strip == fileLines[absStart].strip;
    }
    auto res = editFileInMemory(fileLines, mode, content, EditTarget(insertAnchor, isRangeMode
            ? absEnd : insertAnchor, absStart + 1, isRangeMode ? targetCount
            : cast(long)(range.end - range.start)));
    string[] replaced = isRangeMode ? fileLines[absStart .. absEnd].dup : [];
    return EditFileOutcome(res.lines, res.matched, res.linesChanged, 1,
            autoCount, note, 0, replaced, cast(long) replaced.length,
            isRangeMode && isNoOpEdit(fileLines, res.lines), anchorDuplicate);
}

/// Validate the targeting method combination and the replaceAll/matchIndex
/// combos. Zero targeting parameters are allowed only for mode=append (pure
/// insertion at the end of the file); every other mode requires exactly one.
package void resolveTargeting(long startLine, string marker, string searchContent,
        bool replaceAll, long matchIndex, EditMode mode, bool fragmentOk = false) @safe {
    // -1 is the "not provided" sentinel (the params struct defaults to it);
    // an explicit 0 or negative startLine is a validation error, not "absent".
    // The replace-mode-only check comes first: fragmentOk with a pure EOF
    // append (zero targeting) would otherwise slip past all later checks.
    if (fragmentOk && mode != EditMode.replace)
        throw new Exception("fragmentOk is only valid with replace mode (replace or replaceAll); it has no effect with remove (whole lines are deleted) or insert modes (the marker line is kept)");
    const hasStartLine = startLine != -1;
    const hasMarker = marker.length > 0;
    const hasSearch = searchContent.length > 0;
    const provided = (hasStartLine ? 1 : 0) + (hasMarker ? 1 : 0) + (hasSearch ? 1 : 0);
    if (provided == 0) {
        // append without targeting is valid: a pure insertion one past the
        // last line; the caller anchors the EditTarget at fileLines.length.
        // Every other mode has nothing to target without a targeting method.
        if (mode != EditMode.append)
            throw new Exception("no targeting method specified. Provide exactly one of: startLine (byLine), marker (byMarker), or searchContent (byContent), or use mode=append with no targeting to append at the end of the file");
        return;
    }
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
    // Checked after targeting resolution so targeting-specific contradictions
    // (e.g. "replaceAll is not supported with byLine targeting") take
    // precedence; matchIndex is documented as ignored by byLine.
    if (replaceAll && hasStartLine)
        throw new Exception(
                "replaceAll is not supported with byLine targeting (there is no search pattern)");
    if (replaceAll && matchIndex > 1 && !hasStartLine)
        throw new Exception("matchIndex > 1 cannot be combined with replaceAll; replaceAll already targets every occurrence (omit matchIndex to replace all, or use matchIndex=N to target a single occurrence)");
    // fragmentOk guard last: it acknowledges a byMarker replace-only hazard,
    // so targeting-specific contradictions (replaceAll with byLine,
    // matchIndex > 1 with replaceAll) take precedence over it.
    if (fragmentOk && !hasMarker)
        throw new Exception("fragmentOk is only valid with marker (byMarker) targeting; it has no effect with startLine (byLine) or searchContent (byContent) targeting");
}

/// Resolve the target and apply the edit entirely in memory. Single entry
/// point for the unified editFile tool: supports byLine/byMarker/byContent,
/// auto-count, replaceAll, matchIndex, scope, and all 5 edit modes. mode=append
/// with no targeting appends at the end of the file (zero targeting is
/// otherwise an error).
///
/// Throws:
///     Exception on invalid targeting, failed searches, or invalid ranges;
///     the unified editFile tool converts exceptions into diagnostic JSON.
package EditFileOutcome editFileUnifiedMemory(string[] fileLines, EditMode mode, string content,
        long startLine = -1, long count = 0, string marker = null, string searchContent = null,
        bool replaceAll = false, long matchIndex = 1, long scopeStart = -1,
        long scopeEnd = -1, string verifyContent = null, bool fragmentOk = false) @safe {
    enforce(matchIndex >= 1, i"parameter matchIndex $(matchIndex) must be >= 1".text);
    enforce(!(mode == EditMode.remove && content.length > 0),
            "parameter mode is 'remove' but content is not empty");
    if (replaceAll && (mode == EditMode.append || mode == EditMode.insert_after
            || mode == EditMode.insert_before))
        throw new Exception("replaceAll cannot be used with insert modes; use replace or remove");
    // Scope is validated even when byLine would ignore it, so bad
    // parameters are always rejected.
    const scopeWindow = resolveScope(scopeStart, scopeEnd, fileLines.length);
    auto contentLines = editContentLines(content);
    const contentLineCount = cast(long) contentLines.length;
    const hasStartLine = startLine != -1;
    const hasMarker = marker.length > 0;
    const hasSearch = searchContent.length > 0;
    resolveTargeting(startLine, marker, searchContent, replaceAll, matchIndex, mode, fragmentOk);
    if (!hasStartLine && !hasMarker && !hasSearch) {
        // mode == append here (guaranteed by resolveTargeting): zero targeting
        // means a pure insertion one past the last line; editFileInMemory
        // clamps the insert point to the file length at EOF, so this handles
        // both a non-empty file (insert after the last line) and an empty one.
        auto res = editFileInMemory(fileLines, mode, content, EditTarget(cast(long) fileLines.length,
                cast(long) fileLines.length, cast(long) fileLines.length + 1, 1));
        return EditFileOutcome(res.lines, res.matched, res.linesChanged, 1, false, "");
    }
    if (hasStartLine)
        return executeByLine(fileLines, mode, content, startLine, count,
                replaceAll, verifyContent);
    if (hasMarker)
        return executeByMarker(fileLines, mode, content, contentLines,
                contentLineCount, marker, count, replaceAll, matchIndex,
                scopeWindow.begin, scopeWindow.endExclusive, scopeStart, scopeEnd, fragmentOk);
    // Reaching here means searchContent was non-empty (required by targeting
    // resolution above), so no empty-check is needed.
    return executeByContent(fileLines, mode, content, contentLines, searchContent, count, replaceAll,
            matchIndex, scopeWindow.begin, scopeWindow.endExclusive, scopeStart, scopeEnd);
}

@("byContent + append anchors at the FIRST occurrence when identical section footers repeat")
unittest {
    // Real incident: a file with several sections each ending in an IDENTICAL
    // footer line; append with searchContent=<footer> must insert right after
    // the FIRST section's footer, never a later one. findCodeBlock scans
    // forward and returns the first full-block match, so the anchor is the
    // first occurrence.
    string[] fileLines = [
        "## Section 1", "body one", "footer", "", "## Section 2", "body two",
        "footer", "", "## Section 3", "body three", "footer",
    ];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "NEW", searchContent: "footer");
    assert(res.lines == [
        "## Section 1", "body one", "footer", "NEW", "", "## Section 2",
        "body two", "footer", "", "## Section 3", "body three", "footer",
    ], res.lines.to!string);
    // matched.matchedAt is the 1-based anchor line: the FIRST "footer".
    assert(res.matched.matchedAt == 3, res.matched.to!string);
}

@("byContent + insert_after/insert_before anchor on the matched block (single occurrence)")
unittest {
    string[] fileLines = ["a", "foo", "bar", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_after, "X",
            searchContent: "foo\nbar");
    assert(res.lines == ["a", "foo", "bar", "X", "b"], res.lines.to!string);
    // byContent matchedAt is the 1-based START of the matched block (the
    // block's first line here); for byMarker it is the 1-based anchor line.
    assert(res.matched.matchedAt == 2, res.matched.to!string);

    string[] fileLines2 = ["a", "foo", "bar", "b"];
    auto res2 = editFileUnifiedMemory(fileLines2, EditMode.insert_before, "X",
            searchContent: "foo\nbar");
    assert(res2.lines == ["a", "X", "foo", "bar", "b"], res2.lines.to!string);
    // insert_before anchors on the block's FIRST line.
    assert(res2.matched.matchedAt == 2, res2.matched.to!string);
}

@("byMarker + append anchors at the FIRST occurrence when the marker repeats")
unittest {
    string[] fileLines = ["## A", "end", "## B", "end", "## C"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "X", marker: "end");
    assert(res.lines == ["## A", "end", "X", "## B", "end", "## C"], res.lines.to!string);
    // matched.matchedAt is the 1-based anchor line: the FIRST "end".
    assert(res.matched.matchedAt == 2, res.matched.to!string);
}

@("byMarker + insert_after/insert_before anchor on the marker line (single occurrence)")
unittest {
    string[] fileLines = ["a", "m", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_after, "X", marker: "m");
    assert(res.lines == ["a", "m", "X", "b"], res.lines.to!string);
    // insert_after anchors on the marker line.
    assert(res.matched.matchedAt == 2, res.matched.to!string);

    string[] fileLines2 = ["a", "m", "b"];
    auto res2 = editFileUnifiedMemory(fileLines2, EditMode.insert_before, "X", marker: "m");
    assert(res2.lines == ["a", "X", "m", "b"], res2.lines.to!string);
    // insert_before anchors on the marker line.
    assert(res2.matched.matchedAt == 2, res2.matched.to!string);
}

@("insert modes with matchIndex=2 anchor at the SECOND occurrence")
unittest {
    string[] fileLines = ["## A", "end", "## B", "end", "## C"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "X", marker: "end", matchIndex: 2);
    assert(res.lines == ["## A", "end", "## B", "end", "X", "## C"], res.lines.to!string);
    assert(res.matched.matchedAt == 4, res.matched.to!string);

    string[] fileLines2 = [
        "## Section 1", "body", "footer", "", "## Section 2", "body", "footer",
    ];
    auto res2 = editFileUnifiedMemory(fileLines2, EditMode.append, "NEW",
            searchContent: "footer", matchIndex: 2);
    assert(res2.lines == [
        "## Section 1", "body", "footer", "", "## Section 2", "body", "footer",
        "NEW",
    ], res2.lines.to!string);
    assert(res2.matched.matchedAt == 7, res2.matched.to!string);
}

@("byContent remove/replace results are independent of a trailing newline in searchContent")
unittest {
    string[] fileLines = ["a", "foo", "bar", "b"];

    // Multi-line search block, WITH a trailing newline: removes the block.
    auto resWith = editFileUnifiedMemory(fileLines.dup, EditMode.remove, "",
            searchContent: "foo\nbar\n");
    assert(resWith.lines == ["a", "b"], resWith.lines.to!string);
    assert(resWith.matched.matchedAt == 2, resWith.matched.to!string);

    // The same block WITHOUT the trailing newline: identical result.
    auto resWithout = editFileUnifiedMemory(fileLines.dup, EditMode.remove, "",
            searchContent: "foo\nbar");
    assert(resWithout.lines == resWith.lines, resWithout.lines.to!string);
    assert(resWithout.matched.matchedAt == resWith.matched.matchedAt, resWithout.matched.to!string);

    // replace mode likewise, multi-line block.
    auto repWith = editFileUnifiedMemory(fileLines.dup, EditMode.replace, "X",
            searchContent: "foo\nbar\n");
    auto repWithout = editFileUnifiedMemory(fileLines.dup, EditMode.replace,
            "X", searchContent: "foo\nbar");
    assert(repWith.lines == ["a", "X", "b"], repWith.lines.to!string);
    assert(repWithout.lines == repWith.lines, repWithout.lines.to!string);
}

@("byContent single-line search block with/without trailing newline match identically")
unittest {
    string[] fileLines = ["a", "foo", "b"];

    auto repWith = editFileUnifiedMemory(fileLines.dup, EditMode.replace, "X",
            searchContent: "foo\n");
    auto repWithout = editFileUnifiedMemory(fileLines.dup, EditMode.replace,
            "X", searchContent: "foo");
    assert(repWith.lines == ["a", "X", "b"], repWith.lines.to!string);
    assert(repWithout.lines == repWith.lines, repWithout.lines.to!string);
    assert(repWith.matched.matchedAt == 2, repWith.matched.to!string);
    assert(repWithout.matched.matchedAt == 2, repWithout.matched.to!string);

    auto remWith = editFileUnifiedMemory(fileLines.dup, EditMode.remove, "",
            searchContent: "foo\n");
    auto remWithout = editFileUnifiedMemory(fileLines.dup, EditMode.remove, "",
            searchContent: "foo");
    assert(remWith.lines == ["a", "b"], remWith.lines.to!string);
    assert(remWithout.lines == remWith.lines, remWithout.lines.to!string);
    assert(remWith.matched.matchedAt == 2, remWith.matched.to!string);
    assert(remWithout.matched.matchedAt == 2, remWithout.matched.to!string);
}

@("byMarker multi-line marker with/without trailing newline match identically")
unittest {
    string[] fileLines = ["a", "foo", "bar", "b"];

    auto resWith = editFileUnifiedMemory(fileLines.dup, EditMode.remove, "", marker: "foo\nbar\n");
    auto resWithout = editFileUnifiedMemory(fileLines.dup, EditMode.remove,
            "", marker: "foo\nbar");
    assert(resWith.lines == ["a", "b"], resWith.lines.to!string);
    assert(resWithout.lines == resWith.lines, resWithout.lines.to!string);
    assert(resWithout.matched.matchedAt == 2, resWithout.matched.to!string);

    // replace mode with auto-count: pins the 2-vs-3 targetCount delta the old
    // un-chomped marker behavior produced (it consumed one extra file line).
    auto repWith = editFileUnifiedMemory(fileLines.dup, EditMode.replace,
            "X", marker: "foo\nbar\n");
    assert(repWith.autoCountUsed, repWith.note);
    assert(repWith.matched.matchedAt == 2, repWith.matched.to!string);
    auto repWithout = editFileUnifiedMemory(fileLines.dup, EditMode.replace,
            "X", marker: "foo\nbar");
    assert(repWithout.autoCountUsed, repWithout.note);
    assert(repWithout.lines == repWith.lines, repWithout.lines.to!string);
    assert(repWithout.matched.matchedAt == repWith.matched.matchedAt, repWithout.matched.to!string);
}
