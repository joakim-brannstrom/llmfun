/// In-memory line-range edit primitives shared by the editing tools.
/// Operates on resolved EditTarget ranges only; no targeting awareness.
module llm.tool_call.io.edit_engine;

import std.array : appender;
import std.conv : text;
import std.exception : enforce;
import std.string : chomp, splitLines;

import llm.tool_call.io : EditMode;

/**
 * Resolved edit target: an arbitrary line range within a file.
 *
 * Fields:
 *     startLine: 0-based start index in fileLines (inclusive)
 *     endLine:   0-based end index in fileLines (exclusive)
 *     matchedAt: 1-based line number of the match, for the return JSON
 *     matchedLines: number of lines matched/targeted by the resolver
 */
package struct EditTarget {
    /// 0-based start index in fileLines (inclusive)
    long startLine;
    /// 0-based end index in fileLines (exclusive)
    long endLine;
    /// 1-based line number of the match, for the return JSON
    long matchedAt;
    /// number of lines matched/targeted by the resolver
    long matchedLines;
}

/**
 * Result of applying an in-memory edit.
 *
 * Fields:
 *     lines: resulting file lines after the edit
 *     matched: the resolved target, echoed for the return JSON
 *     linesChanged: net change in line count (added - removed)
 */
package struct EditResult {
    /// Resulting file lines after the edit
    string[] lines;
    /// The resolved target, echoed for the return JSON
    EditTarget matched;
    /// Net change in line count (added - removed)
    long linesChanged;
}

/**
 * Apply an edit mode to an arbitrary line range in memory.
 *
 * This is the single internal edit function used by the unified editFile
 * tool. It replaces editFileMemory, editFileByMarkerMemory,
 * searchAndReplaceMemory and searchAndReplaceAllMemory.
 *
 * Mode behavior:
 *     replace:       Replace lines [startLine, endLine) with the content lines
 *     remove:        Delete lines [startLine, endLine); content must be empty
 *     append:        Keep the line at startLine, insert content after it
 *     insert_after:  Alias for append (same behavior)
 *     insert_before: Keep the line at startLine, insert content before it
 *
 * Content is split into lines and preserved exactly as provided: no
 * indentation or whitespace is added (fixes the old editFileMemory quirk
 * where the whole content was inserted as a single line).
 *
 * Content semantics:
 *     - Lines are split on '\n' and '\r\n' terminators; the terminator is
 *       not preserved (e.g. "a\nb\n" yields ["a", "b"]).
 *     - A single trailing newline is not preserved as an extra empty line.
 *     - Content consisting only of newlines (e.g. "\n" or "\r\n") yields ONE
 *       blank line, never zero lines -- replace with such content inserts a
 *       blank line instead of silently deleting the target range.
 *
 * Parameters:
 *     fileLines: Array of file lines to edit
 *     mode:      Edit mode determining how the edit is applied
 *     content:   Content to insert (split into lines)
 *     target:    Line range to edit; for insert modes only startLine is used
 *
 * Returns:
 *     EditResult with the resulting lines, the echoed target, and the net
 *     line count change (contentLineCount - removedLineCount).
 *
 * Throws:
 *     Exception on invalid target ranges or remove mode with non-empty content.
 */
