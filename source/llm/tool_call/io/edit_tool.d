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

mixin RegisterLlmFunctions!();

/// Parameters for the unified editFile tool.
///
/// Exactly one targeting method must be provided: startLine (byLine),
/// marker (byMarker), or searchContent (byContent).
struct UnifiedEditFileParams {
    @ParamDescription("Path to the file (relative to workarea)")
    string path;

    @ParamDescription("Content to insert or replace with; must be empty string for remove mode")
    string content;

    @ParamDescription(
            `Edit mode: "replace", "remove", "append", "insert_before", or "insert_after" (alias for append)`)
    string mode;

    // Targeting — exactly one of the following must be provided:
    @ParamDescription(
            "byLine targeting: first line of the range (1-based, must be >= 1; omitted by default)")
    @ParamOptional long startLine = -1;

    @ParamDescription("Number of lines to replace/remove from the target. byLine: required for replace/remove. byMarker: default 1, auto-derived from content line count when replacing with multi-line content; for multi-line markers, count auto-derives from the marker line count. byContent: auto-derived from matched block size (explicit count overrides for single edits; not supported with replaceAll). Ignored for insert modes.")
    @ParamOptional long count;

    @ParamDescription("byMarker targeting: case-sensitive substring to find; multi-line markers (up to 20 lines) are also supported — the first line is used as an anchor and subsequent lines are verified as substrings of consecutive file lines")
    @ParamOptional string marker;

    @ParamDescription(
            "byContent targeting: code block to find using trimmed equality matching (multi-line allowed)")
    @ParamOptional string searchContent;

    // Options:
    @ParamDescription(
            "Replace all non-overlapping occurrences (byContent and byMarker only; not supported with byLine)")
    @ParamOptional bool replaceAll;

    @ParamDescription("Preview the result without writing to disk")
    @ParamOptional bool dryRun;

    @ParamDescription("Which occurrence to target (1-based, default 1; values < 1 are invalid). matchIndex > 1 targets the Nth occurrence (byMarker/byContent only) and cannot be combined with replaceAll; ignored by byLine targeting.")
    @ParamOptional long matchIndex = 1;

    @ParamDescription("Scope limiting: restrict the byMarker/byContent search to the 1-based inclusive line range [scopeStart, scopeEnd]. Either or both may be provided: scopeStart alone searches from that line to the end of file; scopeEnd alone searches from line 1 to that line. The first line of a match (anchor) must be inside the range; the match may extend past scopeEnd. Ignored by byLine targeting (byLine does not search). scopeStart must be >= 1; if both are provided, scopeStart must be <= scopeEnd.")
    @ParamOptional long scopeStart = -1;

    @ParamDescription(
            "Scope limiting: end of the search range (1-based, inclusive). See scopeStart.")
    @ParamOptional long scopeEnd = -1;

    @ParamDescription("byLine guard: verify that the target lines match this content before editing. When non-empty, the lines at [startLine, startLine+count) are compared (trimmed equality) against verifyContent. Mismatch throws an error with the actual content found, preventing silent corruption from stale line numbers after successive edits. Only valid with byLine targeting.")
    @ParamOptional string verifyContent;
}

