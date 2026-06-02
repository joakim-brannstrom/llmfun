/// RAG implementation using Embedder abstraction.
/// Decoupled from specific model implementations via the Embedder interface.
///
/// Shuffle-based Rank Randomization
/// --------------------------------
/// Query functions (`querySemantic`, `queryTextSearch`, `queryBestMatch`) apply
/// `randomizeRanks()` to the collected result array before sorting by rank. This
/// eliminates database-order bias: without shuffling, results with identical ranks
/// would always be taken from whichever database returned them first.
///
/// How it works: a Fisher-Yates shuffle is applied to a copy of the `SourceMatch[]`
/// array, then the shuffled copy is sorted by rank and truncated to top-K. The
/// original array is never mutated.

module llm.rag.rag;

import logger = std.logger;
import std.path : baseName, stripExtension;
import std.algorithm : map, filter, joiner, sort, cache, swap, count;
import std.array : array, empty, appender;
import std.random : uniform;
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
import llm.rag.database : SourceMatch;

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
    string[] dbNames;

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
            openDatabase(dbFile.AbsolutePath, dimensions, isReadOnly).match!((Database db) {
                this.dbs.insertBack(db);
                this.dbFiles ~= dbFile;
                this.dbNames ~= dbFile.baseName.stripExtension;
            }, (None _) {});
            isReadOnly = true;
        }
    }

    void destroy() {
        embedder.destroy();
        foreach (ref a; dbs)
            a.destroy;
        dbs.clear;
        dbFiles.length = 0;
        dbNames.length = 0;
    }

    size_t[] resolveDatabaseIndices(string databaseName) {
        if (databaseName.empty) {
            return iota(dbs.length).array;
        }
        return dbNames.enumerate
            .filter!(a => a.value == databaseName)
            .map!(a => a.index)
            .array;
    }

    string[] getDatabaseNames() {
        return dbNames;
    }

    bool databaseExists(string databaseName) {
        if (databaseName.empty)
            return true;
        return resolveDatabaseIndices(databaseName).length > 0;
    }

    bool validateDatabase(string databaseName, ref size_t[] indices) {
        indices = resolveDatabaseIndices(databaseName);
        if (indices.empty && !databaseName.empty) {
            logger.tracef("no database found with name '%s'. Available: [%s]",
                    databaseName, getDatabaseNames().joiner(", "));
            return false;
        }
        if (!databaseName.empty) {
            logger.tracef("query with database filter: '%s' (%d databases)",
                    databaseName, indices.length);
        }
        return true;
    }

    Document[] querySemantic(string query, long getTopK) {
        return querySemantic(query, getTopK, "");
    }

    Document[] queryTextSearch(string query, long getTopK) {
        return queryTextSearch(query, getTopK, "");
    }

    Document[] queryBestMatch(string query, long getTopK) {
        return queryBestMatch(query, getTopK, "");
    }

    Document[] querySemantic(string query, long getTopK, string database) {
        size_t[] indices;
        if (!validateDatabase(database, indices))
            return null;

        Document[] runMatch(float[] embed) {
            return indices.map!(i => dbs[i].querySemantic(Search(embed),
                    getTopK)).cache.joiner.array.randomizeRanks().sort!((a,
                    b) => a.rank > b.rank).take(getTopK)
                .array.map!(a => Document(a.origin, a.text, a.offset, a.line)).array;
        }

        return embedder.embed(query).match!((float[] a) => runMatch(a), (string errMsg) {
            logger.warning(errMsg);
            return null;
        });
    }

    Document[] queryTextSearch(string query, long getTopK, string database) {
        size_t[] indices;
        if (!validateDatabase(database, indices))
            return null;

        return indices.map!(i => dbs[i].queryTextSearch(query, getTopK))
            .cache.joiner.array.randomizeRanks().sort!((a,
                b) => a.rank < b.rank).take(getTopK)
            .map!(a => Document(a.origin, a.text, a.offset, a.line)).array;
    }

    Document[] queryBestMatch(string query, long getTopK, string database) {
        size_t[] indices;
        if (!validateDatabase(database, indices))
            return null;

        Document[] runMatch(float[] embed) {
            return indices.map!(i => dbs[i].queryCombineSemanticText(Search(embed),
                    query, getTopK)).cache.joiner.array.randomizeRanks()
                .sort!((a, b) => a.rank > b.rank).take(getTopK)
                .map!(a => Document(a.origin, a.text, a.offset, a.line)).array;
        }

        return embedder.embed(query).match!((float[] a) => runMatch(a), (string errMsg) {
            logger.tracef(errMsg);
            return queryTextSearch(query, getTopK, database);
        });
    }

    SourceMatch[] queryReadFile(AbsolutePath filePath, long lineNumber) {
        return queryReadFile(filePath, lineNumber, "");
    }

    SourceMatch[] queryReadFile(AbsolutePath filePath, long lineNumber, string database) {
        size_t[] indices;
        if (!validateDatabase(database, indices))
            return null;

        auto results = indices.map!(i => dbs[i].queryByPathAndLine(filePath,
                lineNumber)).cache.joiner.array;

        logger.tracef("Hits %s for %s line %s", results.length, filePath, lineNumber);
        return results;
    }

    bool hasFile(AbsolutePath filePath, string database) {
        size_t[] indices;
        if (!validateDatabase(database, indices))
            return false;
        return indices.map!(i => dbs[i].hasFile(filePath))
            .cache
            .filter!(a => a)
            .count >= 1;
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

private:

/// Fisher-Yates shuffle on a copy of the result array, before sorting by rank.
/// Eliminates database-order bias among results with identical ranks.
/// Returns a new array; does not mutate the input.
SourceMatch[] randomizeRanks(SourceMatch[] results) {
    import std.random : randomShuffle, rndGen;

    return results.randomShuffle(rndGen);
}

// Helper to create a SourceMatch with a given rank
SourceMatch makeMatch(double rank) {
    return SourceMatch(Origin(Unknown()), Offset(0, 0), Line(0, 0), "", rank);
}

unittest {
    // Test 4: Shuffle produces different orderings
    // Probabilistic: 5-element array has 120 permutations;
    // chance of false failure is ~ (1/120)^99 ≈ 0
    {
        SourceMatch[] input = [
            makeMatch(1.0), makeMatch(2.0), makeMatch(3.0), makeMatch(4.0),
            makeMatch(5.0)
        ];
        bool gotDifferent = false;
        auto first = randomizeRanks(input.dup);
        foreach (_; 0 .. 100) {
            auto current = randomizeRanks(input);
            if (current != first) {
                gotDifferent = true;
                break;
            }
        }
        assert(gotDifferent, "Shuffle should produce different orderings");
    }

    // Test 5: Uniform distribution check
    {
        SourceMatch[] input = [
            makeMatch(10.0), makeMatch(20.0), makeMatch(30.0), makeMatch(40.0)
        ];
        long[4] counts;
        foreach (_; 0 .. 10_000) {
            auto result = randomizeRanks(input.dup);
            double rank = result[0].rank;
            if (rank == 10.0)
                counts[0]++;
            else if (rank == 20.0)
                counts[1]++;
            else if (rank == 30.0)
                counts[2]++;
            else if (rank == 40.0)
                counts[3]++;
        }
        foreach (count; counts) {
            import std.math : abs;

            assert(abs(cast(long)(count - 2500)) < 500,
                    "Distribution should be roughly uniform, got count: " ~ count.stringof);
        }
    }
}
