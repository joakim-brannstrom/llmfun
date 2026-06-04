module llm.tool_call.rag;

import logger = std.logger;
import std.algorithm : map, filter, startsWith, count, joiner, endsWith;
import std.array : empty, appender, array;
import std.conv : to;
import std.file : readText, exists, mkdirRecurse, getSize, remove, dirEntries, SpanMode;
import std.format : format, formattedWrite;
import std.json : JSONValue;
import std.range : enumerate;
import std.regex : Regex, regex;
import std.stdio : File;
import std.string : join, splitLines, indexOf, strip, split, replace;
import std.sumtype : match;
import std.exception : enforce;
import std.path : relativePath, buildNormalizedPath;
import std.process : execute;

import my.path : Path, AbsolutePath;

import llm.tool_call;
import llm.rag.rag;
import llm.tool_call.utility;

mixin RegisterLlmFunctions!();

interface RAGContext : Context {
    RAG getRAG();
    bool isPathInsideWorkArea(AbsolutePath path);
    AbsolutePath workArea();
}

private immutable maxTopK = 20;

private string location(Document doc) {
    import std.sumtype : match;

    return doc.origin.match!((Unknown _) => "no source,", (Url a) => a.value, (Path a) => a
            .toString);
}

private string toResult(Document[] docs) {
    return docs.enumerate.map!(doc => format("--- Result %s (%s line %s-%s chars %s-%s) ---\n%s",
            doc.index + 1, location(doc.value), doc.value.line.begin,
            doc.value.line.end, doc.value.offset.begin, doc.value.offset.end, doc.value.data)).join(
            "\n\n");
}

ExecuteFuncResult queryFunc(alias searchFunc)(Context baseCtx, string query,
        long topK, string database) {
    mixin(baseContextToSpecific!RAGContext);

    if (topK < 1 || topK > maxTopK) {
        return ExecuteFuncResult(format!"error: topK parameter must be in range [1, %s]"(maxTopK),
                success: false);
    }
    if (query.length == 0 || query.strip.length == 0) {
        return ExecuteFuncResult("error: query must not be empty", success: false);
    }

    try {
        auto docs = searchFunc(ctx.getRAG, query, topK, database);
        if (docs.length == 0) {
            return ExecuteFuncResult(format!"Search completed but no results found for query: '%s'"(query),
                    success: false);
        }
        return ExecuteFuncResult(toResult(docs), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: database error during search: %s"(e.msg),
                success: false);
    }
}

@Function("Search RAG for topK relevant results by query text. If 'database' is empty string, all databases are searched. Otherwise restricts search to the database with that name. Use listRAGDatabases to discover available database names.")
ExecuteFuncResult querySemantic(Context baseCtx, string query, long topK, string database) {
    return queryFunc!((RAG rag, string query, long topK,
            string database) => rag.querySemantic(query, topK, database))(baseCtx,
            query, topK, database);
}

@Function("Search RAG using FTS5 full-text search for topK relevant results by query text. If 'database' is empty string, all databases are searched. Otherwise restricts search to the database with that name. Use listRAGDatabases to discover available database names.")
ExecuteFuncResult queryTextSearch(Context baseCtx, string query, long topK, string database) {
    return queryFunc!((RAG rag, string query, long topK,
            string database) => rag.queryTextSearch(query, topK, database))(baseCtx,
            query, topK, database);
}

@Function("Search RAG using semantic and FTS5 full-text search for topK relevant results by query text. If 'database' is empty string, all databases are searched. Otherwise restricts search to the database with that name. Use listRAGDatabases to discover available database names.")
ExecuteFuncResult queryBestMatch(Context baseCtx, string query, long topK, string database) {
    return queryFunc!((RAG rag, string query, long topK,
            string database) => rag.queryBestMatch(query, topK, database))(baseCtx,
            query, topK, database);
}

@Function("List all available RAG databases with their names and file paths. Use this to discover database names for filtering queries.")
ExecuteFuncResult listRAGDatabases(Context baseCtx) {
    mixin(baseContextToSpecific!RAGContext);

    try {
        import std.range : iota;

        auto rag = ctx.getRAG();
        auto names = rag.getDatabaseNames();
        auto files = rag.dbFiles;

        if (names.empty) {
            return ExecuteFuncResult("No RAG databases loaded", success: true);
        }

        auto lines = iota(names.length).map!(i => format("  - %-15s -> %s",
                names[i], files[i].toString)).array;

        return ExecuteFuncResult("Available RAG databases:\n" ~ lines.join("\n"), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed to list RAG databases: %s"(e.msg),
                success: false);
    }
}

