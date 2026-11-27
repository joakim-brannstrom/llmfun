/// Generate unified diffs for reporting edits.
module llm.tool_call.io.diff;

import std.algorithm : min;
import std.array : join;
import std.conv : to;

/// One changed region between the pre-edit and post-edit lines: half-open
/// [oldStart, oldEnd) in the old lines and [newStart, newEnd) in the new
/// lines (absolute indices).
private struct DiffRegion {
    size_t oldStart;
    size_t oldEnd;
    size_t newStart;
    size_t newEnd;
}

/// Collect the changed regions between the two line arrays: trim the common
/// prefix/suffix, then recursively split the remaining middles at their
/// longest common run of consecutive lines, but only when that run is longer
/// than twice the context width (so separated changes stay in separate hunks).
private DiffRegion[] diffRegions(const(string[]) oldLines, const(string[]) newLines, size_t context) @safe {
    DiffRegion[] regions;
    collectRegions(oldLines, newLines, 0, oldLines.length, 0, newLines.length, context, regions);
    return regions;
}

private void collectRegions(const(string[]) oldLines, const(string[]) newLines,
        size_t loOld, size_t hiOld, size_t loNew, size_t hiNew, size_t context,
        ref DiffRegion[] regions) @safe {
    // Trim the common prefix.
    while (loOld < hiOld && loNew < hiNew && oldLines[loOld] == newLines[loNew]) {
        loOld++;
        loNew++;
    }
    // Trim the common suffix.
    while (hiOld > loOld && hiNew > loNew && oldLines[hiOld - 1] == newLines[hiNew - 1]) {
        hiOld--;
        hiNew--;
    }
    if (loOld == hiOld && loNew == hiNew)
        return; // identical
    if (loOld == hiOld || loNew == hiNew) {
        // Pure insertion or deletion, anchored at the current position.
        regions ~= DiffRegion(loOld, hiOld, loNew, hiNew);
        return;
    }

    // Find the longest common run of consecutive lines in the middles
    // (ties broken toward the first occurrence).
    const n1 = hiOld - loOld;
    const n2 = hiNew - loNew;
    // The DP is O(n1*n2) time over the changed middles (two rows, O(n2)
    // memory); realistic agent edits keep the middles small.
    size_t bestLen = 0;
    size_t bestOld = 0;
    size_t bestNew = 0;
    auto prev = new size_t[](n2 + 1);
    auto curr = new size_t[](n2 + 1);
    foreach (i; 1 .. n1 + 1) {
        foreach (j; 1 .. n2 + 1) {
            curr[j] = oldLines[loOld + i - 1] == newLines[loNew + j - 1] ? prev[j - 1] + 1 : 0;
            if (curr[j] > bestLen) {
                bestLen = curr[j];
                bestOld = i - bestLen;
                bestNew = j - bestLen;
            }
        }
        auto swap = prev;
        prev = curr;
        curr = swap;
    }

    // Split at the run only when it exceeds twice the context width;
    // otherwise one region covers both sides and emission keeps them in a
    // single hunk.
    if (bestLen > 2 * context) {
        collectRegions(oldLines, newLines, loOld, loOld + bestOld, loNew,
                loNew + bestNew, context, regions);
        collectRegions(oldLines, newLines, loOld + bestOld + bestLen, hiOld,
                loNew + bestNew + bestLen, hiNew, context, regions);
        return;
    }

    regions ~= DiffRegion(loOld, hiOld, loNew, hiNew);
}

/// Generate a unified diff with standard @@ headers and context/-/+ lines.
/// The output is deterministic for identical inputs.
package string unifiedDiffLines(const(string[]) oldLines, const(string[]) newLines,
        size_t context = 3) @safe {
    import std.algorithm : max;

    auto regions = diffRegions(oldLines, newLines, context);
    string[] outLines;
    size_t oldPos = 0; // 0-based index of the next old line to emit
    foreach (r; regions) {
        const leadStart = cast(size_t) max(cast(long) oldPos,
                cast(long) r.oldStart - cast(long) context, 0L);
        const trailEnd = min(oldLines.length, r.oldEnd + context);
        // Safe size_t subtraction: the non-overlap invariant guarantees
        // leadStart <= trailEnd, so oldCount never underflows.
        const oldCount = trailEnd - leadStart;
        const shift = cast(long) r.newStart - cast(long) r.oldStart;
        const newStartIdx = cast(long) leadStart + shift;
        const adds = cast(long)(r.newEnd - r.newStart);
        const dels = cast(long)(r.oldEnd - r.oldStart);
        const newCount = cast(long) oldCount + adds - dels;

        // 1-based @@ headers; the count is omitted only when it is 1. A zero
        // count keeps the start at 0 (standard empty-range form).
        const oldStartDisp = oldCount == 0 ? leadStart : leadStart + 1;
        const newStartDisp = newCount == 0 ? newStartIdx : newStartIdx + 1;
        outLines ~= "@@ -" ~ (oldCount == 1
                ? to!string(oldStartDisp) : to!string(oldStartDisp) ~ "," ~ to!string(oldCount)) ~ " +" ~ (newCount == 1
                ? to!string(newStartDisp) : to!string(newStartDisp) ~ "," ~ to!string(newCount))
            ~ " @@";
        foreach (i; leadStart .. r.oldStart)
            outLines ~= " " ~ oldLines[i];
        foreach (i; r.oldStart .. r.oldEnd)
            outLines ~= "-" ~ oldLines[i];
        foreach (i; r.newStart .. r.newEnd)
            outLines ~= "+" ~ newLines[i];
        foreach (i; r.oldEnd .. trailEnd)
            outLines ~= " " ~ oldLines[i];
        oldPos = trailEnd;
    }
    return outLines.join("\n");
}
