module llm.rag.database;

import core.thread : Thread;
import logger = std.experimental.logger;
import std.algorithm : map, filter, cache;
import std.array : appender, empty;
import std.conv : to;
import std.datetime : dur, SysTime;
import std.exception : collectException, ifThrown;
import std.format : format;
import std.meta : AliasSeq;
import std.sumtype : match;
import std.typecons : Tuple, tuple;

static import miniorm;
import miniorm : ColumnName, TablePrimaryKey, Miniorm, toSqliteDateTime,
    TableConstraint, TableForeignKey, KeyRef, KeyParam, ColumnParam;
import my.path;
import my.named_type;
import my.optional;

public import llm.rag.rag : Topic, Url, Origin, Document, Offset, Line;

immutable timeout = 30.dur!"seconds";
enum SchemaVersion = 6;

private struct VersionTbl {
    @ColumnName("version")
    ulong version_;
    long embedDimensions;
    string model;
}

@TableConstraint("unique_ UNIQUE (urlType, checksum)")
private struct SourceTbl {
    long id;
    long urlType;
    long checksum;
    SysTime added;

    enum UrlType {
        topic,
        url,
        path
    }
}

@TableForeignKey("sourceId", KeyRef("SourceTbl(id)"), KeyParam("ON DELETE CASCADE"))
@TableConstraint("unique_ UNIQUE (sourceId, url)")
private struct OriginUrlTbl {
    long id;
    long sourceId;
    string url;
}

SourceTbl.UrlType convert(Origin x) {
    return x.match!((Topic _) => SourceTbl.UrlType.topic,
            (Path _) => SourceTbl.UrlType.path, (Url _) => SourceTbl.UrlType.url);
}

// I am not sure this is correct but the database has stopped being corrupted
@TableForeignKey("embedId", KeyRef("EmbeddingsTbl_rowids(rowid)"), KeyParam("ON DELETE CASCADE"))
@TableConstraint("unique_ UNIQUE (embedId, charBeginPos, charEndPos)")
private struct TextChunkTbl {
    long id;
    long embedId;
    string text;
    long charBeginPos;
    long charEndPos;
    long lineBegin;
    long lineEnd;
}

private immutable EmbeddingsTblSql = `
CREATE VIRTUAL TABLE EmbeddingsTbl USING vec0(
    id INTEGER PRIMARY KEY,
    sourceId INTEGER NOT NULL,
    embedding FLOAT[%s]
);`;
// FOREIGN KEY(source_id) REFERENCES SourceTbl(id) ON DELETE CASCADE
// FOREIGN KEY(textChunkId) REFERENCES TextChunkTbl(id) ON DELETE CASCADE

// FTS5 virtual table with external content mode - reads directly from TextChunkTbl
private immutable FTSChunksSql = `
CREATE VIRTUAL TABLE FtsChunksTbl USING fts5(
    text,
    content='TextChunkTbl',
    content_rowid='id',
    tokenize='unicode61'
)`;

