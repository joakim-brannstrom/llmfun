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
import std.datetime : SysTime;
import std.path : baseName, stripExtension;
import std.random : uniform;
import std.range : take, enumerate, iota;
import std.stdio : File;
import std.string : strip;
import std.sumtype;
import std.uni : Grapheme;
import std.parallelism;

import miniorm : spinSql;
import llm.test_util : retrySql;
import my.path;
public import my.path : Path;

import llm.common.embedder;
import llm.config : RagDatabaseConfig, RagConfig;
import llm.rag.database : SourceMatch, Database;

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
    SysTime added;
    string databaseName;
}

private struct IndexedMatch {
    SourceMatch match;
    size_t dbIndex;
    alias match this; // Implicit conversion to SourceMatch for .rank access
}

private struct ParallelQueryTask {
    size_t index;
    SourceMatch[]delegate(size_t) dg;
}

private SourceMatch[] executeParallelQuery(ParallelQueryTask qt) {
    try {
        return qt.dg(qt.index);
    } catch (Exception e) {
        logger.warningf("parallelQuery: database[%s] failed: %s", qt.index, e.msg);
        return null;
    }
}

private IndexedMatch[] parallelQuery(size_t[] indices, SourceMatch[]delegate(size_t) queryFn) {
    auto tasks = indices.map!((i) => ParallelQueryTask(i, queryFn)).array;
    auto perDbResults = taskPool.amap!executeParallelQuery(tasks).array;
    auto app = appender!(IndexedMatch[])();
    foreach (i, matches; perDbResults.enumerate) {
        foreach (m; matches) {
            app.put(IndexedMatch(m, indices[i]));
        }
    }
    auto results = app[];
    if (results.empty && indices.length > 0) {
        logger.tracef("parallelQuery: all %s database queries failed", indices.length);
    }
    return results;
}

class RAG {
    import std.container : Array;
    import llm.rag.database;

    Embedder embedder;
    Array!Database dbs;
    DatabaseInfo[] databases;
    // Per-caller batch-size adaptation state for the addToDatabase seam. Kept
    // on the instance (not a shared global) so concurrent indexers on other
    // threads never share mutable batch state (see addToDatabase).
    size_t nBatchCache;

    ref Database db() {
        return dbs[0];
    }

    alias db this;

    this(Embedder embedder, RagDatabaseConfig primary, RagDatabaseConfig[] secondary)
    in (embedder !is null) {
        import my.optional;
        import llm.rag.database : openDatabase;

        this.embedder = embedder;

        openDatabase(primary.path.AbsolutePath, embedder.modelName,
                embedder.dimensions, readOnly: false).match!((Database db) {
            this.dbs.insertBack(db);
            this.databases ~= DatabaseInfo(primary.path,
                primary.path.baseName.stripExtension, primary.description);
        }, (None _) {
            openDatabase(primary.path.AbsolutePath, embedder.modelName,
                embedder.dimensions, readOnly: false, inMemory: true).match!((Database db) {
                this.dbs.insertBack(db);
                this.databases ~= DatabaseInfo(":memory:".Path,
                primary.path.baseName.stripExtension, primary.description);
            }, (None _) {
                logger.errorf("sqlite3 fatal error. This should not happen");
            });
        });

        foreach (cfg; secondary) {
            openDatabase(cfg.path.AbsolutePath, embedder.modelName,
                    embedder.dimensions, readOnly: true, inMemory: false).match!((Database db) {
                this.dbs.insertBack(db);
                this.databases ~= DatabaseInfo(cfg.path,
                    cfg.path.baseName.stripExtension, cfg.description);
            }, (None _) {});
        }
    }

    void destroy() @trusted {
        foreach (ref a; dbs)
            a.destroy;
        dbs.clear;
        databases.length = 0;
        embedder.destroy;
    }

