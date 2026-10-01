module llm.tool_call.rag;

import logger = std.logger;
import std.algorithm : canFind, filter, map, startsWith;
import std.array : empty, appender, array;
import std.conv : to, text;
import std.format : format;
import std.json : JSONValue;
import std.range : enumerate;
import std.regex : Regex, regex;
import std.stdio : File;
import std.string : join, splitLines, strip, toLower;
import std.sumtype : match;

import my.path : Path, AbsolutePath;
import miniorm : spinSql;

import llm.config : ToolLimits, RagConfig;
import llm.rag.database : cleanFts5;
import llm.rag.rag : RAG, Document, Topic, Origin, Offset, Url, add;
import llm.test_util : retrySql;
import llm.tool_call : RegisterLlmFunctions, Context, ParamDescription,
    ParamOptional, ExecuteFuncResult, Function, baseContextToSpecific;
import llm.tool_call.utility : checkAlphaNumUnderscore, pathToWorkarea;

mixin RegisterLlmFunctions!();

interface RAGContext : Context {
    RAG getRAG();
    bool isPathInsideWorkArea(AbsolutePath path);
    AbsolutePath workArea();
    ToolLimits getToolLimits() @safe;
    RagConfig getRagConfig();
}

private string checkTopic(RAGContext ctx, string topic) @safe {
    if (topic.empty)
        return "error: topic must not be empty";
    auto maxLen = ctx.getToolLimits().maxTopicLength;
    if (topic.length > maxLen)
        return i"error: topic too long. Max $(maxLen) characters".text;
    if (auto err = checkAlphaNumUnderscore(topic))
        return err;
    return null;
}

/// Check if a topic name belongs to the protected AGENTS.md namespace.
/// Topics starting with "agent_md_" are managed exclusively by the system
/// (agent_md.d module) and cannot be modified via agent tool calls.
private bool isAgentMdTopic(string topic) @safe pure nothrow {
    return topic.startsWith("agent_md_");
}

/// Reject protected AGENTS.md topics, returning an error message or null if allowed.
private string checkAgentMdProtection(string topic) @safe pure {
    if (isAgentMdTopic(topic)) {
        return i"error: topic '$(topic)' is protected. AGENTS.md topics (agent_md_*) are managed exclusively by the system and cannot be modified via tool calls."
            .text;
    }
    return null;
}

private string location(Document doc) @safe pure {
    return doc.origin.match!((Topic a) => "topic: " ~ a.name, (Url a) => a.value,
            (Path a) => a.toString);
}

private string toResult(Document[] docs) @safe {
    return docs.enumerate.map!(doc => format("--- Result %s ('%s' in database '%s' line %s-%s chars %s-%s) ---\n%s",
            doc.index + 1, location(doc.value), doc.value.databaseName,
            doc.value.line.begin, doc.value.line.end, doc.value.offset.begin,
            doc.value.offset.end, doc.value.data)).join("\n\n");
}

/**
 * Generic query helper that works with param structs.
 * Uses compile-time field detection to determine which RAG query method to call:
 * - Both textQuery and vectorQuery present → queryBestMatch
 * - Only textQuery present - queryTextSearch
 * - Only vectorQuery present - querySemantic
 */