Optional!Database openDatabase(AbsolutePath dbFile_, string model,
        long embedDimensions, bool readOnly = false, bool inMemory = false) nothrow {
    import std.file : exists;
    import std.path : dirName;
    import llm.rag.sqlite3_vec;
    import my.file : getAttrs;
    import core.sys.posix.sys.stat;

    string dbFile = inMemory ? ":memory:" : dbFile_.toString;

    static void setPragmas(ref Miniorm db) {
        // dfmt off
        auto pragmas = [
            // required for foreign keys with cascade to work
            "PRAGMA foreign_keys=ON;",
            // "PRAGMA journal_mode=WAL;",
            // "PRAGMA synchronous=FULL;"
        ];
        // dfmt on

        foreach (p; pragmas) {
            db.run(p);
        }
    }

    logger.trace("opening database ", dbFile).collectException;
    auto dbDir = dbFile.dirName;
    if (readOnly && !dbFile.exists) {
        logger.warningf("Requested read-only database does not exist: %s", dbFile).collectException;
        return none!Database();
    } else if (!dbDir.exists) {
        logger.tracef("No RAG database opened. Directory does not exist: '%s'",
                dbDir).collectException;
        return none!Database();
    } else if (!readOnly) {
        uint attrs;
        if (!getAttrs(dbDir.Path, attrs)) {
            logger.tracef("Unable to get file permissions: '%s'", dbDir).collectException;
            return none!Database();
        }
        if ((attrs & (S_IWUSR)) == 0) {
            logger.tracef("No RAG database opened. Directory is not writable: '%s'",
                    dbDir).collectException;
            return none!Database();
        }
    }

    for (int counter; counter < 10; ++counter) {
        try {
            auto db = Miniorm(dbFile, readOnly ? SQLITE_OPEN_READONLY
                    : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE));
            setPragmas(db);
            sqlite3_vec_init(cast(sqlite3*) db.handle, null, null);
            const versionData = () {
                auto stmt = db.prepare("SELECT version FROM VersionTbl");
                foreach (ref r; stmt.get.execute) {
                    if (r.peek!long(0) == SchemaVersion) {
                        foreach (a; db.run(miniorm.select!VersionTbl)) {
                            return a;
                        }
                    } else {
                        return VersionTbl(r.peek!long(0));
                    }
                }
                return VersionTbl(0);
            }().ifThrown(VersionTbl(0));

            alias Schema = AliasSeq!(VersionTbl, SourceTbl, OriginUrlTbl, TextChunkTbl);

            bool mismatch = versionData.version_ < SchemaVersion
                || versionData.embedDimensions != embedDimensions;

            if (mismatch && readOnly) {
                logger.warningf(
                        "Unable to open '%s' because there is a mismatch between expected configuration of the database.",
                        dbFile);
                logger.warningf(SchemaVersion != versionData.version_,
                        "Expected schema version %s but database has %s",
                        SchemaVersion, versionData.version_);
                logger.warningf(embedDimensions != versionData.embedDimensions,
                        "Expected embed dimensions %s but database have %s",
                        embedDimensions, versionData.embedDimensions);
                logger.warningf(model != versionData.model,
                        "Expected model name '%s' but database have '%s'", model, versionData.model);
                return none!Database();
            }

            if (versionData.version_ < SchemaVersion
                    || versionData.embedDimensions != embedDimensions || versionData.model != model) {
                logger.tracef("Updating database to: schema version %s->%s dimensions %s->%s model %s->%s",
                        versionData.version_, SchemaVersion,
                        versionData.embedDimensions, embedDimensions, versionData.model, model);
                auto trans = db.transaction;
                static foreach (tbl; Schema)
                    db.run("DROP TABLE " ~ tbl.stringof).collectException;
                db.run("DROP TABLE EmbeddingsTbl").collectException;
                db.run("DROP TABLE FtsChunksTbl").collectException;
                db.run(miniorm.buildSchema!Schema);
                db.run(format!EmbeddingsTblSql(embedDimensions));
                db.run(FTSChunksSql);
                db.run(miniorm.insert!VersionTbl, VersionTbl(SchemaVersion,
                        embedDimensions, model));
                trans.commit;
            }
            return Database(db, embedDimensions).some;
        } catch (Exception e) {
            logger.trace(e).collectException;
            logger.warningf("Trying to open/create database '%s' (%s): %s",
                    dbFile, counter, e.msg).collectException;
        }

        Thread.sleep(50.dur!"msecs");
    }
    logger.warningf("Failed to open database '%s'", dbFile).collectException;
    return none!Database();
}

alias SourceId = NamedType!(long, Tag!"SourceId", 0, Comparable, TagStringable);
alias SourceChecksum = NamedType!(long, Tag!"SourceChecksum", 0, Comparable, TagStringable);

struct Source {
    Origin origin;
    SourceChecksum checksum;
    SysTime added;
}

struct Embedding {
    Offset offset;
    Line line;
    string text;
    float[] embed;
}

struct TextChunk {
    Offset offset;
    Line line;
    string text;
}

// TODO: remove by moving embedId to TextChunk
struct TextChunkWithEmbed {
    Offset offset;
    Line line;
    string text;
    long embedId;
}

struct Search {
    float[] embed;
}

struct SourceMatch {
    Origin origin;
    Offset offset;
    Line line;
    string text;
    double rank;
    SysTime added;
}

struct Database {
    Miniorm db;
    alias db this;

    private {
        long embedDimensions;
    }

    this(Miniorm db, long embedDimensions) {
        this.db = db;
        this.embedDimensions = embedDimensions;
    }

    void destroy() {
        db.close();
    }