package EditResult editFileInMemory(string[] fileLines, EditMode mode,
        string content, EditTarget target) @safe {
    enforce(target.startLine >= 0, "startLine must be >= 0");

    // Split content into lines; chomp prevents a trailing empty element when
    // content ends with a newline (e.g. "line1\nline2\n" -> ["line1","line2"]).
    // Non-empty content that splits to nothing (e.g. "\n" or "\r\n") means ONE
    // blank line, not "no content" - otherwise replace mode would silently
    // delete the target range instead of inserting a blank line.
    auto contentLines = editContentLines(content);
    const contentLineCount = cast(long) contentLines.length;

    auto lines = appender!(string[])();
    long linesChanged;

    final switch (mode) {
    case EditMode.replace:
        enforce(target.endLine >= target.startLine,
                "endLine must be >= startLine");
        enforce(target.endLine <= cast(long) fileLines.length,
                i"endLine $(target.endLine) exceeds file length $(fileLines.length)".text);
        lines.put(fileLines[0 .. target.startLine]);
        lines.put(contentLines);
        lines.put(fileLines[target.endLine .. $]);
        linesChanged = contentLineCount - (target.endLine - target.startLine);
        break;
    case EditMode.remove:
        enforce(content.length == 0, "remove mode requires empty content");
        enforce(target.endLine >= target.startLine, "endLine must be >= startLine");
        enforce(target.endLine <= cast(long) fileLines.length,
                i"endLine $(target.endLine) exceeds file length $(fileLines.length)".text);
        lines.put(fileLines[0 .. target.startLine]);
        lines.put(fileLines[target.endLine .. $]);
        linesChanged = contentLineCount - (target.endLine - target.startLine);
        break;
    case EditMode.append:
    case EditMode.insert_after:
        enforce(target.startLine <= cast(long) fileLines.length,
                i"startLine $(target.startLine) exceeds file length $(fileLines.length)".text);
        // Insert after the anchor line; at the end of the file (or on an
        // empty file) the insertion point is clamped to the file length.
        auto insertIdx = target.startLine + 1 < cast(long) fileLines.length
            ? target.startLine + 1 : cast(long) fileLines.length;
        lines.put(fileLines[0 .. insertIdx]);
        lines.put(contentLines);
        lines.put(fileLines[insertIdx .. $]);
        linesChanged = contentLineCount;
        break;
    case EditMode.insert_before:
        enforce(target.startLine <= cast(long) fileLines.length,
                i"startLine $(target.startLine) exceeds file length $(fileLines.length)".text);
        lines.put(fileLines[0 .. target.startLine]);
        lines.put(contentLines);
        lines.put(fileLines[target.startLine .. $]);
        linesChanged = contentLineCount;
        break;
    }

    return EditResult(lines[], target, linesChanged);
}

/// Split content into lines exactly like editFileInMemory does.
package string[] editContentLines(string content) @safe {
    auto contentLines = content.chomp.splitLines;
    if (contentLines.length == 0 && content.length > 0)
        contentLines = [""];
    return contentLines;
}

/// True when the composed result is element-wise (sequence) identical to
/// the input lines. Deliberately NOT a net-line-count check: replacing 2
/// lines with 2 DIFFERENT lines has a zero delta (linesChanged == 0) but is
/// a real change, so linesChanged can never decide this.
package bool isNoOpEdit(const(string[]) oldLines, const(string[]) newLines) @safe {
    return oldLines == newLines;
}

/// Outcome of an in-memory unified edit.
package struct EditFileOutcome {
    /// Resulting file lines after the edit
    string[] lines;
    /// Resolved target of the first operation (echo for the return JSON)
    EditTarget matched;
    /// Net change in line count (added - removed)
    long linesChanged;
    /// Number of edit operations performed (>1 only with replaceAll)
    long operations;
    /// True when count was auto-derived (multi-line byMarker: marker line count;
    /// byContent: matched block size)
    bool autoCountUsed;
    /// Human-readable note describing the auto-count derivation (empty when not auto-derived)
    string note;
    /// Count of matched lines strictly longer than their marker lines
    /// (SUMMED across occurrences for replaceAll; 0 when absent/not byMarker)
    long fragmentLinesReplaced = 0;
    /// The exact pre-edit target lines consumed by replace/remove modes.
    /// Empty for insert modes and the no-targeting append-at-EOF shape.
    string[] replacedLines;
    /// Total consumed line count (equals replacedLines.length before any elision;
    /// elision is a render concern in edit_tool, so the full lines travel intact)
    long replacedTotal = 0;
    /// True when the composed result is element-wise identical to the input
    /// lines (an exact no-op). Never derived from linesChanged (net delta):
    /// replacing N lines with N different lines has a zero delta but is a
    /// real change. Set only for replace/remove modes; insert modes with
    /// empty content are excluded (nothing is consumed, so there is no
    /// old-vs-new text pair to compare).
    bool noOp = false;
    /// True when the content's first (insert_after) or last (insert_before)
    /// line duplicates the anchored line/block (trimmed equality on both
    /// sides, matching the targeting semantics). Warn only: the edit itself
    /// is never mutated (no magic stripping of the duplicated line).
    bool anchorDuplicate = false;
}
