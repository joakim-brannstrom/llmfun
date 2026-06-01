// RAG implementation using Embedder abstraction.
// Decoupled from specific model implementations via the Embedder interface.

module llm.rag.rag;

import logger = std.logger;
import std.algorithm : map, filter, joiner, sort, cache;
import std.array : array, empty, appender;
import std.digest.murmurhash : MurmurHash3;
import std.digest;
import std.range : take, enumerate, iota;
import std.stdio : File;
import std.sumtype;

import miniorm : spinSql;
import my.path;
public import my.path : Path;

import llm.rag.embedder;
import llm.rag.embedder_llama;

struct Unknown {
}

struct Url {
    string value;
}

alias Origin = SumType!(Unknown, Url, Path);

struct Chunk {
    Document doc;
    ulong hash;
    float[] embed;
}

struct Offset {
    long begin;
    long end;
}

struct Line {
    long begin;
    long end;
}

struct Document {
    Origin origin;
    string data;
    Offset offset;
    Line line;
}

class RAG {
    import std.container : Array;
    import llm.rag.database;

    Embedder embedder;
    Array!Database dbs;
    Path[] dbFiles;

    ref Database db() {
        return dbs[0];
    }

    alias db this;

    this(Embedder embedder, Path[] dbFiles, long dimensions) {
        import my.optional;
        import llm.rag.database : openDatabase;

        this.embedder = embedder;
        if (dbFiles.empty)
            dbFiles ~= Path(":memory:");
        bool isReadOnly = false;
        foreach (dbFile; dbFiles) {
            openDatabase(dbFile, dimensions, isReadOnly).match!((Database db) {
                this.dbs.insertBack(db);
                this.dbFiles ~= dbFile;
            }, (None _) {});
            isReadOnly = true;
        }
    }

    void destroy() {
        embedder.destroy();
        foreach (ref a; dbs)
            a.destroy;
        dbs.clear;
    }

    Document[] querySemantic(string query, long getTopK) {
        Document[] runMatch(float[] embed) {
            return iota(dbs.length).map!(i => dbs[i].querySemantic(Search(embed), getTopK))
                .cache
                .joiner
                .array
                .sort!((a, b) => a.rank > b.rank)
                .take(getTopK).array.map!(a => Document(a.origin, a.text, a.offset, a.line)).array;
        }

        return embedder.embed(query).match!((float[] a) => runMatch(a), (string errMsg) {
            logger.warning(errMsg);
            return null;
        });
    }

    Document[] queryTextSearch(string query, long getTopK) {
        return iota(dbs.length).map!(i => dbs[i].queryTextSearch(query, getTopK))
            .cache
            .joiner
            .array
            .sort!((a, b) => a.rank < b.rank)
            .take(getTopK).map!(a => Document(a.origin, a.text, a.offset, a.line)).array;
    }

    Document[] queryBestMatch(string query, long getTopK) {
        Document[] runMatch(float[] embed) {
            return iota(dbs.length).map!(i => dbs[i].queryCombineSemanticText(Search(embed),
                    query, getTopK))
                .cache
                .joiner
                .array
                .sort!((a, b) => a.rank > b.rank)
                .take(getTopK).map!(a => Document(a.origin, a.text, a.offset, a.line)).array;
        }

        return embedder.embed(query).match!((float[] a) => runMatch(a), (string errMsg) {
            logger.tracef(errMsg);
            return queryTextSearch(query, getTopK);
        });
    }

    struct DbSource {
        Path name;
        Source[] sources;
    }

    DbSource[] getSources() {
        auto rval = appender!(DbSource[])();
        foreach (idx; 0 .. dbs.length) {
            rval.put(DbSource(dbFiles[idx], dbs[idx].getSources));
        }
        return rval[];
    }
}

struct RagAddResult {
    size_t length;
    size_t chunks;
}

// Add a document to the RAG.
RagAddResult add(RAG rag, Document doc) {
    import std.utf;
    import std.uni;
    import llm.rag.database;

    long toUint(ubyte[4] a) {
        return a[0] | a[1] << 8 | a[2] << 16 | a[3] << 24;
    }

    long dataHash = toUint(digest!(MurmurHash3!32)(doc.data));

    if (spinSql!(() => rag.hasSource(Source(doc.origin, dataHash.SourceChecksum)))) {
        logger.trace("source already exist in database");
        return RagAddResult(doc.data.length, 1);
    }

    const nBatch = rag.embedder.batchSize();

    auto embeddings = appender!(Embedding[])();
    size_t nChunks;
    size_t startCharPos;
    size_t startLine, endLine;
    Grapheme[] graphemes;
    void addChunk() {
        auto data = graphemes.byCodePoint.toUTF8;

        rag.embedder.embed(data).match!((float[] embed) {
            embeddings.put(Embedding(Offset(startCharPos,
                startCharPos + graphemes.length), Line(startLine, endLine), data, embed));
        }, (string e) {
            logger.tracef("Failed to generate embedding '%s': %s", e, data);
        });
        logger.tracef("add chunk length:%s line(%s-%s) offset(%s-%s)", data.length,
                startLine, endLine, startCharPos, startCharPos + graphemes.length);

        // 50% sliding window
        graphemes = graphemes[$ / 2 .. $];
        startCharPos += graphemes.length;
        startLine = endLine;
        nChunks++;
    }

    const newline = Grapheme('\n');
    foreach (graphem; doc.data.byGrapheme) {
        graphemes ~= graphem;
        if (graphem == newline)
            ++endLine;
        if (graphemes.length >= nBatch) {
            addChunk();
        }
    }
    if (!graphemes.empty) {
        addChunk();
    }

    spinSql!(() {
        auto trans = rag.db.transaction;
        // try to remove the source before adding to ensure old cruft isn't left
        rag.removeSource(doc.origin);
        auto srcId = rag.db.addSource(Source(doc.origin, SourceChecksum(dataHash)));
        foreach (ref e; embeddings[]) {
            rag.db.addEmbedding(srcId, e);
        }
        trans.commit;
    });

    return RagAddResult(doc.data.length, nChunks);
}
