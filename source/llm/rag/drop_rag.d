/// DropRagSync: a background actor that keeps the primary RAG database in
/// step with the drag-and-drop destination directory.
///
/// The actor sweeps the destination in bounded ticks (batch- and deadline-
/// capped, so its thread never blocks for long), reconciles the scan against
/// the indexed origins with the pure `drop_reconcile` core, and applies the
/// plan through the standard RAG add/remove seam: dedup, changed-content
/// replacement and origin purge reuse the database's own transactional
/// behavior. A drop report fast-path indexes a single just-finished file
/// immediately; the periodic sweep is the durable backstop (a relay send to
/// a downed actor drops, and the startup heal pass at spawn covers anything
/// missed while the process was down).
///
/// All scan keys and `seen` map keys are absolute normalized paths: the
/// destination scan yields absolute names and the workarea is not the
/// process CWD. Origins, by contrast, are workarea-relative (the same form
/// the manual rag add path writes), so the actor translates between the two
/// forms with `absoluteFor`/`originFor`.
module llm.rag.drop_rag;

import core.time : Duration, msecs, minutes, seconds;

import logger = std.logger;
import std.datetime.systime : Clock, SysTime;
import std.exception : collectException;
import std.file : DirEntry, SpanMode, dirEntries, exists, isDir;
import std.path : buildNormalizedPath, buildPath, dirName, pathSplitter, relativePath;

import my.actor.behavior : ActorRef;
import my.actor.channel : dynDelayedSend;
import my.actor.msg : delay;
import my.actor.system_msg : ExitMsg;
import my.path : AbsolutePath, Path;

import llm.common.embedder : Embedder;
import llm.config : RagConfig, RagDatabaseConfig;
import llm.dragdrop : DropReport;
import llm.rag.drop_reconcile : cadenceMinMax, cadenceNext, FileRecord, FileStamp, Outcome,
    nextWindow, originReconcileKey, originUnderDestination, reconcileKey, reconcileWindow;
import llm.rag.rag : Document, Origin, RAG, add;
import llm.utility : readFileUtf8;

/// Poison cap for a file whose indexing keeps failing while its stamp stays
/// unchanged; the actor gives up on it (logged once) until the file changes.
enum uint retryCapDefault = 3;

/// Quiescence window for the debounced full-corpus FTS rebuild: one rebuild
/// fires after this much time without an applied change (never per file).
enum ftsWindow = 5.seconds;

/// Threshold between per-file and aggregated indexing logs: a pass or burst
/// that applied at most this many files logs one line per file, a larger one
/// logs a single aggregate line.
enum smallBurst = 16;

/// Per-pass log scratch: accumulates the applied add origins and the
/// files-seen count; flushing produces the shaped lines and clears the
/// scratch.
private struct LogBatch {
    private {
        Path[] applied_;
        size_t seen_;
    }

    /// Record one applied add (its workarea-relative origin).
    void noteApplied(Path origin) {
        applied_ ~= origin;
    }

    /// Record file entries the scan yielded (applied or not).
    void noteSeen(size_t n) {
        seen_ += n;
    }

    /// The per-file line for one applied origin; the drop fast path uses it
    /// directly, since one report indexes exactly one file.
    static string perFileLine(Path origin) {
        return "Index " ~ origin.toString ~ " to RAG";
    }

    /// Produce the burst's lines and clear the scratch. A burst that applied
    /// nothing produces no lines.
    string[] flushLines() {
        import std.conv : to;

        string[] lines;
        if (applied_.length <= smallBurst) {
            foreach (origin; applied_)
                lines ~= perFileLine(origin);
        } else {
            lines ~= "Index " ~ applied_.length.to!string ~ " ("
                ~ seen_.to!string ~ " files) to RAG";
        }
        applied_ = null;
        seen_ = 0;
        return lines;
    }
}

/// Pull-based recursive scan of the destination, implementing the walkDrop
/// policy itself (dragdrop.d:walkDrop): manual shallow recursion that never
/// follows symlinked directories and skips dot-prefixed path segments
/// entirely (hidden files AND the contents of hidden directories). The dot
/// filter applies only to segments BELOW the scan root: the root itself may
/// legitimately contain dot segments (the workarea is CWD-relative, and a
/// user-configured destination may be `dropped/.staging`). This is the
/// range `nextWindow` consumes; `dirEntries(_, SpanMode.depth)` is
/// forbidden as the backing walk (it descends into symlinked directories).
struct DestScan {
    private {
        AbsolutePath root_;
        DirEntry[] buf_;
        size_t pos_;
        AbsolutePath[] pending_;
    }

    this(AbsolutePath root) {
        root_ = root;
        pending_ ~= root_;
        advance();
    }

    bool empty() {
        return pos_ >= buf_.length;
    }

    DirEntry front() {
        return buf_[pos_];
    }

    void popFront() {
        pos_++;
        advance();
    }

    /// Position `front` on the next yieldable file entry, or drain.
    private void advance() {
        import std.algorithm.searching : any, startsWith;
        import std.range.primitives : empty;
        import std.path : relativePath;

        for (;;) {
            while (pos_ < buf_.length) {
                auto entry = buf_[pos_];
                // Only the segments below the scan root are policy-owned:
                // dot segments ABOVE the root (a dotted CWD, a dotted
                // destination directory itself) must not veto every entry.
                string rel;
                try {
                    rel = relativePath(entry.name, root_);
                } catch (Exception) {
                    // not under the root: skip defensively, the next pass
                    // re-scans
                    pos_++;
                    continue;
                }
                if (pathSplitter(rel).any!(seg => seg.startsWith("."))) {
                    // skipped entirely: no yield, no descent
                    pos_++;
                    continue;
                }
                try {
                    if (entry.isSymlink) {
                        // never follow or yield: policy parity with walkDrop
                        pos_++;
                        continue;
                    }
                    if (entry.isDir) {
                        // no yield; the LIFO list keeps the recursion shallow
                        pending_ ~= AbsolutePath(entry.name);
                        pos_++;
                        continue;
                    }
                    if (!entry.isFile) {
                        // not a regular file (fifo, socket, device node):
                        // never yield or read - a FIFO with no writer would
                        // block the read (and the actor thread) forever
                        pos_++;
                        continue;
                    }
                } catch (Exception) {
                    // unstatable entry: skip it, the next pass re-scans
                    pos_++;
                    continue;
                }
                return;
            }
            if (pending_.empty)
                return;
            const dir = pending_[$ - 1];
            pending_ = pending_[0 .. $ - 1];
            buf_ = null;
            pos_ = 0;
            try {
                foreach (entry; dirEntries(dir, SpanMode.shallow))
                    buf_ ~= entry;
            } catch (Exception) {
                // vanished or unreadable directory: skip it, the next pass
                // re-scans
            }
        }
    }
}

/// Outcome of one bounded unit of sweep work (`DropRagActor.runSweep`).
private enum SweepOutcome {
    /// no pass opened (missing destination or database); the caller schedules
    skipped,
    /// the budget ran out mid-pass; the pass continues on the next call
    continued,
    /// the pass finished and its removals were applied
    completed,
}

/// The DropRagSync actor: one immutable configuration snapshot taken at
/// spawn, no config reads at runtime.
final class DropRagActor {
    private {
        AbsolutePath destAbs_;
        Path destRel_;
        AbsolutePath rootAbs_;
        Embedder embedder_;
        AbsolutePath dbPath_;
        long maxFileBytes_;
        RagConfig ragConfig_;
        uint sweepBatchFiles_;
        Duration tickBudget_;
        uint retryCap_;
        ActorRef self_;
        RAG rag_;

        // per-file indexing outcome, keyed by the absolute reconcile key
        FileRecord[AbsolutePath] seen_;
        // one pass = one full scan of the destination
        bool passOpen_;
        Origin[AbsolutePath] originByPath_;
        bool[AbsolutePath] covered_;
        // removals planned for this pass, keyed by the absolute scan key and
        // holding the stored origin to remove
        Origin[AbsolutePath] pending_;
        DestScan scan_;
        // adds planned but not applied within a tick's budget; retried at the
        // start of the next tick (the scan has already consumed their entries)
        AbsolutePath[] backlog_;
        size_t backlogPos_;
        size_t addsAppliedInPass_;
        // per-pass adaptive log scratch: flushed at pass completion or
        // supersession, so every applied add is logged exactly once
        LogBatch logBatch_;
        // applied-change counter for the FTS-rebuild debounce: bumped by
        // EVERY applied change (adds AND removals)
        long counter_;
        // at most one FTS-rebuild debounce message in flight
        bool ftsPending_;
        // sweep backoff: reset to the fast edge by a drop event or a pass
        // with applied changes, advanced by no-change passes, saturating at
        // `cadenceMinMax` minutes where the timer stops until the next
        // drop event. Not the delay of a drop-triggered sweep - those arm
        // immediately - but the steady-state re-arm and skip-retry delay.
        Duration interval_ = 1.minutes;
        // whether an `onSweepTick` is in flight or the cadence timer armed
        bool sweepArmed_;
        // whether the pass that just completed applied at least one change
        bool passAppliedChange_;
        // epoch of the live sweep timer: a superseded delayed tick carries a
        // stale epoch and is ignored on delivery, so at most one tick chain
        // stays live however often a new timer is armed
        uint tickEpoch_;

        void delegate() nothrow ftsRebuildHook_;
        void delegate(const(char)[]) nothrow logSink_;
        bool destWarned_;
        bool memWarned_;
    }