    bool isPrimaryInMemory() {
        return databases[0].path.toString == ":memory:";
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

    private bool validateDatabase(string databaseName, ref size_t[] indices) {
        indices = resolveDatabaseIndices(databaseName);
        if (indices.empty) {
            logger.tracef("no database found with name '%s'. Available: [%-(%s, %)]",
                    databaseName, getDatabaseNames);
            return false;
        }
        logger.tracef("query with database filter: '%s' (%s databases)",
                databaseName, indices.length);
        return true;
    }

    Document[] querySemantic(string query, long getTopK, string database) {
        size_t[] indices;
        if (!validateDatabase(database, indices))
            return null;

        Document[] runMatch(float[] embed) {
            return parallelQuery(indices,
                    (size_t i) => retrySql!(() => dbs[i].querySemantic(Search(embed), getTopK))).randomizeRanks()
                .sort!((a, b) => a.rank > b.rank).take(getTopK).map!(a => Document(origin: a.origin,
                    data: a.text, offset: a.offset, line: a.line, added: a.added,
                    databaseName: databases[a.dbIndex].name)).array;
        }

        return embedder.embedQuery(query).match!((float[] a) => runMatch(a), (EmbedError e) {
            logger.warning(e.errorMsg);
            return null;
        });
    }

    Document[] queryTextSearch(string query, long getTopK, string database) {
        size_t[] indices;
        if (!validateDatabase(database, indices))
            return null;

        return parallelQuery(indices,
                (size_t i) => retrySql!(() => dbs[i].queryTextSearch(query, getTopK))).randomizeRanks()
            .sort!((a, b) => a.rank < b.rank).take(getTopK).map!(a => Document(origin: a.origin, data: a.text, offset: a
                .offset, line: a.line, added: a.added, databaseName: databases[a.dbIndex].name))
            .array;
    }

    Document[] queryBestMatch(string textQuery, string vectorQuery, long getTopK, string database) {
        size_t[] indices;
        if (!validateDatabase(database, indices))
            return null;

        Document[] runMatch(float[] embed) {
            return parallelQuery(indices,
                    (size_t i) => retrySql!(() => dbs[i].queryCombineSemanticText(Search(embed),
                        textQuery, getTopK))).randomizeRanks().sort!((a,
                    b) => a.rank > b.rank).take(getTopK).map!(a => Document(origin: a.origin, data: a.text, offset: a
                    .offset, line: a.line,
                    added: a.added, databaseName: databases[a.dbIndex].name)).array;
        }

        return embedder.embedQuery(vectorQuery).match!((float[] embed) {
            if (embed.empty) {
                logger.trace("Unable to do a combined search because embedding is empty");
                return queryTextSearch(textQuery, getTopK, database);
            }
            return runMatch(embed);
        }, (EmbedError e) {
            logger.tracef(e.errorMsg);
            return queryTextSearch(textQuery, getTopK, database);
        });
    }

    Document[] queryReadFile(Path filePath, long lineNumber, string database) {
        size_t[] indices;
        if (!validateDatabase(database, indices))
            return null;

        auto results = parallelQuery(indices,
                (size_t i) => retrySql!(() => dbs[i].queryByPathAndLine(filePath, lineNumber))).map!(
                a => Document(origin: a.origin, data: a.text, offset: a.offset,
                line: a.line, added: a.added, databaseName: databases[a.dbIndex].name)).array;

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
        bool isPrimary;
    }

    DbSource[] getSources() {
        assert(databases.length == dbs.length, "databases and dbs arrays are out of sync");
        auto rval = appender!(DbSource[])();
        foreach (idx; 0 .. dbs.length) {
            rval.put(DbSource(databases[idx].path, dbs[idx].getSources, idx == 0));
        }
        return rval[];
    }
}

struct RagAddResult {
    size_t length;
    size_t chunks;
}

// Add a document to the RAG. Delegates to the addToDatabase seam using the
// RAG's own per-instance embedder and batch-size cache.
RagAddResult add(RAG rag, Document doc, RagConfig config) {
    return addToDatabase(rag.db, rag.embedder, doc, config, rag.nBatchCache);
}

/// Index a single document into `db`, embedding its chunks with `embedder`.
/// All indexing (knowledge RAG and dialogue) flows through this seam.
///
/// `dedupSalt` (opt-in, empty by default) extends the dedup identity: a
/// non-empty salt makes the identity hash input `dedupSalt ~ "\n" ~ doc.data`
/// instead of `doc.data`, so identical content indexes as a distinct source
/// under each salt. The dialogue worker passes the episode's topic name
/// (topic names are `[a-z0-9_]+` and never contain "\n", so the concatenation
/// is unambiguous); an empty salt hashes `doc.data` exactly as before, keeping
/// knowledge-RAG checksums byte-identical.
RagAddResult addToDatabase(ref Database db, Embedder embedder, Document doc,
        RagConfig config, ref size_t nBatchCache, string dedupSalt = null) {
    import std.algorithm : max, min, countUntil;
    import std.array : Appender;
    import std.json : parseJSON;
    import std.uni : byCodePoint, byGrapheme, isWhite;
    import std.utf : toUTF8;
    import llm.rag.database;
    import llm.utility : getValue, computeContentHash;

    // Dedup identity: salted (topic in the key) for dialogue; the empty-salt
    // path hashes doc.data exactly as before (knowledge-RAG parity).
    const string hashInput = dedupSalt.empty ? doc.data : dedupSalt ~ "\n" ~ doc.data;
    long dataHash = computeContentHash(hashInput);

    if (retrySql!(() => db.hasSource(Source(doc.origin, dataHash.SourceChecksum)))) {
        logger.trace("source already exist in database");
        return RagAddResult(doc.data.length, 0);
    }

    void runOnText(ref size_t chunks, ref Appender!(Embedding[]) embeddings) {
        import core.memory : GC;

        // have to turn off the GC because something in the underlying libraries
        // try to use a pointer while the GC is freeing. The line that most often
        // trigger the GC is appending to graphemes.
        GC.disable();
        scope (exit)
            GC.enable();

        if (nBatchCache == 0)
            nBatchCache = embedder.batchSize();

        immutable nBatchStep = 128;
        immutable MaxIterations = 8;
        size_t nBatch = nBatchCache;

        // used to detect if the fallback mode where nBatch is halfed always used.
        // If it has been used for 5 consecutive turns the nBatch is probably just
        // too high and should be adjusted down.
        int failureCount;
        // detect if we have successfully generated embeddings and then adjust up nBatch if it has been previously lowered
        int successCount;
        void addChunk(Grapheme[] graphemes, size_t startCharPos, size_t startLine, int iteration) {
            auto data = graphemes.byCodePoint.toUTF8;

            float[] emb;
            embedder.embedDocument(data).match!((float[] embed) { emb = embed; }, (EmbedError e) {
                logger.tracef("Failed to generate embedding '%s' (len:%s): %s",
                    e.errorMsg, graphemes.length, data);
                try {
                    const old = nBatch;
                    nBatchCache = max(nBatchStep, nBatchCache);
                    nBatch = max(nBatchStep, min(nBatch, nBatchCache));
                    logger.tracef(old != nBatch, "Changed nBatch (nBatchCache:%s) from %s->%s",
                        nBatchCache, old, nBatch);
                } catch (Exception e) {
                    logger.trace(e.msg);
                }
            });

            if (emb.empty) {
                ++failureCount;
                successCount = 0;
            }

            if (graphemes.length < 4 && emb.empty) {
                logger.tracef("Failed to generate embedding after %s iterations using batch size %s '%s'",
                        iteration, graphemes.length, data);
                return;
            }
            if (emb.empty && iteration < MaxIterations) {
                logger.tracef("%s too large for embedding model. Trying half with nBatch %s",
                        graphemes.length, graphemes.length / 2);
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
            if (iteration == 0) {
                ++successCount;
            }

            embeddings.put(Embedding(Offset(startCharPos, startCharPos + graphemes.length),
                    Line(startLine, startLine + countLines(graphemes)), data, emb));

            logger.tracef("add chunk length:%s line(%s-%s) offset(%s-%s)", data.length, startLine,
                    startLine + countLines(graphemes), startCharPos,
                    startCharPos + graphemes.length);
            ++chunks;
        }

        size_t startCharPos;
        size_t startLine = 1;
        Grapheme[] graphemes;
        foreach (graphem; doc.data.byGrapheme) {
            graphemes ~= graphem;
            if (graphemes.length >= nBatch && graphem[0].isWhite) {
                addChunk(graphemes, startCharPos, startLine, 0);
                const size_t advance = max(cast(size_t) 1,
                        cast(size_t)(graphemes.length * (100.0 - config.windowOverlapPercent) / 100.0));
                const size_t endOfWord = max(0, min(50,
                        graphemes[advance .. $].countUntil!(a => a[0].isWhite))); // assuming a word is never longer than 50
                startCharPos += advance + endOfWord;
                startLine += countLines(graphemes[0 .. advance + endOfWord]);
                if (advance + endOfWord < graphemes.length) {
                    graphemes = graphemes[advance + endOfWord .. $];
                } else {
                    graphemes = null;
                }
            }
            if (failureCount > 2 && nBatch >= nBatchStep * 2) {
                logger.tracef("Adjusting down nBatch %s -> %s", nBatch, nBatch - nBatchStep);
                nBatch -= nBatchStep;
                failureCount = 0;
                failureCount = max(0, failureCount - 1);
                // remember the lower bound so future chunking on other documents works better
                nBatchCache = nBatch;
            } else if (successCount > 5 && nBatch < embedder.batchSize) {
                logger.tracef("Adjusting up nBatch %s -> %s", nBatch, nBatch + nBatchStep);
                nBatch = min(nBatch + nBatchStep, embedder.batchSize);
                successCount = 0;
                // remember the higher bound so future chunking on other documents works better
                nBatchCache = nBatch;
            }
        }
        if (!graphemes.empty) {
            addChunk(graphemes, startCharPos, startLine, 0);
        }
    }

    void runOnTokens(ref size_t chunks, ref Appender!(Embedding[]) embeddings) {
        const nBatch = embedder.batchSize;
        const size_t advance = max(cast(size_t) 1,
                cast(size_t)(nBatch * (100.0 - config.windowOverlapPercent) / 100.0));

        size_t startCharPos;
        size_t startLine = 1;
        size_t halfIndex;
        size_t pinTokenPos;
        int[] tokens;

        Grapheme[] textChunk;
        Grapheme[] currentWord;

        void addChunk() {
            assert(tokens.length <= nBatch, "something is wrong");

            // D2: unpinned flush steps the whole window. Unpinned happens at 0%
            // overlap (advance >= nBatch, pin can never fire), for the final short
            // window, or a pathological oversized word (R9).
            if (halfIndex == 0) {
                halfIndex = textChunk.length;
                pinTokenPos = tokens.length;
            }

            // The overlapped prefix [0 .. pinTokenPos) is dead after the flush
            // (embed returns an owned copy; the stored text is a fresh toUTF8
            // buffer). The slice aliases the previous buffer's storage and
            // `~=` appends grow within the original free capacity (or
            // reallocate), never touching the dead prefix or the retained
            // tail. Do not "fix" this with a per-chunk allocation.
            auto text = textChunk.byCodePoint.toUTF8;
            const lines = countLines(textChunk);

            float[] emb;
            embedder.embedDocument(tokens).match!((float[] embed) { emb = embed; }, (EmbedError e) {
                logger.tracef("Failed to generate embedding '%s' (toks:%s text:%s): %s",
                    e.errorMsg, tokens.length, text.length, text);
            });

            if (!emb.empty) {
                embeddings.put(Embedding(Offset(startCharPos, startCharPos + textChunk.length),
                        Line(startLine, startLine + lines), text, emb));

                logger.tracef("add chunk length:%s line(%s-%s) offset(%s-%s) tokens:%s",
                        text.length, startLine, startLine + lines,
                        startCharPos, startCharPos + textChunk.length, tokens.length);
                ++chunks;
            }

            Grapheme[] advStep = textChunk[0 .. min(halfIndex, textChunk.length)];
            textChunk = textChunk[advStep.length .. $];

            startCharPos += advStep.length;
            startLine += countLines(advStep);
            // D3: the tail is the token suffix past the pin (O(1) slice, no
            // re-tokenization). tokens is the per-word concatenation over
            // textChunk's words (C3) and the pin is a word boundary in both
            // coordinates, so the retained tail is exactly tokens[pinTokenPos .. $].
            tokens = tokens[pinTokenPos .. $];
            halfIndex = 0;
            pinTokenPos = 0;
        }

        foreach (graphem; doc.data.byGrapheme) {
            currentWord ~= graphem;

            // assuming that no sane word is larger than 50 characters
            if (graphem[0].isWhite || currentWord.length > 50) {
                auto wordTokens = embedder.tokenize(currentWord.byCodePoint.toUTF8);
                if (tokens.length + wordTokens.length > nBatch) {
                    addChunk;
                }
                textChunk ~= currentWord;
                tokens ~= wordTokens;
                currentWord = null;
                // D1: token-based pin at the word boundary (replaces the old
                // per-grapheme pin).
                // Invariant (C3): the pin is a word edge in BOTH coordinates
                // (halfIndex / pinTokenPos); they are reset together in addChunk.
                if (halfIndex == 0 && tokens.length > advance) {
                    halfIndex = textChunk.length;
                    pinTokenPos = tokens.length;
                }
            }
        }

        if (!currentWord.empty) {
            auto wordTokens = embedder.tokenize(currentWord.byCodePoint.toUTF8);
            if (tokens.length + wordTokens.length > nBatch) {
                addChunk;
            }
            textChunk ~= currentWord;
            tokens ~= wordTokens;
            // D1: token-based pin at the word boundary (replaces the old
            // per-grapheme pin).
            if (halfIndex == 0 && tokens.length > advance) {
                halfIndex = textChunk.length;
                pinTokenPos = tokens.length;
            }
        }
        if (!textChunk.empty) {
            addChunk();
        }
    }

    size_t chunks;
    auto embeddings = appender!(Embedding[])();

    if (embedder.supportsTokenization) {
        runOnTokens(chunks, embeddings);
    } else {
        runOnText(chunks, embeddings);
    }

    retrySql!(() {
        auto trans = db.transaction;
        // try to remove the source before adding to ensure old cruft isn't left
        db.removeSource(doc.origin);
        auto srcId = db.addSource(Source(doc.origin, SourceChecksum(dataHash)));
        foreach (ref e; embeddings[]) {
            db.addEmbedding(srcId, e);
        }
        trans.commit;
    });

    return RagAddResult(doc.data.length, chunks);
}

private:

size_t countLines(Grapheme[] graphemes) {
    immutable newline = Grapheme('\n');
    return graphemes.filter!(a => a == newline).count;
}

size_t countLines(string s) {
    return s.filter!(a => a == '\n').count;
}

/// Eliminates database-order bias among results with identical ranks.
T[] randomizeRanks(T)(T[] results) {
    import std.random : randomShuffle, rndGen;

    return results.randomShuffle(rndGen);
}

// Helper to create a SourceMatch with a given rank
SourceMatch makeMatch(double rank) {
    return SourceMatch(Origin(Topic("")), Offset(0, 0), Line(0, 0), "", rank, SysTime.init);
}

version (unittest) {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.uni : byCodePoint, isWhite;

    // Deterministic hash-based fake (supportsTokenization == false).
    // embed(x) returns a fixed-dim (8) vector derived from x's contents so
    // equal text -> equal vector and different text -> different vector.
    private class TestEmbedder : Embedder {
        private int batch;

        this(int batchSize = 50) {
            this.batch = batchSize;
        }

        override string modelName() {
            return "test";
        }

        override long dimensions() {
            return 8;
        }

        override bool supportsTokenization() {
            return false;
        }

        EmbedResult embed(string text) {
            // FNV-1a 64-bit: deterministic, no imports needed
            immutable ulong prime = 0x00000100000001B3;
            ulong h = 0xCBF29CE484222325;
            foreach (b; text[])
                h = (h ^ cast(ulong) b) * prime;
            auto vec = new float[8];
            foreach (i; 0 .. 8)
                vec[i] = cast(float)((h >> (8 * i)) & 0xFF) / 255.0;
            return EmbedResult(vec);
        }

        override EmbedResult embedQuery(string text) {
            return embed(text);
        }

        override EmbedResult embedDocument(string text) {
            return embed(text);
        }

        EmbedResult embed(int[] tokens) {
            // never called: supportsTokenization is false
            char[] text = new char[tokens.length];
            foreach (i, ref c; text)
                c = cast(char)(tokens[i] & 0x7F);
            return embed(cast(string) text);
        }

        override EmbedResult embedQuery(int[] tokens) {
            return embed(tokens);
        }

        override EmbedResult embedDocument(int[] tokens) {
            return embed(tokens);
        }

        override int[] tokenize(string text) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            return batch;
        }

        override void destroy() {
        }
    }

    private AbsolutePath makeScratchDir(long line = __LINE__) {
        import std.conv : to;

        auto dir = ("llmfun_test/rag/" ~ line.to!string).AbsolutePath;
        mkdirRecurse(dir);
        return dir;
    }

    // 50 single-letter words separated by spaces: "a b c ... " (100 chars).
    private string sampleText() {
        string text;
        foreach (i; 0 .. 50)
            text ~= cast(char)('a' + i % 26) ~ " ";
        return text;
    }

    // Test-only tokenizing embedder: exactly 1 token per
    // whitespace-separated word (FNV-1a 64 folded to int), deterministic
    // 8-dim vectors (derived from the token bytes). Makes the token-path
    // chunk geometry hand-computable (Tests 9-11).
    private class TokenizingTestEmbedder : Embedder {
        private int batch;
        long embedCalls; // embedDocument(int[]) call count
        long embedTokens; // total embedded token count

        this(int batchSize = 50) {
            this.batch = batchSize;
        }

        override string modelName() {
            return "tokenizing-test";
        }

        override long dimensions() {
            return 8;
        }

        override bool supportsTokenization() {
            return true;
        }

        override int[] tokenize(string text) {
            int[] toks;
            string word;
            foreach (cp; text.byCodePoint) {
                if (cp.isWhite) {
                    if (!word.empty)
                        toks ~= fnv(word);
                    word = null;
                } else
                    word ~= cp;
            }
            if (!word.empty)
                toks ~= fnv(word);
            return toks;
        }

        private static int fnv(string s) {
            immutable ulong prime = 0x00000100000001B3;
            ulong h = 0xCBF29CE484222325;
            foreach (b; s[])
                h = (h ^ cast(ulong) b) * prime;
            return cast(int)(h ^ (h >> 32));
        }

        EmbedResult embed(int[] tokens) {
            immutable ulong prime = 0x00000100000001B3;
            ulong h = 0xCBF29CE484222325;
            foreach (t; tokens)
                h = (h ^ cast(ulong) t) * prime;
            auto v = new float[8];
            foreach (i; 0 .. 8)
                v[i] = cast(float)((h >> (8 * i)) & 0xFF) / 255.0;
            return EmbedResult(v);
        }

        override EmbedResult embedQuery(string text) {
            return embed(tokenize(text));
        }

        override EmbedResult embedDocument(string text) {
            return embed(tokenize(text));
        }

        override EmbedResult embedQuery(int[] tokens) {
            return embed(tokens);
        }

        override EmbedResult embedDocument(int[] tokens) {
            ++embedCalls;
            embedTokens += tokens.length;
            return embed(tokens);
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            return batch;
        }

        override void destroy() {
        }
    }

    // One-letter words `first..last` (1-based, inclusive), space-separated.
    private string wordsRange(long first, long last) {
        string s;
        foreach (i; first .. last + 1)
            s ~= cast(char)('a' + (i - 1) % 26) ~ " ";
        return s;
    }
}

// Test 4: Shuffle produces different orderings
// Probabilistic: 5-element array has 120 permutations;
// chance of false failure is ~ (1/120)^99 ≈ 0
unittest {
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
unittest {
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

// Test 6: addToDatabase indexes a scratch DB; overlap honored; re-add no-op
unittest {
    import std.conv : to;
    import llm.rag.database : openDatabase, Search;
    import my.optional;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    auto emb = new TestEmbedder(50);
    float[] vec(string s) {
        return emb.embed(s).match!((float[] v) => v, (EmbedError e) {
            assert(false, "TestEmbedder failed: " ~ e.errorMsg);
            return null;
        });
    }

    auto dbOpt = openDatabase((dir ~ "t.db").AbsolutePath, "test", 8, readOnly: false);
    assert(dbOpt.hasValue, "openDatabase failed");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);

    auto doc = Document(origin: Origin(Topic("t1")), data: sampleText());
    size_t nBatchCache = 0;
    immutable cfg = RagConfig(windowOverlapPercent: 10);
    auto res = addToDatabase(db, emb, doc, cfg, nBatchCache);
    assert(res.length == doc.data.length);
    assert(res.chunks == 3, "10% overlap: expected 3 chunks, got " ~ res.chunks.to!string);

    // consecutive chunks overlap with 10% overlap
    auto chunks = db.querySemantic(Search(vec(doc.data)), 100).sort!((a,
            b) => a.offset.begin < b.offset.begin).array;
    assert(chunks.length == res.chunks);
    assert(chunks[1].offset.begin < chunks[0].offset.end, "chunk 1 must overlap chunk 0");
    assert(chunks[2].offset.begin < chunks[1].offset.end, "chunk 2 must overlap chunk 1");

    // re-adding the same document is a no-op (hasSource short-circuit)
    auto res2 = addToDatabase(db, emb, doc, cfg, nBatchCache);
    assert(res2.chunks == 0, "re-add of unchanged source must be a no-op");
    assert(db.getSources().length == 1);
    db.destroy;

    // 0% overlap produces non-overlapping chunks for the same text
    auto db0Opt = openDatabase((dir ~ "t0.db").AbsolutePath, "test", 8, readOnly: false);
    assert(db0Opt.hasValue);
    auto db0 = db0Opt.match!((Database d) => d, (None _) => Database.init);
    nBatchCache = 0;
    immutable cfg0 = RagConfig(windowOverlapPercent: 0);
    auto res0 = addToDatabase(db0, emb, doc, cfg0, nBatchCache);
    assert(res0.chunks == 2, "0% overlap: expected 2 chunks, got " ~ res0.chunks.to!string);
    auto chunks0 = db0.querySemantic(Search(vec(doc.data)), 100).sort!((a,
            b) => a.offset.begin < b.offset.begin).array;
    assert(chunks0[1].offset.begin >= chunks0[0].offset.end, "0% overlap must not overlap");
    db0.destroy;
}

// Test 6b: addToDatabase dedup salt (FX1/B4). The same content under two
// different topic/salt pairs indexes as two distinct sources (content-only
// dedup would collapse them to one); a re-add under the same salt is still a
// no-op; and a salt-less add keeps today's bare-content identity (a third
// source here). Mirrors production: the worker's salt is the episode's topic
// name, which is also the document origin.
unittest {
    import std.conv : to;
    import llm.rag.database : openDatabase;
    import my.optional;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    auto emb = new TestEmbedder(50);
    auto dbOpt = openDatabase((dir ~ "s.db").AbsolutePath, "test", 8, readOnly: false);
    assert(dbOpt.hasValue, "openDatabase failed");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    const text = sampleText();
    immutable cfg = RagConfig(windowOverlapPercent: 10);

    // saltA indexes its chunks...
    auto docA = Document(origin: Origin(Topic("saltA")), data: text);
    size_t nBatchA = 0;
    auto resA = addToDatabase(db, emb, docA, cfg, nBatchA, "saltA");
    assert(resA.chunks > 0, "salted add must index chunks, got " ~ resA.chunks.to!string);

    // ...and the same content under saltB must NOT be deduped against it.
    auto docB = Document(origin: Origin(Topic("saltB")), data: text);
    size_t nBatchB = 0;
    auto resB = addToDatabase(db, emb, docB, cfg, nBatchB, "saltB");
    assert(resB.chunks > 0, "identical content under a different salt must index");
    assert(db.getSources().length == 2,
            "two topic/salt pairs must yield two sources, got " ~ db.getSources().length.to!string);

    // Re-add under saltA: the salted identity matches -> no-op (per-salt dedup).
    size_t nBatchA2 = 0;
    auto resAgain = addToDatabase(db, emb, docA, cfg, nBatchA2, "saltA");
    assert(resAgain.chunks == 0, "re-add with the same salt must be a no-op");
    assert(db.getSources().length == 2, "deduped re-add must not add a source");

    // Knowledge-RAG parity: a salt-less add keeps the bare-content identity ->
    // a third, distinct source.
    auto docP = Document(origin: Origin(Topic("plain")), data: text);
    size_t nBatchP = 0;
    auto resP = addToDatabase(db, emb, docP, cfg, nBatchP);
    assert(resP.chunks > 0, "salt-less add must index chunks");
    assert(db.getSources().length == 3,
            "salt-less identity must differ from the salted ones, got " ~ db.getSources()
                .length.to!string);
}

// Test 7: re-adding a changed source purges stale chunks
// (removeSource-then-add order inside one transaction)
// The FTS5 index is external-content (database.d FTSChunksSql): it is not
// maintained by the indexing path, so the test rebuilds it explicitly,
// mirroring the production caller contract (tool_call/rag.d, app_rag.d).
// The purge itself is verified at the TextChunkTbl row level.
unittest {
    import llm.rag.database : openDatabase;
    import my.optional;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    auto emb = new TestEmbedder(50);
    auto dbOpt = openDatabase((dir ~ "p.db").AbsolutePath, "test", 8, readOnly: false);
    assert(dbOpt.hasValue, "openDatabase failed");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    long countChunkRowsContaining(string needle) {
        static immutable sql = "SELECT count(*) FROM TextChunkTbl WHERE text LIKE :needle";
        auto stmt = db.prepare(sql);
        stmt.get.bind(":needle", "%" ~ needle ~ "%");
        foreach (ref r; stmt.get.execute) {
            return r.peek!long(0);
        }
        return -1;
    }

    auto origin = Origin(Topic("t1"));
    immutable cfg = RagConfig(windowOverlapPercent: 10);

    // first version
    auto text1 = sampleText() ~ "zebra ";
    size_t nBatchCache = 0;
    auto res1 = addToDatabase(db, emb, Document(origin: origin, data: text1), cfg, nBatchCache);
    assert(res1.chunks > 0);
    assert(countChunkRowsContaining("zebra") > 0, "first version must be stored");
    db.fts5Rebuild;
    auto hit1 = db.queryTextSearch("zebra", 10);
    assert(hit1 != null && hit1.length > 0, "first version must be searchable");

    // changed content, same origin: old chunks must be purged, not accumulated
    auto text2 = sampleText() ~ "quokka ";
    nBatchCache = 0;
    auto res2 = addToDatabase(db, emb, Document(origin: origin, data: text2), cfg, nBatchCache);
    assert(res2.chunks > 0);
    assert(db.getSources().length == 1, "changed source must replace, not accumulate");
    assert(countChunkRowsContaining("zebra") == 0, "stale chunk rows must be purged");
    assert(countChunkRowsContaining("quokka") > 0, "new chunk rows must be stored");
    db.fts5Rebuild;
    auto stale = db.queryTextSearch("zebra", 10);
    assert(stale == null || stale.length == 0, "stale chunks of changed source must be purged");
    auto fresh = db.queryTextSearch("quokka", 10);
    assert(fresh != null && fresh.length > 0, "new chunks must be queryable");
}

// Test 8: add(rag, ...) and addToDatabase(rag.db, rag.embedder, ...)
// produce identical database contents
unittest {
    import std.conv : to;
    import llm.rag.database : openDatabase, Search;
    import my.optional;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    auto embA = new TestEmbedder(50);
    auto rag = new RAG(embA, RagDatabaseConfig(dir ~ "a.db", "a"), null);
    scope (exit)
        rag.destroy;

    auto embB = new TestEmbedder(50);
    auto dbBOpt = openDatabase((dir ~ "b.db").AbsolutePath, "test", 8, readOnly: false);
    assert(dbBOpt.hasValue);
    auto dbB = dbBOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        dbB.destroy;

    float[] vecA(string s) {
        return embA.embed(s).match!((float[] v) => v, (EmbedError e) {
            assert(false, "TestEmbedder failed: " ~ e.errorMsg);
            return null;
        });
    }

    auto doc = Document(origin: Origin(Topic("t1")), data: sampleText());
    immutable cfg = RagConfig(windowOverlapPercent: 10);
    size_t nBatchCache = 0;
    auto resA = add(rag, doc, cfg);
    size_t nBatchCacheB = 0;
    auto resB = addToDatabase(dbB, embB, doc, cfg, nBatchCacheB);
    assert(resA.chunks > 0 && resA.chunks == resB.chunks,
            "chunk counts differ: " ~ resA.chunks.to!string ~ " vs " ~ resB.chunks.to!string);

    auto dbA = rag.db;

    // same source: origin + checksum
    auto srcA = dbA.getSources;
    auto srcB = dbB.getSources;
    assert(srcA.length == 1 && srcB.length == 1);
    assert(srcA[0].origin == srcB[0].origin, "origins differ");
    assert(srcA[0].checksum == srcB[0].checksum, "checksums differ");

    // same chunk texts and offsets for a series of probe vectors
    string[] probes = [doc.data, "a b c", "y z x"];
    foreach (probe; probes) {
        auto ra = dbA.querySemantic(Search(vecA(probe)), 100);
        auto rb = dbB.querySemantic(Search(vecA(probe)), 100);
        assert(ra.length == rb.length, "probe '" ~ probe ~ "': result counts differ");
        foreach (i; 0 .. ra.length) {
            assert(ra[i].text == rb[i].text, "probe '" ~ probe ~ "': chunk text differs");
            assert(ra[i].offset == rb[i].offset, "probe '" ~ probe ~ "': chunk offset differs");
        }
    }

    // stored vectors equal the expected per-chunk vectors: querying with
    // a chunk's own vector returns that chunk at rank 1 (distance 0)
    foreach (m; dbA.querySemantic(Search(vecA(doc.data)), 100)) {
        auto hit = dbA.querySemantic(Search(vecA(m.text)), 10);
        assert(hit.length > 0 && hit[0].rank == 1 && hit[0].text == m.text,
                "chunk must self-locate: " ~ m.text);
    }
}

// Test 9: token path, 10% overlap — uniform token-based steps (F1).
// 100 one-letter words (200 graphemes); 1 token/word; nBatch 50 →
// advance 45. Expected windows (1-based word idx): [1..50], [47..96],
// [93..100] → grapheme offsets (0,100), (92,192), (184,200).
unittest {
    import std.conv : to;
    import llm.rag.database : openDatabase, Search;
    import my.optional;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    immutable text = wordsRange(1, 100);

    auto emb = new TokenizingTestEmbedder(50);
    auto dbOpt = openDatabase((dir ~ "t10.db").AbsolutePath, "test", 8, readOnly: false);
    assert(dbOpt.hasValue, "openDatabase failed");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);

    auto doc = Document(origin: Origin(Topic("t10")), data: text);
    size_t nBatchCache = 0;
    immutable cfg = RagConfig(windowOverlapPercent: 10);
    auto res = addToDatabase(db, emb, doc, cfg, nBatchCache);
    assert(res.chunks == 3, "10% token overlap: expected 3 chunks, got " ~ res.chunks.to!string);
    assert(emb.embedCalls == 3, "expected 3 embedDocument calls");
    assert(emb.embedTokens == 108,
            "expected 50+50+8 embedded tokens, got " ~ emb.embedTokens.to!string);

    auto chunks = db.querySemantic(Search(emb.embedDocument(text)
            .match!((float[] v) => v, (EmbedError e) {
                assert(false, "TokenizingTestEmbedder failed: " ~ e.errorMsg);
                return null;
            })), 100).sort!((a, b) => a.offset.begin < b.offset.begin).array;
    assert(chunks.length == 3);
    assert(chunks[0].offset.begin == 0 && chunks[0].offset.end == 100, "chunk 0 offset");
    assert(chunks[1].offset.begin == 92 && chunks[1].offset.end == 192, "chunk 1 offset");
    assert(chunks[2].offset.begin == 184 && chunks[2].offset.end == 200, "chunk 2 offset");
    assert(chunks[0].text == wordsRange(1, 50), "chunk 0 text");
    assert(chunks[1].text == wordsRange(47, 96), "chunk 1 text");
    assert(chunks[2].text == wordsRange(93, 100), "chunk 2 text");

    // F1: every chunk-start advance in [advance, advance + W_max]
    // words; here W_max == 1 (one token per word), advance == 45.
    foreach (i; 0 .. 2) {
        long stepWords = (cast(long) chunks[i + 1].offset.begin - cast(long) chunks[i].offset.begin) / 2;
        assert(stepWords >= 45 && stepWords <= 46,
                "step uniformity violated: " ~ stepWords.to!string);
    }
    db.destroy;
}

// Test 10: token path, 0% overlap — contiguous, non-overlapping (F3).
// BEHAVIOR CHANGE: the old grapheme pin left ~47-49% effective overlap
// here (3 chunks); advance == nBatch so the pin never fires and the D2
// whole-window step yields exactly 2 contiguous chunks.
unittest {
    import std.conv : to;
    import llm.rag.database : openDatabase, Search;
    import my.optional;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    immutable text = wordsRange(1, 100);

    auto emb = new TokenizingTestEmbedder(50);
    auto dbOpt = openDatabase((dir ~ "t0.db").AbsolutePath, "test", 8, readOnly: false);
    assert(dbOpt.hasValue, "openDatabase failed");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);

