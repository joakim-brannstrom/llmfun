/// Handles the 'rag' subcommand: RAG database management (add, remove, list, sync).
module llm.app_rag;

import logger = std.logger;
import std.algorithm;
import std.array : appender, empty, array;
import std.conv : to;
import std.file : exists, readText, isFile, isDir, dirEntries, SpanMode;
import std.format : format;
import std.path : extension, baseName, buildNormalizedPath;
import std.string : strip, startsWith, join, toStringz, split;
import std.sumtype : match;

import llm.app_config : UserConfig, userToLlmConfig, createRag;
import llm.config;
import llm.rag.rag : Origin, Topic, Url, Path, Document, add;
import miniorm : spinSql;
import my.filter : ReFilter;
import my.optional;
import my.path : AbsolutePath;

int appMain(UserConfig uconf, UserConfig.Rag conf) {
    import llm.subsystem : initLlmfunLocalModel, deinitLlmfunLocalModel;

    initLlmfunLocalModel();
    scope (exit)
        deinitLlmfunLocalModel();

    if (conf.setupDirs) {
        makeLocalSetupFileStructure(LlmConfig.init);
    }
    auto llmConf = readConfig(uconf.config, false, uconf.noCwdConfig, uconf.trustedConfig)
        .userToLlmConfig(conf);

    // Read-only dialogue report: no embedder, local model, or primary RAG
    // database is needed (session DBs are opened read-only, sources only).
    if (conf.dialogue) {
        return dialogueReport(llmConf);
    }

    auto rag = createRag(llmConf, openSecondary: false);
    if (rag is null)
        return 1;

    scope (exit) {
        rag.destroy;
    }

    if (rag.isPrimaryInMemory) {
        logger.errorf("No primary database opened for read/write. Tried to open '%s'",
                llmConf.ragPrimary.path);
        return 1;
    }

    long printNoneExistingPaths(T)(T paths) {
        long counter;
        foreach (p; paths.filter!(a => !exists(a))) {
            logger.warningf("Path '%s' does not exist", p);
            counter++;
        }
        return counter;
    }

    ReFilter buildRagFilter() {
        auto filter = llmConf.ragFilter;

        if (!conf.ragInclude.empty) {
            filter.include = conf.ragInclude;
        }
        if (!conf.ragExclude.empty) {
            filter.exclude = conf.ragExclude;
        }

        if (filter.include.empty) {
            logger.warning("ragFilter include is empty - all file types will be indexed");
        }

        try {
            return filter.to();
        } catch (Exception e) {
            logger.warningf("Invalid ragFilter regex pattern: %s - falling back to defaults",
                    e.msg);
            filter.include = [".*\\.txt", ".*\\.md"];
            filter.exclude = [];
            return filter.to();
        }
    }

    auto ragFilter = buildRagFilter();

    Path[] collectFiles(Path root, ReFilter filter) {
        if (!exists(root)) {
            return null;
        }
        auto files = appender!(Path[])();
        if (isFile(root)) {
            files.put(root.buildNormalizedPath.Path);
        } else if (isDir(root)) {
            try {
                foreach (p; dirEntries(root, SpanMode.depth).filter!(a => a.isFile)
                        .filter!(a => filter.match(a.name))) {
                    files.put(p.name.buildNormalizedPath.Path);
                }
            } catch (Exception e) {
                logger.warningf("Unable to scan '%s': %s", root, e.msg);
            }
        }
        return files[];
    }

    long addData() {
        if (conf.path.empty) {
            logger.warning("No --path provided. Nothing to add.");
            return 0;
        }

        const failed = printNoneExistingPaths(conf.path);

        foreach (p; conf.path.filter!(a => exists(a))) {
            auto files = collectFiles(p.Path, ragFilter);

            if (files.empty) {
                logger.infof("No files matched in %s", p);
                continue;
            }

            logger.infof("Adding files from %s", p);
            foreach (f; files) {
                try {
                    auto result = add(rag, Document(Origin(f),
                            readText(f.toString)), llmConf.ragConfig);
                    if (result.chunks > 0) {
                        logger.infof("  Added/updated: %s (%s chunks)", f, result.chunks);
                    } else {
                        logger.infof("  Skipped (unchanged): %s", f);
                    }
                } catch (Exception e) {
                    logger.warningf("Unable to add '%s': %s", f, e.msg);
                }
            }
        }
        return failed;
    }

    long removeData() {
        if (conf.path.empty) {
            if (conf.ragInclude.empty && conf.ragExclude.empty
                    && llmConf.ragFilter.include.empty && llmConf.ragFilter.exclude.empty) {
                logger.warning("No PATHS provided and/or no --include/--exclude filters active (CLI or config). " ~ "Nothing to remove. Use --include <pattern> or --exclude <pattern> to select sources for removal, " ~ "or provide --path for a specific file/directory.");
                return 0;
            }
        }

        // path-based removal
        if (!conf.path.empty) {
            long entriesRemoved = 0;
            long entriesFailed = 0;
            foreach (p; conf.path) {
                auto path = p.Path;
                try {
                    if (path.isFile) {
                        logger.infof("Removing embeddings from file %s", p);
                        entriesRemoved += rag.removeSource(Origin(path));
                    } else if (path.isDir) {
                        logger.infof("Removing embeddings from files in %s", p);
                        foreach (entry; dirEntries(path, SpanMode.depth).filter!(a => a.isFile)
                                .filter!(a => ragFilter.match(a.name))) {
                            entriesRemoved += rag.removeSource(Origin(entry.Path));
                        }
                    } else {
                        if (p.startsWith("http://") || p.startsWith("https://")) {
                            logger.infof("Removing URL %s", p);
                            entriesRemoved += rag.removeSource(Origin(Url(p)));
                        } else {
                            logger.warningf("Path '%s' does not exist and is not a URL, skipping",
                                    p);
                            entriesFailed++;
                        }
                    }
                } catch (Exception e) {
                    entriesFailed++;
                    logger.warningf("Failed to remove '%s': %s", p, e.msg);
                }
            }
            logger.infof("Removed %s embeddings, %s failed", entriesRemoved, entriesFailed);
            return entriesFailed;
        }

        // Filter-based source iteration and matching
        struct RemoveCandidate {
            Origin origin;
            string matchStr;
        }

        long entriesRemoved = 0; // Scoped to filter-based branch
        long entriesFailed = 0;

        auto candidates = appender!(RemoveCandidate[])();
        long topicSkipped = 0;
        foreach (src; rag.db.getSources) {
            src.origin.match!((Topic a) { ++topicSkipped; return; }, (Path a) {
                if (ragFilter.match(a.toString))
                    candidates.put(RemoveCandidate(src.origin, a.toString));
            }, (Url a) {
                if (ragFilter.match(a.value))
                    candidates.put(RemoveCandidate(src.origin, a.value));
            });
        }
        if (topicSkipped > 0) {
            logger.infof("Skipped %s topic source(s) — topics have no file paths to filter",
                    topicSkipped);
        }

        auto candidateArray = candidates.data;

        logger.infof("Found %s source(s) matching filter for removal", candidateArray.length);
        foreach (c; candidateArray) {
            logger.infof("  Will remove: '%s'", c.matchStr);
        }

        foreach (c; candidateArray) {
            try {
                entriesRemoved += rag.removeSource(c.origin);
            } catch (Exception e) {
                entriesFailed++;
                logger.warningf("Failed to remove '%s': %s", c.matchStr, e.msg);
            }
        }

        logger.infof("Removed %s embeddings from %s source(s), %s failed",
                entriesRemoved, candidateArray.length, entriesFailed);
        return entriesFailed;
    }

    void listSources() {
        logger.info("List all sources");
        foreach (dbSrc; rag.getSources) {
            logger.infof("Database '%s'", dbSrc.name);
            foreach (src; dbSrc.sources) {
                auto cs = src.checksum.get;
                src.origin.match!((Topic a) {
                    logger.infof("topic:'%s' (%s)", a.name, cs);
                }, (Path a) { logger.infof("path:'%s' (%s)", a, cs); }, (Url a) {
                    logger.infof("url:'%s' (%s)", a.value, cs);
                });
            }
        }
    }

    long syncData() {
        import my.set : Set;

        if (conf.path.empty) {
            logger.warning("PATHS is required for sync");
            return 1;
        }

        const invalidPaths = printNoneExistingPaths(conf.path);

        // Build normalized paths for prefix matching
        Path[] normalizedPaths = conf.path
            .filter!(a => exists(a))
            .map!(a => a.buildNormalizedPath.Path)
            .array;
        if (normalizedPaths.empty) {
            logger.warning("No valid paths to sync");
            return 1;
        }

        // Collect files from all paths, deduplicate
        Set!string seenFiles;
        Path[] allFiles;
        foreach (np; normalizedPaths) {
            auto files = collectFiles(np, ragFilter);
            foreach (f; files) {
                if (!seenFiles.contains(f)) {
                    seenFiles.add(f);
                    allFiles ~= f;
                }
            }
        }

        long added = 0;
        long skipped = 0;
        long failed = 0;

        Set!string syncedOrigins;

        // Phase 1: Scan and add
        logger.warningf(invalidPaths > 0, "Skipped %s invalid path(s)", invalidPaths);
        foreach (p; allFiles) {
            syncedOrigins.add(p);
            try {
                if (conf.dryRun) {
                    logger.infof("  [dry-run] Would add: %s", p);
                    added++;
                } else {
                    auto result = rag.add(Document(Origin(p),
                            readText(p.toString)), llmConf.ragConfig);
                    if (result.chunks > 0) {
                        logger.infof("  Added/updated: %s (%s chunks)", p, result.chunks);
                        added++;
                    } else {
                        logger.infof("  Skipped (unchanged): %s", p);
                        skipped++;
                    }
                }
            } catch (Exception e) {
                logger.warningf("Failed to process '%s': %s", p, e.msg);
                failed++;
            }
        }

        // Phase 2: Remove stale sources
        logger.info("Phase 2: Checking for deleted sources");
        long removed = 0;
        long removeFailed = 0;

        // Helper: check if normPath is under any managed path with boundary check
        bool isUnderManagedPath(string normPath) {
            foreach (np; normalizedPaths.map!(a => a.toString)
                    .filter!(a => normPath.startsWith(a))) {
                return true;
            }
            return false;
        }

        foreach (src; rag.getSources.map!(a => a.sources).joiner) {
            src.origin.match!((Topic a) { return; }, (Path a) {
                auto normPath = a.toString.buildNormalizedPath;
                if (isUnderManagedPath(normPath) && !syncedOrigins.contains(normPath)) {
                    auto reason = exists(a) ? "excluded by filter" : "deleted from filesystem";
                    try {
                        if (conf.dryRun) {
                            logger.infof("  [dry-run] Would remove: %s (%s)", a, reason);
                        } else {
                            logger.infof("  Removing: %s (%s)", a, reason);
                            rag.removeSource(Origin(a.Path));
                        }
                        removed++;
                    } catch (Exception e) {
                        logger.warningf("  Failed to remove '%s': %s", a, e.msg);
                        removeFailed++;
                    }
                }
            }, (Url a) { return; });
        }

        logger.infof("Sync complete: %s added/updated, %s skipped, %s removed, %s failed",
                added, skipped, removed, failed + removeFailed);
        if (!conf.dryRun && (added > 0 || removed > 0)) {
            spinSql!(() { rag.fts5Rebuild; });
        }
        return failed + removeFailed;
    }

    if (conf.add) {
        long failed = addData();
        spinSql!(() { rag.vacuum; rag.fts5Rebuild; });
        return failed != 0 ? 1 : 0;
    } else if (conf.rm) {
        long failed = removeData();
        spinSql!(() { rag.vacuum; rag.fts5Rebuild; });
        return failed != 0 ? 1 : 0;
    } else if (conf.sync) {
        return syncData() != 0 ? 1 : 0;
    } else if (conf.list) {
        listSources();
    }

    return 0;
}

