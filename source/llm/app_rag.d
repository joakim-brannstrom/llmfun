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