    /// Params:
    ///   destination = absolute path of the drag-and-drop destination dir.
    ///   destinationRel = the same destination as a workarea-relative path
    ///       (e.g. "dropped"): DropReport.finalPath and the written origins
    ///       are workarea-relative, while the scan is absolute.
    ///   embedder = embedder dedicated to this actor (the spawn site builds
    ///       a fresh instance; never shared with other connections).
    ///   dbPath = primary database file; opened lazily on first use and held
    ///       for the actor's lifetime (own connection, own thread). The
    ///       database description passed to listings is "DropRagSync": the
    ///       label names the connection owner, while the primary/secondary
    ///       role is tracked separately by `isPrimary`, which the sweep
    ///       relies on.
    ///   maxFileBytes = per-file size cap; larger files are skipped.
    ///   ragConfig = chunking config snapshot for the add path.
    ///   sweepBatchFiles = maximum file entries pulled per tick.
    ///   tickBudget = wall-clock budget per tick's scan pull.
    this(AbsolutePath destination, Path destinationRel, Embedder embedder, AbsolutePath dbPath, long maxFileBytes,
            RagConfig ragConfig = RagConfig.init, uint sweepBatchFiles = 256,
            Duration tickBudget = 500.msecs) {
        destAbs_ = destination;
        destRel_ = reconcileKey(destinationRel);
        // The stored origins are relative to the workarea root (the parent
        // chain of `destinationRel`), so the inverse mapping needs that root:
        // strip one directory per `destinationRel` segment off the absolute
        // destination.
        size_t depth;
        foreach (_; pathSplitter(destRel_.toString))
            depth++;
        auto root = destAbs_.toString;
        foreach (_; 0 .. depth)
            root = dirName(root);
        rootAbs_ = AbsolutePath(root);
        embedder_ = embedder;
        dbPath_ = dbPath;
        maxFileBytes_ = maxFileBytes;
        ragConfig_ = ragConfig;
        sweepBatchFiles_ = sweepBatchFiles;
        tickBudget_ = tickBudget;
        retryCap_ = retryCapDefault;
    }

    /// Arm the startup heal pass: an immediate self-send sweeps whatever
    /// accumulated in the destination while the process was down.
    void onSpawn(ActorRef self) @safe {
        self_ = self;
        logger.tracef("DropRagSync actor started (destination=%s)", destRel_);
        armSweep(0.msecs);
    }

    void onExit(ExitMsg _) {
        if (rag_ !is null) {
            try {
                rag_.destroy;
            } catch (Exception e) {
                logger.errorf("DropRagSync: closing the database failed: %s",
                        e.msg).collectException;
            }
            rag_ = null;
        }
    }

    /// DropWorker report relay entry point. Never throws: a failure here
    /// must not take the caller down, and the sweep is the durable path.
    void onDropReport(DropReport r) {
        try {
            handleDropReport(r);
        } catch (Exception e) {
            logger.errorf("DropRagSync: drop report handling failed: %s", e.msg).collectException;
        }
    }

    private void handleDropReport(DropReport r) {
        if (r.finalPath.toString.length == 0)
            return;
        const rel = reconcileKey(r.finalPath);
        if (!originUnderDestination(rel, destRel_))
            return;
        const absKey = absoluteFor(rel);
        const vanished = !exists(absKey.toString);
        if (rel == destRel_ || (!vanished && DirEntry(absKey.toString).isDir)) {
            // A directory is not indexable content, and the report carries
            // no list of what landed inside: only the sweep's scan can
            // discover the dropped files.
            interval_ = 1.minutes;
            requestSweep();
            return;
        }
        restartCadence();
        if (vanished)
            return;
        if (safeAdd(absKey, false)) {
            unplanRemoval(absKey);
            bumpActivity();
        }
    }

    /// Withdraw a removal planned for this pass, after the fast path just
    /// re-indexed the file: the in-flight pass may have scanned before the
    /// file reappeared, and applying its purge at pass completion would
    /// delete the just-added content. Marks the file covered (later windows
    /// cannot re-plan the purge) and drops the pending entry.
    private void unplanRemoval(AbsolutePath absKey) {
        covered_[absKey] = true;
        pending_.remove(absKey);
    }

    /// Periodic sweep tick: also the startup heal pass and the continuation
    /// of a deadline-cut pass. Runs one bounded unit of sweep work
    /// (`runSweep`) and maps the outcome to the re-arm cadence: a continued
    /// outcome re-arms at once, so a large destination drains across ticks
    /// without waiting a full cadence; skipped and completed re-arm at the
    /// cadence interval.
    void onSweepTick() {
        onSweepTick(tickEpoch_);
    }

    /// Delayed delivery of a sweep tick, carrying the epoch of the arm that
    /// scheduled it: a tick superseded by a newer arm is ignored, so arming
    /// never accumulates parallel tick chains.
    void onSweepTick(uint epoch) {
        if (epoch != tickEpoch_)
            return;
        try {
            const outcome = runSweep(Clock.currTime + tickBudget_);
            if (outcome == SweepOutcome.continued)
                armSweep(1.msecs);
            // skipped: the destination or database is unavailable - retry at
            // the current cadence without advancing it
            else if (outcome == SweepOutcome.skipped)
                armSweep(interval_);
            else {
                const cad = cadenceNext(cast(uint) interval_.total!"minutes", passAppliedChange_);
                interval_ = cad.nextMin.minutes;
                if (cad.rearm)
                    armSweep(interval_);
                else
                    sweepArmed_ = false;
            }
        } catch (Exception e) {
            logger.errorf("DropRagSync: sweep tick failed: %s", e.msg).collectException;
            // an open pass must resume promptly, not wait out the cadence
            armSweep(passOpen_ ? 0.msecs : interval_);
        }
    }

    /// Debounce delivery: fires `ftsWindow` after the last armed bump. A
    /// payload counter equal to the current one means no further change
    /// landed during the window: run the rebuild. Any change in between
    /// re-arms instead, so a burst rebuilds exactly once, after its last
    /// change.
    void onFtsQuiesced(long counter) {
        ftsPending_ = false;
        if (counter == counter_)
            rebuildFts();
        else
            armFtsDebounce();
    }

    /// Run the full-corpus FTS rebuild, fails-soft: chunks are committed
    /// either way, only text search lags until a later rebuild. Never called
    /// per file: the debounce owns every call.
    private void rebuildFts() nothrow {
        version (unittest)
            if (ftsRebuildHook_ !is null) {
                ftsRebuildHook_();
                return;
            }
        try {
            openRag().fts5Rebuild;
        } catch (Exception e) {
            logger.warningf("DropRagSync: fts5 rebuild failed: %s", e.msg).collectException;
        }
    }

    /// Arm the quiescence debounce unless one is already in flight: after
    /// `ftsWindow` without another applied change, `onFtsQuiesced` fires
    /// with the counter at arm time.
    private void armFtsDebounce() {
        if (ftsPending_)
            return;
        ftsPending_ = true;
        try {
            // Payload type must exactly match the handler parameter:
            // my.actor drops mismatching delayed sends silently.
            dynDelayedSend(self_.address, delay(ftsWindow), "onFtsQuiesced", counter_);
        } catch (Exception e) {
            // nothing in flight: a later applied change can re-arm
            ftsPending_ = false;
            logger.errorf("DropRagSync: arming the fts5 rebuild debounce failed: %s",
                    e.msg).collectException;
        }
    }

    /// Record `n` applied changes (adds AND removals) and arm the FTS
    /// rebuild debounce. A delete-only pass bumps too, so removed content
    /// stops being text-searchable after quiescence.
    private void bumpActivity(size_t n = 1) {
        if (n == 0)
            return;
        counter_ += n;
        armFtsDebounce();
    }

    /// Emit one shaped info line. Never throws; the unittest
    /// seam `logSink_` captures instead of logging, so tests observe the
    /// shaped output without touching the global logger.
    private void emitInfo(Args...)(string fmt, Args args) nothrow {
        import std.format : format;

        string line;
        try {
            line = fmt.format(args);
        } catch (Exception) {
            line = fmt; // malformed template: emit it raw
        }
        version (unittest)
            if (logSink_ !is null) {
                logSink_(line).collectException;
                return;
            }
        logger.info(line).collectException;
    }

    /// Emit one shaped warning line; same capture seam as `emitInfo`.
    private void emitWarn(Args...)(string fmt, Args args) nothrow {
        import std.format : format;

        string line;
        try {
            line = fmt.format(args);
        } catch (Exception) {
            line = fmt; // malformed template: emit it raw
        }
        version (unittest)
            if (logSink_ !is null) {
                logSink_(line).collectException;
                return;
            }
        logger.warning(line).collectException;
    }

