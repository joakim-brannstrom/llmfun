/// Public editFile tool: the parameter struct, logging helpers, and the tool
/// entry point that dispatches to the unified in-memory edit engine.
module llm.tool_call.io.edit_tool;

import logger = std.logger;
import std.algorithm : canFind;
import std.array : array, empty, join;
import std.conv : to, text;
import std.json : JSONValue, JSONOptions;
import std.stdio : File;

import llm.tool_call;
import llm.tool_call.utility;
import llm.tool_call.io : classifyEditError, FileContext, sanitizeLogPath,
    fileEndsWithNewline, writeLines, EditMode, parseEditMode;
import llm.tool_call.io.diagnostic : buildMarkerDiagnostic, buildBlockDiagnostic;
import llm.tool_call.io.edit_dispatch : editFileUnifiedMemory;
import llm.tool_call.io.diff : unifiedDiffLines;
import llm.utility : computeContentHash;
import llm.config : PathCategory;

mixin RegisterLlmFunctions!();

/// Parameters for the unified editFile tool.
///
/// Exactly one targeting method must be provided: startLine (byLine),
/// marker (byMarker), or searchContent (byContent). The exception:
/// mode=append with no targeting appends at the end of the file.
struct UnifiedEditFileParams {
    @ParamDescription("Path to the file (relative to workarea)")
    string path;

    @ParamDescription("Content to insert or replace with; must be empty string for remove mode")
    string content;

    @ParamDescription(`Edit mode: "replace", "remove", "append", "insert_before", or "insert_after" (alias for append). Insert modes: the targeted block/marker line is KEPT and the content is added adjacent to it — do not repeat the targeted text in content. Insert modes anchor at the FIRST occurrence of the targeted block/marker; when the search repeats (e.g. identical per-section footers) placement is ambiguous: use a unique end marker per section, or append without targeting (no targeting appends at end of file)`)
    string mode;

    @ParamDescription(
            "byLine targeting: first line of the range (1-based, must be >= 1; omitted by default)")
    @ParamOptional long startLine = -1;

    @ParamDescription("Number of lines to replace/remove from the target. byLine: required for replace/remove. byMarker: a single-line marker with multi-line replace content requires an explicit count (pass count=N to replace N lines starting at the marker); multi-line markers auto-derive the target from the marker line count; remove with a single-line marker auto-derives to 1. byContent: auto-derives from the matched block size; not supported with replaceAll. Insert modes ignore count.")
    @ParamOptional long count;

    @ParamDescription("byMarker targeting: case-sensitive substring to find, matched per line (line-granular: each line containing the marker is a target line, and the marker text itself is not preserved unless the content re-includes it). Multi-line markers (up to 20 lines) are also supported: the first line is an anchor and subsequent lines are verified as substrings of consecutive file lines. A marker matching as a strict substring inside a longer line replaces the WHOLE line, destroying the text outside the match, unless fragmentOk is set (replace/byMarker only)")
    @ParamOptional string marker;

    @ParamDescription("byContent targeting: code block to find using whole-line trimmed equality matching (multi-line allowed; a substring of a longer line never matches). Replace swaps the matched lines for the content lines, so the content must carry the full replacement text of any lines to keep. append/insert_after insert the content after the matched block; insert_before inserts before it. Insert modes anchor at the FIRST occurrence when the block repeats (see mode)")
    @ParamOptional string searchContent;

    @ParamDescription("Replace every line containing the marker / every matched search block with the content lines (line-granular, not fragment-level). byContent and byMarker only; not supported with byLine")
    @ParamOptional bool replaceAll;

    @ParamDescription("Preview the result without writing to disk")
    @ParamOptional bool dryRun;

    @ParamDescription("Which occurrence to target (1-based, default 1; values < 1 are invalid). Only byMarker/byContent; ignored by byLine; cannot be combined with replaceAll.")
    @ParamOptional long matchIndex = 1;

    @ParamDescription("Scope limiting: restrict the byMarker/byContent search to the 1-based inclusive line range [scopeStart, scopeEnd]. Either or both may be provided: scopeStart alone searches from that line to the end of file; scopeEnd alone searches from line 1 to that line. The first line of a match (anchor) must be inside the range; the match may extend past scopeEnd. Ignored by byLine targeting (byLine does not search). scopeStart must be >= 1; if both are provided, scopeStart must be <= scopeEnd.")
    @ParamOptional long scopeStart = -1;