/// Per-session statistics for the read-only dialogue report.
private struct DialogueSessionStats {
    string sessionId;
    size_t sources;
    long minTurn;
    long maxTurn;
    bool hasTurns;
}

/// Compute per-session statistics from a single session database (read-only).
///
/// The report path never issues vector queries (getSources only), so no
/// embedder is needed. The stored model and dimensions are probed from
/// VersionTbl first, because openDatabase refuses a read-only open on a
/// dimension mismatch. Returns none when the DB cannot be probed or opened.
private Optional!DialogueSessionStats sessionStats(AbsolutePath dbPath) {
    import llm.rag.database : Database, openDatabase;
    import llm.rag.dialogue_index : EpisodeMeta, decodeTopicName;
    import llm.rag.sqlite3_vec;
    import miniorm : Miniorm;

    string model;
    long dims = 0;
    bool probed = false;
    try {
        auto probe = Miniorm(dbPath.toString, SQLITE_OPEN_READONLY);
        auto stmt = probe.prepare("SELECT model, embedDimensions FROM VersionTbl");
        foreach (ref r; stmt.get.execute) {
            model = r.peek!string(0);
            dims = r.peek!long(1);
            probed = true;
        }
    } catch (Exception e) {
        logger.warningf("Unable to probe version of dialogue database '%s': %s", dbPath, e.msg);
        return none!DialogueSessionStats();
    }
    if (!probed || dims <= 0) {
        logger.warningf("No version info in dialogue database '%s', skipping", dbPath);
        return none!DialogueSessionStats();
    }

    auto dbOpt = openDatabase(dbPath, model, dims, readOnly: true);
    if (!hasValue(dbOpt)) {
        logger.warningf("Unable to open dialogue database '%s' read-only, skipping", dbPath);
        return none!DialogueSessionStats();
    }
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy();

    DialogueSessionStats stats;
    foreach (src; db.getSources) {
        stats.sources++;
        src.origin.match!((Topic t) {
            decodeTopicName(t.name).match!((EpisodeMeta m) {
                if (!stats.hasTurns) {
                    stats.minTurn = m.turnStart;
                    stats.maxTurn = m.turnEnd;
                    stats.hasTurns = true;
                } else {
                    if (m.turnStart < stats.minTurn)
                        stats.minTurn = m.turnStart;
                    if (m.turnEnd > stats.maxTurn)
                        stats.maxTurn = m.turnEnd;
                }
            }, (None _) {});
        }, (Url _) {}, (Path _) {});
    }
    return some(stats);
}