private ExecuteFuncResult queryFunc(P)(RAGContext ctx, P params) {
    if (ctx.getRAG() is null) {
        return ExecuteFuncResult("error: RAG not available", success: false);
    }
    if (params.topK < 1 || params.topK > ctx.getToolLimits().maxTopK) {
        return ExecuteFuncResult(i"error: params.topK parameter must be in range [1, $(
                ctx.getToolLimits().maxTopK)]".text, success: false);
    }

    string textQuery;
    static if (__traits(hasMember, P, "textQuery")) {
        textQuery = cleanFts5(params.textQuery);
        if (textQuery.strip.empty)
            return ExecuteFuncResult("error: textQuery must not be empty", success: false);
    }

    string vectorQuery;
    static if (__traits(hasMember, P, "vectorQuery")) {
        vectorQuery = params.vectorQuery;
        if (vectorQuery.strip.empty)
            return ExecuteFuncResult("error: vectorQuery must not be empty", success: false);
    }

    try {
        Document[] docs;
        static if (__traits(hasMember, P, "textQuery") && __traits(hasMember, P, "vectorQuery")) {
            docs = ctx.getRAG().queryBestMatch(textQuery, vectorQuery,
                    params.topK, params.database);
        } else static if (__traits(hasMember, P, "textQuery")) {
            docs = ctx.getRAG().queryTextSearch(textQuery, params.topK, params.database);
        } else {
            docs = ctx.getRAG().querySemantic(vectorQuery, params.topK, params.database);
        }

        if (docs.length == 0) {
            auto queryDesc = () {
                static if (__traits(hasMember, P, "textQuery")
                        && __traits(hasMember, P, "vectorQuery")) {
                    return i"textQuery: $(textQuery) vectorQuery: $(vectorQuery)".text;
                } else static if (__traits(hasMember, P, "textQuery")) {
                    return i"textQuery: $(textQuery)".text;
                } else {
                    return i"vectorQuery: $(vectorQuery)".text;
                }
            }();
            return ExecuteFuncResult(i"error: search completed but no results found for $(queryDesc)".text,
                    success: false);
        }
        return ExecuteFuncResult(toResult(docs), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: database error during search: $(e.msg)".text,
                success: false);
    }
}

struct QuerySemanticParams {
    @ParamDescription("Natural language query for semantic similarity search")
    string vectorQuery;

    @ParamDescription("Number of results to return")
    @ParamOptional long topK = 5;

    @ParamDescription("Database name to search, or '*' for all databases")
    @ParamOptional string database = "*";
}

@Function("Search the RAG using semantic queries. Use listRAGDatabases to discover available database names",
    tags:
        ["rag"]) ExecuteFuncResult querySemantic(Context baseCtx, QuerySemanticParams params) {
    mixin(baseContextToSpecific!RAGContext);
    return queryFunc(ctx, params);
}

struct QueryTextSearchParams {
    @ParamDescription("FTS5 full-text search query. See function description for syntax details")
    string textQuery;

    @ParamDescription("Number of results to return")
    @ParamOptional long topK = 5;

    @ParamDescription("Database name to search, or '*' for all databases")
    @ParamOptional string database = "*";
}

@Function("Search RAG using FTS5 full-text search for topK relevant results. The `textQuery` is passed directly to SQLite FTS5",
    tags:
        ["rag"]) ExecuteFuncResult queryTextSearch(Context baseCtx, QueryTextSearchParams params) {
    mixin(baseContextToSpecific!RAGContext);
    auto res = queryFunc(ctx, params);
    if (!res.success) {
        res.msg ~= fts5Help(ctx.getToolLimits.queryFtl5Mode, params.textQuery);
    }
    return res;
}

struct QueryBestMatchParams {
    @ParamDescription("FTS5 full-text search query")
    string textQuery;

    @ParamDescription("Natural language query for semantic similarity search")
    string vectorQuery;

    @ParamDescription("Number of results to return")
    @ParamOptional long topK = 5;

    @ParamDescription("Database name to search, or '*' for all databases")
    @ParamOptional string database = "*";
}

@Function("Search RAG using combined semantic and FTS5 full-text. Use `listRAGDatabases` to discover available database names",
    tags:
        ["rag"]) ExecuteFuncResult queryBestMatch(Context baseCtx, QueryBestMatchParams params) {
    mixin(baseContextToSpecific!RAGContext);
    auto res = queryFunc(ctx, params);
    if (!res.success) {
        res.msg ~= fts5Help(ctx.getToolLimits.queryFtl5Mode, params.textQuery);
    }
    return res;
}

struct ListRAGDatabasesParams {
}

@Function("List all available RAG databases with their names and file paths. Use this to discover database names for filtering queries",
    tags:
        ["rag"]) ExecuteFuncResult listRAGDatabases(Context baseCtx, ListRAGDatabasesParams params) {
    mixin(baseContextToSpecific!RAGContext);
    if (ctx.getRAG() is null) {
        return ExecuteFuncResult("error: RAG not available", success: false);
    }

    try {
        auto infos = ctx.getRAG().getDatabaseInfo();
        if (infos.empty) {
            return ExecuteFuncResult("No RAG databases loaded", success: true);
        }
        import std.typecons : tuple;

        auto lines = appender!(string[])();
        bool isFirst = true;
        foreach (a; infos) {
            lines.put(i"$(a.name) -$(isFirst ? " [primary] - " : "") '$(a.description)'".text);
            isFirst = false;
        }

        return ExecuteFuncResult(i"Available RAG databases:\n$(lines[].join("\n"))".text,
                success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: failed to list RAG databases: $(e.msg)".text,
                success: false);
    }
}

