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
/// array, then the shuffled copy is sorted (stable) by rank and truncated to top-K. The
/// original array is never mutated.

module llm.rag.rag;

import logger = std.logger;
import std.algorithm : map, filter, joiner, sort, cache, count, SwapStrategy;
import std.array : array, empty, appender;
import std.conv : text;
import std.datetime : SysTime;
import std.parallelism;
import std.path : baseName, stripExtension;
import std.range : enumerate, iota;
import std.stdio : File;
import std.string : strip;
import std.sumtype;
import std.uni : Grapheme;

import miniorm : spinSql;
import llm.test_util : retrySql;
import my.path;
public import my.path : Path;

import llm.common.embedder;
import llm.config : RagDatabaseConfig, RagConfig;
import llm.rag.database : SourceMatch, Database;
import llm.rag.optimize_counter : incrementOptimizeCounter, readOptimizeCounter;

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

private string toPrefix(Origin origin) {
    return origin.match!((Topic a) => i"Topic: $(a.name)".text,
            (Url a) => i"Url: $(a.value)".text, (Path a) => i"File: $(a.toString)".text) ~ " | ";
}

private immutable size_t MaxFromSource = 2;

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

// takePerSource: round-robin over the (rank-sorted) matches. Each pass picks
// at most maxTakeFromSource entries per database, so the result interleaves
// each database's best, next best, ... instead of taking everything from a
// single database. NOT strict global rank order. topK bounds the total picks.
private IndexedMatch[] takePerSource(IndexedMatch[] matches, const size_t topK,
        const size_t maxTakeFromSource)