@Function("Load file content into RAG index")
ExecuteFuncResult loadFileToRAG(Context baseCtx, string path) {
    mixin(baseContextToSpecific!RAGContext);
    auto absPath = AbsolutePath(path);
    if (!ctx.isPathInsideWorkArea(absPath)) {
        return ExecuteFuncResult(format!"error: path '%s' must be inside the allowed workarea"(path),
                success: false);
    }

    try {
        auto data = readText(absPath.toString);
        auto relPath = relativePath(absPath.toString, ctx.workArea.toString);
        auto normalizedPath = buildNormalizedPath(relPath);
        auto result = ctx.getRAG().add(Document(Origin(Path(normalizedPath)), data, Offset.init));
        return ExecuteFuncResult(format!"File '%s' (%s length) added as %s chunks to the RAG"(path,
                result.length, result.chunks), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed loading file into rag: %s"(e.msg),
                success: false);
    }
}

@Function("Load content into RAG index")
ExecuteFuncResult loadContentToRAG(Context baseCtx, string content) {
    mixin(baseContextToSpecific!RAGContext);

    try {
        auto result = ctx.getRAG().add(Document(Origin(Unknown.init), content, Offset.init));
        return ExecuteFuncResult(format!"Content (%s length) added as %s chunks to the RAG"(result.length,
                result.chunks), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed loading file into rag: %s"(e.msg),
                success: false);
    }
}

/**
 * Private helper: prefix each line of text with its absolute line number.
 * When appendLoc is non-zero, splits text into lines and prefixes each with
 * "LINE_NUM→ " starting from startLineNumber.
 */
private string applyAppendLoc(string text, long startLineNumber, long appendLoc) {
    if (!appendLoc)
        return text;

    auto buf = appender!(string)();
    auto lines = text.splitLines();
    long lineNum = startLineNumber;
    foreach (i, l; lines) {
        if (i > 0)
            buf.put('\n');
        formattedWrite(buf, "%s→ %s", lineNum++, l);
    }
    buf.put('\n');
    return buf[];
}

@Function("Read a specific line from a file in the RAG index. Returns the text chunk(s) containing the given line number. If 'database' is empty string, all databases are searched. The 'appendLoc' parameter (non-zero = true) prefixes each line with its line number, matching readFile behavior.")
ExecuteFuncResult queryReadFile(Context baseCtx, string filePath, long lineNumber,
        string database, long appendLoc) {
    mixin(baseContextToSpecific!RAGContext);

    if (filePath.empty) {
        return ExecuteFuncResult("error: filePath must not be empty", success: false);
    }
    if (lineNumber < 1) {
        return ExecuteFuncResult(format!"error: lineNumber must be >= 1, got: %s"(lineNumber),
                success: false);
    }
    if (!database.empty && !ctx.getRAG().databaseExists(database)) {
        return ExecuteFuncResult(format!"Database '%s' not found"(database), success: false);
    }

    auto fileAsPath = Path(filePath);

    try {
        auto matches = ctx.getRAG().queryReadFile(fileAsPath, lineNumber, database);

        if (matches.length == 0) {
            if (!ctx.getRAG().hasFile(fileAsPath, database)) {
                return ExecuteFuncResult(format!"File '%s' not found in RAG index"(filePath),
                        success: false);
            }
            return ExecuteFuncResult(format!"File '%s' exists in RAG but no chunk contains line %s"(filePath,
                    lineNumber), success: false);
        }

        string[] resultBlocks;
        foreach (i, match; matches) {
            string originStr = match.origin.match!((Unknown _) => "no source",
                    (Url a) => a.value, (Path a) => a.toString);

            string text = applyAppendLoc(match.text, match.line.begin, appendLoc);

            resultBlocks ~= format("--- Result %d (%s line %s-%s chars %s-%s) ---\n%s", i + 1, originStr,
                    match.line.begin, match.line.end, match.offset.begin, match.offset.end, text);
        }

        return ExecuteFuncResult(resultBlocks.join("\n\n"), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: database error during query: %s"(e.msg),
                success: false);
    }
}