    SourceId addSource(Source src) {
        import std.datetime : Clock;

        void addOrigin(long srcId, string url) {
            static immutable sql = "INSERT OR IGNORE INTO OriginUrlTbl (sourceId, url) VALUES(:sourceId, :url)";
            auto stmt = db.prepare(sql);
            stmt.get.bind(":sourceId", srcId);
            stmt.get.bind(":url", url);
            stmt.get.execute;
        }

        static immutable sql = "INSERT OR IGNORE INTO SourceTbl (urlType, checksum, added) VALUES(:urlType, :checksum, :added)";

        auto stmt = db.prepare(sql);
        stmt.get.bind(":urlType", cast(long) convert(src.origin));
        stmt.get.bind(":checksum", src.checksum.get);
        stmt.get.bind(":added", Clock.currTime.toSqliteDateTime);
        stmt.get.execute;

        if (db.changes == 1) {
            const id = db.lastInsertRowid;
            src.origin.match!((Topic a) => addOrigin(id, a.name),
                    (Path a) => addOrigin(id, a.toString), (Url a) => addOrigin(id, a.value));
            return SourceId(id);
        }
        return getSource(src.origin).match!((None _) => SourceId.init, a => a.id);
    }

    Optional!(Tuple!(Source, "src", SourceId, "id")) getSource(Origin origin) {
        alias ReturnT = typeof(return);
        ReturnT urlSource(string url) {
            static immutable sql = "SELECT t0.id, t0.checksum, t0.added FROM SourceTbl t0, OriginUrlTbl t1 WHERE "
                ~ "t0.urlType=:urlType AND t0.id=t1.sourceId AND t1.url=:url";
            auto stmt = db.prepare(sql);
            stmt.get.bind(":urlType", cast(long) convert(origin));
            stmt.get.bind(":url", url);
            foreach (ref r; stmt.get.execute) {
                auto src = Source(origin, r.peek!long(1).SourceChecksum,
                        miniorm.fromSqLiteDateTime(r.peek!string(2)));
                auto srcId = r.peek!long(0).SourceId;
                return tuple!("src", "id")(src, srcId).some;
            }
            return ReturnT(None.init);
        }

        return origin.match!((Topic a) => urlSource(a.name),
                (Path a) => urlSource(a.toString), (Url a) => urlSource(a.value));
    }

    Optional!Source getSource(SourceId id) {
        Optional!Source getUrl(SourceTbl.UrlType kind) {
            static immutable sql = "SELECT t0.checksum, t1.url, t0.added FROM SourceTbl t0, OriginUrlTbl t1 "
                ~ "WHERE t0.id=:id AND t0.id=t1.sourceId";
            auto stmt = db.prepare(sql);
            stmt.get.bind(":id", id.get);
            foreach (ref r; stmt.get.execute) {
                auto added = miniorm.fromSqLiteDateTime(r.peek!string(2));
                if (kind == SourceTbl.UrlType.url)
                    return some(Source(Origin(Url(r.peek!string(1))),
                            r.peek!long(0).SourceChecksum, added));
                if (kind == SourceTbl.UrlType.path)
                    return some(Source(Origin(Path(r.peek!string(1))),
                            r.peek!long(0).SourceChecksum, added));
                if (kind == SourceTbl.UrlType.topic)
                    return some(Source(Origin(Topic(r.peek!string(1))),
                            r.peek!long(0).SourceChecksum, added));
            }
            return none!Source();
        }

        static immutable kindSql = "SELECT urlType FROM SourceTbl WHERE id=:id";
        auto stmt = db.prepare(kindSql);
        stmt.get.bind(":id", id.get);
        foreach (ref r; stmt.get.execute) {
            const kind = cast(SourceTbl.UrlType) r.peek!long(0);
            return getUrl(kind);
        }
        return none!Source();
    }

    Source[] getSources() {
        static immutable sql = "SELECT id FROM SourceTbl";

        auto rval = appender!(Source[])();
        auto stmt = db.prepare(sql);
        auto res = stmt.get.execute;
        foreach (ref r; res) {
            getSource(r.peek!long(0).SourceId).match!((None _) {}, (Source a) => rval.put(a));
        }
        return rval[];
    }

