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
import miniorm : ColumnName, TablePrimaryKey, Miniorm, spinSql, toSqliteDateTime,
    TableConstraint, TableForeignKey, KeyRef, KeyParam, ColumnParam;
import my.path;
import my.named_type;
import my.optional;

public import llm.rag.rag : Topic, Url, Origin, Document, Offset, Line;

immutable timeout = 30.dur!"seconds";
enum SchemaVersion = 5;

private struct VersionTbl {
    @ColumnName("version")
    ulong version_;
    long embedDimensions;
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

Optional!Database openDatabase(AbsolutePath dbFile_, long embedDimensions, bool readOnly = false) nothrow {
    import std.file : exists;
    import std.path : dirName;
    import llm.rag.sqlite3_vec;
    import my.file : getAttrs;
    import core.sys.posix.sys.stat;

    string dbFile = dbFile_.toString;

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
    if (!dbDir.exists) {
        if (readOnly) {
            logger.warningf("Requested read-only database directory does not exist: %s, falling back to in-memory",
                    dbDir).collectException;
        } else {
            logger.tracef("No RAG database opened. Directory does not exist: '%s'",
                    dbDir).collectException;
        }
        dbFile = ":memory:";
    } else if (!readOnly) {
        uint attrs;
        if (!getAttrs(dbDir.Path, attrs)) {
            logger.tracef("Unable to get file permissions: '%s'", dbDir).collectException;
        }
        if ((attrs & (S_IWUSR)) == 0) {
            logger.tracef("No RAG database opened. Directory is not writable: '%s'",
                    dbDir).collectException;
            dbFile = ":memory:";
        }
    }

    for (int counter; counter < 100; ++counter) {
        ++counter;
        try {
            auto db = Miniorm(dbFile, readOnly ? SQLITE_OPEN_READONLY
                    : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE));
            setPragmas(db);
            sqlite3_vec_init(cast(sqlite3*) db.handle, null, null);
            const versionData = () {
                foreach (a; db.run(miniorm.select!VersionTbl))
                    return a;
                return VersionTbl(0);
            }().ifThrown(VersionTbl(0));

            alias Schema = AliasSeq!(VersionTbl, SourceTbl, OriginUrlTbl, TextChunkTbl);

            bool mismatch = versionData.version_ < SchemaVersion
                || versionData.embedDimensions != embedDimensions;

            if (mismatch && readOnly) {
                logger.warningf("Unable to open '%s' because there is a mismatch between either db schema or embedding dimensions. Expect schema %s, db has schema %s. Expected dimensions %s, db has %s dimensions",
                        dbFile, SchemaVersion, versionData.version_,
                        embedDimensions, versionData.embedDimensions);
                return none!Database();
            }

            if (versionData.version_ < SchemaVersion
                    || versionData.embedDimensions != embedDimensions) {
                logger.tracef("Updating database to schema version %s->%s with %s->%s dimensions",
                        versionData.version_,
                        SchemaVersion, versionData.embedDimensions, embedDimensions);
                auto trans = db.transaction;
                static foreach (tbl; Schema)
                    db.run("DROP TABLE " ~ tbl.stringof).collectException;
                db.run("DROP TABLE EmbeddingsTbl").collectException;
                db.run("DROP TABLE FtsChunksTbl").collectException;
                db.run(miniorm.buildSchema!Schema);
                db.run(format!EmbeddingsTblSql(embedDimensions));
                db.run(FTSChunksSql);
                db.run(miniorm.insert!VersionTbl, VersionTbl(SchemaVersion, embedDimensions));
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
        this.db.log((string m) => logger.trace(m));
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
            static immutable sql = "SELECT t0.id, t0.checksum FROM SourceTbl t0, OriginUrlTbl t1 WHERE "
                ~ "t0.urlType=:urlType AND t0.id=t1.sourceId AND t1.url=:url";
            auto stmt = db.prepare(sql);
            stmt.get.bind(":urlType", cast(long) convert(origin));
            stmt.get.bind(":url", url);
            foreach (ref r; stmt.get.execute) {
                auto src = Source(origin, r.peek!long(1).SourceChecksum);
                auto srcId = r.peek!long(0).SourceId;
                return ReturnT(tuple!("src", "id")(src, srcId).some);
            }
            return ReturnT(None.init);
        }

        return origin.match!((Topic a) => urlSource(a.name),
                (Path a) => urlSource(a.toString), (Url a) => urlSource(a.value));
    }

    Optional!Source getSource(SourceId id) {
        Optional!Source getUrl(SourceTbl.UrlType kind) {
            static immutable sql = "SELECT t0.checksum,t1.url FROM SourceTbl t0, OriginUrlTbl t1 "
                ~ "WHERE t0.id=:id AND t0.id=t1.sourceId";
            auto stmt = db.prepare(sql);
            stmt.get.bind(":id", id.get);
            foreach (ref r; stmt.get.execute) {
                if (kind == SourceTbl.UrlType.url)
                    return some(Source(Origin(Url(r.peek!string(1))),
                            r.peek!long(0).SourceChecksum));
                if (kind == SourceTbl.UrlType.path)
                    return some(Source(Origin(Path(r.peek!string(1))),
                            r.peek!long(0).SourceChecksum));
                if (kind == SourceTbl.UrlType.topic)
                    return some(Source(Origin(Topic(r.peek!string(1))),
                            r.peek!long(0).SourceChecksum));
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
        logger.trace("Hits ", ids.length);

        auto rval = appender!(SourceMatch[])();
        foreach (id; ids[]) {
            auto src = getSource(id.sourceId.SourceId);
            src.match!((Source src) {
                auto chunk = getChunk(id.embedId);
                rval.put(SourceMatch(src.origin, offset: chunk.offset, line: chunk.line,
                    text: chunk.text, rank: id.rank));
            }, (None _) {});
        }

        return rval[];
    }

    SourceMatch[] queryTextSearch(string query, long limit) {
        static immutable ftsSql = "SELECT rowid, rank "
            ~ "FROM FtsChunksTbl WHERE FtsChunksTbl MATCH :query ORDER BY rank LIMIT :limit";

        auto stmt = db.prepare(ftsSql);
        stmt.get.bind(":query", query.quoteFts5);
        stmt.get.bind(":limit", limit);

        auto results = appender!(Tuple!(long, "rowid", double, "rank")[])();
        foreach (ref r; stmt.get.execute) {
            results.put(tuple!("rowid", "rank")(r.peek!long(0), r.peek!double(1)));
        }
        logger.trace("Hits ", results.length);

        auto rval = appender!(SourceMatch[])();
        foreach (res; results) {
            // rowid in FtsChunksTbl maps to TextChunkTbl.id (content_rowid='id')
            auto chunk = getChunkByRowid(res.rowid);
            if (!chunk.text.empty) {
                auto src = getSourceByEmbedId(chunk.embedId);
                src.match!((Source src) {
                    rval.put(SourceMatch(src.origin, offset: chunk.offset,
                        line: chunk.line, text: chunk.text, rank: res.rank));
                }, (None _) {});
            }
        }

        return rval[];
    }

    SourceMatch[] queryByPathAndLine(Path filePath, long lineNumber) {
        // INNER JOIN on OriginUrlTbl is correct: every path-type source always has
        // a corresponding OriginUrlTbl entry (see addSource line 253-254).
        // This is consistent with hasFile() which uses the same JOIN pattern.
        static immutable sql = "SELECT t0.text, t0.charBeginPos, t0.charEndPos, t0.lineBegin, t0.lineEnd, t3.url "
            ~ "FROM TextChunkTbl t0 " ~ "JOIN EmbeddingsTbl t1 ON t0.embedId = t1.id "
            ~ "JOIN SourceTbl t2 ON t1.sourceId = t2.id " ~ "JOIN OriginUrlTbl t3 ON t2.id = t3.sourceId "
            ~ "WHERE t2.urlType = :urlType AND t3.url = :url "
            ~ "AND t0.lineBegin <= :lineNumber AND t0.lineEnd >= :lineNumber";

        auto stmt = db.prepare(sql);
        stmt.get.bind(":urlType", cast(long) SourceTbl.UrlType.path);
        stmt.get.bind(":url", filePath.toString);
        stmt.get.bind(":lineNumber", lineNumber);

        auto rval = appender!(SourceMatch[])();
        foreach (ref r; stmt.get.execute) {
            rval.put(SourceMatch(Origin(Path(r.peek!string(5))), offset: Offset(begin: r.peek!long(1),
                    end: r.peek!long(2)), line: Line(begin: r.peek!long(3),
                    end: r.peek!long(4)), text: r.peek!string(0), rank: 0));
        }

        logger.tracef("queryByPathAndLine hits %d for %s line %d", rval.length,
                filePath, lineNumber);
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

        double countTextChunks() {
            static immutable sql = "SELECT count(*) FROM TextChunkTbl";

            double count = 0.0;
            auto stmt = db.prepare(sql);
            foreach (ref r; stmt.get.execute)
                count = cast(double) r.peek!long(0);
            return count;
        }

        double reduceRankBias(double chunkCount, double rank) {
            import std.math : log;

            immutable k = 60;
            return 1.0 / (k + rank) * log(chunkCount);
        }

        const chunkCount = countTextChunks();
        auto stmt = db.prepare(sql);
        stmt.get.bind(":embedding", embedding.embed);
        stmt.get.bind(":text_query", query.quoteFts5);
        stmt.get.bind(":limit", limit);

        auto results = appender!(Tuple!(long, "id", double, "rank")[])();
        foreach (ref r; stmt.get.execute) {
            // TODO: this should not be needed
            if (results.length >= limit)
                break;
            results.put(tuple!("id", "rank")(r.peek!long(0),
                    reduceRankBias(chunkCount, r.peek!double(1))));
        }
        logger.trace("Hits ", results.length);

        auto rval = appender!(SourceMatch[])();
        foreach (res; results[].map!(a => tuple(getChunkByRowid(a.id), a.rank))
                .cache
                .filter!(a => !a[0].text.empty)) {
            auto src = getSourceByEmbedId(res[0].embedId);
            src.match!((Source src) {
                rval.put(SourceMatch(src.origin, offset: res[0].offset, line: res[0].line,
                    text: res[0].text, rank: res[1]));
            }, (None _) {});
        }

        return rval[];
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

private:

// Full-text query syntax for FTS5 sqlite manual
// The following block contains a summary of the FTS query syntax in BNF form. A detailed explanation follows.
//
// <phrase>    := string [*]
// <phrase>    := <phrase> + <phrase>
// <neargroup> := NEAR ( <phrase> <phrase> ... [, N] )
// <query>     := [ [-] <colspec> :] [^] <phrase>
// <query>     := [ [-] <colspec> :] <neargroup>
// <query>     := [ [-] <colspec> :] ( <query> )
// <query>     := <query> AND <query>
// <query>     := <query> OR <query>
// <query>     := <query> NOT <query>
// <colspec>   := colname
// <colspec>   := { colname1 colname2 ... }
string quoteFts5(string s) {
    import std.algorithm : among, splitter, count;
    import std.ascii : isAlphaNum;
    import std.string : join;
    import std.uni : byCodePoint;

    static string quoteIfNeeded(string s) {
        if (s.byCodePoint.filter!(a => !(a.isAlphaNum || a == '_')).count == 0)
            return s;
        return "\"" ~ s ~ "\"";
    }

    return s.splitter
        .filter!(a => !a.among("AND", "NOT", "NOT", "NEAR"))
        .map!(a => quoteIfNeeded(a))
        .join(" ");
}
