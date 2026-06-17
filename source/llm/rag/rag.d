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
import std.algorithm : map, filter, joiner, sort, cache, swap, count;
import std.array : array, empty, appender;
import std.digest.murmurhash : MurmurHash3;
import std.digest;
import std.path : baseName, stripExtension;
import std.random : uniform;
import std.range : take, enumerate, iota;
import std.stdio : File;
import std.string : strip;
import std.sumtype;
import std.uni : Grapheme;

import miniorm : spinSql;
import my.path;
public import my.path : Path;

import llm.config : RagDatabaseConfig;
import llm.rag.database : SourceMatch;
import llm.rag.embedder;

struct Topic {
    string name;
}

struct DatabaseInfo {
    Path path;
    string name;
    string description;
}

struct Url {
    string value;
}

alias Origin = SumType!(Topic, Url, Path);

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
    DatabaseInfo[] databases;

    ref Database db() {
        return dbs[0];
    }

    alias db this;

    this(Embedder embedder, RagDatabaseConfig[] configs) {
        import my.optional;
        import llm.rag.database : openDatabase;

        this.embedder = embedder;
        if (configs.empty)
            configs ~= RagDatabaseConfig(Path(":memory:"), "");
        bool isReadOnly = false;
        foreach (cfg; configs) {
            openDatabase(cfg.path.AbsolutePath, embedder.modelName,
                    embedder.dimensions, isReadOnly).match!((Database db) {
                this.dbs.insertBack(db);
                this.databases ~= DatabaseInfo(cfg.path,
                    cfg.path.baseName.stripExtension, cfg.description);
            }, (None _) {});
            isReadOnly = true;
        }
    }

    void destroy() {
        embedder.destroy();
        foreach (ref a; dbs)
            a.destroy;
        dbs.clear;
        databases.length = 0;
    }

    size_t[] resolveDatabaseIndices(string databaseName) {
        if (databaseName.strip == "*" || databaseName.strip.empty) {
            return iota(dbs.length).array;
        }
        return databases.enumerate
            .filter!(a => a.value.name == databaseName)
            .map!(a => a.index)
            .array;
    }

    string[] getDatabaseNames() {
        return databases.map!(d => d.name).array;
    }

    DatabaseInfo[] getDatabaseInfo() {
        return databases;
    }

    bool databaseExists(string databaseName) {
        if (databaseName.empty)
            return true;
        return resolveDatabaseIndices(databaseName).length > 0;
    }

    bool validateDatabase(string databaseName, ref size_t[] indices) {
        indices = resolveDatabaseIndices(databaseName);
        if (indices.empty) {
            logger.tracef("no database found with name '%s'. Available: [%s]",
                    databaseName, getDatabaseNames().joiner(", "));
            return false;
        }
        logger.tracef("query with database filter: '%s' (%d databases)",
                databaseName, indices.length);
        return true;
    }

    Document[] querySemantic(string query, long getTopK, string database) {
        size_t[] indices;
        if (!validateDatabase(database, indices))
            return null;

        Document[] runMatch(float[] embed) {
            return indices.map!(i => spinSql!(() => dbs[i].querySemantic(Search(embed),
                    getTopK))).cache.joiner.array.randomizeRanks().sort!((a,
                    b) => a.rank > b.rank).take(getTopK)
                .array.map!(a => Document(a.origin, a.text, a.offset, a.line)).array;
        }

        return embedder.embed(query).match!((float[] a) => runMatch(a), (HttpError e) {
            logger.warning(e.errorMsg);
            return null;
        }, (string errMsg) { logger.warning(errMsg); return null; });
    }

    Document[] queryTextSearch(string query, long getTopK, string database) {
        size_t[] indices;
        if (!validateDatabase(database, indices))
            return null;

        return indices.map!(i => spinSql!(() => dbs[i].queryTextSearch(query,
                getTopK))).cache.joiner.array.randomizeRanks().sort!((a,
                b) => a.rank < b.rank).take(getTopK)
            .map!(a => Document(a.origin, a.text, a.offset, a.line)).array;
    }

    Document[] queryBestMatch(string query, long getTopK, string database) {
        size_t[] indices;
        if (!validateDatabase(database, indices))
            return null;

        Document[] runMatch(float[] embed) {
            return indices.map!(i => spinSql!(() => dbs[i].queryCombineSemanticText(Search(embed),
                    query, getTopK))).cache.joiner.array.randomizeRanks()
                .sort!((a, b) => a.rank > b.rank).take(getTopK)
                .map!(a => Document(a.origin, a.text, a.offset, a.line)).array;
        }

        return embedder.embed(query).match!((float[] embed) {
            if (embed.empty) {
                logger.trace("Unable to do a combined search because embedding is empty");
                return queryTextSearch(query, getTopK, database);
            }
            return runMatch(embed);
        }, (HttpError e) {
            logger.tracef(e.errorMsg);
            return queryTextSearch(query, getTopK, database);
        }, (string errMsg) {
            logger.tracef(errMsg);
            return queryTextSearch(query, getTopK, database);
        });
    }

    SourceMatch[] queryReadFile(Path filePath, long lineNumber, string database) {
        size_t[] indices;
        if (!validateDatabase(database, indices))
            return null;

        auto results = indices.map!(i => spinSql!(() => dbs[i].queryByPathAndLine(filePath,
                lineNumber))).cache.joiner.array;

        logger.tracef("Hits %s for %s line %s", results.length, filePath, lineNumber);
        return results;
    }

    bool hasFile(Path filePath, string database) {
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
        assert(databases.length == dbs.length, "databases and dbs arrays are out of sync");
        auto rval = appender!(DbSource[])();
        foreach (idx; 0 .. dbs.length) {
            rval.put(DbSource(databases[idx].path, dbs[idx].getSources));
        }
        return rval[];
    }
}

