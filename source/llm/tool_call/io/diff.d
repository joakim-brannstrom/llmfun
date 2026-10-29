module llm.tool_call.io.diff;

import logger = std.logger;
import std.algorithm : map, filter, startsWith, count, joiner, endsWith, splitter, canFind, min;
import std.array : empty, appender, array, join;
import std.conv : to, text;
import std.exception : enforce;
import std.file : readText, exists, mkdirRecurse, getSize, remove, dirEntries, SpanMode;
import std.json : JSONValue, parseJSON, JSONOptions, JSONType;
import std.path : relativePath;
import std.process : execute;
import std.range : enumerate;
import std.regex : Regex, regex;
import std.stdio : File;
import std.string : splitLines, indexOf, strip, split, replace, toLower, chomp;
import std.sumtype : match;

import my.path : AbsolutePath;

import llm.tool_call;
import llm.tool_call.utility;
import llm.config : ToolLimits;
import llm.types : IAgent;
import llm.tool_call.io : classifyEditError, FileContext, sanitizeLogPath,
    fileEndsWithNewline, writeLines;

mixin RegisterLlmFunctions!();

/// Extract the 1-based hunk number from an applyDiff error message prefixed
/// with "Hunk N: ", or -1 when the message carries no hunk number.
long hunkNumberFrom(string msg) @safe {
    if (!msg.startsWith("Hunk "))
        return -1;
    auto rest = msg[5 .. $];
    auto colon = rest.indexOf(":");
    if (colon <= 0)
        return -1;
    try {
        const n = rest[0 .. colon].to!long;
        return n >= 1 ? n : -1;
    } catch (Exception) {
        return -1;
    }
}

struct ApplyDiffParams {
    @ParamDescription("Path to the file (relative to workarea)")
    string path;

    @ParamDescription("Unified diff patch to apply")
    string diff;

    @ParamDescription("Preview the result without writing to disk")
    @ParamOptional bool dryRun;

    @ParamDescription(
            "Fuzzy context matching: ignore leading/trailing whitespace differences in context lines (default: true)")
    @ParamOptional bool fuzzy = true;
}

@Function("Apply a unified diff patch to a file. " ~ "Diff: each hunk starts with `@@ -oldStart[,oldCount] +newStart[,newCount] @@`, followed by lines starting " ~ "with ' ' (context), '-' (remove) or '+' (add). Context lines must match the current content of the file; use `read_file` first to obtain it. Context matching is fuzzy by default (leading/trailing whitespace differences are ignored); pass fuzzy=false for exact matching. Hunk header counts are advisory: the actual body lines determine what is applied; mismatches produce warnings, not errors. " ~ "Returns a JSON object with fields: ok (bool), linesChanged (int), hunksApplied (int), warnings (array of strings). When dryRun is true, also includes: preview (string).") ExecuteFuncResult applyDiff(
        Context baseCtx, ApplyDiffParams params) {
    mixin(baseContextToSpecific!FileContext);

    const logPath = sanitizeLogPath(params.path);
    const dryRun = params.dryRun ? "true" : "false";

    auto path_ = pathToWorkarea(ctx, params.path, checkExist: true);
    if (!path_.valid) {
        logger.warningf("tool=applyDiff event=failure path=%s errorType=invalid_path dryRun=%s",
                logPath, dryRun);
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }
    if (params.diff.empty) {
        logger.warningf("tool=applyDiff event=failure path=%s errorType=invalid_parameter dryRun=%s",
                logPath, dryRun);
        return ExecuteFuncResult("error: diff must not be empty", success: false);
    }
    try {
        auto fileLines = File(path_.toString).byLineCopy.array;
        auto diffLines = params.diff.splitLines.filter!(a => !a.empty).array;
        auto result = applyDiffMemory(fileLines, diffLines, params.fuzzy);

        auto json = JSONValue.emptyObject;
        json["ok"] = true;
        json["hunksApplied"] = cast(long) result.hunksApplied;
        json["linesChanged"] = cast(long) result.lines.length - cast(long) fileLines.length;
        JSONValue[] warnJson;
        foreach (w; result.warnings)
            warnJson ~= JSONValue(w);
        json["warnings"] = JSONValue(warnJson);

        const trailingNewline = fileEndsWithNewline(path_.toString);
        if (params.dryRun) {
            auto preview = result.lines.join("\n");
            if (trailingNewline && result.lines.length > 0)
                preview ~= "\n";
            json["preview"] = preview;
        } else {
            writeLines(path_, result.lines, !trailingNewline);
        }

        logger.tracef(
                "tool=applyDiff event=success path=%s hunksApplied=%d linesChanged=%d fuzzy=%s warnings=%d dryRun=%s",
                logPath, cast(long) result.hunksApplied,
                cast(long) result.lines.length - cast(long) fileLines.length, params.fuzzy
                ? "true" : "false", cast(long) result.warnings.length, dryRun);
        return ExecuteFuncResult(json.toString(JSONOptions.doNotEscapeSlashes), success: true);
    } catch (Exception e) {
        const hunkNum = hunkNumberFrom(e.msg);
        logger.warningf("tool=applyDiff event=failure path=%s errorType=%s hunk=%d dryRun=%s",
                logPath, classifyEditError(e.msg), hunkNum, dryRun);
        return ExecuteFuncResult(i"error: failed applying diff to file '$(params.path)': $(e.msg)".text,
                success: false);
    }
}