    /// Emit the pass log scratch and clear it.
    private void flushLogBatch() {
        foreach (line; logBatch_.flushLines())
            emitInfo(line);
    }

    /// Run one bounded unit of sweep work: open the pass if none is open,
    /// drain the add backlog, then pull reconcile windows until `deadline`
    /// or pass completion (removals applied, state cleared). The budget
    /// bounds BOTH the scan pull and the add work: adds that do not fit
    /// are queued and retried at the start of the next call. Never
    /// schedules anything itself: the outcome tells the caller whether to
    /// continue immediately, re-arm the cadence, or treat the sweep as
    /// skipped (missing destination or database).
    private SweepOutcome runSweep(SysTime deadline) {
        if (!passOpen_ && !startPass())
            return SweepOutcome.skipped;
        SweepOutcome outcome;
        size_t addsThisTick;
        Origin[] indexedKeys;
        foreach (key, _; originByPath_)
            indexedKeys ~= Origin(key);
        // Adds deferred from an earlier deadline-cut tick run first: the
        // scan has already consumed their entries, so they must not wait
        // for the next pass.
        while (backlogPos_ < backlog_.length && Clock.currTime < deadline) {
            if (safeAdd(backlog_[backlogPos_], batchLog: true))
                addsThisTick++;
            backlogPos_++;
        }
        for (;;) {
            auto slice = nextWindow(scan_, sweepBatchFiles_, deadline, destAbs_.Path);
            logBatch_.noteSeen(slice.entries.length);
            auto plan = reconcileWindow!AbsolutePath(slice.entries,
                    indexedKeys, seen_, originByPath_, retryCap_);
            // The scan did yield these entries: mark them covered BEFORE the
            // adds, so an add that does not fit the tick can neither
            // trigger a spurious removal of its origin nor be re-planned
            // from a window that will never yield it again this pass.
            foreach (entry; slice.entries)
                covered_[AbsolutePath(entry.name)] = true;
            foreach (key; plan.adds) {
                if (Clock.currTime >= deadline) {
                    // The scan pull was bounded but the adds are not: queue
                    // the un-applied add for the next tick so a large batch
                    // cannot monopolize the thread. seen_ is only written on
                    // success, so whatever is still queued (or dropped at
                    // pass completion) re-plans on a later pass.
                    backlog_ ~= key;
                    continue;
                }
                if (safeAdd(key, batchLog: true))
                    addsThisTick++;
            }
            absorbRemovals(plan.removes);
            if (!slice.exhausted) {
                outcome = SweepOutcome.continued;
                break;
            }
            if (plan.passComplete) {
                completePass();
                outcome = SweepOutcome.completed;
                break;
            }
        }
        bumpActivity(addsThisTick);
        return outcome;
    }

    /// Open the pass: fetch the indexed origins under the destination once,
    /// then start a fresh scan. Returns false on a skip (missing
    /// destination, broken database): no pass opens and the caller decides
    /// the retry. No removals are planned when the destination itself is
    /// missing: an absent directory is not evidence that its files were
    /// deleted.
    private bool startPass() {
        if (!exists(destAbs_.toString) || !isDir(destAbs_.toString)) {
            if (!destWarned_) {
                destWarned_ = true;
                emitWarn("drop destination '%s' does not exist; waiting for drops", destRel_);
            }
            return false;
        }
        destWarned_ = false;
        RAG rag;
        try {
            rag = openRag();
        } catch (Exception e) {
            logger.errorf("DropRagSync: opening the RAG database failed: %s",
                    e.msg).collectException;
            return false;
        }
        foreach (dbSources; rag.getSources()) {
            if (!dbSources.isPrimary)
                continue;
            foreach (source; dbSources.sources) {
                const key = originReconcileKey(source.origin);
                if (key.toString.length == 0 || !originUnderDestination(key, destRel_))
                    continue;
                originByPath_[absoluteFor(key)] = source.origin;
            }
        }
        scan_ = DestScan(destAbs_);
        passOpen_ = true;
        addsAppliedInPass_ = 0;
        passAppliedChange_ = false;
        return true;
    }

    /// Lazily open the primary database connection on first use.
    private RAG openRag() {
        if (rag_ is null) {
            rag_ = new RAG(embedder_, RagDatabaseConfig(dbPath_, "DropRagSync"), null, ragConfig_);
            if (rag_.isPrimaryInMemory && !memWarned_) {
                memWarned_ = true;
                logger.errorf("DropRagSync: the primary database %s could not be opened; indexing fell back to a throwaway in-memory database",
                        dbPath_);
            }
        }
        return rag_;
    }

    /// Index one file now. Returns true when the add applied (a dedup no-op
    /// counts as applied: the origin is freshly confirmed). `batchLog`
    /// selects the adaptive pass logging; the drop fast path logs its single
    /// file per-file instead. A vanished file is a failed attempt: the next
    /// pass's scan will not list it.
    private bool safeAdd(AbsolutePath key, bool batchLog) {
        if (!exists(key.toString)) {
            // Missing-file race: listed by the scan, gone before the read.
            // Trace-grade detail: an ordinary race, not an error.
            logger.tracef("DropRagSync: %s vanished before it could be read",
                    originFor(key)).collectException;
            recordFailure(key, "file vanished");
            return false;
        }
        try {
            auto de = DirEntry(key.toString);
            const stamp = FileStamp(de.timeLastModified, de.size);
            if (!de.isFile) {
                // Non-regular file (fifo, socket, device node) reaching the
                // fast path: never read it - a FIFO with no writer would
                // block the read forever. Recorded skipped: stable across
                // passes, never retried.
                logger.tracef("DropRagSync: skipping non-regular file %s",
                        originFor(key)).collectException;
                seen_[key] = FileRecord(Outcome.skipped, stamp);
                return false;
            }
            if (de.size > maxFileBytes_) {
                seen_[key] = FileRecord(Outcome.skipped, stamp);
                emitInfo("skipping %s: file exceeds the %s KB cap",
                        originFor(key), (maxFileBytes_ + 1023) / 1024);
                return false;
            }
            const data = readFileUtf8(key);
            const origin = originFor(key);
            openRag().add(Document(origin: Origin(origin), data: data), ragConfig_);
            seen_[key] = FileRecord(Outcome.indexed, stamp);
            addsAppliedInPass_++;
            if (batchLog)
                logBatch_.noteApplied(origin);
            else
                emitInfo(LogBatch.perFileLine(origin));
            return true;
        } catch (Exception e) {
            if (rag_ is null) {
                // A database-level failure (the RAG constructor threw, or the
                // in-memory fallback left no usable database): not a per-file
                // poison, the next pass retries from scratch.
                logger.errorf("DropRagSync: the RAG database is unavailable: %s",
                        e.msg).collectException;
                return false;
            }
            recordFailure(key, e.msg);
            return false;
        }
    }

    /// Record a failed indexing attempt. A stamp change resets the attempt
    /// count; at `retryCap` unchanged-stamp attempts the file is given up
    /// on (logged once at the transition) until the file changes.
    private void recordFailure(AbsolutePath key, string reason) {
        FileStamp stamp = FileStamp.init;
        const old = key in seen_;
        if (old !is null)
            stamp = old.stamp;
        try {
            auto de = DirEntry(key.toString);
            stamp = FileStamp(de.timeLastModified, de.size);
        } catch (Exception) {
            // stat failed: keep the previous stamp; the next pass re-decides
            // from the scan anyway
        }
        const attempts = (old !is null && old.stamp == stamp) ? old.attempts + 1 : 1;
        if (old !is null && old.attempts < retryCap_ && attempts >= retryCap_)
            emitInfo("giving up on %s after %s attempts: %s", originFor(key), attempts, reason);
        seen_[key] = FileRecord(Outcome.failed, stamp, attempts, reason);
    }

    /// Absorb this tick's removal candidates into the pending map. First
    /// re-prune the pending entries against the grown covered set: an origin
    /// planned early in the pass but scanned by a later window must not be
    /// purged at completion. A candidate whose key is already covered or
    /// pending is skipped, so only origins absent from the WHOLE pass
    /// survive to completion. The candidates come in the scan key form
    /// (absolute-path origins); the stored origin they map to via
    /// `originByPath_` is what the removal applies to.
    private void absorbRemovals(Origin[] candidates) {
        foreach (key, _; pending_)
            if ((key in covered_) !is null)
                pending_.remove(key);
        foreach (origin; candidates) {
            const key = originReconcileKey(origin);
            if (key.toString.length == 0)
                continue;
            const absKey = AbsolutePath(key);
            if ((absKey in covered_) !is null || (absKey in pending_) !is null)
                continue;
            pending_[absKey] = (absKey in originByPath_) !is null ? originByPath_[absKey] : origin;
        }
    }