/// All valid per-session database files directly under dir.
///
/// std.file's DirEntry.name is the full path, so baseName() must be applied
/// before any session-id handling. Only D12-valid names pass (path-traversal
/// guard); anything else is silently ignored.
private AbsolutePath[] validSessionDatabases(AbsolutePath dir) {
    import llm.session.types : SessionId, isValidId;

    AbsolutePath[] result;
    foreach (entry; dirEntries(dir, SpanMode.shallow)) {
        auto fileName = baseName(entry.name);
        if (!entry.isFile || extension(fileName) != ".db")
            continue;

        auto sessionId = fileName[0 .. $ - ".db".length];
        if (!isValidId(SessionId(sessionId)))
            continue;

        result ~= (dir ~ fileName).AbsolutePath;
    }
    return result;
}

/// Read-only report over the per-session dialogue history databases.
///
/// Per session: source count and the indexed turn range (min turnStart - max
/// turnEnd, decoded from topic names, never from worker memory). A missing
/// directory or a session with no history is an info line, not an error.
/// Returns a process exit code (0 on success, even when nothing to report).
int dialogueReport(LlmConfig llmConf) {
    auto dir = llmConf.dialogueDir.AbsolutePath;
    if (!exists(dir) || !isDir(dir)) {
        logger.infof("No dialogue history to report. Directory '%s' is missing or is not a directory.",
                dir);
        return 0;
    }

    size_t sessionsReported = 0;
    size_t totalSources = 0;
    foreach (dbPath; validSessionDatabases(dir)) {
        auto sessionId = baseName(dbPath.toString)[0 .. $ - ".db".length];
        sessionStats(dbPath).match!((DialogueSessionStats s) {
            string turns = s.hasTurns ? format("turns %s-%s", s.minTurn, s.maxTurn) : "no turn info";
            logger.infof("session '%s': %s source(s), %s", sessionId, s.sources, turns);
            sessionsReported++;
            totalSources += s.sources;
        }, (None _) {});
    }

    if (sessionsReported == 0) {
        logger.infof("No dialogue history found in '%s'.", dir);
        return 0;
    }

    logger.infof("Total: %s session(s), %s source(s)", sessionsReported, totalSources);
    return 0;
}