in (maxTakeFromSource > 0) {
    auto rval = appender!(IndexedMatch[])();
    auto input = matches;
    auto output = appender!(IndexedMatch[])();
    output.reserve(input.length);

    while (rval.length < topK && !input.empty) {
        int[size_t] picked;
        foreach (item; input) {
            if (rval.length >= topK)
                return rval[];
            if (picked.get(item.dbIndex, 0) >= maxTakeFromSource) {
                output.put(item);
            } else {
                picked.update(item.dbIndex, () => 1, (ref int v) { ++v; });
                rval.put(item);
            }
        }
        input = output[];
        output.clear;
    }

    return rval[];
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

    this(Embedder embedder, RagDatabaseConfig primary,
            RagDatabaseConfig[] secondary, RagConfig ragConfig = RagConfig.init)
    in (embedder !is null) {
        import my.optional;
        import llm.rag.database : openDatabase;

        this.embedder = embedder;

        openDatabase(primary.path.AbsolutePath, embedder.modelName, embedder.dimensions, readOnly: false,
                busyTimeout: ragConfig.busyTimeout, mmrLambda: ragConfig.mmrLambda).match!(
                (Database db) {
            this.dbs.insertBack(db);
            this.databases ~= DatabaseInfo(primary.path,
                primary.path.baseName.stripExtension, primary.description);
        }, (None _) {
            openDatabase(primary.path.AbsolutePath, embedder.modelName, embedder.dimensions, readOnly: false,
                inMemory: true, busyTimeout: ragConfig.busyTimeout,
                mmrLambda: ragConfig.mmrLambda).match!((Database db) {
                this.dbs.insertBack(db);
                this.databases ~= DatabaseInfo(":memory:".Path,
                primary.path.baseName.stripExtension, primary.description);
            }, (None _) {
                logger.errorf("sqlite3 fatal error. This should not happen");
            });
        });

        foreach (cfg; secondary) {
            openDatabase(cfg.path.AbsolutePath, embedder.modelName, embedder.dimensions, readOnly: true,
                    inMemory: false, busyTimeout: ragConfig.busyTimeout,
                    mmrLambda: ragConfig.mmrLambda).match!((Database db) {
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

    /// Remove the source and log it to the optimize counter.
    long removeSource(Origin origin) {
        auto removed = dbs[0].removeSource(origin);
        if (removed != 0)
            incrementOptimizeCounter(dbs[0].db);
        return removed;
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

        // rank: shorter distance is better so lowest is best
        Document[] runMatch(float[] embed) {
            return parallelQuery(indices,
                    (size_t i) => retrySql!(() => dbs[i].querySemantic(Search(embed), getTopK))).randomizeRanks(query)
                .sort!((a, b) => a.rank < b.rank, SwapStrategy.stable).array.takePerSource(getTopK, MaxFromSource)
                .map!(a => Document(origin: a.origin, data: a.text, offset: a.offset, line: a.line,
                        added: a.added, databaseName: databases[a.dbIndex].name)).array;
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

        // rank: bm25 where more negative is better
        return parallelQuery(indices,
                (size_t i) => retrySql!(() => dbs[i].queryTextSearch(query, getTopK))).randomizeRanks(query).sort!((a,
                b) => a.rank < b.rank, SwapStrategy.stable).array.takePerSource(getTopK, MaxFromSource)
            .map!(a => Document(origin: a.origin, data: a.text, offset: a.offset,
                    line: a.line, added: a.added, databaseName: databases[a.dbIndex].name)).array;
    }

    Document[] queryBestMatch(string textQuery, string vectorQuery, long getTopK, string database) {
        size_t[] indices;
        if (!validateDatabase(database, indices))
            return null;

        Document[] runMatch(float[] embed) {
            // rank: rrf where higher is better
            return parallelQuery(indices,
                    (size_t i) => retrySql!(() => dbs[i].queryCombineSemanticText(Search(embed),
                        textQuery, getTopK))).randomizeRanks(textQuery).sort!((a,
                    b) => a.rank > b.rank, SwapStrategy.stable).array.takePerSource(getTopK, MaxFromSource)
                .map!(a => Document(origin: a.origin, data: a.text, offset: a.offset, line: a.line,
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

    /// Origin label used by source listings and read results.
    static string originLabel(Origin origin) {
        return origin.match!((Topic a) => "topic:" ~ a.name,
                (Url a) => "url:" ~ a.value, (Path a) => a.toString);
    }

    struct SourceListing {
        string database;
        string origin;
        long chunks;
    }

    /// List all sources (documents) with chunk counts, optionally scoped to
    /// one database ('*' or empty = all).
    SourceListing[] listSources(string database = "*") {
        size_t[] indices;
        if (!validateDatabase(database, indices))
            return null;

        auto rval = appender!(SourceListing[])();
        foreach (idx; indices) {
            foreach (entry; dbs[idx].getSourceEntries) {
                rval.put(SourceListing(databases[idx].name,
                        originLabel(entry.src.origin), dbs[idx].chunkCount(entry.id)));
            }
        }
        return rval[];
    }

    struct FullSource {
        Origin origin;
        string text;
        long chunks;
        SysTime added;
        string databaseName;
    }

    /// Read the full reconstructed text of every source whose path equals
    /// filePath or ends with it (suffix match, e.g. the bare file name).
    FullSource[] readSource(Path filePath, string database) {
        import my.optional : None;

        size_t[] indices;
        if (!validateDatabase(database, indices))
            return null;

        auto rval = appender!(FullSource[])();
        foreach (idx; indices) {
            foreach (id; retrySql!(() => dbs[idx].findPathSources(filePath))) {
                auto src = dbs[idx].getSource(id);
                src.match!((Source s) {
                    rval.put(FullSource(s.origin, dbs[idx].sourceText(id),
                        dbs[idx].chunkCount(id), s.added, databases[idx].name));
                }, (None _) {});
            }
        }
        return rval[];
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

    auto chunkPrefix = doc.origin.toPrefix;

    void runOnText(ref size_t chunks, ref Appender!(Embedding[]) embeddings, string text) {
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
        // The chunk prefix is prepended to every chunk before embedding, so it
        // eats into the batch budget. If the prefix alone fills the budget,
        // reserve nothing: the halving fallback below covers models that still
        // reject the prefix + chunk text.
        size_t nBatch = nBatchCache > chunkPrefix.length
            ? nBatchCache - chunkPrefix.length : nBatchCache;

        // used to detect if the fallback mode where nBatch is halfed always used.
        // If it has been used for 5 consecutive turns the nBatch is probably just
        // too high and should be adjusted down.
        int failureCount;
        // detect if we have successfully generated embeddings and then adjust up nBatch if it has been previously lowered
        int successCount;
        void addChunk(Grapheme[] graphemes, size_t startCharPos, size_t startLine, int iteration) {
            auto data = graphemes.byCodePoint.toUTF8;

            float[] emb;
            embedder.embedDocument(chunkPrefix ~ data).match!((float[] embed) {
                emb = embed;
            }, (EmbedError e) {
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
        foreach (graphem; text.byGrapheme) {
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

    void runOnTokens(ref size_t chunks, ref Appender!(Embedding[]) embeddings, string text) {
        auto prefixTokens = embedder.tokenize(chunkPrefix, addSpecial: false);
        const nBatch = () {
            return embedder.batchSize > prefixTokens.length
                ? embedder.batchSize - prefixTokens.length : embedder.batchSize;
        }();
        const size_t advance = max(cast(size_t) 1,
                cast(size_t)(nBatch * (100.0 - config.windowOverlapPercent) / 100.0));

        size_t startCharPos;
        size_t startLine = 1;
        size_t halfIndex;
        size_t pinTokenPos;
        int[] tokens;

        Grapheme[] textChunk;
        Grapheme[] currentWord;

        void addChunk(ref int[] tokens, ref Grapheme[] textChunk) {
            assert(tokens.length <= nBatch, "something is wrong");

            // unpinned flush steps the whole window. Unpinned happens at 0%
            // overlap (advance >= nBatch, pin can never fire), for the final short
            // window, or a pathological oversized word.
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
            // reuse the internal logic and model understanding of llama.cpp by
            // re-tokenize. If models in the future are added that use other
            // schemas than BOS/EOS the RAG system do not need to be updated,
            // llama.cpp do it for us. This is less efficient than using
            // `tokens` as is and wrap with BOS/EOS but should be more stable
            // to future changes.
            // TODO: the above approach did not work. It resulted in more tokens than context and failed.
            embedder.embedDocument(prefixTokens ~ tokens).match!((float[] embed) {
                emb = embed;
            }, (EmbedError e) {
                logger.warningf("DROPPED chunk. Failed to generate embedding: '%s' (toks:%s text:%s): %s",
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
            // The tail is the token suffix past the pin (O(1) slice, no
            // re-tokenization). tokens is the per-word concatenation over
            // textChunk's words and the pin is a word boundary in both
            // coordinates, so the retained tail is exactly tokens[pinTokenPos .. $].
            tokens = tokens[pinTokenPos .. $];
            halfIndex = 0;
            pinTokenPos = 0;
        }

        foreach (graphem; text.byGrapheme) {
            currentWord ~= graphem;

            // assuming that no sane word is larger than 50 characters
            if (graphem[0].isWhite || currentWord.length > 50) {
                auto wordTokens = embedder.tokenize(currentWord.byCodePoint.toUTF8,
                        addSpecial: false);
                if (tokens.length + wordTokens.length > nBatch) {
                    addChunk(tokens, textChunk);
                }
                textChunk ~= currentWord;
                tokens ~= wordTokens;
                currentWord = null;
                // token-based pin at the word boundary (replaces the old
                // per-grapheme pin).
                // Invariant: the pin is a word edge in BOTH coordinates
                // (halfIndex / pinTokenPos); they are reset together in addChunk.
                if (halfIndex == 0 && tokens.length > advance) {
                    halfIndex = textChunk.length;
                    pinTokenPos = tokens.length;
                }
            }
        }

        if (!currentWord.empty) {
            auto wordTokens = embedder.tokenize(currentWord.byCodePoint.toUTF8, addSpecial: false);
            if (tokens.length + wordTokens.length > nBatch) {
                addChunk(tokens, textChunk);
            }
            textChunk ~= currentWord;
            tokens ~= wordTokens;
        }
        if (!textChunk.empty) {
            addChunk(tokens, textChunk);
        }
    }

    size_t chunks;
    auto embeddings = appender!(Embedding[])();

    if (embedder.supportsTokenization) {
        runOnTokens(chunks, embeddings, doc.data);
    } else {
        runOnText(chunks, embeddings, doc.data);
    }

    // Explicit begin/commit instead of miniorm's Transaction: a rollback that
    // fails on an already-broken database must be catchable here, not escape
    // the RAII struct's nothrow destructor and abort the process.
    retrySql!(() {
        db.begin;
        try {
            // try to remove the source before adding to ensure old cruft isn't left
            db.removeSource(doc.origin);
            auto srcId = db.addSource(Source(doc.origin, SourceChecksum(dataHash)));
            foreach (ref e; embeddings[]) {
                db.addEmbedding(srcId, e);
            }
            db.commit;
        } catch (Exception e) {
            try {
                db.rollback;
            } catch (Exception re) {
                logger.warningf("RAG add: rollback failed while handling '%s': %s", e.msg, re.msg);
            }
            throw e;
        }
    });

    // Bump the optimize counter after the successful commit; a top-level add counts 1.
    // The update path is the removeSource-then-addSource inside the transaction above, so this single increment covers it.
    // Separate statement on purpose: a crash before it loses a count, which only delays an optimize run.
    incrementOptimizeCounter(db.db);

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

/* Deterministic shuffle based on the query to eliminate database-order bias
 * among results with identical ranks.
 * It is important that it is deterministic because the agent loop is informed
 * that the same query will give the same result. If it would return different
 * results it breaks the agentic loop and it would determine that the
 * database/query functions are unstable.
 */
T[] randomizeRanks(T)(T[] results, string query) {
    import std.random : randomShuffle, Random;
    import llm.utility : computeContentHash;

    auto rnd = Random(cast(uint) computeContentHash(query));
    return results.randomShuffle(rnd);
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

        override int[] tokenize(string text, bool addSpecial) {
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

        override int[] tokenize(string text, bool addSpecial) {
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
            return embed(tokenize(text, true));
        }

        override EmbedResult embedDocument(string text) {
            return embed(tokenize(text, true));
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

@("randomizeRanks is deterministic per query (same query shuffles the same way) and seed-sensitive (different queries differ)")
unittest {
    import std.conv : to;

    SourceMatch[] input = [
        makeMatch(1.0), makeMatch(2.0), makeMatch(3.0), makeMatch(4.0),
        makeMatch(5.0)
    ];
    auto first = randomizeRanks(input.dup, "foo");
    foreach (_; 0 .. 100) {
        auto current = randomizeRanks(input.dup, "foo");
        foreach (i; 0 .. first.length) {
            assert(current[i].rank == first[i].rank,
                    "same query must give the same ordering: "
                    ~ current[i].rank.to!string ~ " vs " ~ first[i].rank.to!string);
        }
    }

    bool differs = false;
    auto other = randomizeRanks(input.dup, "bar");
    foreach (i; 0 .. first.length) {
        if (other[i].rank != first[i].rank)
            differs = true;
    }
    assert(differs, "different queries should shuffle differently (1/120 collision)");
}

@(
        "randomizeRanks preserves the rank multiset as a permutation and different queries change the shuffle")
unittest {
    import std.conv : to;
    import std.algorithm : sort, canFind;

    SourceMatch[] input = [
        makeMatch(10.0), makeMatch(20.0), makeMatch(30.0), makeMatch(40.0)
    ];
    double[] expected = [10.0, 20.0, 30.0, 40.0];
    foreach (_; 0 .. 100) {
        auto result = randomizeRanks(input.dup, "foo");
        double[] got = result.map!(m => m.rank).array;
        got.sort;
        assert(got == expected, "result must be a permutation of the input ranks: " ~ got
                .to!string);
    }

    // 20 distinct query seeds: the first position must vary (P(all equal)
    // = 4 * (1/4)^19 ~= 1e-11 for a uniform shuffle).
    double[] firstRanks;
    foreach (i; 0 .. 20) {
        firstRanks ~= randomizeRanks(input.dup, "query-" ~ i.to!string)[0].rank;
    }
    size_t distinct = 0;
    double[] seen;
    foreach (r; firstRanks) {
        if (!seen.canFind(r)) {
            seen ~= r;
            distinct++;
        }
    }
    assert(distinct > 1,
            "different queries should shuffle differently, distinct firsts: " ~ distinct.to!string);
}

@(
        "addToDatabase indexes a scratch DB honoring the overlap setting, and re-adding the same document is a no-op")
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
    assert(res0.chunks == 3, "0% overlap: expected 3 chunks, got " ~ res0.chunks.to!string);
    auto chunks0 = db0.querySemantic(Search(vec(doc.data)), 100).sort!((a,
            b) => a.offset.begin < b.offset.begin).array;
    assert(chunks0[1].offset.begin >= chunks0[0].offset.end, "0% overlap must not overlap");
    db0.destroy;
}

// addToDatabase dedup salt. The same content under two
// different topic/salt pairs indexes as two distinct sources (content-only
// dedup would collapse them to one); a re-add under the same salt is still a
// no-op; and a salt-less add keeps today's bare-content identity (a third
// source here). Mirrors production: the worker's salt is the episode's topic
// name, which is also the document origin.
@("the same content under two different topic salts indexes as two sources, a same-salt re-add is a no-op, and a salt-less add keeps bare-content identity")
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

// re-adding a changed source purges stale chunks
// (removeSource-then-add order inside one transaction)
// The FTS5 index is external-content (database.d FTSChunksSql): it is not
// maintained by the indexing path, so the test rebuilds it explicitly,
// mirroring the production caller contract (tool_call/rag.d, app_rag.d).
// The purge itself is verified at the TextChunkTbl row level.
@(
        "re-adding a changed source purges its stale chunks and the new chunks are text-searchable after the FTS rebuild")
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

@("add(rag, ...) and addToDatabase(rag.db, rag.embedder, ...) produce identical database contents")
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
    // a chunk's own vector (prefix + text, as embedded) returns that
    // chunk at rank 1 (distance 0)
    foreach (m; dbA.querySemantic(Search(vecA(doc.data)), 100)) {
        auto hit = dbA.querySemantic(Search(vecA(toPrefix(m.origin) ~ m.text)), 10);
        assert(hit.length > 0 && hit[0].rank == 1 && hit[0].text == m.text,
                "chunk must self-locate: " ~ m.text);
    }
}

@(
        "token-path 10% overlap chunks with uniform token-based steps and the expected offsets and embedded token counts")
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
    assert(emb.embedTokens == 117,
            "expected 50+50+17 embedded tokens, got " ~ emb.embedTokens.to!string);

    auto chunks = db.querySemantic(Search(emb.embedDocument(text)
            .match!((float[] v) => v, (EmbedError e) {
                assert(false, "TokenizingTestEmbedder failed: " ~ e.errorMsg);
                return null;
            })), 100).sort!((a, b) => a.offset.begin < b.offset.begin).array;
    assert(chunks.length == 3);
    assert(chunks[0].offset.begin == 0 && chunks[0].offset.end == 94, "chunk 0 offset");
    assert(chunks[1].offset.begin == 86 && chunks[1].offset.end == 180, "chunk 1 offset");
    assert(chunks[2].offset.begin == 172 && chunks[2].offset.end == 200, "chunk 2 offset");
    assert(chunks[0].text == wordsRange(1, 47), "chunk 0 text");
    assert(chunks[1].text == wordsRange(44, 90), "chunk 1 text");
    assert(chunks[2].text == wordsRange(87, 100), "chunk 2 text");

    // every chunk-start advance in [advance, advance + W_max]
    // words; here W_max == 1 (one token per word), advance == 42.
    foreach (i; 0 .. 2) {
        long stepWords = (cast(long) chunks[i + 1].offset.begin - cast(long) chunks[i].offset.begin) / 2;
        assert(stepWords >= 42 && stepWords <= 43,
                "step uniformity violated: " ~ stepWords.to!string);
    }
    db.destroy;
}

@("token-path 0% overlap yields contiguous non-overlapping chunks with the expected offsets")
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
    assert(res0.chunks == 3, "0% token overlap: expected 3 chunks, got " ~ res0.chunks.to!string);
    assert(emb.embedTokens == 109,
            "expected 50+50+9 embedded tokens, got " ~ emb.embedTokens.to!string);

    auto chunks0 = db.querySemantic(Search(emb.embedDocument(text)
            .match!((float[] v) => v, (EmbedError e) {
                assert(false, "TokenizingTestEmbedder failed: " ~ e.errorMsg);
                return null;
            })), 100).sort!((a, b) => a.offset.begin < b.offset.begin).array;
    assert(chunks0.length == 3);
    assert(chunks0[0].offset.begin == 0 && chunks0[0].offset.end == 94, "chunk 0 offset");
    assert(chunks0[1].offset.begin == 94 && chunks0[1].offset.end == 188, "chunk 1 offset");
    assert(chunks0[2].offset.begin == 188 && chunks0[2].offset.end == 200, "chunk 2 offset");
    assert(chunks0[0].text == wordsRange(1, 47), "chunk 0 text");
    assert(chunks0[1].text == wordsRange(48, 94), "chunk 1 text");
    assert(chunks0[2].text == wordsRange(95, 100), "chunk 2 text");
    foreach (i; 0 .. 2)
        assert(chunks0[i + 1].offset.begin >= chunks0[i].offset.end, "0% overlap must not overlap");
    db.destroy;
}

@("re-adding the unchanged document on the token path is a no-op via the hasSource short-circuit")
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

// Auto-optimize write counter: a top-level add counts 1, the update path (remove + add inside one transaction) counts 1 total, and a deduped no-op re-add counts nothing.
@(
        "the auto-optimize counter counts an add and an update as one write each and a no-op re-add as none")
unittest {
    import std.conv : to;
    import llm.rag.database : openDatabase;
    import llm.rag.optimize_counter : readOptimizeCounter;
    import my.optional;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    auto emb = new TestEmbedder(50);
    auto dbOpt = openDatabase((dir ~ "c.db").AbsolutePath, "test", 8, readOnly: false);
    assert(dbOpt.hasValue, "openDatabase failed");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    const origin = Origin(Topic("c1"));
    immutable cfg = RagConfig(windowOverlapPercent: 10);

    size_t nBatch1 = 0;
    addToDatabase(db, emb, Document(origin: origin, data: sampleText()), cfg, nBatch1);
    assert(readOptimizeCounter(db.db) == 1,
            "one top-level add must count 1, got " ~ readOptimizeCounter(db.db).to!string);

    // deduped re-add of unchanged content is a no-op: no increment
    size_t nBatch2 = 0;
    addToDatabase(db, emb, Document(origin: origin, data: sampleText()), cfg, nBatch2);
    assert(readOptimizeCounter(db.db) == 1,
            "deduped re-add must not count, got " ~ readOptimizeCounter(db.db).to!string);

    // changed content under the same origin is an update: still one write total
    size_t nBatch3 = 0;
    addToDatabase(db, emb, Document(origin: origin, data: sampleText() ~ "more "), cfg, nBatch3);
    assert(readOptimizeCounter(db.db) == 2,
            "an update must count 1 (2 total), got " ~ readOptimizeCounter(db.db).to!string);
}

// The RAG-level removeSource wrapper: a removal that removed something counts 1, a removal of an unknown origin counts nothing.
@("rag.removeSource counts one write for an actual removal and none for an unknown origin")
unittest {
    import std.conv : to;
    import llm.rag.optimize_counter : readOptimizeCounter;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    auto emb = new TestEmbedder(50);
    auto rag = new RAG(emb, RagDatabaseConfig(dir ~ "r.db", "r"), null);
    scope (exit)
        rag.destroy;

    immutable cfg = RagConfig(windowOverlapPercent: 10);
    auto doc = Document(origin: Origin(Topic("r1")), data: sampleText());
    size_t nBatchCache = 0;
    add(rag, doc, cfg);
    auto db = rag.db;
    assert(readOptimizeCounter(db.db) == 1,
            "the add must count 1, got " ~ readOptimizeCounter(db.db).to!string);

    auto removed = rag.removeSource(Origin(Topic("r1")));
    assert(removed != 0, "the source must actually be removed");
    assert(readOptimizeCounter(db.db) == 2,
            "an actual removal must count 1 (2 total), got " ~ readOptimizeCounter(db.db)
                .to!string);

    auto nothing = rag.removeSource(Origin(Topic("never-added")));
    assert(nothing == 0, "removing an unknown origin must remove nothing");
    assert(readOptimizeCounter(db.db) == 2,
            "a no-op removal must not count, got " ~ readOptimizeCounter(db.db).to!string);
}

// Schema bump migration: a database at the previous schema version is wiped and rebuilt on a read-write open, which recreates the counter table empty.
@(
        "a database at the previous schema version wipes and rebuilds on read-write open and the counter starts at 0")
unittest {
    import std.conv : to;
    import llm.rag.database : openDatabase;
    import llm.rag.optimize_counter : readOptimizeCounter;
    import my.optional;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    auto dbOpt = openDatabase((dir ~ "m.db").AbsolutePath, "test", 8, readOnly: false);
    assert(dbOpt.hasValue, "openDatabase failed");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    db.destroy;

    // Simulate a database created by the previous schema version: version 6 without the counter table (a fresh v7 database already has it).
    {
        auto oldOpt = openDatabase((dir ~ "m.db").AbsolutePath, "test", 8, readOnly: false);
        assert(oldOpt.hasValue, "reopen failed");
        auto oldDb = oldOpt.match!((Database d) => d, (None _) => Database.init);
        oldDb.run(`UPDATE VersionTbl SET version = 6`);
        oldDb.run(`DROP TABLE OptimizeCounterTbl`);
        oldDb.destroy;
    }

    auto newOpt = openDatabase((dir ~ "m.db").AbsolutePath, "test", 8, readOnly: false);
    assert(newOpt.hasValue, "migration open failed");
    auto newDb = newOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        newDb.destroy;

    // Trailing-space SQL keeps this read off the versionData cache entry.
    // The statement must be held in a local: an inline prepare().get.execute range outlives the wrapper's reference-counted payload and reads garbage.
    bool found;
    long ver;
    {
        auto stmt = newDb.prepare(`SELECT version FROM VersionTbl `);
        foreach (ref r; stmt.get.execute) {
            found = true;
            ver = r.peek!long(0);
        }
    }
    assert(found, "VersionTbl must have a row after the rebuild");
    assert(ver == 8, "the rebuilt database must be at version 8, got " ~ ver.to!string);

    assert(readOptimizeCounter(newDb.db) == 0,
            "the rebuild must recreate the counter at 0, got " ~ readOptimizeCounter(newDb.db)
                .to!string);

    // the counter table is part of the schema: an increment on the rebuilt database works
    import llm.rag.optimize_counter : incrementOptimizeCounter;

    incrementOptimizeCounter(newDb.db);
    assert(readOptimizeCounter(newDb.db) == 1,
            "the rebuilt database must have a working counter, got " ~ readOptimizeCounter(newDb.db)
                .to!string);
}

// The MMR tests need vectors that CLUSTER: the hash-based TestEmbedder avalanches any text difference, so near-identical chunks across origins are impossible with it.
// This embedder maps the origin prefix in the embedding input ("Topic: r1 | ...") to one of three fixed directions: the three redundant topics share one vector, the diverse topic gets an orthogonal one.
private static class ClusterEmbedder : Embedder {
    import std.algorithm : canFind;

    override string modelName() {
        return "cluster";
    }

    override long dimensions() {
        return 8;
    }

    override bool supportsTokenization() {
        return false;
    }

    override int batchSize() {
        return 512;
    }

    override void destroy() {
    }

    private EmbedResult vec(string text) {
        import std.algorithm : canFind;

        auto v = new float[8];
        v[] = 0.0f;
        if (canFind(text, "Topic: r"))
            v[0] = 1.0f; // redundant cluster (any r-salt origin)
        else if (canFind(text, "Topic: d1"))
            v[1] = 1.0f; // diverse cluster, orthogonal to the redundant one
        else
            v[2] = 1.0f;
        return EmbedResult(v);
    }

    EmbedResult embed(string text) {
        return vec(text);
    }

    override EmbedResult embedQuery(string text) {
        return vec(text);
    }

    override EmbedResult embedDocument(string text) {
        return vec(text);
    }

    override EmbedResult embedQuery(int[] tokens) {
        assert(false, "token path never used: supportsTokenization is false");
    }

    override EmbedResult embedDocument(int[] tokens) {
        assert(false, "token path never used: supportsTokenization is false");
    }

    override int[] tokenize(string text, bool addSpecial) {
        assert(false, "token path never used: supportsTokenization is false");
    }

    override string detokenize(int[] tokens) {
        assert(false, "token path never used: supportsTokenization is false");
    }
}

private string lettersText(long shift) {
    string text;
    foreach (i; 0 .. 50)
        text ~= cast(char)('a' + (i + shift) % 26) ~ " ";
    return text;
}

@("mmrLambda 1.0 keeps the KNN ranking; 0.3 diversifies it on a redundant corpus")
unittest {
    import std.algorithm : canFind;
    import std.conv : to;
    import llm.rag.database : openDatabase, Search;
    import my.optional;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    auto emb = new ClusterEmbedder();
    immutable redundant = lettersText(0) ~ " alpha";
    // must contain an fts token no other doc has
    immutable diverse = lettersText(13) ~ " zqxword";

    auto dbAOpt = openDatabase((dir ~ "m1.db").AbsolutePath, "cluster", 8,
            readOnly: false, mmrLambda: 1.0);
    assert(dbAOpt.hasValue, "openDatabase (lambda 1.0) failed");
    auto dbA = dbAOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        dbA.destroy;

    auto dbBOpt = openDatabase((dir ~ "m2.db").AbsolutePath, "cluster", 8,
            readOnly: false, mmrLambda: 0.3);
    assert(dbBOpt.hasValue, "openDatabase (lambda 0.3) failed");
    auto dbB = dbBOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        dbB.destroy;

    immutable cfg = RagConfig(windowOverlapPercent: 10);
    foreach (i; 1 .. 7) {
        size_t nBatchCache = 0;
        immutable salt = "r" ~ i.to!string;
        addToDatabase(dbA, emb, Document(origin: Origin(Topic(salt)), data: redundant),
                cfg, nBatchCache, salt);
        nBatchCache = 0;
        addToDatabase(dbB, emb, Document(origin: Origin(Topic(salt)), data: redundant),
                cfg, nBatchCache, salt);
    }
    size_t nBatchCacheD = 0;
    addToDatabase(dbA, emb, Document(origin: Origin(Topic("d1")), data: diverse),
            cfg, nBatchCacheD, "d1");
    addToDatabase(dbB, emb, Document(origin: Origin(Topic("d1")), data: diverse),
            cfg, nBatchCacheD, "d1");

    assert(dbA.getSources.length == 7, "seven distinct salts must index seven sources");

    // probe = the redundant cluster vector: distance 0 to the six redundant chunks, 1 to the diverse one
    float[] probe() {
        auto v = new float[8];
        v[] = 0.0f;
        v[0] = 1.0f;
        return v;
    }

    // lambda 1.0 = plain KNN: the top-4 are all redundant chunks (distance 0), the diverse chunk (distance 1) is excluded. Ranks are 1-4 by distance.
    auto ra = dbA.querySemantic(Search(probe()), 4);
    assert(ra.length == 4, "plain KNN must return four chunks, got " ~ ra.length.to!string);
    foreach (i, m; ra)
        assert(m.text == redundant, "plain KNN rank " ~ (i + 1)
                .to!string ~ " must be a redundant chunk, got " ~ m.text);
    foreach (i, m; ra)
        assert(m.rank == i + 1, "plain KNN rank must be " ~ (i + 1)
                .to!string ~ ", got " ~ m.rank.to!string);

    // lambda 0.3 = MMR over the over-fetched pool (k*5 = 20 candidates, 7 rows exist so the pool covers all).
    // After the best redundant pick the identical duplicates score 0.3*1 - 0.7*1 < 0 while the diverse chunk scores 0.3*0 - 0.7*0 = 0.
    // It is therefore selected (position 2 of the MMR order) and part of the returned set even though its distance is the worst.
    auto rb = dbB.querySemantic(Search(probe()), 4);
    assert(rb.length == 4, "MMR KNN must return four chunks, got " ~ rb.length.to!string);
    assert(rb[0].text == redundant, "MMR must keep the best match first, got " ~ rb[0].text);
    assert(canFind(rb.map!(m => m.text).array, diverse),
            "MMR must select the diverse chunk into the top-4 set");
    assert(rb[3].text == diverse,
            "the diverse chunk must be the last by distance, got " ~ rb[3].text);
    assert(ra != rb, "MMR must change the KNN result set on the redundant corpus");
}

@("mmrLambda applies to the vec side of the RRF fusion query")
unittest {
    import std.algorithm : canFind, sort;
    import std.conv : to;
    import llm.rag.database : openDatabase, Search;
    import my.optional;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    auto emb = new ClusterEmbedder();
    immutable redundant = lettersText(0) ~ " alpha";
    immutable diverse = lettersText(13) ~ " zqxword";

    auto dbAOpt = openDatabase((dir ~ "f1.db").AbsolutePath, "cluster", 8,
            readOnly: false, mmrLambda: 1.0);
    assert(dbAOpt.hasValue, "openDatabase (lambda 1.0) failed");
    auto dbA = dbAOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        dbA.destroy;

    auto dbBOpt = openDatabase((dir ~ "f2.db").AbsolutePath, "cluster", 8,
            readOnly: false, mmrLambda: 0.3);
    assert(dbBOpt.hasValue, "openDatabase (lambda 0.3) failed");
    auto dbB = dbBOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        dbB.destroy;

    // The fusion path over-fetches the vec CTE by RrfPoolMultiplier (10): with
    // limit 4 the plain pool is k=40, so MMR inside vec0 only engages when the
    // corpus holds MORE than 40 rows. 40 redundant docs + 1 diverse doc = 41.
    immutable cfg = RagConfig(windowOverlapPercent: 10);
    foreach (i; 1 .. 41) {
        size_t nBatchCache = 0;
        immutable salt = "r" ~ i.to!string;
        addToDatabase(dbA, emb, Document(origin: Origin(Topic(salt)), data: redundant),
                cfg, nBatchCache, salt);
        nBatchCache = 0;
        addToDatabase(dbB, emb, Document(origin: Origin(Topic(salt)), data: redundant),
                cfg, nBatchCache, salt);
    }
    size_t nBatchCacheD = 0;
    addToDatabase(dbA, emb, Document(origin: Origin(Topic("d1")), data: diverse),
            cfg, nBatchCacheD, "d1");
    addToDatabase(dbB, emb, Document(origin: Origin(Topic("d1")), data: diverse),
            cfg, nBatchCacheD, "d1");

    assert(dbA.getSources.length == 41, "41 distinct salts must index 41 sources");

    // The external-content fts5 index is not auto-synced; rebuild it so the fts side of the fusion sees the just-indexed chunks.
    dbA.fts5Rebuild();
    dbB.fts5Rebuild();

    // probe = the redundant cluster vector
    float[] probe() {
        auto v = new float[8];
        v[] = 0.0f;
        v[0] = 1.0f;
        return v;
    }

    // lambda 1.0, byte-identical to the plain ranking: the plain vec pool (k=40) holds the 40 redundant chunks (distance 0).
    // The diverse chunk (distance 1) is excluded from the pool, so its fused score is the fts term only, 1/(10+1) = 1/11.
    // The rank-1 redundant chunk ties it exactly (1/11), so the diverse chunk sits at fused position 1 or 2 either way and is always inside the top-4; the three redundant chunks complete the set.
    auto ra = dbA.queryCombineSemanticText(Search(probe()), "zqxword", 4);
    assert(ra.length == 4, "plain fusion must return four chunks, got " ~ ra.length.to!string);
    assert(canFind(ra.map!(m => m.text).array, diverse),
            "the diverse chunk must be in the plain fused top-4 (fts rank 1)");
    size_t nRedundant;
    foreach (m; ra)
        if (m.text == redundant)
            nRedundant++;
    assert(nRedundant == 3,
            "plain fusion top-4 must hold three redundant chunks, got " ~ nRedundant.to!string);
    // The SQL gives a chunk matching neither engine the coalesce default rank 1000.
    // The diverse chunk's plain fused score is therefore 1/11 (fts rank 1) plus 1/(10+1000) for the absent vec match, not bare 1/11.
    immutable plainDiverseScore = 1.0 / 11 + 1.0 / 1010;
    foreach (m; ra)
        if (m.text == diverse)
            assert(m.rank > plainDiverseScore - 1e-9 && m.rank < plainDiverseScore + 1e-9,
                    "the diverse chunk's plain fused score must be 1/11 + 1/1010, got "
                    ~ m.rank.to!string);

    // lambda 0.3: MMR engages (41 rows > k=40) and the diverse chunk is selected into the vec pool (position 2 of the MMR order).
    // It takes the vec rank 40 (worst distance among the selected) on top of its fts rank 1, so its fused score 1/11 + 1/50 strictly beats every redundant chunk's best 1/11.
    // It ranks first - impossible without the mmr_lambda constraint, which excludes it from the pool entirely at lambda 1.0.
    auto rb = dbB.queryCombineSemanticText(Search(probe()), "zqxword", 4);
    assert(rb.length == 4, "MMR fusion must return four chunks, got " ~ rb.length.to!string);
    assert(rb[0].text == diverse,
            "MMR must lift the diverse chunk to fused rank 1, got " ~ rb[0].text);
    foreach (i; 1 .. 4)
        assert(rb[i].text == redundant, "MMR fused rank " ~ (i + 1)
                .to!string ~ " must be a redundant chunk, got " ~ rb[i].text);
    assert(rb[0].rank > 1.0 / 11 + 1.0 / 50 - 1e-9 && rb[0].rank < 1.0 / 11 + 1.0 / 50 + 1e-9,
            "the diverse chunk's MMR fused score must be 1/11 + 1/50, got " ~ rb[0].rank.to!string);
    assert(ra != rb, "MMR must change the fused ranking on the redundant corpus");
}