    @ParamDescription("Scope limiting: end of the search range. See scopeStart.")
    @ParamOptional long scopeEnd = -1;

    @ParamDescription("byLine guard: verify that the target lines match this content before editing. When non-empty, the lines at [startLine, startLine+count) are compared (trimmed equality) against verifyContent. Mismatch throws an error with the actual content found, preventing silent corruption from stale line numbers after successive edits. Only valid with byLine targeting.")
    @ParamOptional string verifyContent;

    @ParamDescription("byMarker replace/replaceAll only: acknowledge that markers matching as strict substrings inside longer lines replace those WHOLE lines (the text outside the marker match is destroyed). Default false: the edit is refused when any matched line is strictly longer than its marker line. Providing fragmentOk with startLine (byLine), searchContent (byContent), or with remove/insert modes is an error.")
    @ParamOptional bool fragmentOk;

    @ParamDescription(
            "Include a unified diff of the edit in the result (pre-edit vs post-edit lines)")
    @ParamOptional bool includeDiff;
}

/// Resolve the targeting method of the unified editFile params for logging.
/// Returns "byLine", "byMarker", "byContent", "none" (absent/ambiguous), or
/// "append_eof" (mode=append with no targeting - a valid call shape).
package string targetingMethodOf(UnifiedEditFileParams params) @safe {
    const provided = (params.startLine != -1 ? 1 : 0) + (params.marker.length > 0
            ? 1 : 0) + (params.searchContent.length > 0 ? 1 : 0);
    if (provided != 1) {
        if (provided == 0) {
            try {
                if (parseEditMode(params.mode) == EditMode.append) {
                    // TODO: returning magic strings that callers catch and
                    // match is bad design.
                    return "append_eof";
                }
            } catch (Exception) {
                // Invalid mode: reported as "none" here; parseEditMode's own
                // error surfaces later in editFile.
            }
        }
        return "none";
    }
    if (params.startLine != -1)
        return "byLine";
    if (params.marker.length > 0)
        return "byMarker";
    return "byContent";
}

/// Compact scope representation for log output: "10-20", "from-10",
/// "up-to-20", or "none".
package string scopeLogValue(long scopeStart, long scopeEnd) @safe {
    const hasStart = scopeStart != -1;
    const hasEnd = scopeEnd != -1;
    if (hasStart && hasEnd)
        return i"$(scopeStart)-$(scopeEnd)".text;
    if (hasStart)
        return i"from-$(scopeStart)".text;
    if (hasEnd)
        return i"up-to-$(scopeEnd)".text;
    return "none";
}

/// The byLine stale-target error: one message for both the
/// dirty-path case (a successful edit earlier in this session) and the
/// best-effort mismatch case (changed on disk since the last readFile).
package string staleBylineTargetError(string path) @safe {
    return i"Path '$(path)' was modified by a successful edit earlier in this session; line numbers may be stale — re-read it with readFile, then retry with fresh startLine/count, or switch to byMarker/byContent targeting."
        .text;
}