    /// Reconstruct a source's full text from its chunks: chunks are read in
    /// charBeginPos order and each chunk's leading graphemes overlapping the
    /// previous chunk's end are stripped (the sliding-window overlap), so the
    /// result equals the original document text (chunks are parts of one
    /// document - no separator). Returns "" for a source without chunks; SQL
    /// errors propagate to the caller (the worker catches them).
    string sourceText(SourceId id) {
        import std.conv : text;
        import std.range : drop;
        import std.uni : byGrapheme, byCodePoint;

        static immutable sql = `SELECT t.text, t.charBeginPos, t.charEndPos FROM TextChunkTbl t
            JOIN EmbeddingsTbl e ON t.embedId = e.id
            WHERE e.sourceId = :id ORDER BY t.charBeginPos`;

        auto rval = appender!string();
        long prevEnd;
        auto stmt = db.prepare(sql);
        stmt.get.bind(":id", id.get);
        foreach (ref r; stmt.get.execute) {
            const string chunk = r.peek!string(0);
            const long begin = r.peek!long(1);
            const long end = r.peek!long(2);
            const size_t k = begin < prevEnd ? cast(size_t)(prevEnd - begin) : 0;
            rval.put(chunk.byGrapheme.drop(k).byCodePoint.text);
            prevEnd = end;
        }
        return rval[];
    }

    long removeSource(Origin origin) {
        return getSource(origin).match!((None _) => 0, a => removeSource(a.id));
    }

    bool hasSource(Source src) {
        static immutable sql = "SELECT count(*) FROM SourceTbl WHERE urlType=:urlType AND checksum=:checksum";

        auto stmt = db.prepare(sql);
        stmt.get.bind(":urlType", cast(long) convert(src.origin));
        stmt.get.bind(":checksum", src.checksum.get);
        auto res = stmt.get.execute;
        return res.oneValue!long != 0;
    }

    bool hasFile(Path path) {
        static immutable sql = "SELECT t1.url FROM SourceTbl as t0, OriginUrlTbl as t1 WHERE t0.urlType=:urlType AND t0.id=t1.sourceId AND t1.url=:url";

        auto stmt = db.prepare(sql);
        stmt.get.bind(":urlType", cast(long) SourceTbl.UrlType.path);
        stmt.get.bind(":url", path.toString);

        foreach (ref r; stmt.get.execute) {
            return true;
        }
        return false;
    }

    /// Return: embeddings removed
    long removeSource(SourceId id) {
        static immutable sql = "DELETE FROM SourceTbl WHERE id=:id";
        static immutable embedSql = "DELETE FROM EmbeddingsTbl WHERE sourceId=:id";

        auto stmt = db.prepare(sql);
        stmt.get.bind(":id", id.get);
        stmt.get.execute();

        stmt = db.prepare(embedSql);
        stmt.get.bind(":id", id.get);
        stmt.get.execute();
        auto embedRemoved = db.changes; // from embedSql DELETE

        cleanupEmbeddings();
        auto cleanupRemoved = db.changes; // from cleanup

        return embedRemoved + cleanupRemoved;
    }

    void cleanupEmbeddings() {
        static immutable sql = "DELETE FROM EmbeddingsTbl WHERE NOT EXISTS (SELECT id FROM SourceTbl)";
        auto stmt = db.prepare(sql);
        stmt.get.execute;
    }

    private float[] fixDimension(float[] embed) {
        if (embed.length == embedDimensions)
            return embed;
        if (embed.length > embedDimensions)
            return embed[0 .. embedDimensions];
        auto r = embed;
        r.length = embedDimensions;
        r[embed.length .. $] = 0.0;
        return r;
    }

    void addEmbedding(SourceId id, Embedding emb) {
        static immutable embedSql = "INSERT INTO EmbeddingsTbl (sourceId, embedding) VALUES(:sourceId, :embedding)";
        static immutable chunkSql = "INSERT INTO TextChunkTbl (embedId, text, charBeginPos, charEndPos, lineBegin, lineEnd) VALUES(:embedId, :text, :charBeginPos, :charEndPos, :lineBegin, :lineEnd)";

        {
            auto stmt = db.prepare(embedSql);
            stmt.get.bind(":sourceId", id.get);
            stmt.get.bind(":embedding", fixDimension(emb.embed));
            stmt.get.execute;
        }
        auto embedId = db.lastInsertRowid;

        {
            auto stmt = db.prepare(chunkSql);
            stmt.get.bind(":embedId", embedId);
            stmt.get.bind(":text", emb.text);
            stmt.get.bind(":charBeginPos", emb.offset.begin);
            stmt.get.bind(":charEndPos", emb.offset.end);
            stmt.get.bind(":lineBegin", emb.line.begin);
            stmt.get.bind(":lineEnd", emb.line.end);
            stmt.get.execute;
        }
    }