    /// Close the pass: apply the pending removals after all adds of the pass
    /// (an identical-content replacement is already indexed by then, so the
    /// purge cannot drop the content), prune stale `seen` records, log the
    /// aggregate and clear all pass state; the caller re-arms the cadence.
    private void completePass() {
        flushLogBatch();
        size_t removals;
        foreach (key, origin; pending_) {
            try {
                openRag().removeSource(origin);
                removals++;
                seen_.remove(key);
                emitInfo("removed %s from RAG (file deleted)", originReconcileKey(origin));
            } catch (Exception e) {
                logger.errorf("DropRagSync: removing source %s failed: %s",
                        originReconcileKey(origin), e.msg).collectException;
            }
        }
        bumpActivity(removals);
        passAppliedChange_ = addsAppliedInPass_ > 0 || removals > 0;
        // Prune per-file records for files that no longer exist (they were
        // not covered by the completed scan): without this, deleted files'
        // records accumulate for the process lifetime. Records for files
        // that still exist are kept: they carry the poison-cap state.
        AbsolutePath[] stale;
        foreach (key, _; seen_)
            if ((key in covered_) is null && !exists(key.toString))
                stale ~= key;
        foreach (key; stale)
            seen_.remove(key);
        passOpen_ = false;
        clearPassState();
    }

    private void clearPassState() {
        originByPath_ = null;
        covered_ = null;
        pending_ = null;
        backlog_ = null;
        backlogPos_ = 0;
        addsAppliedInPass_ = 0;
        logBatch_ = LogBatch.init;
        scan_ = DestScan.init;
    }

    /// Drop-event sweep trigger: discard any in-flight pass state and arm an
    /// immediate tick (0 delay - the cadence interval never delays a
    /// drop-triggered sweep).
    private void requestSweep() {
        // A superseded pass already applied real work: emit its batched log
        // lines before the scratch is cleared, so every applied add is
        // logged exactly once even when the pass never completes.
        flushLogBatch();
        passOpen_ = false;
        clearPassState();
        armSweep(0.msecs);
    }

    /// Arm one logical sweep timer (epoch-guarded): a newer arm supersedes
    /// any tick already in flight, so rapid re-arming never accumulates
    /// parallel tick chains. Marks a tick as pending until saturation stops
    /// the cadence.
    private void armSweep(Duration d) @safe {
        tickEpoch_++;
        sweepArmed_ = true;
        try {
            // Payload type must exactly match the handler parameter:
            // my.actor drops mismatching delayed sends silently.
            dynDelayedSend(self_.address, delay(d), "onSweepTick", tickEpoch_);
        } catch (Exception e) {
            // nothing in flight: a later drop event can re-arm
            sweepArmed_ = false;
            logger.errorf("DropRagSync: arming the sweep tick failed: %s", e.msg).collectException;
        }
    }

    /// Restart the sweep cadence after a drop event: the backoff returns to
    /// the fast edge, and a stopped (saturated) timer runs again. A running
    /// timer keeps its delay; the reset interval applies from the next pass
    /// completion, and an open pass's tick chain needs no fresh timer.
    private void restartCadence() {
        interval_ = 1.minutes;
        if (sweepArmed_)
            return;
        armSweep(passOpen_ ? 0.msecs : interval_);
    }

    /// Workarea-relative origin -> absolute scan key.
    private AbsolutePath absoluteFor(Path relKey) {
        import std.path : absolutePath;

        return AbsolutePath(absolutePath(relKey.toString, rootAbs_.toString));
    }

    /// Absolute scan key -> workarea-relative origin path.
    private Path originFor(AbsolutePath absKey) {
        return Path(buildNormalizedPath(buildPath(destRel_.toString,
                relativePath(absKey.toString, destAbs_.toString))));
    }
}

version (unittest) {
    import core.thread : Thread;
    import core.time : Duration, msecs, seconds;
    import std.algorithm.iteration : map;
    import std.algorithm.sorting : sort;
    import std.array : array, replicate;
    import std.conv : to;
    import std.string : toStringz;
    import std.datetime.stopwatch : AutoStart, StopWatch;
    import core.sys.posix.sys.stat : chmod;
    import std.file : mkdirRecurse, remove, rmdirRecurse, symlink;
    import std.path : absolutePath, dirName;
    import std.stdio : File;
    import std.sumtype : match;

    import my.actor.channel : dynSend;
    import my.actor.system : makeSystem;
    import my.optional : None, hasValue;

    import llm.rag.database : Database, Offset, Search, openDatabase;
    import llm.common.embedder : EmbedError, EmbedResult;
    import llm.test_util : TestEmbedder;

    private string makeScratchDir(long line = __LINE__) {
        auto dir = buildPath("llmfun_test", "rag_drop_rag", line.to!string);
        mkdirRecurse(dir);
        return dir;
    }

    /// Create `relPath` under `scratch` with `content`.
    private void writeFile(string scratch, string relPath, string content) {
        const path = buildPath(scratch, relPath);
        mkdirRecurse(dirName(path));
        File(path, "w").write(content);
    }

    private string relPathOf(string scratch, string relPath) {
        return buildPath(scratch, relPath);
    }

    private size_t sourceCount(ref Database db) {
        return db.getSources().length;
    }

    /// FTS5 MATCH hits for `needle`: zero until a rebuild indexes the text;
    /// the external-content table serves plain reads from TextChunkTbl, so
    /// only an index MATCH proves the rebuild ran.
    private long ftsMatches(ref Database db, string needle) {
        static immutable sql = "SELECT count(*) FROM FtsChunksTbl WHERE FtsChunksTbl MATCH :needle";
        auto stmt = db.prepare(sql);
        stmt.get.bind(":needle", needle);
        foreach (ref r; stmt.get.execute)
            return r.peek!long(0);
        return -1;
    }

    /// All origins of the database, sorted for order-independent asserts.
    private string[] originSet(ref Database db) {
        return db.getSources()
            .map!(s => s.origin.match!((const Path p) => p.toString, (_) => "?non-file"))
            .array.sort.array;
    }

    private long chunkRowsContaining(ref Database db, string needle) {
        static immutable sql = "SELECT count(*) FROM TextChunkTbl WHERE text LIKE :needle";
        auto stmt = db.prepare(sql);
        stmt.get.bind(":needle", "%" ~ needle ~ "%");
        foreach (ref r; stmt.get.execute)
            return r.peek!long(0);
        return -1;
    }

    /// Direct-construction helper: `onSweepTick` runs inline; the cadence
    /// re-arms are no-ops because `self_` is null (dynDelayedSend is a no-op
    /// on an invalid address).
    private DropRagActor makeActor(string dir, uint batchFiles = 256,
            long maxFileBytes = 10_000_000, Duration tickBudget = 500.msecs) {
        return new DropRagActor(relPathOf(dir, "dropped").AbsolutePath, "dropped".Path,
                new TestEmbedder, relPathOf(dir, "p.db").AbsolutePath,
                maxFileBytes, RagConfig(), batchFiles, tickBudget);
    }

    /// Run ticks until the pass completes (passOpen_ false again).
    private void runPass(DropRagActor a) {
        do {
            a.onSweepTick();
        }
        while (a.passOpen_);
    }

    private size_t actorSources(DropRagActor a) {
        return a.openRag().db.getSources().length;
    }

    /// `TestEmbedder` that counts document-embedding calls: the
    /// identical-re-drop dedup pin reads it (a dedup no-op must not embed).
    /// A plain field is safe here: only the actor thread writes it, and the
    /// test reads it after polling for the observable database effect, so no
    /// read races a write.
    private final class CountingEmbedder : TestEmbedder {
        size_t docCalls;
        override EmbedResult embedDocument(string text) {
            docCalls++;
            return super.embedDocument(text);
        }
    }

    /// Poll `pred` every 10 ms until it holds or `budget` elapses; false on
    /// timeout (so the assert reports the missed condition, not a hang).
    private bool waitFor(scope bool delegate() pred, Duration budget) {
        auto sw = StopWatch(AutoStart.yes);
        for (;;) {
            if (pred())
                return true;
            if (sw.peek >= budget)
                return false;
            Thread.sleep(10.msecs);
        }
    }

    /// Whether the vector query over `text` surfaces `originStr`.
    private bool semanticHit(ref Database db, TestEmbedder emb, string text, string originStr) {
        auto vec = emb.embedQuery(text).match!((float[] v) => v, (EmbedError e) {
            assert(false, "the test embedder must not fail: " ~ e.errorMsg);
            return null;
        });
        foreach (m; db.querySemantic(Search(vec), 100))
            if (originStr == m.origin.match!((const Path p) => p.toString, (_) => "?non-file"))
                return true;
        return false;
    }
}

@("a spawned actor indexes the destination into its own database connection")
unittest {
    auto sys = makeSystem;
    scope (exit)
        sys.shutdown;
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/a.md", "alpha note");
    writeFile(dir, "dropped/sub/b.md", "beta note");

    // The actor opens its own connection; the test holds a second one open
    // to the same file: safe under rollback-journal locking.
    auto dbOpt = openDatabase(relPathOf(dir, "p.db").AbsolutePath, "test", 8, readOnly: false);
    assert(dbOpt.hasValue, "openDatabase failed");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    // No drop report: the startup heal pass must pick both files up.
    auto actor = sys.spawn!DropRagActor(relPathOf(dir, "dropped").AbsolutePath,
            "dropped".Path, new TestEmbedder, relPathOf(dir, "p.db").AbsolutePath, 10_000_000);

    auto sw = StopWatch(AutoStart.yes);
    for (;;) {
        if (sourceCount(db) >= 2 || sw.peek >= 5.seconds)
            break;
        Thread.sleep(10.msecs);
    }
    assert(originSet(db) == ["dropped/a.md", "dropped/sub/b.md"].sort.array,
            "the startup heal must index both files, got " ~ originSet(db).to!string);
}

