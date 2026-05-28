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
import std.path : relativePath;
import std.process : execute;

import my.path : AbsolutePath;

import llm.tool_call;
import llm.rag.rag;
import llm.tool_call.utility;

mixin RegisterLlmFunctions!();

interface RAGContext : Context {
    RAG getRAG();
    bool isPathInsideWorkArea(AbsolutePath path);
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

ExecuteFuncResult queryFunc(alias searchFunc)(Context baseCtx, string query, long topK) {
    mixin(baseContextToSpecific!RAGContext);

    if (topK < 1 || topK > maxTopK) {
        return ExecuteFuncResult(format!"error: topK parameter must be in range [1, %s]"(maxTopK),
                success: false);
    }
    if (query.length == 0 || query.strip.length == 0) {
        return ExecuteFuncResult("error: query must not be empty", success: false);
    }

    try {
        auto docs = searchFunc(ctx.getRAG, query, topK);
        if (docs.length == 0) {
            return ExecuteFuncResult(format!"No results found for query: '%s'"(query),
                    success: false);
        }
        return ExecuteFuncResult(toResult(docs), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: database error during search: %s"(e.msg),
                success: false);
    }
}

@Function("Search RAG for topK relevant results by query text")
ExecuteFuncResult querySemantic(Context baseCtx, string query, long topK) {
    return queryFunc!((RAG rag, string query, long topK) => rag.querySemantic(query, topK))(baseCtx,
            query, topK);
}

@Function("Search RAG using FTS5 full-text search for topK relevant results by query text")
ExecuteFuncResult queryTextSearch(Context baseCtx, string query, long topK) {
    auto res = queryFunc!((RAG rag, string query, long topK) => rag.queryTextSearch(query, topK))(baseCtx,
            query, topK);
    if (!res.success) {
        return queryFunc!((RAG rag, string query, long topK) => rag.querySemantic(query, topK))(baseCtx,
                query, topK);
    }
    return res;
}

@Function(
        "Search RAG using semantic and FTS5 full-text search for topK relevant results by query text")
ExecuteFuncResult queryBestMatch(Context baseCtx, string query, long topK) {
    return queryFunc!((RAG rag, string query, long topK) => rag.queryBestMatch(query, topK))(baseCtx,
            query, topK);
}

@Function("Load file content into RAG index")
ExecuteFuncResult loadFileToRAG(Context baseCtx, string path) {
    mixin(baseContextToSpecific!RAGContext);
    auto path_ = AbsolutePath(path);
    if (!ctx.isPathInsideWorkArea(path_)) {
        return ExecuteFuncResult(format!"error: path '%s' must be inside the allowed workarea"(path),
                success: false);
    }

    try {
        auto data = readText(path_.toString);
        auto result = ctx.getRAG().add(Document(Origin(path_), data, Offset.init));
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