/// Resolve the targeting method of the unified editFile params for logging.
/// Returns "byLine", "byMarker", "byContent", or "none" (absent/ambiguous).
package string targetingMethodOf(UnifiedEditFileParams params) @safe {
    const provided = (params.startLine != -1 ? 1 : 0) + (params.marker.length > 0
            ? 1 : 0) + (params.searchContent.length > 0 ? 1 : 0);
    if (provided != 1)
        return "none";
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

@Function("Edit a file by applying a change to a target. " ~ "Target the file with exactly one method: startLine+count (byLine, 1-based; startLine must be >= 1), " ~ "marker (byMarker, substring to find; multi-line markers up to 20 lines are also supported — first line is an anchor, subsequent lines verified against consecutive file lines), or " ~ "searchContent (byContent, code block matched by trimmed equality). " ~ "Modes: replace (replace targeted lines with content), remove (delete targeted lines, content must be empty), " ~ "append (keep target line, add content after it), insert_after (same as append), " ~ "insert_before (add content before target line, keep target line). " ~ "byMarker replace auto-derives count from the number of content lines when content is multi-line and count is omitted; for multi-line markers, count auto-derives from the marker line count; set count=1 to replace only the marker line; empty content replaces exactly the marker line. " ~ "byContent auto-derives count from the matched block size; an explicit count overrides for single edits but is not supported with replaceAll. " ~ "byContent append/insert_after inserts content after the matched block; insert_before inserts before it. " ~ "replaceAll replaces every non-overlapping occurrence (byContent and byMarker only, replace/remove modes). " ~ "matchIndex selects the Nth occurrence (1-based, must be >= 1; matchIndex > 1 targets a single occurrence, cannot be combined with replaceAll, and is ignored by byLine targeting). " ~ "scopeStart/scopeEnd (1-based, inclusive) limit the byMarker/byContent search to a line range for large files; either or both may be given (scopeStart alone searches from that line to EOF, scopeEnd alone searches from line 1 to that line); the first line of a match must be inside the range; the match may extend past scopeEnd; ignored by byLine. " ~ "byLine targeting can optionally verify target content with verifyContent to prevent silent corruption from stale line numbers. " ~ "The file's trailing-newline state is preserved, and the dryRun preview matches the exact bytes that would be written. " ~ "Returns a JSON object with fields: ok (bool), matchedAt (1-based line), matchedLines (int), linesChanged (int), operations (int), lineShift (int, net line count change). " ~ "When auto-count is used, also includes: autoCountUsed (true) and note (string). " ~ "When dryRun is true, also includes: preview (string). " ~ "On failure returns JSON with ok=false, an error string, and a diagnostic field with closest-match details.")
ExecuteFuncResult editFile(Context baseCtx, UnifiedEditFileParams params) {
    mixin(baseContextToSpecific!FileContext);

    const logPath = sanitizeLogPath(params.path);
    const targeting = targetingMethodOf(params);
    const dryRun = params.dryRun ? "true" : "false";

    auto path_ = pathToWorkarea(ctx, params.path, checkExist: true);
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

    string[] fileLines;
    try {
        fileLines = File(path_.toString).byLineCopy.array;
        auto outcome = editFileUnifiedMemory(fileLines, mode_, params.content, params.startLine, params.count,
                params.marker, params.searchContent, params.replaceAll, params.matchIndex,
                params.scopeStart, params.scopeEnd, params.verifyContent);

        auto json = JSONValue.emptyObject;
        json["ok"] = true;
        json["lineShift"] = outcome.linesChanged;
        json["matchedAt"] = outcome.matched.matchedAt;
        json["matchedLines"] = outcome.matched.matchedLines;
        json["linesChanged"] = outcome.linesChanged;
        json["operations"] = outcome.operations;
        if (outcome.autoCountUsed) {
            json["autoCountUsed"] = true;
            json["note"] = outcome.note;
        }

        // Preserve the original file's trailing-newline state so an edit
        // never changes it, and the dryRun preview matches the exact bytes
        // that would be written.
        const trailingNewline = fileEndsWithNewline(path_.toString);
        if (params.dryRun) {
            auto preview = outcome.lines.join("\n");
            if (trailingNewline && outcome.lines.length > 0)
                preview ~= "\n";
            json["preview"] = preview;
        } else {
            writeLines(path_, outcome.lines, !trailingNewline);
        }
        logger.tracef("tool=editFile event=success mode=%s targeting=%s path=%s linesChanged=%d autoCountUsed=%s dryRun=%s scope=%s",
                mode_.to!string, targeting, logPath, outcome.linesChanged, outcome.autoCountUsed
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