    private TextChunk getChunk(long embedId) {
        static immutable sql = "SELECT text,charBeginPos,charEndPos,lineBegin,lineEnd FROM TextChunkTbl WHERE embedId=:id";
        auto stmt = db.prepare(sql);
        stmt.get.bind(":id", embedId);

        foreach (ref r; stmt.get.execute) {
            return TextChunk(offset: Offset(begin: r.peek!long(1), end: r.peek!long(2)),
                    Line(begin: r.peek!long(3), end: r.peek!long(4)), text: r.peek!string(0));
        }
        return TextChunk.init;
    }

    SourceMatch[] querySemantic(Search search, long limit) {
        static immutable embedSql = "SELECT id,sourceId," ~ "row_number() OVER (ORDER BY distance) as rank FROM EmbeddingsTbl WHERE embedding MATCH :embedding AND k = :limit ORDER BY distance";

        auto stmt = db.prepare(embedSql);
        stmt.get.bind(":embedding", fixDimension(search.embed));
        stmt.get.bind(":limit", limit);
        auto ids = appender!(Tuple!(long, "embedId", long, "sourceId", double, "rank")[])();
        foreach (ref r; stmt.get.execute) {
            ids.put(tuple!("embedId", "sourceId", "rank")(r.peek!long(0),
                    r.peek!long(1), r.peek!double(2)));
        }
        logger.trace("Hits ", ids[].length);

        auto rval = appender!(SourceMatch[])();
        foreach (id; ids[]) {
            auto src = getSource(id.sourceId.SourceId);
            src.match!((Source src) {
                auto chunk = getChunk(id.embedId);
                rval.put(SourceMatch(src.origin, offset: chunk.offset, line: chunk.line,
                    text: chunk.text, rank: id.rank, added: src.added));
            }, (None _) {});
        }

        return rval[];
    }

    SourceMatch[] queryTextSearch(string query, long limit) {
        static immutable ftsSql = "SELECT rowid, rank "
            ~ "FROM FtsChunksTbl WHERE FtsChunksTbl MATCH :query ORDER BY rank LIMIT :limit";

        try {
            auto stmt = db.prepare(ftsSql);
            stmt.get.bind(":query", query);
            stmt.get.bind(":limit", limit);

            auto results = appender!(Tuple!(long, "rowid", double, "rank")[])();
            foreach (ref r; stmt.get.execute) {
                results.put(tuple!("rowid", "rank")(r.peek!long(0), r.peek!double(1)));
            }
            logger.trace("Hits ", results[].length);

            auto rval = appender!(SourceMatch[])();
            foreach (res; results) {
                // rowid in FtsChunksTbl maps to TextChunkTbl.id (content_rowid='id')
                auto chunk = getChunkByRowid(res.rowid);
                if (!chunk.text.empty) {
                    auto src = getSourceByEmbedId(chunk.embedId);
                    src.match!((Source src) {
                        rval.put(SourceMatch(src.origin, offset: chunk.offset,
                            line: chunk.line, text: chunk.text, rank: res.rank, added: src.added));
                    }, (None _) {});
                }
            }

            return rval[];
        } catch (Exception e) {
            logger.trace(e.msg);
        }
        return null;
    }

    SourceMatch[] queryByPathAndLine(Path filePath, long lineNumber) {
        // dfmt off
         static immutable sql = "SELECT t0.text, t0.charBeginPos, t0.charEndPos, t0.lineBegin, t0.lineEnd, t3.url, t2.added "
             ~ "FROM TextChunkTbl t0 "
             ~ "JOIN EmbeddingsTbl t1 ON t0.embedId = t1.id "
             ~ "JOIN SourceTbl t2 ON t1.sourceId = t2.id "
             ~ "JOIN OriginUrlTbl t3 ON t2.id = t3.sourceId "
             ~ "WHERE t2.urlType = :urlType AND t3.url = :url "
             ~ "AND t0.lineBegin <= :lineNumber AND t0.lineEnd >= :lineNumber";
        // dfmt on

        auto stmt = db.prepare(sql);
        stmt.get.bind(":urlType", cast(long) SourceTbl.UrlType.path);
        stmt.get.bind(":url", filePath.toString);
        stmt.get.bind(":lineNumber", lineNumber);

        auto rval = appender!(SourceMatch[])();
        foreach (ref r; stmt.get.execute) {
            rval.put(SourceMatch(Origin(Path(r.peek!string(5))), offset: Offset(begin: r.peek!long(1),
                    end: r.peek!long(2)), line: Line(begin: r.peek!long(3),
                    end: r.peek!long(4)), text: r.peek!string(0), rank: 0,
                    added: miniorm.fromSqLiteDateTime(r.peek!string(6))));
        }

        logger.tracef("queryByPathAndLine hits %s for %s line %s",
                rval[].length, filePath, lineNumber);
        return rval[];
    }