@("the batch cap bounds one tick and later ticks resume the pass")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    foreach (i; 0 .. 5)
        writeFile(dir, "dropped/f" ~ i.to!string ~ ".md", "content number " ~ i.to!string);

    auto a = makeActor(dir, 2);
    scope (exit)
        a.onExit(ExitMsg.init);
    a.onSweepTick();
    assert(actorSources(a) == 2, "one tick must index at most batchFiles files");
    a.onSweepTick();
    assert(actorSources(a) == 4, "the next tick must resume the pass where it stopped");
    runPass(a);
    assert(actorSources(a) == 5, "the final tick must finish the pass");
}

@("an unchanged destination is a no-op pass (dedup and seen map)")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/a.md", "stable content");

    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);
    runPass(a);
    assert(actorSources(a) == 1);
    runPass(a);
    assert(actorSources(a) == 1, "an unchanged file must not be re-added");
}

@("changed content replaces the source without changing the origin count")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/a.md", "zebra zero");

    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);
    runPass(a);
    assert(actorSources(a) == 1);
    auto rag = a.openRag();
    assert(chunkRowsContaining(rag.db, "zebra") > 0, "the first version must be stored");

    writeFile(dir, "dropped/a.md", "quokka queen");
    runPass(a);
    assert(actorSources(a) == 1, "a changed source must replace, not accumulate");
    assert(chunkRowsContaining(rag.db, "zebra") == 0, "stale chunk rows must be purged");
    assert(chunkRowsContaining(rag.db, "quokka") > 0, "new chunk rows must be stored");
}

@("an unreadable file retries below the cap and poisons at the cap")
unittest {
    // NOTE: this test relies on chmod 0 making the file unreadable, which
    // does not hold when the tests run as root (root reads anything); run
    // the suite unprivileged.
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/broken.md", "unreadable");
    chmod(relPathOf(dir, "dropped/broken.md").toStringz, 0u);

    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);
    immutable key = relPathOf(dir, "dropped/broken.md").AbsolutePath;
    foreach (i; 1 .. 4)
        runPass(a);
    assert(key in a.seen_, "a failed add must be recorded");
    assert(a.seen_[key].outcome == Outcome.failed && a.seen_[key].attempts == 3,
            "three failed passes must poison the file, got attempts="
            ~ a.seen_[key].attempts.to!string);
    assert(actorSources(a) == 0);

    runPass(a);
    assert(a.seen_[key].attempts == 3, "a poisoned file must not be re-attempted");

    // A stamp change re-opens the file (the retry cap is per stamp, not forever).
    chmod(relPathOf(dir, "dropped/broken.md").toStringz, 420);
    writeFile(dir, "dropped/broken.md", "readable now");
    runPass(a);
    assert(a.seen_[key].outcome == Outcome.indexed, "a stamp change must reset the poison");
    assert(actorSources(a) == 1);
}

@("a file over the size cap is skipped, not indexed, and stays skipped while unchanged")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/big.md", "x".replicate(100));

    auto a = makeActor(dir, 256, 16);
    scope (exit)
        a.onExit(ExitMsg.init);
    immutable key = relPathOf(dir, "dropped/big.md").AbsolutePath;
    runPass(a);
    assert(actorSources(a) == 0, "an over-cap file must not be indexed");
    assert(a.seen_[key].outcome == Outcome.skipped, "the cap decision must be recorded");

    runPass(a);
    assert(actorSources(a) == 0, "an unchanged skipped file must not be re-planned");

    writeFile(dir, "dropped/big.md", "small now");
    runPass(a);
    assert(actorSources(a) == 1, "a stamp change must re-plan the skipped file");
}

@("a drop report fast-path indexes one file; empty and foreign paths are inert")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/x.md", "fast path");
    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);

    a.onDropReport(DropReport(1, 2, Path("dropped/x.md"), "x"));
    assert(actorSources(a) == 1, "a finished drop must be indexed immediately");
    assert(a.counter_ == 1, "the fast path must bump the applied-change counter");

    a.onDropReport(DropReport(2, 0, Path.init, ""));
    a.onDropReport(DropReport(3, 2, Path("other/z.md"), "z"));
    assert(actorSources(a) == 1, "empty and out-of-destination paths must be inert");
}

@("a fast-path add before pass completion cancels the pending purge")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/a.md", "stable original");
    foreach (i; 0 .. 5)
        writeFile(dir, "dropped/f" ~ i.to!string ~ ".md", "filler " ~ i.to!string);

    auto a = makeActor(dir, 2);
    scope (exit)
        a.onExit(ExitMsg.init);
    runPass(a);
    assert(actorSources(a) == 6, "the first pass must index everything");

    // The file vanishes; a pass scans part of the tree and plans the purge,
    // but cannot complete within the remaining ticks.
    remove(relPathOf(dir, "dropped/a.md"));
    a.onSweepTick();
    a.onSweepTick();
    assert(a.passOpen_, "six files at batch 2 must leave the pass open after two ticks");
    const absKey = relPathOf(dir, "dropped/a.md").AbsolutePath;
    assert((absKey in a.pending_) !is null,
            "the vanished file's origin must be planned for removal");

    // The user re-drops the file before the pass completes: the fast path
    // indexes it and must withdraw the pending purge, or pass completion
    // would delete the just-added content.
    writeFile(dir, "dropped/a.md", "re-dropped content");
    a.onDropReport(DropReport(7, 2, Path("dropped/a.md"), "a"));
    assert((absKey in a.pending_) is null, "the purge must be withdrawn");
    assert((absKey in a.covered_) !is null, "the fast path must cover the key");

    runPass(a);
    assert(originSet(a.openRag().db) == [
        "dropped/a.md", "dropped/f0.md", "dropped/f1.md", "dropped/f2.md",
        "dropped/f3.md", "dropped/f4.md"
    ].sort.array, "the re-dropped file must survive pass completion");
    assert(chunkRowsContaining(a.openRag().db, "re-dropped") > 0,
            "the fast-path content must be searchable after the pass");
}

@("origins planned early in a pass but scanned later are not purged")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/a.md", "alpha one");
    foreach (i; 0 .. 4)
        writeFile(dir, "dropped/f" ~ i.to!string ~ ".md", "filler " ~ i.to!string);

    auto a = makeActor(dir, 2);
    scope (exit)
        a.onExit(ExitMsg.init);
    runPass(a);
    assert(actorSources(a) == 5, "the first pass must index everything");

    // Second pass over the unchanged tree, batched: the first window plans
    // removals for every origin it does not cover, but the later windows
    // then cover exactly those files. The pending list must be re-pruned
    // against the grown covered set, or pass completion would purge files
    // that are still on disk.
    runPass(a);
    assert(actorSources(a) == 5,
            "an unchanged multi-tick pass must not purge origins it later scans");
}

@("a directory drop report triggers a full sweep of the destination")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/sub/b.md", "beta");
    writeFile(dir, "dropped/c.md", "gamma");
    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);

    a.onDropReport(DropReport(1, 2, Path("dropped"), "dir"));
    runPass(a);
    auto rag = a.openRag();
    assert(originSet(rag.db) == ["dropped/c.md", "dropped/sub/b.md"],
            "a directory drop must sweep the whole tree, got " ~ originSet(rag.db).to!string);
}

@("a removed file is purged at pass completion after all adds")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/a.md", "twin content a");
    writeFile(dir, "dropped/b.md", "twin content b");

    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);
    runPass(a);
    assert(actorSources(a) == 2);

    remove(relPathOf(dir, "dropped/b.md"));
    writeFile(dir, "dropped/c.md", "twin content c");
    runPass(a);
    // The replacement was indexed before the purge ran: the surviving sources
    // are exactly the files still on disk.
    auto rag = a.openRag();
    assert(originSet(rag.db) == ["dropped/a.md", "dropped/c.md"],
            "the purge must own only the vanished origin, got " ~ originSet(rag.db).to!string);

    remove(relPathOf(dir, "dropped/c.md"));
    runPass(a);
    assert(actorSources(a) == 1, "a vanished file must be purged at pass completion");
}

@("a missing destination dir is a no-op pass with no removals and no database")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);
    a.onSweepTick();
    assert(!relPathOf(dir, "p.db").exists, "a skip pass must not open the database at all");

    mkdirRecurse(relPathOf(dir, "dropped"));
    writeFile(dir, "dropped/a.md", "arrived");
    runPass(a);
    assert(actorSources(a) == 1, "a created destination must be swept on the next pass");
}