@Function("Edit a file by applying a change to a target. " ~ "Target the file with exactly one method: startLine+count (byLine, 1-based), " ~ "marker (byMarker, substring to find), or searchContent (byContent, code block matched by trimmed equality). " ~ "The file's trailing-newline state is preserved, and the dryRun preview matches the exact bytes that would be written. " ~ "Returns a JSON object with fields: ok (bool), matchedAt (1-based line; for a mode=append call with no targeting, the line where the new content starts, which is one past the last line for a non-empty file), matchedLines (int), linesChanged (int, signed: new total line count minus old total line count), operations (int). " ~ "When a fragment marker replaced whole line(s) with fragmentOk, also includes: fragmentLinesReplaced (int). " ~ "Replace/remove edits that consume lines also include: replaced (the consumed lines; elided when more than 10: first 5 + last 5 + an ellipsis marker), replacedTotal (int), replacedTruncated (bool). " ~ "When the edit produces no change (new content identical to the replaced text), also includes: noOp (true) and note. " ~ "When an insert-mode edit (insert_after, its alias append, or insert_before) has a content boundary line duplicating the kept anchor line/block, also includes: anchorDuplicate (true) and note. " ~ "When includeDiff is true, also includes: diff (string, the unified diff between the pre-edit and post-edit lines). " ~ "When auto-count is used, also includes: autoCountUsed (true) and note (string). " ~ "When dryRun is true, also includes: preview (string). " ~ "On failure returns JSON with ok=false, an error string, and a diagnostic field with closest-match details. " ~ "byLine edits on a path modified by a successful edit earlier in this session are rejected until the file is re-read with read_file; files modified by other processes between turns are caught only by a best-effort content-hash cross-check.") ExecuteFuncResult editFile(
        Context baseCtx, UnifiedEditFileParams params) {
    mixin(baseContextToSpecific!FileContext);

    const logPath = sanitizeLogPath(params.path);
    const targeting = targetingMethodOf(params);
    const dryRun = params.dryRun ? "true" : "false";

    auto path_ = pathToWorkarea(ctx, params.path, PathCategory.write, checkExist: true);
    if (!path_.valid) {
        logger.warningf("tool=editFile event=failure mode=%s targeting=%s path=%s errorType=invalid_path dryRun=%s",
                params.mode, targeting, logPath, dryRun);
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    EditMode mode_;
    try {
        mode_ = parseEditMode(params.mode);
    } catch (Exception e) {
        logger.warningf("tool=editFile event=failure mode=%s targeting=%s path=%s errorType=invalid_mode dryRun=%s",
                params.mode, targeting, logPath, dryRun);
        return ExecuteFuncResult(i"error: invalid mode '$(params.mode)': $(e.msg)".text,
                success: false);
    }

    // byLine targeting on a path modified since its last readFile: line
    // numbers may be stale - fail loudly, forcing a re-read first.
    if (params.startLine != -1) {
        string staleError;
        if (ctx.isDirty(path_)) {
            staleError = staleBylineTargetError(params.path);
        } else {
            const snapHash = ctx.observedHash(path_);
            if (snapHash >= 0) {
                try {
                    import std.file : readText;

                    if (computeContentHash(readText(path_)) != snapHash) {
                        staleError = staleBylineTargetError(params.path);
                    }
                } catch (Exception e) {
                    logger.tracef("tool=editFile event=stale_check_skip path=%s reason=%s",
                            logPath, e.msg);
                }
            }
        }
        if (!staleError.empty) {
            auto json = JSONValue.emptyObject;
            json["ok"] = false;
            json["error"] = staleError;
            logger.warningf("tool=editFile event=failure mode=%s targeting=%s path=%s errorType=stale_byline_target dryRun=%s scope=%s",
                    params.mode, targeting, logPath, dryRun,
                    scopeLogValue(params.scopeStart, params.scopeEnd));
            return ExecuteFuncResult(json.toString(JSONOptions.doNotEscapeSlashes), success: false);
        }
    }

    string[] fileLines;
    try {
        fileLines = File(path_.toString).byLineCopy.array;
        auto outcome = editFileUnifiedMemory(fileLines, mode_, params.content, params.startLine, params.count,
                params.marker, params.searchContent, params.replaceAll, params.matchIndex,
                params.scopeStart, params.scopeEnd, params.verifyContent, params.fragmentOk);

        auto json = JSONValue.emptyObject;
        json["ok"] = true;
        const netLineCountChange = cast(long) outcome.lines.length - cast(long) fileLines.length;
        json["matchedAt"] = outcome.matched.matchedAt;
        json["matchedLines"] = outcome.matched.matchedLines;
        json["linesChanged"] = netLineCountChange;
        json["operations"] = outcome.operations;
        if (outcome.autoCountUsed) {
            json["autoCountUsed"] = true;
            json["note"] = outcome.note;
        }
        if (params.fragmentOk && outcome.fragmentLinesReplaced > 0)
            json["fragmentLinesReplaced"] = outcome.fragmentLinesReplaced;

        // echo the exact pre-edit target lines consumed by
        // replace/remove modes, truncated when large (>11 lines: then
        // 5 + marker + 5 = 11 rendered entries are strictly fewer than the
        // total, so the marker stays truthful).
        if (outcome.replacedLines.length > 0) {
            if (outcome.replacedLines.length > 11) {
                json["replaced"] = outcome.replacedLines[0 .. 5] ~ [
                    "… (" ~ to!string(
                            outcome.replacedTotal) ~ " lines replaced; first 5 and last 5 shown)"
                ] ~ outcome.replacedLines[$ - 5 .. $];
                json["replacedTotal"] = outcome.replacedTotal;
                json["replacedTruncated"] = true;
            } else {
                json["replaced"] = JSONValue(outcome.replacedLines);
            }
        }

        if (outcome.noOp) {
            json["noOp"] = true;
            // autoCount shares this slot and CAN coexist with noOp (e.g.
            // byContent replace with count omitted and content identical to
            // the matched block): keep the auto-count derivation note and
            // append the no-op explanation instead of clobbering it.
            json["note"] = (outcome.note.length > 0 ? outcome.note ~ "; " : "") ~ "the edit produced no change (new content identical to the replaced text); if you intended a change, the content equals the search text";
        }

        // Session monitoring must see silent no-ops by name: the
        // success-path no-op is logged as a first-class event even though ok
        // stays true. Path is already sanitized (logPath above).
        if (outcome.noOp)
            logger.warningf("tool=editFile event=noop mode=%s targeting=%s path=%s dryRun=%s",
                    params.mode, targeting, logPath, dryRun);

        if (outcome.anchorDuplicate) {
            json["anchorDuplicate"] = true;
            // The auto-count note CAN coexist with anchorDuplicate (byContent
            // inserts auto-derive the target count from the matched block, so
            // autoCountUsed=true there): keep the auto-count derivation note
            // and append the anchor-duplication explanation instead of
            // clobbering it.
            const dupNote = (mode_ == EditMode.insert_after || mode_ == EditMode.append) ? "content's first line duplicates the matched block's last line; the anchor was kept, so it now appears twice — drop it from content if unintended" : "content's last line duplicates the matched block's first line; the anchor was kept, so it now appears twice — drop it from content if unintended";
            json["note"] = (outcome.note.length > 0 ? outcome.note ~ "; " : "") ~ dupNote;
        }

        if (params.includeDiff)
            json["diff"] = unifiedDiffLines(fileLines, outcome.lines);

        // Preserve the original file's trailing-newline state
        const trailingNewline = fileEndsWithNewline(path_.toString);
        if (params.dryRun) {
            auto preview = outcome.lines.join("\n");
            if (trailingNewline && outcome.lines.length > 0)
                preview ~= "\n";
            json["preview"] = preview;
        } else {
            writeLines(path_, outcome.lines, !trailingNewline);
            ctx.markDirty(path_);
        }
        logger.tracef("tool=editFile event=success mode=%s targeting=%s path=%s linesChanged=%d autoCountUsed=%s dryRun=%s scope=%s",
                mode_.to!string, targeting, logPath, netLineCountChange, outcome.autoCountUsed
                ? "true" : "false", dryRun, scopeLogValue(params.scopeStart, params.scopeEnd));
        return ExecuteFuncResult(json.toString(JSONOptions.doNotEscapeSlashes), success: true);
    } catch (Exception e) {
        auto json = JSONValue.emptyObject;
        json["ok"] = false;
        json["error"] = e.msg;
        if (e.msg.canFind("not found")) {
            if (!params.marker.empty) {
                json["diagnostic"] = buildMarkerDiagnostic(fileLines,
                        params.marker, params.scopeStart, params.scopeEnd);
                json["suggestion"] = "re-read the file to find the exact marker text, then retry with a marker that exists in the file";
            } else if (!params.searchContent.empty) {
                json["diagnostic"] = buildBlockDiagnostic(fileLines,
                        params.searchContent, params.scopeStart, params.scopeEnd);
                json["suggestion"] = "re-read the file to verify exact content, then adjust your search block";
            }
        } else if (e.msg.canFind("matchIndex=")) {
            // Nth-occurrence out of bounds: the error already reports the
            // actual match count; attach the closest-match diagnostic too.
            if (!params.marker.empty)
                json["diagnostic"] = buildMarkerDiagnostic(fileLines,
                        params.marker, params.scopeStart, params.scopeEnd);
            else if (!params.searchContent.empty)
                json["diagnostic"] = buildBlockDiagnostic(fileLines,
                        params.searchContent, params.scopeStart, params.scopeEnd);
            json["suggestion"] = "lower matchIndex to the reported occurrence count, or use replaceAll=true (with default matchIndex=1) to target every occurrence";
        }
        logger.warningf("tool=editFile event=failure mode=%s targeting=%s path=%s errorType=%s dryRun=%s scope=%s",
                mode_.to!string, targeting,
                logPath, classifyEditError(e.msg), dryRun,
                scopeLogValue(params.scopeStart, params.scopeEnd));
        return ExecuteFuncResult(json.toString(JSONOptions.doNotEscapeSlashes), success: false);
    }
}