    auto doc = Document(origin: Origin(Topic("t0")), data: text);
    size_t nBatchCache = 0;
    immutable cfg0 = RagConfig(windowOverlapPercent: 0);
    auto res0 = addToDatabase(db, emb, doc, cfg0, nBatchCache);
    assert(res0.chunks == 2, "0% token overlap: expected 2 chunks, got " ~ res0.chunks.to!string);
    assert(emb.embedTokens == 100,
            "expected 50+50 embedded tokens, got " ~ emb.embedTokens.to!string);

    auto chunks0 = db.querySemantic(Search(emb.embedDocument(text)
            .match!((float[] v) => v, (EmbedError e) {
                assert(false, "TokenizingTestEmbedder failed: " ~ e.errorMsg);
                return null;
            })), 100).sort!((a, b) => a.offset.begin < b.offset.begin).array;
    assert(chunks0.length == 2);
    assert(chunks0[0].offset.begin == 0 && chunks0[0].offset.end == 100, "chunk 0 offset");
    assert(chunks0[1].offset.begin == 100 && chunks0[1].offset.end == 200, "chunk 1 offset");
    assert(chunks0[0].text == wordsRange(1, 50), "chunk 0 text");
    assert(chunks0[1].text == wordsRange(51, 100), "chunk 1 text");
    assert(chunks0[1].offset.begin >= chunks0[0].offset.end, "0% overlap must not overlap");
    db.destroy;
}

// Test 11: token path dedup — re-adding the unchanged document is a
// no-op (hasSource short-circuit; same behavior as runOnText Test 6).
unittest {
    import llm.rag.database : openDatabase;
    import my.optional;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    auto emb = new TokenizingTestEmbedder(50);
    auto dbOpt = openDatabase((dir ~ "td.db").AbsolutePath, "test", 8, readOnly: false);
    assert(dbOpt.hasValue, "openDatabase failed");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);

    auto doc = Document(origin: Origin(Topic("td")), data: wordsRange(1, 100));
    size_t nBatchCache = 0;
    immutable cfg = RagConfig(windowOverlapPercent: 10);
    auto res = addToDatabase(db, emb, doc, cfg, nBatchCache);
    assert(res.chunks == 3);

    size_t nBatchCache2 = 0;
    auto res2 = addToDatabase(db, emb, doc, cfg, nBatchCache2);
    assert(res2.chunks == 0, "re-add of unchanged source must be a no-op");
    assert(db.getSources().length == 1);
    db.destroy;
}