@("DestScan yields nested files and skips dot entries and symlinks")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/a.md", "alpha");
    writeFile(dir, "dropped/.hidden.txt", "skip me");
    writeFile(dir, "dropped/.hidden/x.md", "hidden content");
    writeFile(dir, "dropped/sub/b.md", "beta");
    writeFile(dir, "outside.md", "outside");
    writeFile(dir, "outside_dir/y.md", "inside symlinked dir");
    symlink(relPathOf(dir, "outside.md"), relPathOf(dir, "dropped/link.md"));
    symlink(relPathOf(dir, "outside_dir"), relPathOf(dir, "dropped/linkdir"));

    immutable root = relPathOf(dir, "dropped").absolutePath;
    auto rels = DestScan(root.AbsolutePath).map!(de => relativePath(de.name,
            root)).array.sort.array;
    assert(rels == ["a.md", "sub/b.md"],
            "exactly the regular non-hidden files must be yielded, got " ~ rels.to!string);

    assert(DestScan(relPathOf(dir, "no_such_dir").AbsolutePath).empty,
            "a missing scan root must yield an empty scan, not throw");
}

@("dot segments above the scan root do not veto the yield")
unittest {
    import std.datetime.systime : Clock;

    // The scratch root itself sits under a dot segment: an entry-name-wide
    // dot filter would silently empty the whole scan (a dotted CWD, or a
    // user-configured destination like `dropped/.staging`).
    auto dir = buildPath("llmfun_test", ".dotroot", "rag_drop_rag", 881.to!string);
    mkdirRecurse(dir);
    // Clean only this test's own subtree: the shared .dotroot parent must
    // survive, other tests' scratch trees live beside this one in parallel.
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/a.md", "alpha");
    writeFile(dir, "dropped/.hidden.txt", "skip me");
    writeFile(dir, "dropped/sub/b.md", "beta");

    immutable root = relPathOf(dir, "dropped").absolutePath;
    auto rels = DestScan(root.AbsolutePath).map!(de => relativePath(de.name,
            root)).array.sort.array;
    assert(rels == ["a.md", "sub/b.md"],
            "files under a dotted scan root must be yielded, got " ~ rels.to!string);

    // nextWindow with the root passed applies the dot policy only below it.
    auto scan = DestScan(root.AbsolutePath);
    const slice = nextWindow(scan, 8, Clock.currTime + 10.seconds, root.Path);
    assert(slice.exhausted, "a batch large enough for the tree must exhaust the scan");
    assert(slice.entries.length == 2,
            "both non-hidden files must be yielded, got " ~ slice.entries.length.to!string);
}

@("the fts debounce rebuilds once per burst and re-arms on a change")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);
    size_t rebuilds;
    a.ftsRebuildHook_ = delegate() nothrow{ rebuilds++; };

    writeFile(dir, "dropped/x.md", "first note");
    a.onDropReport(DropReport(1, 2, Path("dropped/x.md"), "x"));
    assert(a.ftsPending_, "the first applied change must arm the debounce");
    writeFile(dir, "dropped/y.md", "second note");
    a.onDropReport(DropReport(2, 2, Path("dropped/y.md"), "y"));
    assert(a.counter_ == 2 && a.ftsPending_,
            "a second change bumps the counter without arming a second debounce");

    a.onFtsQuiesced(a.counter_ - 1);
    assert(rebuilds == 0, "a stale delivery must not rebuild");
    assert(a.ftsPending_, "a stale delivery must re-arm");
    a.onFtsQuiesced(a.counter_);
    assert(rebuilds == 1, "the quiesced delivery must rebuild exactly once");
    assert(!a.ftsPending_);
}

@("a sweep burst rebuilds once after quiescence, not per file")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    foreach (i; 0 .. 3)
        writeFile(dir, "dropped/f" ~ i.to!string ~ ".md", "content " ~ i.to!string);

    auto a = makeActor(dir, 2);
    scope (exit)
        a.onExit(ExitMsg.init);
    size_t rebuilds;
    a.ftsRebuildHook_ = delegate() nothrow{ rebuilds++; };

    runPass(a);
    assert(a.counter_ == 3, "each applied add must bump the counter, got " ~ a.counter_.to!string);
    a.onFtsQuiesced(a.counter_);
    assert(rebuilds == 1, "one burst must rebuild exactly once, got " ~ rebuilds.to!string);
    assert(!a.ftsPending_);
}

@("a delete-only pass bumps the applied-change counter")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/a.md", "doomed content");

    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);
    runPass(a);
    assert(a.counter_ == 1);

    remove(relPathOf(dir, "dropped/a.md"));
    runPass(a);
    assert(a.counter_ == 2, "an applied removal must bump the counter");
    assert(actorSources(a) == 0);
}

@("the sweep cadence backs off to saturation, stops, and restarts on an event")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/a.md", "stable content");

    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);
    runPass(a);
    assert(a.interval_ == 1.minutes, "a pass with applied changes keeps the fast edge");
    assert(a.sweepArmed_, "below the cap the timer must re-arm");
    foreach (i; 2 .. 11) {
        runPass(a);
        assert(a.interval_ == i.minutes,
                "a no-change pass must back off one minute, got " ~ a.interval_
                    .total!"minutes"
                    .to!string ~ " (want " ~ i.to!string ~ ")");
        assert(a.sweepArmed_, "below the cap the timer must re-arm");
    }
    runPass(a);
    assert(a.interval_ == 10.minutes && !a.sweepArmed_,
            "at saturation the timer must stop instead of re-arming");

    // A drop event restarts the cadence at the fast edge.
    a.onDropReport(DropReport(2, 2, Path("dropped/a.md"), "a"));
    assert(a.interval_ == 1.minutes && a.sweepArmed_, "a drop event must restart a stopped timer");
}

@("ticks within a pass never touch the cadence; only completion does")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/a.md", "stable");
    foreach (_; 0 .. 5)
        writeFile(dir, "dropped/f" ~ _.to!string ~ ".md", "filler " ~ _.to!string);

    auto a = makeActor(dir, 2);
    scope (exit)
        a.onExit(ExitMsg.init);
    runPass(a);
    assert(a.interval_ == 1.minutes, "a pass with applied changes keeps the fast edge");
    foreach (_; 0 .. 4)
        runPass(a);
    assert(a.interval_ == 5.minutes, "no-change passes must back off, got " ~ a.interval_
            .total!"minutes"
            .to!string);

    writeFile(dir, "dropped/new.md", "fresh content");
    do {
        a.onSweepTick();
        if (a.passOpen_)
            assert(a.interval_ == 5.minutes,
                    "a mid-pass tick must not advance or reset the cadence");
    }
    while (a.passOpen_);
    assert(a.interval_ == 1.minutes, "a completed pass with applied changes must reset to 1");
    assert(a.sweepArmed_);
}

@("text search lags until the debounced fts5 rebuild fires")
unittest {
    auto sys = makeSystem;
    scope (exit)
        sys.shutdown;
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/fts.md", "xylophone quartet");

    auto dbOpt = openDatabase(relPathOf(dir, "p.db").AbsolutePath, "test", 8, readOnly: false);
    assert(dbOpt.hasValue, "openDatabase failed");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    auto actor = sys.spawn!DropRagActor(relPathOf(dir, "dropped").AbsolutePath,
            "dropped".Path, new TestEmbedder, relPathOf(dir, "p.db").AbsolutePath, 10_000_000);

    auto sw = StopWatch(AutoStart.yes);
    for (;;) {
        if (sourceCount(db) >= 1 || sw.peek >= 5.seconds)
            break;
        Thread.sleep(10.msecs);
    }
    assert(sourceCount(db) >= 1, "the startup heal must index the file");
    assert(ftsMatches(db, "xylophone") == 0, "the fts index must lag until the debounce fires");

    for (;;) {
        if (ftsMatches(db, "xylophone") >= 1 || sw.peek >= 20.seconds)
            break;
        Thread.sleep(10.msecs);
    }
    assert(ftsMatches(db, "xylophone") >= 1, "the debounced rebuild must make the text searchable");
}

@("a stale sweep tick epoch is ignored")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/a.md", "stale epoch content");

    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);
    immutable old = a.tickEpoch_;

    // A directory drop report arms a fresh sweep timer, superseding the
    // epoch the old tick would carry.
    a.onDropReport(DropReport(1, 2, Path("dropped"), "dir"));
    assert(a.tickEpoch_ > old, "a new arm must supersede the in-flight tick");

    a.onSweepTick(old);
    assert(!a.passOpen_, "a stale tick must not open a pass");
    assert(!relPathOf(dir, "p.db").exists, "a stale tick must not even open the database");

    // The live epoch still drives the sweep.
    a.onSweepTick(a.tickEpoch_);
    assert(actorSources(a) == 1, "the live epoch must run the sweep");
}

@("a small burst logs one line per file and no aggregate")
unittest {
    import std.algorithm.iteration : filter;
    import std.algorithm.searching : all, any, canFind, startsWith;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    foreach (i; 0 .. 5)
        writeFile(dir, "dropped/f" ~ i.to!string ~ ".md", "note " ~ i.to!string);

    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);
    const(char)[][] lines;
    a.logSink_ = (const(char)[] l) nothrow{ lines ~= l.idup; };

    runPass(a);
    const indexLines = lines.filter!(l => l.startsWith("Index ")).array;
    assert(indexLines.length == 5,
            "five files must produce five per-file lines, got " ~ indexLines.length.to!string);
    assert(indexLines.all!(l => !l.canFind(" files)")),
            "a small burst must not log an aggregate line");
    assert(indexLines.any!(l => l == "Index dropped/f0.md to RAG"),
            "the per-file lines must name the origins");
}

