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
import miniorm : spinSql;

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

    return doc.origin.match!((Topic a) => "topic: " ~ a.name, (Url a) => a.value,
            (Path a) => a.toString);
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
            return ExecuteFuncResult(format!"error: search completed but no results found for query: '%s'"(query),
                    success: false);
        }
        return ExecuteFuncResult(toResult(docs), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: database error during search: %s"(e.msg),
                success: false);
    }
}

@Function("Search RAG for topK relevant results by query text. If 'database' is '*', all databases are searched. Otherwise restricts search to the database with that name. Use listRAGDatabases to discover available database names.")
ExecuteFuncResult querySemantic(Context baseCtx, string query, long topK, string database) {
    return queryFunc!((RAG rag, string query, long topK,
            string database) => rag.querySemantic(query, topK, database))(baseCtx,
            query, topK, database);
}

@Function("Search RAG using FTS5 full-text search for topK relevant results by query text. If 'database' is '*', all databases are searched. Otherwise restricts search to the database with that name. Use listRAGDatabases to discover available database names.")
ExecuteFuncResult queryTextSearch(Context baseCtx, string query, long topK, string database) {
    return queryFunc!((RAG rag, string query, long topK,
            string database) => rag.queryTextSearch(query, topK, database))(baseCtx,
            query, topK, database);
}

@Function("Search RAG using semantic and FTS5 full-text search for topK relevant results by query text. If 'database' is '*', all databases are searched. Otherwise restricts search to the database with that name. Use listRAGDatabases to discover available database names.")
ExecuteFuncResult queryBestMatch(Context baseCtx, string query, long topK, string database) {
    return queryFunc!((RAG rag, string query, long topK,
            string database) => rag.queryBestMatch(query, topK, database))(baseCtx,
            query, topK, database);
}

@Function("List all available RAG databases with their names and file paths. Use this to discover database names for filtering queries.")
ExecuteFuncResult listRAGDatabases(Context baseCtx) {
    mixin(baseContextToSpecific!RAGContext);

    try {
        auto infos = ctx.getRAG.getDatabaseInfo();
        if (infos.empty) {
            return ExecuteFuncResult("No RAG databases loaded", success: true);
        }
        import std.typecons : tuple;

        auto lines = appender!(string[])();
        bool isFirst = true;
        foreach (a; infos) {
            lines.put(format!"%s -%s '%s'"(a.name, isFirst ? " [primary] - " : "", a.description));
            isFirst = false;
        }

        return ExecuteFuncResult(format!"Available RAG databases:\n%-(%s\n%)"(lines[]),
                success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed to list RAG databases: %s"(e.msg),
                success: false);
    }
}

@Function("Load file content into RAG index")
ExecuteFuncResult loadFileToRAG(Context baseCtx, string path) {
    mixin(baseContextToSpecific!RAGContext);

    auto path_ = pathToWorkarea(ctx, path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    try {
        auto data = readText(path_);
        auto relPath = relativePath(path_.toString, ctx.workArea.toString);
        auto normalizedPath = buildNormalizedPath(relPath);
        auto result = ctx.getRAG().add(Document(Origin(Path(normalizedPath)), data, Offset.init));
        spinSql!(() => ctx.getRAG.fts5Rebuild);
        return ExecuteFuncResult(format!"File '%s' (%s length) added as %s chunks to the RAG"(path,
                result.length, result.chunks), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed loading file into rag: %s"(e.msg),
                success: false);
    }
}

@Function(
        "Load content into RAG index with a topic name. Topic must be alphanumeric + underscore, max 100 chars.")
ExecuteFuncResult loadContentToRAG(Context baseCtx, string topic, string content) {
    mixin(baseContextToSpecific!RAGContext);

    if (topic.empty) {
        return ExecuteFuncResult("error: topic must not be empty", success: false);
    }
    if (topic.length > 100) {
        return ExecuteFuncResult("error: topic too long. Max 100 characters", success: false);
    }
    if (auto err = checkAlphaNumUnderscore(topic)) {
        return ExecuteFuncResult(err, success: false);
    }
    if (content.empty) {
        return ExecuteFuncResult("error: content must not be empty", success: false);
    }
    const ulong maxContentSize = 1024 * 1024; // 1MB
    if (content.length > maxContentSize) {
        return ExecuteFuncResult(format!"error: content too large. Max %s bytes"(maxContentSize),
                success: false);
    }

    try {
        auto result = ctx.getRAG().add(Document(Origin(Topic(topic)), content, Offset.init));
        spinSql!(() => ctx.getRAG.fts5Rebuild);
        return ExecuteFuncResult(format!"Content (%s length) added to '%s' as %s chunks to the RAG"(result.length,
                topic, result.chunks), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed loading topic into rag: %s"(e.msg),
                success: false);
    }
}

@Function("Remove topic from RAG index. Topic must be alphanumeric + underscore, max 100 chars.")
ExecuteFuncResult removeTopicFromRAG(Context baseCtx, string topic) {
    mixin(baseContextToSpecific!RAGContext);

    if (topic.empty) {
        return ExecuteFuncResult("error: topic must not be empty", success: false);
    }
    if (topic.length > 100) {
        return ExecuteFuncResult("error: topic too long. Max 100 characters", success: false);
    }
    if (auto err = checkAlphaNumUnderscore(topic)) {
        return ExecuteFuncResult(err, success: false);
    }

    try {
        const chunks = spinSql!(() => ctx.getRAG().removeSource(Origin(Topic(topic))));
        spinSql!(() => ctx.getRAG.fts5Rebuild);
        return ExecuteFuncResult(format!"removed topic '%s' with %s chunks from RAG"(topic,
                chunks), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed to remove topic '%s' from RAG: %s"(topic,
                e.msg), success: false);
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

@Function("Read a specific line from a file in the RAG index. Returns the text chunk(s) containing the given line number. If 'database' is '*', all databases are searched. The 'appendLoc' parameter (non-zero = true) prefixes each line with its line number, matching readFile behavior.")
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
        return ExecuteFuncResult(format!"error: database '%s' not found"(database), success: false);
    }

    auto fileAsPath = Path(filePath);

    try {
        auto matches = ctx.getRAG().queryReadFile(fileAsPath, lineNumber, database);

        if (matches.length == 0) {
            if (!ctx.getRAG().hasFile(fileAsPath, database)) {
                return ExecuteFuncResult(format!"error: file '%s' not found in RAG index"(filePath),
                        success: false);
            }
            return ExecuteFuncResult(format!"error: file '%s' exists in RAG but no chunk contains line %s"(filePath,
                    lineNumber), success: false);
        }

        string[] resultBlocks;
        foreach (i, match; matches) {
            string originStr = match.origin.match!((Topic a) => format!"topic: '%s'"(a.name),
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