    SourceMatch[] queryCombineSemanticText(Search embedding, string query, long limit) {
        static immutable sql = `
WITH vec_matches AS (
  SELECT
    id AS rowid,                      -- EmbeddingsTbl.id
    row_number() OVER (ORDER BY distance) AS rank_number
  FROM EmbeddingsTbl
  WHERE embedding MATCH :embedding
    AND k = :limit
),
fts_matches AS (
  SELECT
    rowid,                            -- TextChunkTbl.id
    row_number() OVER (ORDER BY rank) AS rank_number
  FROM FtsChunksTbl
  WHERE text MATCH :text_query
  LIMIT :limit
)
SELECT
  id,
  (
    1.0 / (60 + coalesce(vec_matches.rank_number, 1000))
    + 1.0 / (60 + coalesce(fts_matches.rank_number, 1000))
  ) AS fusion_score
FROM TextChunkTbl
LEFT JOIN vec_matches ON TextChunkTbl.embedId = vec_matches.rowid
LEFT JOIN fts_matches ON TextChunkTbl.id = fts_matches.rowid
ORDER BY fusion_score DESC;
`;

        try {
            auto stmt = db.prepare(sql);
            stmt.get.bind(":embedding", embedding.embed);
            stmt.get.bind(":text_query", query);
            stmt.get.bind(":limit", limit);

            auto results = appender!(Tuple!(long, "id", double, "rank")[])();
            foreach (ref r; stmt.get.execute) {
                // TODO: this should not be needed
                if (results[].length >= limit)
                    break;
                results.put(tuple!("id", "rank")(r.peek!long(0), r.peek!double(1)));
            }
            logger.trace("Hits ", results[].length);

            auto rval = appender!(SourceMatch[])();
            foreach (res; results[].map!(a => tuple(getChunkByRowid(a.id), a.rank))
                    .cache
                    .filter!(a => !a[0].text.empty)) {
                auto src = getSourceByEmbedId(res[0].embedId);
                src.match!((Source src) {
                    rval.put(SourceMatch(src.origin, offset: res[0].offset,
                        line: res[0].line, text: res[0].text, rank: res[1], added: src.added));
                }, (None _) {});
            }
            return rval[];
        } catch (Exception e) {
            logger.trace(e.msg);
        }
        return null;
    }

    private TextChunkWithEmbed getChunkByRowid(long rowid) {
        static immutable sql = "SELECT text,charBeginPos,charEndPos,lineBegin,lineEnd,embedId "
            ~ "FROM TextChunkTbl WHERE id=:id";
        auto stmt = db.prepare(sql);
        stmt.get.bind(":id", rowid);

        foreach (ref r; stmt.get.execute) {
            return TextChunkWithEmbed(offset: Offset(begin: r.peek!long(1),
                    end: r.peek!long(2)), line: Line(begin: r.peek!long(3),
                    end: r.peek!long(4)), text: r.peek!string(0), embedId: r.peek!long(5));
        }
        return TextChunkWithEmbed.init;
    }

    // TODO: return type should be a SourceId, not Source
    Optional!Source getSourceByEmbedId(long embedId) {
        static immutable sql = "SELECT sourceId FROM EmbeddingsTbl WHERE id=:embedId";
        auto stmt = db.prepare(sql);
        stmt.get.bind(":embedId", embedId);

        foreach (ref r; stmt.get.execute) {
            return getSource(r.peek!long(0).SourceId);
        }
        return none!Source();
    }

    /// Compact the database by running a VACUUM operation
    void vacuum() {
        db.run("VACUUM");
    }