@("a large burst logs exactly one aggregate line and no per-file lines")
unittest {
    import std.algorithm.iteration : filter;
    import std.algorithm.searching : startsWith;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    foreach (i; 0 .. 30)
        writeFile(dir, "dropped/f" ~ i.to!string ~ ".md", "burst " ~ i.to!string);

    auto a = makeActor(dir, 256, 10_000_000, 10.seconds);
    scope (exit)
        a.onExit(ExitMsg.init);
    const(char)[][] lines;
    a.logSink_ = (const(char)[] l) nothrow{ lines ~= l.idup; };

    runPass(a);
    const indexLines = lines.filter!(l => l.startsWith("Index ")).array;
    assert(indexLines.length == 1,
            "a large burst must log one aggregate line, got " ~ indexLines.length.to!string);
    assert(indexLines[0] == "Index 30 (30 files) to RAG",
            "the aggregate must carry the applied and seen counts, got " ~ indexLines[0]);
}

@("a poison give-up is logged exactly once across subsequent passes")
unittest {
    import std.algorithm.iteration : filter;
    import std.algorithm.searching : canFind;

    // NOTE: relies on chmod 0 making the file unreadable, which does not
    // hold when the tests run as root (root reads anything).
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/broken.md", "unreadable");
    chmod(relPathOf(dir, "dropped/broken.md").toStringz, 0u);

    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);
    const(char)[][] lines;
    a.logSink_ = (const(char)[] l) nothrow{ lines ~= l.idup; };

    foreach (i; 0 .. 4)
        runPass(a);
    const giveups = lines.filter!(l => l.canFind("giving up on")).array;
    assert(giveups.length == 1,
            "the give-up must be logged once at the transition, got " ~ giveups.length.to!string);
    assert(giveups[0].canFind("after 3 attempts"),
            "the give-up line must carry the attempt count, got " ~ giveups[0]);
}

@("the log batch flushes per-file below the threshold and aggregates above it")
unittest {
    LogBatch b;
    assert(b.flushLines().length == 0, "an empty batch must produce no lines");
    b.noteSeen(3);
    assert(b.flushLines().length == 0, "a seen-only batch must produce no lines");

    foreach (i; 0 .. smallBurst)
        b.noteApplied(Path("dropped/f" ~ i.to!string ~ ".md"));
    auto lines = b.flushLines();
    assert(lines.length == smallBurst,
            "a small batch must log per file, got " ~ lines.length.to!string);
    assert(lines[0] == "Index dropped/f0.md to RAG");
    assert(b.flushLines().length == 0, "a flush must clear the scratch");

    foreach (i; 0 .. smallBurst + 1)
        b.noteApplied(Path("dropped/g" ~ i.to!string ~ ".md"));
    b.noteSeen(smallBurst + 4);
    lines = b.flushLines();
    assert(lines.length == 1, "a large batch must log one aggregate, got " ~ lines
            .length.to!string);
    assert(lines[0] == "Index " ~ (smallBurst + 1)
            .to!string ~ " (" ~ (smallBurst + 4).to!string ~ " files) to RAG",
            "the aggregate must carry both counts, got " ~ lines[0]);
}

@("an over-cap file is never read and its skip is logged with the KB cap")
unittest {
    import std.algorithm.iteration : filter;
    import std.algorithm.searching : startsWith;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/big.md", "x".replicate(3000));

    auto a = makeActor(dir, 256, 2048);
    scope (exit)
        a.onExit(ExitMsg.init);
    const(char)[][] lines;
    a.logSink_ = (const(char)[] l) nothrow{ lines ~= l.idup; };

    runPass(a);
    assert(actorSources(a) == 0, "an over-cap file must not be indexed");
    const skips = lines.filter!(l => l.startsWith("skipping ")).array;
    assert(skips.length == 1, "the skip must be logged once, got " ~ skips.length.to!string);
    assert(skips[0] == "skipping dropped/big.md: file exceeds the 2 KB cap",
            "the skip line must carry the KB cap, got " ~ skips[0]);
}

@("symlinked files and directories in the destination are never indexed")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "outside.md", "outside secret");
    writeFile(dir, "outside_dir/y.md", "dir secret");
    mkdirRecurse(relPathOf(dir, "dropped"));
    symlink(relPathOf(dir, "outside.md"), relPathOf(dir, "dropped/link.md"));
    symlink(relPathOf(dir, "outside_dir"), relPathOf(dir, "dropped/linkdir"));

    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);
    runPass(a);
    assert(originSet(a.openRag().db).length == 0, "no symlink target may be indexed");
    assert(chunkRowsContaining(a.openRag().db, "secret") == 0,
            "outside content must never reach the database");
}

@("a fifo in the destination is skipped: the pass completes without reading it")
unittest {
    import core.sys.posix.sys.stat : mkfifo;
    import std.string : toStringz;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    mkdirRecurse(relPathOf(dir, "dropped"));
    writeFile(dir, "dropped/real.md", "real content");
    // 0o644 has no D octal literal: 420. The fifo is never written to: a
    // blocking open on it would hang the actor thread forever.
    const fifoPath = relPathOf(dir, "dropped/pipe.fifo");
    assert(mkfifo(fifoPath.toStringz, 420) == 0, "mkfifo must succeed in the scratch dir");

    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);
    runPass(a);
    assert(!a.passOpen_, "the pass must complete with the fifo present");
    assert(actorSources(a) == 1, "only the regular file may be indexed");

    // Belt and braces: the drop fast path must also refuse to read it.
    immutable key = fifoPath.AbsolutePath;
    assert(!a.safeAdd(key, true), "a fifo must never be read by the actor");
    assert((key in a.seen_) !is null && a.seen_[key].outcome == Outcome.skipped,
            "a fifo must be recorded as skipped");
}

@("a file that vanishes before the read is a failed attempt, no exception")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    mkdirRecurse(relPathOf(dir, "dropped"));

    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);
    immutable key = relPathOf(dir, "dropped/ghost.md").AbsolutePath;

    const applied = a.safeAdd(key, true);
    assert(!applied, "a vanished file cannot be indexed");
    assert((key in a.seen_) !is null && a.seen_[key].outcome == Outcome.failed
            && a.seen_[key].attempts == 1, "the vanished file must be recorded as a failed attempt");
    assert(actorSources(a) == 0);

    // The stale record is pruned at the next pass completion: the scan no
    // longer lists the file.
    runPass(a);
    assert((key in a.seen_) is null, "a vanished file's record must be pruned");
}

@("a missing destination logs once and a later drop event still sweeps")
unittest {
    import std.algorithm.iteration : filter;
    import std.algorithm.searching : canFind;

    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);
    const(char)[][] lines;
    a.logSink_ = (const(char)[] l) nothrow{ lines ~= l.idup; };

    a.onSweepTick();
    a.onSweepTick();
    const warns = lines.filter!(l => l.canFind("does not exist")).array;
    assert(warns.length == 1,
            "the missing destination must be logged once, got " ~ warns.length.to!string);
    assert(warns[0] == "drop destination 'dropped' does not exist; waiting for drops",
            "the warning must use the contract wording, got " ~ warns[0]);

    mkdirRecurse(relPathOf(dir, "dropped"));
    writeFile(dir, "dropped/a.md", "arrived late");
    a.onDropReport(DropReport(1, 2, Path("dropped"), "dir"));
    runPass(a);
    assert(actorSources(a) == 1, "a created destination must be swept after a drop event");
}

@("a report for a directory under the destination sweeps its files, not a file add")
unittest {
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);
    writeFile(dir, "dropped/newdir/b.md", "beta");
    writeFile(dir, "dropped/top.md", "gamma");

    auto a = makeActor(dir);
    scope (exit)
        a.onExit(ExitMsg.init);
    a.onDropReport(DropReport(1, 2, Path("dropped/newdir"), "newdir"));
    runPass(a);
    assert(originSet(a.openRag().db) == [
        "dropped/newdir/b.md", "dropped/top.md"
    ].sort.array, "the directory report must trigger a subtree sweep, got " ~ originSet(a.openRag()
            .db).to!string);
    assert(actorSources(a) == 2, "the directory itself must not be indexed as a source");
}

@(
        "full lifecycle: directory drop, text lag, delete-only purge, identical re-drop, restart heal, manual origin form")