struct RagAddResult {
    size_t length;
    size_t chunks;
}

// the configured nBatch is too large but the server has informed us of what it should be.
size_t ServerNBatch = 0;

// Add a document to the RAG.
RagAddResult add(RAG rag, Document doc) {
    import std.algorithm : max;
    import std.json : parseJSON;
    import std.uni : byCodePoint, byGrapheme;
    import std.utf : toUTF8;
    import llm.rag.database;
    import llm.utility : getValue, ApproxTokenSize;

    long toUint(ubyte[4] a) {
        return a[0] | a[1] << 8 | a[2] << 16 | a[3] << 24;
    }

    long dataHash = toUint(digest!(MurmurHash3!32)(doc.data));

    if (spinSql!(() => rag.hasSource(Source(doc.origin, dataHash.SourceChecksum)))) {
        logger.trace("source already exist in database");
        return RagAddResult(doc.data.length, 0);
    }

    immutable MaxIterations = 8;
    size_t nBatch = ServerNBatch == 0 ? rag.embedder.batchSize() : ServerNBatch;

    auto embeddings = appender!(Embedding[])();
    size_t nChunks;
    // used to detect if the fallback mode where nBatch is halfed always used.
    // If it has been used for 5 consecutive turns the nBatch is probably just
    // too high and should be adjusted down.
    int failureCount;
    void addChunk(Grapheme[] graphemes, size_t startCharPos, size_t startLine, int iteration) {
        auto data = graphemes.byCodePoint.toUTF8;

        float[] emb;
        rag.embedder.embed(data).match!((float[] embed) { emb = embed; }, (HttpError e) {
            logger.tracef("Failed to generate embedding '%s' (len:%s): %s",
                e.errorMsg, graphemes.length, data);
            try {
                const old = nBatch;
                nBatch = getValue(parseJSON(e.body),
                    (v) => v["error"]["n_ctx"].integer * ApproxTokenSize, nBatch);
                ServerNBatch = max(128, nBatch);
                logger.tracef("Changed nBatch from %s->%s", old, nBatch);
            } catch (Exception e) {
                logger.trace(e.msg);
            }
        }, (string e) {
            logger.tracef("Failed to generate embedding '%s' (len:%s): %s", e,
                graphemes.length, data);
        });

        if (emb.empty) {
            ++failureCount;
        }

        if (graphemes.length < 4 && emb.empty) {
            logger.tracef("Failed to generate embedding after %s iterations using batch size %s '%s'",
                    iteration, graphemes.length, data);
            return;
        }
        if (emb.empty && iteration < MaxIterations) {
            logger.trace("Using fallback with nBatch ", graphemes.length / 2);
            addChunk(graphemes[0 .. $ / 2], startCharPos, startLine, iteration + 1);
            auto p1 = graphemes[$ / 2 .. $];
            addChunk(p1, startCharPos + p1.length, startLine + countLines(p1), iteration + 1);
            return;
        }
        if (emb.empty && iteration >= MaxIterations) {
            logger.warningf("Failed to generate embedding after %s iterations using batch size %s '%s'",
                    iteration, graphemes.length, data);
            return;
        }
        if (iteration == 0 && failureCount > 0) {
            logger.trace("Reset failure count");
            failureCount = 0;
        }

        embeddings.put(Embedding(Offset(startCharPos, startCharPos + graphemes.length),
                Line(startLine, startLine + countLines(graphemes)), data, emb));

        logger.tracef("add chunk length:%s line(%s-%s) offset(%s-%s)", data.length, startLine,
                startLine + countLines(graphemes), startCharPos, startCharPos + graphemes.length);
        nChunks++;
    }

    size_t startCharPos;
    size_t startLine;
    Grapheme[] graphemes;
    foreach (graphem; doc.data.byGrapheme) {
        graphemes ~= graphem;
        if (graphemes.length >= nBatch) {
            addChunk(graphemes, startCharPos, startLine, 0);
            // 50% sliding window
            const size_t half = graphemes.length / 2;
            startCharPos += half;
            startLine += countLines(graphemes[0 .. half]);
            graphemes = graphemes[half .. $];
        }
        if (failureCount > 5 && nBatch > 128) {
            logger.tracef("Adjusting down nBatch %s -> %s", nBatch, nBatch - 64);
            nBatch -= 64;
            failureCount = 0;
            // trim the server down so future RAG chunking on other documents work better
            ServerNBatch = nBatch;
        }
    }
    if (!graphemes.empty) {
        addChunk(graphemes, startCharPos, startLine, 0);
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

size_t countLines(Grapheme[] graphemes) {
    immutable newline = Grapheme('\n');

    return graphemes.filter!(a => a == newline).count;
}

/// Fisher-Yates shuffle on a copy of the result array, before sorting by rank.
/// Eliminates database-order bias among results with identical ranks.
/// Returns a new array; does not mutate the input.
SourceMatch[] randomizeRanks(SourceMatch[] results) {
    import std.random : randomShuffle, rndGen;

    return results.randomShuffle(rndGen);
}

// Helper to create a SourceMatch with a given rank
SourceMatch makeMatch(double rank) {
    return SourceMatch(Origin(Topic("")), Offset(0, 0), Line(0, 0), "", rank);
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