struct LoadFileToRAGParams {
    @ParamDescription("Path to the file to load into the RAG index")
    string path;
}

@Function("Load file content into RAG index", tags:
        ["rag"]) ExecuteFuncResult loadFileToRAG(Context baseCtx, LoadFileToRAGParams params) {
    import std.path : relativePath, buildNormalizedPath;
    import llm.utility : readFileUtf8;

    mixin(baseContextToSpecific!RAGContext);

    if (ctx.getRAG() is null) {
        return ExecuteFuncResult("error: RAG not available", success: false);
    }
    auto path_ = pathToWorkarea(ctx, params.path, checkExist: true);
    if (!path_.valid) {
        return ExecuteFuncResult(path_.errorMsg, success: false);
    }

    try {
        auto data = readFileUtf8(path_);
        auto relPath = relativePath(path_.toString, ctx.workArea.toString);
        auto normalizedPath = buildNormalizedPath(relPath);
        auto result = ctx.getRAG().add(Document(Origin(Path(normalizedPath)),
                data, Offset.init), ctx.getRagConfig());
        retrySql!(() => ctx.getRAG.fts5Rebuild);
        return ExecuteFuncResult(i"File '$(params.path)' ($(result.length) length) added as $(
                result.chunks) chunks to the RAG".text, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: failed loading file into rag: $(e.msg)".text,
                success: false);
    }
}

struct LoadContentToRAGParams {
    @ParamDescription("Topic name for the content (alphanumeric + underscore)")
    string topic;

    @ParamDescription("Content to store in the RAG index")
    string content;
}

@Function("Load content into RAG index with a topic name", tags:
        ["rag"]) ExecuteFuncResult loadContentToRAG(Context baseCtx, LoadContentToRAGParams params) {
    mixin(baseContextToSpecific!RAGContext);

    if (ctx.getRAG() is null) {
        return ExecuteFuncResult("error: RAG not available", success: false);
    }

    if (auto e = checkTopic(ctx, params.topic)) {
        return ExecuteFuncResult(e, success: false);
    }
    if (auto e = checkAgentMdProtection(params.topic)) {
        return ExecuteFuncResult(e, success: false);
    }
    if (params.content.empty) {
        return ExecuteFuncResult("error: content must not be empty", success: false);
    }
    const ulong maxContentSize = 1024 * 1024; // 1MB
    if (params.content.length > maxContentSize) {
        return ExecuteFuncResult("error: content too large. Max $(maxContentSize) bytes".text,
                success: false);
    }

    try {
        auto result = ctx.getRAG().add(Document(Origin(Topic(params.topic)),
                params.content, Offset.init), ctx.getRagConfig());
        retrySql!(() => ctx.getRAG.fts5Rebuild);
        return ExecuteFuncResult(i"Content ($(result.length) length) added to '$(params.topic)' as $(
                result.chunks) chunks to the RAG".text, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: failed loading topic into rag: $(e.msg)".text,
                success: false);
    }
}

struct RemoveTopicFromRAGParams {
    @ParamDescription(
            "Topic name to remove from the RAG index (alphanumeric + underscore, limited length)")
    string topic;
}