unittest {
    // One scenario walking the whole DropRagSync lifecycle end to end:
    // a directory drop indexes every file (semantic search is immediate,
    // text search lags behind the debounced rebuild), a delete-only pass
    // purges the deleted files' sources and text entries, an identical
    // re-drop is searchable again, a fresh actor over the same database
    // re-indexes a lost source on its startup sweep without re-embedding
    // the survivors, and a source added through the manual tool path's
    // exact origin form is removed by the sweep once its file vanishes.
    //
    // The spawned actor carries steps 1-4 (real debounce timings, real
    // dynSend deliveries); the direct-constructed healer carries the
    // deterministic steps 5-6 (runPass drives passes inline, so asserts
    // never race an in-flight pass).
    //
    // After the delete-only purge the re-created file DOES re-embed, because
    // the dedup identity (urlType, checksum) row was removed with its source.
    // The no-re-embed guarantee is pinned where it actually holds: the
    // identical rewrite of a surviving file (stamp change only) must stay a
    // dedup no-op when the fresh actor re-plans it.
    auto dir = makeScratchDir();
    scope (exit)
        rmdirRecurse(dir);

    // The test holds its own connection while the actors open theirs: safe
    // under rollback-journal locking, as in the other spawned-actor tests.
    auto dbOpt = openDatabase(relPathOf(dir, "p.db").AbsolutePath, "test", 8, readOnly: false);
    assert(dbOpt.hasValue, "openDatabase failed");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    auto embedder1 = new CountingEmbedder;
    auto sys1 = makeSystem;
    bool sys1Down;
    scope (exit)
        if (!sys1Down)
            sys1.shutdown;

    // The destination does not exist yet: the startup heal skips (warns
    // once, opens no database), so the directory drop report below is what
    // drives the first sweep.
    auto actor1 = sys1.spawn!DropRagActor(relPathOf(dir, "dropped").AbsolutePath,
            "dropped".Path, embedder1, relPathOf(dir, "p.db").AbsolutePath, 10_000_000);

    // -- 1. directory drop --------------------------------------------------
    writeFile(dir, "dropped/w_quokka.md", "quokka grazing notes");
    writeFile(dir, "dropped/sub/f_fuchsia.md", "fuchsia blossoms in the atrium");
    writeFile(dir, "dropped/w_marimba.md", "marimba rhythm exercises");
    writeFile(dir, "dropped/w_zeppelin.md", "zeppelin hangar sketches");
    writeFile(dir, "dropped/w_sarong.md", "sarong fabric catalog");
    dynSend(actor1.weakRef, "onDropReport", DropReport(1, 2, Path("dropped"), "dir"));

    assert(waitFor(() => sourceCount(db) == 5, 10.seconds),
            "the directory drop must index all five files, got " ~ sourceCount(db).to!string);
    assert(originSet(db) == [
        "dropped/sub/f_fuchsia.md", "dropped/w_marimba.md",
        "dropped/w_quokka.md", "dropped/w_sarong.md", "dropped/w_zeppelin.md"
    ].sort.array, "origins must be workarea-relative, got " ~ originSet(db).to!string);

    // Semantic search is current immediately: the embeddings commit with the
    // source rows, so the vector query surfaces every origin.
    assert(semanticHit(db, embedder1, "quokka grazing notes",
            "dropped/w_quokka.md"), "quokka must be vector-searchable right away");
    assert(semanticHit(db, embedder1, "fuchsia blossoms in the atrium",
            "dropped/sub/f_fuchsia.md"), "the nested file must be vector-searchable right away");
    assert(semanticHit(db, embedder1, "marimba rhythm exercises",
            "dropped/w_marimba.md"), "marimba must be vector-searchable right away");
    assert(semanticHit(db, embedder1, "zeppelin hangar sketches",
            "dropped/w_zeppelin.md"), "zeppelin must be vector-searchable right away");
    assert(semanticHit(db, embedder1, "sarong fabric catalog",
            "dropped/w_sarong.md"), "sarong must be vector-searchable right away");

    // -- 2. text search lags until the debounced rebuild --------------------
    assert(ftsMatches(db, "zeppelin") == 0, "text search must lag until the debounce fires");
    assert(waitFor(() => ftsMatches(db, "zeppelin") >= 1, 30.seconds),
            "the debounced rebuild must make the text searchable");

    // -- 3. delete-only pass -------------------------------------------------
    remove(relPathOf(dir, "dropped/sub/f_fuchsia.md"));
    remove(relPathOf(dir, "dropped/w_marimba.md"));
    dynSend(actor1.weakRef, "onSweepTick");
    // The wait folds the chunk condition in on purpose: removeSource runs
    // DELETE SourceTbl, the cascade and the embedding cleanup as separate
    // auto-commit transactions, so a source-count-only poll can observe the
    // purge half-applied (chunk rows still present).
    assert(waitFor(() => sourceCount(db) == 3 && chunkRowsContaining(db,
            "fuchsia") == 0 && chunkRowsContaining(db, "marimba") == 0, 10.seconds),
            "the deleted files must be purged with their chunks at pass completion, got " ~ sourceCount(db)
                .to!string ~ " sources, " ~ chunkRowsContaining(db,
                    "fuchsia").to!string ~ "/" ~ chunkRowsContaining(db,
                "marimba").to!string ~ " leftover chunks");
    assert(waitFor(() => ftsMatches(db, "fuchsia") == 0 && ftsMatches(db, "marimba") == 0, 30.seconds),
            "the re-triggered rebuild must drop the deleted content from text search");
    assert(ftsMatches(db, "zeppelin") >= 1, "surviving content must stay text-searchable");

    // -- 4. identical re-drop ------------------------------------------------
    const docCallsBefore = embedder1.docCalls;
    writeFile(dir, "dropped/sub/f_fuchsia.md", "fuchsia blossoms in the atrium");
    dynSend(actor1.weakRef, "onSweepTick");
    assert(waitFor(() => sourceCount(db) == 4, 10.seconds),
            "the re-created file must be indexed again, got " ~ sourceCount(db).to!string);
    assert(semanticHit(db, embedder1, "fuchsia blossoms in the atrium",
            "dropped/sub/f_fuchsia.md"), "the re-created file must be vector-searchable again");
    assert(embedder1.docCalls > docCallsBefore,
            "a re-add after the purge re-embeds: its dedup row went with the source");

    // Identical rewrite of a SURVIVING indexed file: only the stamp changes,
    // the content hash does not - the fresh actor below must keep this a
    // dedup no-op.
    writeFile(dir, "dropped/w_zeppelin.md", "zeppelin hangar sketches");

    // -- 5. restart heal ------------------------------------------------------
    sys1.shutdown;
    sys1Down = true;

    // Simulate a lost write: the source row vanishes while the file stays on
    // disk; the actor that knew about it is gone with the old process.
    db.removeSource(Origin(Path("dropped/w_quokka.md")));
    assert(sourceCount(db) == 3, "the lost write must drop the source count first");

    auto embedder2 = new CountingEmbedder;
    auto healer = new DropRagActor(relPathOf(dir, "dropped").AbsolutePath, "dropped".Path,
            embedder2, relPathOf(dir, "p.db").AbsolutePath, 10_000_000,
            RagConfig(), 256, 500.msecs);
    scope (exit)
        healer.onExit(ExitMsg.init);
    runPass(healer);
    assert(originSet(db) == [
        "dropped/sub/f_fuchsia.md", "dropped/w_quokka.md",
        "dropped/w_sarong.md", "dropped/w_zeppelin.md"
    ].sort.array, "the startup heal must re-index the lost source, got " ~ originSet(db).to!string);
    assert(semanticHit(db, embedder2, "quokka grazing notes",
            "dropped/w_quokka.md"), "the healed file must be vector-searchable again");
    assert(embedder2.docCalls == 1, "only the healed file may be re-embedded (the byte-identical rewrite of "
            ~ "a survivor must stay a dedup no-op), got " ~ embedder2.docCalls.to!string);

    // -- 6. manual-path origin form -------------------------------------------
    // A destination file indexed through the manual tool path: the writer
    // stores the workarea-relative normalized path as the origin (the exact
    // tool_call expression, with the scratch dir standing in for the
    // workarea). The sweep must treat that source as its own - matched
    // component-wise - and purge it once the file vanishes. The writer's
    // normalization variants (separator, leading ./, relative form) are
    // pinned separately by the originUnderDestination unittests in
    // drop_reconcile.d; this test pins the exact form the tool path writes.
    import llm.rag.rag : addToDatabase;

    writeFile(dir, "dropped/manual.md", "sgraffito manual entry");
    // The tool path resolves the user input to an absolute path first and the
    // workarea root is absolute, so mirror that here (relativePath passes a
    // relative input through unchanged).
    const manualPath = absolutePath(relPathOf(dir, "dropped/manual.md"));
    const manualOrigin = Path(buildNormalizedPath(relativePath(manualPath, absolutePath(dir))));
    size_t nBatchManual;
    const added = addToDatabase(db, embedder2, Document(Origin(manualOrigin),
            "sgraffito manual entry", Offset.init), RagConfig(), nBatchManual);
    assert(added.length > 0, "the manual add must index the file");
    assert(sourceCount(db) == 5, "the manual source must join the actor's four");

    remove(manualPath);
    runPass(healer);
    assert(originSet(db) == [
        "dropped/sub/f_fuchsia.md", "dropped/w_quokka.md",
        "dropped/w_sarong.md", "dropped/w_zeppelin.md"
    ].sort.array, "the sweep must purge the manually indexed source once its file " ~ "is gone, got " ~ originSet(db)
        .to!string);
    assert(chunkRowsContaining(db, "sgraffito") == 0,
            "the manual source's chunks must go with the purge");
}