    /// Must be called for the index to reflect the changes to TextChunkTbl
    void fts5Rebuild() {
        db.run("INSERT INTO FtsChunksTbl(FtsChunksTbl) VALUES('rebuild')");
    }
}

immutable string fts5SimpleHelp = `Full-text query text search. Each word separated by a space is an implicit boolean AND`;

immutable string fts5Help = q"(Full-text query syntax for FTS5 sqlite manual.

Only alphanumerics, underscore, `(`, `)`, `*`, and `^` are accepted unquoted.
Any other token is automatically wrapped in double quotes as a literal.

- Boolean: AND, OR, NOT (uppercase). Precedence, highest to lowest:
  implicit AND (whitespace) > NOT > AND > OR.
- Grouping: `( )` for sub-expressions.
  Note: `(a OR b) c` is a syntax error. Use `(a OR b) AND c` explicitly.
- Terms: barewords are alphanumerics + underscore. Anything else is auto-quoted.
- Prefix: `term*` matches terms starting with "term". Keep `*` outside any quotes.
- Start-of-column: `^term` matches only if `term` is the first token of a column.
- Proximity: `NEAR(term1 term2, N)` matches terms within N tokens (default 10).
  The comma is required between the last term and the count.
- Commas are reserved for `NEAR(...)`. Do not use them anywhere else.

Column filters (`colname:` or `{col1 col2}:`) are NOT supported and will error.
Use listRAGDatabases to discover available database names.

Examples:
  test AND code
  (test OR unittest) AND NOT python
  NEAR(code block, 3)
  ^header AND body*

BNF:

  <term>      := [^] string[*]
  <neargroup> := NEAR ( <term> <term> ... [, N] )
  <query>     := <query> AND <query>
  <query>     := <query> OR <query>
  <query>     := <query> NOT <query>
)";

string cleanFts5(string s) {
    import std.algorithm : all, canFind, map, splitter;
    import std.ascii : isAlphaNum;
    import std.range : enumerate;
    import std.string : join, replace;
    import std.uni : byCodePoint;

    // Break parentheses into standalone tokens so they can be recognized.
    // Commas are handled context-sensitively below, not padded here.
    string padded = s.replace("(", " ( ").replace(")", " ) ");

    // Bareword: optional '^', then alphanumerics + '_', then optional '*'.
    static bool isBareword(string t) {
        if (t.length == 0)
            return false;
        size_t end = t.length;
        if (t[$ - 1] == '*')
            end--;
        if (end == 0)
            return false;
        size_t start = 0;
        if (t[0] == '^')
            start++;
        if (start >= end)
            return false;
        return t[start .. end].byCodePoint.all!(c => c.isAlphaNum || c == '_');
    }

    static string quoteIfNeeded(string t) {
        if (t == "(" || t == ")")
            return t;
        if (t.byCodePoint.all!(c => c == '*' || c == '^'))
            return "";
        if (isBareword(t))
            return t;
        return "\"" ~ t.replace(`"`, `""`) ~ "\"";
    }

    // Walk tokens, tracking NEAR-paren depth so commas are only syntax
    // inside a NEAR(...) group.
    string[] outTokens;
    int nearDepth = 0;
    bool nearPending = false;

    foreach (tok; padded.splitter) {
        if (tok == "NEAR") {
            outTokens ~= "NEAR";
            nearPending = true;
            continue;
        }
        if (tok == "(") {
            if (nearPending) {
                nearDepth++;
                nearPending = false;
            }
            outTokens ~= "(";
            continue;
        }
        if (tok == ")") {
            if (nearDepth > 0)
                nearDepth--;
            nearPending = false;
            outTokens ~= ")";
            continue;
        }
        nearPending = false;

        if (nearDepth > 0 && tok.canFind(',')) {
            // Inside NEAR(...): split on commas; emit each piece quoted
            // normally and each comma as a standalone token.
            foreach (part; tok.splitter(',').enumerate) {
                if (part.index > 0)
                    outTokens ~= ",";
                if (part.value.length > 0)
                    outTokens ~= quoteIfNeeded(part.value);
            }
            continue;
        }

        // Outside NEAR: a bare comma is dropped; an embedded comma stays
        // glued to the token and gets quoted (tokenizer will strip it).
        if (tok == ",")
            continue;

        auto q = quoteIfNeeded(tok);
        if (q.length > 0)
            outTokens ~= q;
    }

    return outTokens.join(" ");
}