/// Result of applying a unified diff to in-memory file lines.
struct ApplyDiffResult {
    /// Resulting file lines after applying the diff.
    string[] lines;
    /// Hunk count mismatch warnings (hunk header counts are advisory).
    string[] warnings;
    /// Number of hunks successfully applied.
    size_t hunksApplied;
}

ApplyDiffResult applyDiffMemory(string[] fileLines, string[] diffLines, bool fuzzy = true) @safe {
    size_t fileIdx = 0; // current position in fileLines (0‑based)
    string[] result;
    string[] warnings; // hunk count mismatch warnings (header counts are advisory)
    size_t hunksApplied = 0; // number of hunks successfully applied
    size_t lineIdx = 0;
    // 1. Skip leading --- / +++ headers
    while (lineIdx < diffLines.length && (diffLines[lineIdx].startsWith("---")
            || diffLines[lineIdx].startsWith("+++"))) {
        lineIdx++;
    }

    // 2. Must start with a hunk header after headers
    if (lineIdx >= diffLines.length || !diffLines[lineIdx].startsWith("@@"))
        throw new Exception("Diff does not contain any hunk header (@@ ... @@)");
    size_t hunkNum = 0; // 1-based hunk number for diagnostics

    while (lineIdx < diffLines.length) {
        hunkNum++;
        auto line = diffLines[lineIdx];

        // Parse header: "@@ -oldStart[,oldCount] +newStart[,newCount] @@"
        auto secondAt = line.indexOf("@@", 2);
        enforce(secondAt != -1, i"Hunk $(hunkNum): Invalid hunk header (missing closing @@): $(line)"
                .text);
        auto header = line[2 .. secondAt].strip;
        auto parts = header.split;
        enforce(parts.length >= 2, i"Hunk $(hunkNum): Invalid hunk header format: $(line)".text);

        // Old range
        auto oldRange = parts[0];
        enforce(oldRange.startsWith("-"), i"Hunk $(hunkNum): Old range must start with '-': $(line)"
                .text);
        oldRange = oldRange[1 .. $];
        long oldStart;
        size_t oldCount;
        if (oldRange.indexOf(',') != -1) {
            auto rp = oldRange.split(",");
            oldStart = rp[0].to!long;
            oldCount = rp[1].to!size_t;
        } else {
            oldStart = oldRange.to!long;
            oldCount = 1;
        }
        size_t oldPos = cast(size_t)(oldStart - 1); // 0‑based index in fileLines

        // New range
        auto newRange = parts[1];
        enforce(newRange.startsWith("+"), i"Hunk $(hunkNum): New range must start with '+': $(line)"
                .text);
        newRange = newRange[1 .. $];
        long newStart;
        size_t newCount;
        if (newRange.indexOf(',') != -1) {
            auto rp = newRange.split(",");
            newStart = rp[0].to!long;
            newCount = rp[1].to!size_t;
        } else {
            newStart = newRange.to!long;
            newCount = 1;
        }
        if (oldPos < fileIdx)
            throw new Exception(i"Hunk $(hunkNum): Hunk tries to go backward (oldStart=$(oldStart), current file position=$(
                    fileIdx + 1))".text);

        // Copy lines from current position up to the start of this hunk
        while (fileIdx < oldPos) {
            if (fileIdx >= fileLines.length) {
                logger.tracef("Unexpected error. fileIdx:%s fileLines:%s\nfileLines:%s\ndiffLines:%s",
                        fileIdx, fileLines.length, fileLines, diffLines);
                throw new Exception("Unexpected error. Unable to apply diff");
            }
            result ~= fileLines[fileIdx];
            fileIdx++;
        }

        lineIdx++; // consume the '@@' line
        size_t processedOld = 0; // count of '-' and ' ' lines in this hunk
        size_t processedNew = 0; // count of '+' and ' ' lines in this hunk

        // Process hunk body lines
        while (lineIdx < diffLines.length && !diffLines[lineIdx].startsWith("@@")
                && !diffLines[lineIdx].startsWith("---") && !diffLines[lineIdx].startsWith("+++")) {

            auto hunkLine = diffLines[lineIdx];
            if (hunkLine.empty) {
                lineIdx++;
                continue;
            }

            auto firstChar = hunkLine[0];
            auto content = hunkLine[1 .. $];

            switch (firstChar) {
            case ' ': // context line - present in both old and new
                enforce(fileIdx < fileLines.length, i"Hunk $(hunkNum): Unexpected end of file at line $(
                        fileIdx + 1) (hunk context)".text);
                if (fuzzy) {
                    // Fuzzy matching: ignore leading/trailing whitespace differences.
                    // Only the matching is fuzzy — what gets written is always the
                    // actual file line (or '+' lines from the diff), never altered.
                    enforce(fileLines[fileIdx].strip == content.strip,
                            i"Hunk $(hunkNum): Context mismatch at line $(fileIdx + 1): expected '$(
                                content)' but found '$(fileLines[fileIdx])' (fuzzy matching)".text);
                } else {
                    enforce(fileLines[fileIdx] == content,
                            i"Hunk $(hunkNum): Context mismatch at line $(fileIdx + 1): expected '$(
                                content)' but found '$(fileLines[fileIdx])'".text);
                }
                result ~= fileLines[fileIdx];
                fileIdx++;
                processedOld++;
                processedNew++;
                break;
            case '-': // removal line (consumed from old file, not added to result)
                enforce(fileIdx < fileLines.length, i"Hunk $(hunkNum): Unexpected end of file at line $(
                        fileIdx + 1) (hunk removal)".text);
                fileIdx++;
                processedOld++;
                break;
            case '+': // addition line (goes into result, does not consume old file line)
                result ~= content;
                processedNew++;
                break;
            default:
                throw new Exception(i"Hunk $(hunkNum): Invalid hunk line (must start with ' ', '-' or '+'): $(
                        hunkLine)".text);
            }
            lineIdx++;
        }
        // Hunk header counts are advisory: if declared counts differ from the
        // actual body lines, emit a warning and continue with the body counts.
        if (processedOld != oldCount) {
            auto warn = i"Hunk $(hunkNum): declared $(oldCount) old lines, body has $(processedOld) — using body count"
                .text;
            warnings ~= warn;
            logger.warningf("%s", warn);
        }
        if (processedNew != newCount) {
            auto warn = i"Hunk $(hunkNum): declared $(newCount) new lines, body has $(processedNew) — using body count"
                .text;
            warnings ~= warn;
            logger.warningf("%s", warn);
        }
        hunksApplied++;
    }

    // Append remaining file lines after the last hunk
    while (fileIdx < fileLines.length) {
        result ~= fileLines[fileIdx];
        fileIdx++;
    }

    return ApplyDiffResult(result, warnings, hunksApplied);
}

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

/// Generate a unified diff (standard @@ headers, context/-/+ lines) between
/// the pre-edit and post-edit lines. Pure and deterministic. The output
/// round-trips through applyDiffMemory for non-empty old files (it parses
/// standard unified diffs); a `@@ -0,0` new-file hunk is NOT accepted by
/// applyDiffMemory, which requires at least one pre-edit line. The generator
/// itself is NOT wired into applyDiff — reuse is via the round-trip, not
/// code sharing.
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