@Function("Remove topic from RAG index", tags:
        ["rag"]) ExecuteFuncResult removeTopicFromRAG(Context baseCtx,
        RemoveTopicFromRAGParams params) {
    mixin(baseContextToSpecific!RAGContext);

    if (ctx.getRAG() is null) {
        return ExecuteFuncResult("error: RAG not available", success: false);
    }

    if (auto e = checkTopic(ctx, params.topic)) {
        return ExecuteFuncResult(e, success: false);
    }
    if (auto e = checkAgentMdProtection(params.topic)) {
        return ExecuteFuncResult(e, success: false);
    }

    try {
        const chunks = retrySql!(() => ctx.getRAG().removeSource(Origin(Topic(params.topic))));
        retrySql!(() => ctx.getRAG.fts5Rebuild);
        return ExecuteFuncResult(i"removed topic '$(params.topic)' with $(chunks) chunks from RAG".text,
                success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: failed to remove topic '$(params.topic)' from RAG: $(
                e.msg)".text, success: false);
    }
}

/**
 * Private helper: prefix each line of text with its absolute line number.
 * When appendLoc is true, splits text into lines and prefixes each with
 * "LINE_NUM→ " starting from startLineNumber.
 */
private string applyAppendLoc(string text, long startLineNumber, bool appendLoc) {
    if (!appendLoc)
        return text;

    auto buf = appender!(string)();
    auto lines = text.splitLines();
    long lineNum = startLineNumber;
    foreach (i, l; lines) {
        if (i > 0)
            buf.put('\n');
        buf.put(i"$(lineNum++)→ $(l)".text);
    }
    buf.put('\n');
    return buf[];
}

struct QueryReadFileParams {
    @ParamDescription("Path of the file in the RAG index. The exact indexed path or any suffix ending at a path boundary (e.g. the bare file name) works.")
    string filePath;

    @ParamDescription("Line number to read (1-based)")
    long lineNumber;

    @ParamDescription("Database name to search, or '*' for all databases")
    @ParamOptional string database = "*";

    @ParamDescription("Prefix each line with its line number")
    @ParamOptional bool appendLoc = true;
}

@Function("Read a specific line from a file in the RAG index. Resolves bare file names. Use readRAGSource to read a whole document",
    tags:
        ["rag"]) ExecuteFuncResult queryReadFile(Context baseCtx, QueryReadFileParams params) {
    mixin(baseContextToSpecific!RAGContext);

    if (ctx.getRAG() is null) {
        return ExecuteFuncResult("error: RAG not available", success: false);
    }
    if (params.filePath.empty) {
        return ExecuteFuncResult("error: filePath must not be empty", success: false);
    }
    if (params.lineNumber < 1) {
        return ExecuteFuncResult(i"error: lineNumber must be >= 1, got: $(params.lineNumber)".text,
                success: false);
    }
    if (!params.database.empty && !ctx.getRAG().databaseExists(params.database)) {
        return ExecuteFuncResult(i"error: database '$(params.database)' not found".text,
                success: false);
    }

    try {
        auto fileAsPath = Path(params.filePath);
        auto matches = ctx.getRAG().queryReadFile(fileAsPath, params.lineNumber, params.database);

        if (matches.length == 0) {
            if (!ctx.getRAG().hasFile(fileAsPath, params.database)) {
                return ExecuteFuncResult(i"error: file '$(params.filePath)' not found in RAG index".text,
                        success: false);
            }
            return ExecuteFuncResult(i"error: file '$(params.filePath)' exists in RAG but no chunk contains line $(
                    params.lineNumber)".text, success: false);
        }

        string[] resultBlocks;
        foreach (i, match; matches) {
            string originStr = match.origin.match!((Topic a) => i"topic: '$(a.name)'".text,
                    (Url a) => a.value, (Path a) => a.toString);

            string text = applyAppendLoc(match.data, match.line.begin, params.appendLoc);

            resultBlocks ~= format("--- Result %s ('%s' in database '%s' line %s-%s chars %s-%s) ---\n%s", i + 1,
                    originStr, match.databaseName, match.line.begin,
                    match.line.end, match.offset.begin, match.offset.end, text);
        }

        return ExecuteFuncResult(resultBlocks.join("\n\n"), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: database error during query: $(e.msg)".text,
                success: false);
    }
}

struct ListRAGSourcesParams {
    @ParamDescription(
            "Only list sources whose path/topic/url contains this filter (case-insensitive). Empty lists all sources")
    @ParamOptional string filter;

    @ParamDescription("Database name to list, or '*' for all databases")
    @ParamOptional string database = "*";

    @ParamDescription("Maximum number of sources to list")
    @ParamOptional long limit = 50;
}

@Function("List documents (sources) indexed in the RAG: file paths, topics and URLs with chunk counts. Use this to resolve the exact stored path of a document, then read it with readRAGSource",
    tags:
        ["rag"]) ExecuteFuncResult listRAGSources(Context baseCtx, ListRAGSourcesParams params) {
    mixin(baseContextToSpecific!RAGContext);

    if (ctx.getRAG() is null) {
        return ExecuteFuncResult("error: RAG not available", success: false);
    }
    if (params.limit < 1) {
        return ExecuteFuncResult(i"error: limit must be >= 1, got: $(params.limit)".text,
                success: false);
    }
    if (!params.database.empty && !ctx.getRAG().databaseExists(params.database)) {
        return ExecuteFuncResult(i"error: database '$(params.database)' not found".text,
                success: false);
    }

    try {
        auto all = ctx.getRAG().listSources(params.database);
        const filterLower = params.filter.toLower;
        auto matched = all.filter!(a => filterLower.empty
                || a.origin.toLower.canFind(filterLower)).array;

        if (matched.empty) {
            return ExecuteFuncResult(i"no sources found matching filter '$(params.filter)'".text,
                    success: true);
        }

        auto lines = appender!(string[])();
        foreach (i, s; matched) {
            if (i >= params.limit)
                break;
            lines.put(i"  [$(s.database)] $(s.origin) ($(s.chunks) chunks)".text);
        }
        const shown = matched.length > params.limit ? params.limit : matched.length;
        const header = i"Sources in RAG ($(matched.length) matched, showing $(shown)):".text;
        return ExecuteFuncResult(header ~ "\n" ~ lines[].join("\n"), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: failed to list RAG sources: $(e.msg)".text,
                success: false);
    }
}

struct ReadRAGSourceParams {
    @ParamDescription("Path of the document in the RAG index. The exact indexed path or any suffix ending at a path boundary (e.g. the bare file name) works")
    string filePath;

    @ParamDescription("Database name to read from, or '*' for all databases")
    @ParamOptional string database = "*";

    @ParamDescription("Maximum number of bytes to return per document")
    @ParamOptional long maxBytes = 65536;
}

@Function("Read a whole document from the RAG index by path (resolves bare file names). Use it to read a document that another result referenced",
    tags:
        ["rag"]) ExecuteFuncResult readRAGSource(Context baseCtx, ReadRAGSourceParams params) {
    mixin(baseContextToSpecific!RAGContext);

    if (ctx.getRAG() is null) {
        return ExecuteFuncResult("error: RAG not available", success: false);
    }
    if (params.filePath.empty) {
        return ExecuteFuncResult("error: filePath must not be empty", success: false);
    }
    if (params.maxBytes < 1024) {
        return ExecuteFuncResult("error: maxBytes must be >= 1024", success: false);
    }

    try {
        auto matches = ctx.getRAG().readSource(Path(params.filePath), params.database);
        if (matches.length == 0) {
            return ExecuteFuncResult(i"error: no document ending with '$(params.filePath)' found in the RAG index"
                    .text, success: false);
        }

        auto blocks = appender!(string[])();
        foreach (i, m; matches) {
            auto text = truncateUtf8(m.text, params.maxBytes);
            const truncated = text.length < m.text.length;
            const suffixText = truncated ? ", truncated" : "";
            auto header = i"--- Document $(i + 1) '$(RAG.originLabel(m.origin))' in database '$(
                    m.databaseName)' ($(m.chunks) chunks, $(m.text.length) bytes$(suffixText)) ---"
                .text;
            blocks.put(header ~ "\n" ~ text);
        }

        return ExecuteFuncResult(blocks[].join("\n\n"), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: failed to read document from RAG: $(e.msg)".text,
                success: false);
    }
}

private:

/// Cut `text` to at most `maxBytes` bytes without splitting a UTF-8
/// codepoint (the cut walks back off continuation bytes).
string truncateUtf8(string text, long maxBytes) {
    if (text.length <= cast(size_t) maxBytes)
        return text;
    size_t end = cast(size_t) maxBytes;
    while (end > 0 && (text[end] & 0xC0) == 0x80)
        --end;
    return text[0 .. end];
}

string fts5Help(long mode, string query) {
    import llm.rag.database : fts5Help, fts5SimpleHelp;

    auto help = mode == 0 ? fts5Help : fts5SimpleHelp;
    return i"\nFiltered textQuery: $(cleanFts5(query))\n\ntextQuery must be according to the FTS5 syntax.\n$(
            help)".text;
}