version (unittest) {
    import llm.common.embedder : Embedder, EmbedResult, EmbedError;
    import my.path : AbsolutePath;
    import std.file : mkdirRecurse, rmdirRecurse;

    /// Deterministic test embedder: FNV-1a 64-bit over the input, expanded
    /// into 8 float dimensions.
    private class TestEmbedder : Embedder {
        override string modelName() {
            return "app_rag_test_embedder";
        }

        override long dimensions() {
            return 8;
        }

        override bool supportsTokenization() {
            return false;
        }

        override int batchSize() {
            return 1;
        }

        override EmbedResult embed(string text) {
            ulong h = 14695981039346656037;
            foreach (byte b; text) {
                h ^= b;
                h *= 1099511628211;
            }
            auto vec = new float[8];
            foreach (i, ref f; vec) {
                f = cast(float)((h >> (8 * i)) & 0xFF) / 255.0;
            }
            return EmbedResult(vec);
        }

        override EmbedResult embed(int[] tokens) {
            // never called: supportsTokenization is false
            char[] text = new char[tokens.length];
            foreach (i, ref c; text)
                c = cast(char)(tokens[i] & 0x7F);
            return embed(cast(string) text);
        }

        override int[] tokenize(string text) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override void destroy() {
        }
    }

    private AbsolutePath makeScratchDir(long line = __LINE__) {
        import std.conv : to;

        auto dir = ("llmfun_test/app_rag/" ~ line.to!string).AbsolutePath;
        mkdirRecurse(dir);
        return dir;
    }
}

