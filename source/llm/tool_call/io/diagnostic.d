/// Diagnostic helpers for failed search operations: closest-match
/// suggestions and scope-aware error messages.
module llm.tool_call.io.diagnostic;

import std.algorithm : min;
import std.array : empty;
import std.conv : text;
import std.json : JSONValue;
import std.string : chomp, splitLines, strip;

/// Longest common substring between two strings (byte-level, sufficient for
/// diagnostics). Returns the best-matching substring of `a`, or null.
package string longestCommonSubstring(string a, string b) @safe {
    // Diagnostics only: cap inputs to keep the failure path fast on large files.
    if (a.length > 200)
        a = a[0 .. 200];
    if (b.length > 200)
        b = b[0 .. 200];
    if (a.length == 0 || b.length == 0)
        return null;
    auto prev = new size_t[b.length + 1];
    auto cur = new size_t[b.length + 1];
    size_t bestLen = 0;
    size_t bestEnd = 0; // end index (exclusive) in a
    foreach (i; 0 .. a.length) {
        cur[] = 0;
        foreach (j; 0 .. b.length) {
            if (a[i] == b[j]) {
                cur[j + 1] = prev[j] + 1;
                if (cur[j + 1] > bestLen) {
                    bestLen = cur[j + 1];
                    bestEnd = i + 1;
                }
            }
        }
        auto tmp = prev;
        prev = cur;
        cur = tmp;
    }
    if (bestLen == 0)
        return null;
    return a[bestEnd - bestLen .. bestEnd];
}

/// Human-readable description of an active scope for error messages, or ""
/// when no scope is in effect. 1-based inclusive line numbers.
///
/// Examples: "within scope [10, 20]", "within scope from line 10",
/// "within scope up to line 20".
package string scopeDescription(long scopeStart, long scopeEnd) @safe {
    const hasStart = scopeStart != -1;
    const hasEnd = scopeEnd != -1;
    if (hasStart && hasEnd)
        return i"within scope [$(scopeStart), $(scopeEnd)]".text;
    if (hasStart)
        return i"within scope from line $(scopeStart)".text;
    if (hasEnd)
        return i"within scope up to line $(scopeEnd)".text;
    return "";
}

/// Append `scopeDescription` to an error message, or return `msg` unchanged
/// when no scope is active (avoids a trailing space in the no-scope case).
package string appendScope(string msg, long scopeStart, long scopeEnd) @safe {
    const desc = scopeDescription(scopeStart, scopeEnd);
    return desc.empty ? msg : msg ~ " " ~ desc;
}

/// Closest-match diagnostic when a byMarker search fails.
///
/// When a scope is active (scopeStart/scopeEnd != -1, 1-based inclusive), the
/// search is limited to the scoped range, `searchedLines` reports the number
/// of lines actually searched, and the diagnostic includes a "scope" field
/// with the effective (clamped) searched range.
package JSONValue buildMarkerDiagnostic(string[] fileLines, string marker,
        long scopeStart = -1, long scopeEnd = -1) {
    auto diag = JSONValue.emptyObject;
    const hasScopeStart = scopeStart != -1;
    const hasScopeEnd = scopeEnd != -1;
    const scopeActive = hasScopeStart || hasScopeEnd;
    size_t begin = 0;
    size_t endExclusive = fileLines.length;
    if (scopeActive) {
        begin = hasScopeStart ? cast(size_t)(scopeStart - 1) : 0;
        endExclusive = hasScopeEnd ? cast(size_t) scopeEnd : fileLines.length;
        endExclusive = min(endExclusive, fileLines.length); // clamp to file end
        begin = min(begin, fileLines.length); // clamp: scope starts beyond EOF
        auto sc = JSONValue.emptyObject;
        sc["start"] = cast(long) begin + 1; // effective 1-based inclusive
        sc["end"] = cast(long) endExclusive; // effective 1-based inclusive
        diag["scope"] = sc;
    }
    diag["searchedLines"] = endExclusive > begin ? cast(long)(endExclusive - begin) : 0;
    long bestLine = -1;
    string bestSub;
    foreach (i; begin .. endExclusive) {
        const line = fileLines[i];
        auto sub = longestCommonSubstring(marker, line);
        if (sub !is null && sub.length > bestSub.length) {
            bestSub = sub;
            bestLine = cast(long) i + 1;
        }
    }
    if (bestLine > 0) {
        auto cm = JSONValue.emptyObject;
        cm["atLine"] = bestLine;
        cm["matchedText"] = bestSub;
        diag["closestMatch"] = cm;
    }
    return diag;
}

