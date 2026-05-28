// RAG implementation using Embedder abstraction.
// Decoupled from specific model implementations via the Embedder interface.

module llm.rag.rag;

import logger = std.logger;
import std.algorithm;
import std.array : array, empty, appender;
import std.digest.murmurhash : MurmurHash3;
import std.digest;
import std.stdio : File;
import std.sumtype;

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
    import llm.rag.database;

    Embedder embedder;
    Database db;
    alias db this;

    this(Embedder embedder, Database db) {
        this.embedder = embedder;
        this.db = db;
    }

    this(Embedder embedder, Path dbFile, long dimensions) {
        import llm.rag.database : openDatabase;

        this.embedder = embedder;
        this.db = openDatabase(dbFile, dimensions);
    }

    void destroy() {
        embedder.destroy();
        db.destroy;
    }

    Document[] query(string query, int getTopK) {
        Document[] runMatch(float[] embed) {
            auto res = db.getBestMatch(Search(embed), getTopK);
            return res.map!(a => Document(a.origin, a.text, a.offset, a.line)).array;
        }

        return embedder.embed(query).match!((float[] a) => runMatch(a), (string errMsg) {
            logger.warning(errMsg);
            return null;
        });
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

    if (rag.hasSource(Source(doc.origin, dataHash.SourceChecksum))) {
        logger.trace("source already exist in database");
        return RagAddResult(doc.data.length, 1);
    }
    // try to remove the source before adding to ensure old cruft isn't left
    rag.removeSource(doc.origin);

    auto trans = rag.db.transaction;

    auto srcId = rag.db.addSource(Source(doc.origin, SourceChecksum(dataHash)));
    const nBatch = rag.embedder.batchSize();

    size_t nChunks;
    size_t startCharPos;
    size_t startLine, endLine;
    Grapheme[] graphemes;
    void addChunk() {
        auto data = graphemes.byCodePoint.toUTF8;

        rag.embedder.embed(data).match!((float[] embed) {
            rag.db.addEmbedding(srcId, Embedding(Offset(startCharPos,
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

    trans.commit;

    return RagAddResult(doc.data.length, nChunks);
}