unittest {
    // Seeded dialogue directory: two valid sessions, one invalid file name.
    // sessionStats must report the per-session source counts and the turn
    // ranges decoded from the topic names; dialogueReport must exit 0.
    import llm.common.embedder : EmbedResult;
    import llm.rag.database : Database, openDatabase;
    import llm.rag.dialogue_index : encodeTopicName;
    import llm.rag.rag : addToDatabase;
    import my.optional;
    import my.path : AbsolutePath, Path;
    import std.file : rmdirRecurse, write;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    auto emb = new TestEmbedder;
    auto cfg = RagConfig.init;
    size_t nBatchCache;

    auto seed = (string sessionId, long ts, long te) {
        auto dbPath = (dir ~ (sessionId ~ ".db")).AbsolutePath;
        auto dbOpt = openDatabase(dbPath, emb.modelName(), emb.dimensions());
        assert(hasValue(dbOpt));
        auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
        scope (exit)
            db.destroy();
        auto doc = Document(origin: Origin(Topic(encodeTopicName(sessionId, ts,
                te, 1000))), data: "episode text for " ~ sessionId ~ " turns "
                ~ ts.to!string ~ "-" ~ te.to!string);
        addToDatabase(db, emb, doc, cfg, nBatchCache);
    };

    seed("20240101-120000-abcd", 1, 3);
    seed("20240101-120000-abcd", 4, 6);
    seed("20240102-000000-beef", 7, 9);

    // A file with a non-D12 session name must be ignored by the report.
    write(dir ~ "not_a_session.db", "junk");

    // The report must see exactly the two valid session databases
    // (DirEntry.name is the full path; baseName must be applied, and the
    // invalid name must be filtered out).
    auto sessionDbs = validSessionDatabases(dir);
    assert(sessionDbs.length == 2);

    auto stats1 = sessionStats((dir ~ "20240101-120000-abcd.db").AbsolutePath);
    assert(hasValue(stats1));
    assert(stats1.match!((DialogueSessionStats s) => s.sources == 2, (None _) => false));
    assert(stats1.match!((DialogueSessionStats s) => s.minTurn == 1
            && s.maxTurn == 6, (None _) => false));

    auto stats2 = sessionStats((dir ~ "20240102-000000-beef.db").AbsolutePath);
    assert(hasValue(stats2));
    assert(stats2.match!((DialogueSessionStats s) => s.sources == 1, (None _) => false));
    assert(stats2.match!((DialogueSessionStats s) => s.minTurn == 7
            && s.maxTurn == 9, (None _) => false));

    // A non-database file must be reported as none (probe fails).
    auto statsBad = sessionStats((dir ~ "not_a_session.db").AbsolutePath);
    assert(!hasValue(statsBad));

    auto conf = LlmConfig.init;
    conf.dialogueDir = Path(dir.toString);
    assert(dialogueReport(conf) == 0);
}

unittest {
    // Missing dialogue directory: info line, exit 0 (N3).
    import my.path : Path;

    auto conf = LlmConfig.init;
    conf.dialogueDir = Path("/nonexistent_dialogue_dir_t16");
    assert(dialogueReport(conf) == 0);
}

unittest {
    // Existing but empty directory: info line, exit 0 (N3).
    import my.path : AbsolutePath, Path;
    import std.file : rmdirRecurse;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    auto conf = LlmConfig.init;
    conf.dialogueDir = Path(dir.toString);
    assert(dialogueReport(conf) == 0);
}