/// Length of the common prefix of two strings (byte-level).
package size_t commonPrefixLength(string a, string b) @safe {
    auto n = a.length < b.length ? a.length : b.length;
    size_t i = 0;
    while (i < n && a[i] == b[i])
        i++;
    return i;
}

/// Closest-match diagnostic when a byContent search fails.
///
/// Reports the search lines, the file line closest to the anchor (first
/// non-empty search line) and, where possible, the first mismatch detail.
///
/// When a scope is active (scopeStart/scopeEnd != -1, 1-based inclusive), the
/// anchor search is limited to the scoped range and the diagnostic includes a
/// "scope" field with the effective (clamped) searched range.
package JSONValue buildBlockDiagnostic(string[] fileLines, string searchContent,
        long scopeStart = -1, long scopeEnd = -1) {
    auto diag = JSONValue.emptyObject;
    auto searchLines = searchContent.chomp.splitLines;
    JSONValue[] searchJson;
    foreach (s; searchLines)
        searchJson ~= JSONValue(s);
    diag["searchLines"] = JSONValue(searchJson);

    const hasScopeStart = scopeStart != -1;
    const hasScopeEnd = scopeEnd != -1;
    const scopeActive = hasScopeStart || hasScopeEnd;
    size_t begin = 0;
    size_t endExclusive = fileLines.length;
    if (scopeActive) {
        begin = hasScopeStart ? cast(size_t)(scopeStart - 1) : 0;
        endExclusive = hasScopeEnd ? cast(size_t) scopeEnd : fileLines.length;
        endExclusive = min(endExclusive, fileLines.length); // clamp to file end
        begin = min(begin, fileLines.length); // clamp: scope starts beyond EOF
        auto sc = JSONValue.emptyObject;
        sc["start"] = cast(long) begin + 1; // effective 1-based inclusive
        sc["end"] = cast(long) endExclusive; // effective 1-based inclusive
        diag["scope"] = sc;
    }
    diag["searchedLines"] = endExclusive > begin ? cast(long)(endExclusive - begin) : 0;

    // Anchor is the first non-empty search line (same rule as findCodeBlock).
    size_t anchorIdx = 0;
    while (anchorIdx < searchLines.length && searchLines[anchorIdx].strip.length == 0)
        anchorIdx++;
    if (anchorIdx >= searchLines.length)
        return diag;
    const anchorStripped = searchLines[anchorIdx].strip;

    long bestAt = -1;
    size_t bestMatches = 0;
    size_t bestScore = 0;
    string[] bestMatchedLines;
    string bestMismatch;
    foreach (i; begin .. endExclusive) {
        const fileLine = fileLines[i];
        const score = commonPrefixLength(fileLine.strip, anchorStripped);
        if (score == 0)
            continue;
        size_t filePos = i + 1;
        size_t matchedCount = 0;
        string mismatch;
        if (score < anchorStripped.length) {
            mismatch = i"line $(anchorIdx + 1) (file line $(i + 1)): expected '$(anchorStripped)' but found '$(
                    fileLine.strip)'".text;
        }
        foreach (searchIdx; anchorIdx + 1 .. searchLines.length) {
            const ss = searchLines[searchIdx].strip;
            if (ss.length == 0)
                continue; // empty search lines are flexible
            if (filePos >= fileLines.length) {
                mismatch = i"line $(searchIdx + 1): expected '$(ss)' but reached end of file".text;
                break;
            }
            if (fileLines[filePos].strip != ss) {
                mismatch = i"line $(searchIdx + 1) (file line $(filePos + 1)): expected '$(ss)' but found '$(
                        fileLines[filePos])'".text;
                break;
            }
            filePos++;
            matchedCount++;
        }
        if (matchedCount > bestMatches || (matchedCount == bestMatches && score > bestScore)) {
            bestMatches = matchedCount;
            bestScore = score;
            bestAt = cast(long) i + 1;
            bestMatchedLines = fileLines[i .. filePos].dup;
            bestMismatch = mismatch;
        }
    }
    if (bestAt > 0) {
        auto cm = JSONValue.emptyObject;
        cm["atLine"] = bestAt;
        JSONValue[] mj;
        foreach (l; bestMatchedLines)
            mj ~= JSONValue(l);
        cm["matchedLines"] = JSONValue(mj);
        if (!bestMismatch.empty)
            cm["mismatchAt"] = bestMismatch;
        diag["closestMatch"] = cm;
    }
    return diag;
}
