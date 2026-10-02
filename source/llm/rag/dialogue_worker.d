/// Dialogue indexing worker: a my.actor actor (on the app's System) that owns its own embedder and indexes evicted raw dialogue into per-session RAG databases.
///
/// The worker runs on an actor mailbox (spawned by DialogueIndex) and never shares its embedder or its batch-size state with the agent thread. It creates its OWN embedder from the EmbedConfig at spawn, opens a per-session write connection (WAL), and indexes each episode through the shared addToDatabase seam (which does the per-thread nBatchCache adaptation). All communication is via value messages (DiJob / DiDrain / DiDrained / DiDegraded / RiJob / RiRecord / RiDone); there is no shared state and no lock. A RiJob additionally spawns a DETACHED SummarizerActor that makes the LLM call OFF the mailbox (dedicated timeout); DiDrain bounds those in-flight summarizers up to a drain deadline before finalizing. The worker runs for the lifetime of the process (in-flight episodes are lost at exit -- a known hole) and keeps serving after a drain (later drains re-checkpoint and re-close).
module llm.rag.dialogue_worker;

import core.sync.mutex : Mutex;
import core.time : Duration, dur;
import logger = std.logger;
import std.algorithm : filter;
import std.array : empty;
import std.datetime : SysTime, Clock;
import std.exception : collectException;
import std.json : JSONValue;
import std.range : empty;
import std.string : strip;
import std.sumtype : match;
import std.typecons : Tuple;

import my.actor;
import my.optional;
import my.path : AbsolutePath;

import llm.chat;
import llm.common.config : EmbedConfig, RemoteEmbedConfig, ServerConfig;
import llm.common.embedder : Embedder, EmbedderFactory, createEmbedder, EmbedResult, EmbedError;
import llm.config : RagConfig, SummaryModelConfig, toRequestConfig;
import llm.query : LlmRequester, toJson, LlamaRequestError;
import llm.rag.database : Database, openDatabase, Source, SourceChecksum, SourceId;
import llm.rag.dialogue_index : Kind, EpisodeMeta, encodeTopicName, decodeTopicName;
import llm.rag.rag : Document, Origin, Topic, addToDatabase;
import llm.session.types : SessionId, isValidId;
import llm.summary_agent : stripFences;

/// A single evicted dialogue episode to be indexed verbatim.
///
/// `topicName` is the fully-encoded Topic name (built by DialogueIndex via the episode codec); the worker uses it directly as the Document origin, so it is never parsed or re-encoded here.
struct DiEpisode {
    string topicName;
    string text;
    long turnEnd;
}

/// One compression checkpoint's worth of episodes for a single session.
///
/// `episodes` must be an `immutable` dynamic array (message discipline: the worker reads the buffer asynchronously, so the sender must not keep a mutable view of it across the boundary). The strings are `char[]` (already immutable) and the worker dup's what it needs.
struct DiJob {
    string sessionId;
    immutable(DiEpisode[]) episodes;
}

/// Drain request: the reply target carries a gate; waiters pause on `ScopedActor.receiveTimeout` and close the gate when they stop waiting. A closed gate means the reply is skipped.
struct DiDrain {
    WeakAddress replyTo;
    CompletionGate gate;
    Duration budget = Duration.zero; // zero = ReasoningDrainBudget fallback

    this(WeakAddress replyTo, CompletionGate gate, Duration budget = Duration.zero) @safe nothrow {
        this.replyTo = replyTo;
        this.gate = gate;
        this.budget = budget;
    }
}

/// Sent back to DiDrain.replyTo once the mailbox has drained.
struct DiDrained {
}

/// Sent to the owner when the worker cannot create its embedder. Indexing is then disabled for the process lifetime (known hole, no recovery); the worker still answers DiDrain so disposal never hangs.
struct DiDegraded {
    immutable string reason;
}

// Reasoning protocol: RiJob in -> per-job detached SummarizerActor (LLM call OFF the mailbox, dedicated timeout) -> RiRecord / RiDone back -> indexed verbatim into the session DB (kind r_).

/// One eviction's pre-formatted, budget-capped trace (built by ReasoningIndex.onCheckpoint). All value types; traceText dup'd (DiEpisode discipline).
struct RiJob {
    string sessionId;
    string traceText;
    long turnStart;
    long turnEnd;
}

/// Completed record: topicName already encoded (r_); recordText = the fence-stripped model response, verbatim.
struct RiRecord {
    string topicName;
    string recordText;
}

/// Completion with no record (empty response / LLM failure); `reason` is a short code (truncated when logged).
struct RiDone {
    string topicName;
    string reason;
}

/// DI seam (mirrors EmbedderFactory): null = real dedicated-config LlmRequester call; a test fake may sleep to prove off-mailbox. The seam is a function pointer, not a `string delegate(...)`: a plain (unshared) delegate carries a hidden context pointer, so it cannot cross into the actor via `spawn` at all (message values must not alias sender state). A function pointer (the EmbedderFactory shape this seam mirrors) has no context pointer and is spawn-legal; test fakes are module-scope functions, and null keeps the "real LlmRequester" meaning.
alias SummarizerFn = string function(string prompt, string traceText);

// Dedicated budget: NEVER inherits the summary model's timeout chain (unbounded when timeoutSeconds unset).
immutable int ReasoningTimeoutS = 300;
immutable int ReasoningMaxTokens = 512;

// The bounded drain deadline (the summarizer's dedicated timeout plus 30s of slack). Production default: a DiDrain carrying no override makes the worker wait this long for in-flight completions before finalizing. Unit tests exercise the deadline path in seconds via the per-drain DiDrain.budget override -- the budget travels in the message, so no cross-thread mutable global is involved. The budget override exists because hard-coding the sum in the drain handler would be infeasible to test at 330s.
Duration ReasoningDrainBudget = (ReasoningTimeoutS + 30).dur!"seconds";

/// Completion delivery gate (single protocol: lockedOpen / unlock / close; no raw field access). The waiter holds `open_` while its target is provably not torn down; the closer runs before any teardown and takes the same mutex as the delivery path, so a late completion either delivers or skips -- never both.
///
/// Delivery inside the locked section must be NON-BLOCKING: the target must be an UNBOUNDED mailbox (plain `sys.spawn`, never `spawnBounded`) -- a bounded, full mailbox drops the completion silently, and a blocking send from a non-actor sender would stall the closer in close().
final class CompletionGate {
    private Mutex mutex_;
    private bool open_ = true;

    this() @safe nothrow {
        mutex_ = new Mutex; // ctor-assigned; never field-inited
    }

    /// True if still open and the caller now holds the mutex (release with unlock() exactly once).
    bool lockedOpen() @trusted {
        mutex_.lock();
        if (open_)
            return true;
        mutex_.unlock();
        return false;
    }

    /// Release the mutex taken by a successful lockedOpen().
    void unlock() @trusted {
        mutex_.unlock();
    }

    /// Close the gate (idempotent); all later lockedOpen() calls return false.
    void close() @trusted {
        mutex_.lock();
        open_ = false;
        mutex_.unlock();
    }
}

/// One-shot reasoning run (off the mailbox): the ONLY mailbox contact is the ONE gate-guarded completion send; all exceptions are caught internally (an escaping Error terminates the process).
private void runSummarizer(const SummaryModelConfig* cfg, string prompt, string traceText,
        string topicName, SummarizerFn fn, CompletionGate gate, WeakAddress worker) {
    string raw;
    bool got;
    string reason;
    try {
        if (fn !is null) {
            raw = fn(prompt, traceText);
            got = true;
        } else {
            auto rc = toRequestConfig(cast(SummaryModelConfig)*cfg); // mutable copy: the config builder is not const-correct
            rc.timeoutS = ReasoningTimeoutS;
            rc.maxRetries = 1;
            rc.header["max_tokens"] = JSONValue(ReasoningMaxTokens);
            Chat chat;
            chat.setSystemPrompt(prompt);
            chat.add(Message(Role.user, userQuery: true, content: traceText, thinking: null));
            auto rq = LlmRequester(rc);
            auto response = rq.request(chat, null);
            response.toJson.match!((JSONValue j) {
                foreach (choice; j["choices"].array) {
                    raw = choice["message"]["content"].str.strip;
                    got = true;
                }
            }, (LlamaRequestError e) { reason = "llm_error: " ~ e.response; });
        }
    } catch (Exception e) {
        reason = "llm_error: " ~ e.msg;
    }
    string recordText;
    if (got && raw !is null)
        recordText = stripFences(raw); // verbatim, fence-stripped
    if (gate.lockedOpen()) {
        try {
            if (recordText.length > 0)
                dynSend(worker, "riRecord", RiRecord(topicName, recordText));
            else {
                if (reason.empty)
                    reason = "empty";
                dynSend(worker, "riDone", RiDone(topicName, reason));
            }
        } finally {
            gate.unlock();
        }
    } else {
        logger.warningf("summarizer: worker gone before completion; record lost (topic '%s')",
                topicName);
    }
}

/// One-shot summarizer: no public methods -- after onSpawn the actor self-terminates via the empty-behavior rule. Completion is delivered under the gate; a closed gate skips without touching the worker's address.
final class SummarizerActor {
    private {
        SummaryModelConfig cfg_;
        string prompt_;
        string traceText_;
        string topicName_;
        SummarizerFn fn_;
        CompletionGate gate_;
        WeakAddress worker_;
    }

    this(SummaryModelConfig cfg, string prompt, string traceText, string topicName,
            SummarizerFn fn, CompletionGate gate, WeakAddress worker) @safe nothrow {
        cfg_ = cfg;
        prompt_ = prompt;
        traceText_ = traceText;
        topicName_ = topicName;
        fn_ = fn;
        gate_ = gate;
        worker_ = worker;
    }

    void onSpawn(ActorRef self) {
        runSummarizer(&cfg_, prompt_, traceText_, topicName_, fn_, gate_, worker_);
    }
}

/// The compile-checked send surface for DialogueWorkerActor
//(Channel!DialogueWorkerAPI). The by-name deadline self-message
//(drainDeadline) is deliberately NOT here: the Channel surface is the external
//protocol, while the deadline is an internal scheduling ticket.
interface DialogueWorkerAPI {
    void diJob(DiJob job);
    void riJob(RiJob job);
    void riRecord(RiRecord rec);
    void riDone(RiDone done);
    void diDrain(DiDrain drain);
}

/// The dialogue worker actor: per-session WAL databases on an actor mailbox.
//The embedder is created on spawn, on the actor's own thread (never shared);
//riJob spawns a DETACHED SummarizerActor whose LLM call runs OFF the mailbox
//and whose completion is delivered through the worker's gate; drain replies
//are gate-guarded per waiter, and the worker keeps serving after a drain
//(later drains re-checkpoint and re-close).
final class DialogueWorkerActor : DialogueWorkerAPI {
    /// One drain request's reply target + its gate.
    private struct DrainWaiter {
        WeakAddress replyTo;
        CompletionGate gate;
    }

    // Ctor-stored config (no I/O, no sends -- the ctor runs on the caller's thread; the actor has no address yet).
    private {
        System* sys_;
        WeakAddress owner_; // supervisor/agent or scoped test admin; empty = skip the degraded notice
        AbsolutePath dialogueDir_;
        EmbedConfig embedConfig_;
        RagConfig dialogueRagCfg_;
        EmbedderFactory embedderFactory_;
        SummaryModelConfig summaryCfg_;
        string reasoningPrompt_;
        SummarizerFn summarizerFn_;
    }

    // Worker state: only this mailbox reads or writes it; the detached summarizers share no state with it.
    private {
        WeakAddress self_;
        CompletionGate gate_;
        bool exited_;
        size_t nBatchCache_; // owned here (per-thread), passed by reference into the addToDatabase seam
        Database[string] dbs_; // per-session write connections (lazy)
        long outstandingThreads_; // in-flight detached summarizers
        Embedder embedder_;
        bool degraded_;
        bool drainPending_;
        DrainWaiter[] drainWaiters_;
        bool drainArmed_;
        long drainSeq_; // deadline ticket; a stale ticket no-ops
    }

    this(System* sys, WeakAddress owner, AbsolutePath dialogueDir, EmbedConfig embedConfig,
            RagConfig dialogueRagCfg, EmbedderFactory embedderFactory,
            SummaryModelConfig summaryCfg = SummaryModelConfig.init,
            string reasoningPrompt = "", SummarizerFn summarizerFn = null) @safe nothrow {
        sys_ = sys;
        owner_ = owner;
        dialogueDir_ = dialogueDir;
        embedConfig_ = embedConfig;
        dialogueRagCfg_ = dialogueRagCfg;
        embedderFactory_ = embedderFactory;
        summaryCfg_ = summaryCfg;
        reasoningPrompt_ = reasoningPrompt;
        summarizerFn_ = summarizerFn;
    }

    void onSpawn(ActorRef self) {
        self_ = self.address();
        gate_ = new CompletionGate;
        try {
            // Create our OWN embedder on this thread. There is NO shared embedder between this worker and any other thread. Tests inject a factory (embedderFactory_ != null); production goes through the registry.
            try {
                embedder_ = embedderFactory_ is null ? createEmbedder(embedConfig_) : embedderFactory_(
                        embedConfig_);
            } catch (Exception e) {
                embedder_ = null;
                logger.warningf("dialogue worker: embedder factory threw: %s", e.msg);
            }
            degraded_ = embedder_ is null;
            if (degraded_) {
                logger.warning(
                        "dialogue worker: no embedder available; indexing disabled for process lifetime");
                if (!owner_.empty)
                    dynSend(owner_, "diDegraded", "no embedder available");
            }
        } catch (Throwable t) {
            killOnError("onSpawn", t);
        }
    }

    void onExit(ExitMsg _) {
        exited_ = true;
        // First action: close the worker-side gate under the same mutex as the summarizer's delivery path, so a late completion either delivers or skips -- never both.
        if (gate_ !is null)
            gate_.close();
        // Destroy any remaining handles WITHOUT checkpoint (known loss window): WAL sidecars, if any, are recovered by SQLite on the next read-write open. Every handler early-returns on exited_ from here on -- no ensureDb reopen, no embedder call, no finalize during shutdown.
        foreach (db; dbs_.byKeyValue) {
            db.value.destroy;
        }
        dbs_ = null;
    }

    void onException(Exception e) {
        // Shell-level exception (dispatch plumbing outside the handler bodies): log-only -- the actor survives and the gate stays valid.
        logger.warningf("dialogue worker actor exception: %s", e.msg);
    }

    void onError(ErrorMsg e) {
        import std.conv : to;

        logger.warningf("dialogue worker actor error: %s", e.reason.to!string);
    }

    override void diJob(DiJob job) {
        if (exited_) {
            logger.tracef("dialogue worker: diJob dropped post-exit");
            return;
        }
        try {
            indexJob(job);
        } catch (Exception e) {
            logger.errorf("dialogue worker: indexJob threw: %s", e.msg);
        } catch (Throwable t) {
            killOnError("diJob", t);
        }
    }

    override void riJob(RiJob job) {
        if (exited_) {
            logger.tracef("dialogue worker: riJob dropped post-exit");
            return;
        }
        try {
            if (degraded_ || !ensureDb(job.sessionId)) {
                // no LLM spend while degraded; ensureDb rejects invalid ids.
                logger.tracef("dialogue worker: skipping reasoning job for '%s'", job.sessionId);
                return;
            }
            long epochMillis = Clock.currTime.toUnixTime * 1000; // worker clock rather than the checkpoint's timestamp (deliberate, kept)
            // NOTE: kind param is LAST: the leading-defaulted-param form does not compile against the 4-arg call sites.
            string topicName = encodeTopicName(job.sessionId, job.turnStart,
                    job.turnEnd, epochMillis, Kind.reasoning);
            ++outstandingThreads_;
            try {
                sys_.spawn!(Config.detached, SummarizerActor)(summaryCfg_,
                        reasoningPrompt_, job.traceText, topicName, summarizerFn_, gate_, self_);
            } catch (Exception e) {
                // A failed spawn must not leak the counter, or a later drain would burn its full budget waiting for a completion that will never arrive.
                --outstandingThreads_;
                logger.tracef("dialogue worker: reasoning spawn failed (topic '%s'): %s",
                        topicName, e.msg);
                return;
            }
            logger.tracef("dialogue worker: spawned reasoning summarizer '%s' turns %s-%s (%s chars, topic '%s')",
                    job.sessionId, job.turnStart, job.turnEnd, job.traceText.length, topicName);
        } catch (Exception e) {
            logger.errorf("dialogue worker: riJob threw: %s", e.msg);
        } catch (Throwable t) {
            killOnError("riJob", t);
        }
    }

    override void riRecord(RiRecord rec) {
        if (exited_) {
            logger.tracef("dialogue worker: riRecord dropped post-exit");
            return;
        }
        try {
            recordJob(rec);
        } catch (Exception e) {
            logger.errorf("dialogue worker: recordJob threw: %s", e.msg);
        } catch (Throwable t) {
            killOnError("riRecord", t);
            return;
        }
        // Early finalize: the last completion landing while a drain is pending
        // closes the stores now instead of out-waiting the deadline. Same
        // guard as the rest of the handler: an Error here must kill the actor
        // (onExit teardown), not escape into the pool worker's Throwable
        // swallow and orphan it.
        try {
            if (drainPending_ && outstandingThreads_ == 0)
                finalizeDispose();
        } catch (Exception e) {
            logger.errorf("dialogue worker: finalize threw: %s", e.msg);
        } catch (Throwable t) {
            killOnError("riRecord", t);
        }
    }

    override void riDone(RiDone done) {
        if (exited_) {
            logger.tracef("dialogue worker: riDone dropped post-exit");
            return;
        }
        try {
            doneJob(done);
        } catch (Exception e) {
            logger.errorf("dialogue worker: doneJob threw: %s", e.msg);
        } catch (Throwable t) {
            killOnError("riDone", t);
            return;
        }
        try {
            if (drainPending_ && outstandingThreads_ == 0)
                finalizeDispose();
        } catch (Exception e) {
            logger.errorf("dialogue worker: finalize threw: %s", e.msg);
        } catch (Throwable t) {
            killOnError("riDone", t);
        }
    }

    override void diDrain(DiDrain d) {
        if (exited_) {
            logger.tracef("dialogue worker: diDrain dropped post-exit");
            return;
        }
        try {
            const budget = (d.budget == Duration.zero) ? ReasoningDrainBudget : d.budget;
            // The requester is ALWAYS registered as a waiter, even when the
            // worker is idle: the finalize below (immediate on the idle path)
            // answers this drain under its gate in its reply loop. A drain
            // must never leave the caller waiting out its own timeout.
            drainPending_ = true;
            drainWaiters_ ~= DrainWaiter(d.replyTo, d.gate);
            if (outstandingThreads_ == 0) {
                finalizeDispose();
                return;
            }
            if (!drainArmed_) {
                drainArmed_ = true;
                const seq = ++drainSeq_;
                dynDelayedSend(self_, Clock.currTime + budget, "drainDeadline", seq);
            }
        } catch (Exception e) {
            logger.errorf("dialogue worker: drainAndClose threw: %s", e.msg);
        } catch (Throwable t) {
            killOnError("diDrain", t);
        }
    }

    // By-name self-message (the deadline armed by diDrain). Public because
    // message registration only sees public methods -- but NOT part of
    // DialogueWorkerAPI: it is an internal scheduling ticket, not the send
    // surface.
    void drainDeadline(long seq) {
        if (exited_)
            return;
        if (seq != drainSeq_ || !drainPending_)
            return; // stale ticket (early finalize or a newer drain): no-op
        logger.warningf(
                "dialogue worker: drain deadline; %s reasoning thread(s) still in flight; record(s) lost (advisory)",
                outstandingThreads_);
        try {
            finalizeDispose();
        } catch (Exception e) {
            logger.errorf("dialogue worker: drainAndClose threw: %s", e.msg);
        } catch (Throwable t) {
            killOnError("drainDeadline", t);
        }
    }

    // Open (or reuse) the WAL write connection for a session. WAL is applied
    // once per opened connection so the agent thread can read committed
    // episodes concurrently. Returns true on success. NOTE: openDatabase
    // returns None (not an error) when the parent dir is missing or unwritable
    // -- the DialogueIndex constructor mkdirRecurse's the directory, so this
    // only fails on an unwritable directory.
    private bool ensureDb(string sessionId) {
        // Defense in depth: onCheckpoint validates, but the worker must never be a path-traversal vector for a future caller that bypasses it.
        if (sessionId.empty || !isValidId(SessionId(sessionId))) {
            logger.warningf("dialogue worker: refusing invalid session '%s'", sessionId);
            return false;
        }
        if (sessionId in dbs_)
            return true;
        auto path = (dialogueDir_ ~ (sessionId ~ ".db")).AbsolutePath;
        auto dbOpt = openDatabase(path, embedder_.modelName(),
                embedder_.dimensions(), readOnly: false);
        return dbOpt.match!((Database d) {
            try {
                d.run("PRAGMA journal_mode=WAL;");
            } catch (Exception e) {
                logger.warningf("dialogue worker: WAL pragma failed on '%s': %s", path, e.msg);
            }
            dbs_[sessionId] = d;
            return true;
        }, (None _) {
            logger.warningf("dialogue worker: cannot open session DB '%s', skipping", path);
            return false;
        });
    }

    // Rebuild the FTS5 index of one session (fails soft: chunks are committed
    // either way; only text search is affected, and only until a later job's
    // rebuild). The external-content FtsChunksTbl is not auto-synced with
    // TextChunkTbl; this is the same seam the RAG add tools use.
    private void rebuildFts(string sessionId) {
        try {
            dbs_[sessionId].fts5Rebuild;
        } catch (Exception e) {
            logger.warningf("dialogue worker: fts5 rebuild failed for '%s': %s", sessionId, e.msg);
        }
    }

    // Index one checkpoint's episodes. Observability: exactly ONE tracef per
    // completed job (session id, episode/chunk/failure counts, turn range min
    // turnStart - max turnEnd); per-chunk logging is forbidden and the episode
    // text is never logged. Episodes are single-turn (DialogueIndex groups by
    // turnId, so each DiEpisode has turnStart == turnEnd), hence
    // min(ep.turnEnd) over the job IS its min turnStart.
    private void indexJob(DiJob job) {
        if (degraded_ || job.sessionId.empty || !ensureDb(job.sessionId))
            return;
        long jobMinTurn;
        long jobMaxTurn;
        size_t successes;
        size_t totalChunks;
        size_t failures;
        foreach (ep; job.episodes.filter!(a => !a.text.empty)) {
            // A turn split across two compressions re-arrives under the same
            // topic; merge the existing episode text with the new piece
            // instead of replacing it (topic name and turn range stay
            // unchanged). Read failures degrade to the plain piece.
            string pieceText = ep.text;
            try {
                auto existingOpt = dbs_[job.sessionId].getSource(Origin(Topic(ep.topicName)));
                if (hasValue(existingOpt)) {
                    auto existing = existingOpt.match!(a => a.value,
                            (None _) => Tuple!(Source, "src", SourceId, "id").init);
                    auto oldText = dbs_[job.sessionId].sourceText(existing.id);
                    if (!oldText.empty) {
                        pieceText = oldText ~ "\n" ~ ep.text;
                        logger.tracef("dialogue worker: merged episode '%s' in '%s' (%s + %s chars)",
                                ep.topicName, job.sessionId, oldText.length, ep.text.length);
                    }
                }
            } catch (Exception e) {
                logger.warningf("dialogue worker: cannot read existing episode '%s' in '%s', replacing: %s",
                        ep.topicName, job.sessionId, e.msg);
            }
            auto doc = Document(origin: Origin(Topic(ep.topicName)), data: pieceText);

            try {
                // the topic name is the dedup salt, so identical text
                // re-arriving under a different topic is not deduped away.
                auto res = addToDatabase(dbs_[job.sessionId], embedder_, doc,
                        dialogueRagCfg_, nBatchCache_, ep.topicName);
                totalChunks += res.chunks;
                ++successes;
            } catch (Exception e) {
                logger.warningf("dialogue worker: failed to index episode '%s' in '%s': %s",
                        ep.topicName, job.sessionId, e.msg);
                ++failures;
            }
            if (jobMinTurn == 0 || ep.turnEnd < jobMinTurn)
                jobMinTurn = ep.turnEnd;
            if (ep.turnEnd > jobMaxTurn)
                jobMaxTurn = ep.turnEnd;
        }
        // every job that committed at least one chunk ends with a synchronous
        // FTS5 rebuild, so committed chunks are never left text-invisible
        // (known hole: a crash between the last chunk commit and this
        // rebuild).
        if (totalChunks > 0)
            rebuildFts(job.sessionId);
        logger.tracef("dialogue worker: session '%s' indexed %s episodes (%s chunks, %s failures, turns %s-%s)",
                job.sessionId, successes, totalChunks, failures, jobMinTurn, jobMaxTurn);
    }

    // Reasoning completion: index the fence-stripped record verbatim (same
    // seam, dedup salt = topic name) and rebuild FTS. FAST: no LLM work.
    // --outstandingThreads_ first, so a pending drain observes the decrement
    // even if indexing below fails.
    private void recordJob(RiRecord r) {
        --outstandingThreads_;
        auto metaOpt = decodeTopicName(r.topicName);
        string sid = metaOpt.match!((EpisodeMeta m) => m.sessionId, (_) => "");
        if (!hasValue(metaOpt) || degraded_ || !ensureDb(sid)) {
            logger.tracef("dialogue worker: dropping reasoning record '%s' (db unavailable)",
                    r.topicName);
            return;
        }
        auto doc = Document(origin: Origin(Topic(r.topicName)), data: r.recordText);
        try {
            auto res = addToDatabase(dbs_[sid], embedder_, doc,
                    dialogueRagCfg_, nBatchCache_, r.topicName);
            if (res.chunks > 0)
                rebuildFts(sid);
            logger.tracef("dialogue worker: indexed reasoning record '%s' (%s chunks, %s chars)",
                    r.topicName, res.chunks, r.recordText.length);
        } catch (Exception e) {
            logger.tracef("dialogue worker: failed to index reasoning record '%s': %s",
                    r.topicName, e.msg);
        }
    }

    private void doneJob(RiDone r) {
        --outstandingThreads_;
        logger.tracef("dialogue worker: reasoning record '%s' skipped: %s",
                r.topicName, r.reason.length > 200 ? r.reason[0 .. 200] : r.reason);
    }

    // Checkpoint + close all write connections, then answer every pending
    // drain waiter under its gate. Leaves the session DBs in a clean,
    // sidecar-free state for the next process's read-only opens (normal drain
    // path; an abnormal death still leaves WAL sidecars, which SQLite recovers
    // on the next read-write open). Does NOT close gate_: the worker may still
    // serve later jobs and later drains (later drains re-arm: new seq, new
    // deadline).
    private void finalizeDispose() {
        foreach (sessionId, ref db; dbs_) {
            try {
                db.run("PRAGMA wal_checkpoint(TRUNCATE);");
            } catch (Exception e) {
                logger.warningf("dialogue worker: wal_checkpoint failed for '%s': %s",
                        sessionId, e.msg);
            }
            db.destroy;
        }
        dbs_ = null;
        drainPending_ = false;
        drainArmed_ = false;
        foreach (ref w; drainWaiters_) {
            if (w.gate is null || !w.gate.lockedOpen())
                continue; // closed: skip, never touch replyTo
            try {
                dynSend(w.replyTo, "diDrained", DiDrained());
            } finally {
                w.gate.unlock();
            }
        }
        drainWaiters_ = null;
    }

    // A core.exception.Error escaping a handler would be swallowed by the pool
    // worker (TaskPool.doJob catches Throwable) and orphan the actor -- no
    // onExit, no gate close, no DB close. A kill exit runs onExit (gate closed
    // first) and force-shuts even an actor with a user onExit, so teardown
    // always happens; ordinary Exceptions keep the log-and-continue in the
    // handlers.
    private void killOnError(string method, Throwable t) {
        logger.errorf("dialogue worker: %s Error: %s", method, t.msg);
        sendExit(self_, ExitReason.kill);
    }
}

version (unittest) {
    import core.atomic : atomicLoad, atomicStore, atomicOp;
    import std.datetime : DateTime, UTC;
    import std.file : mkdirRecurse;
    import llm.test_util : TestArea, testArea, sharedLogSwapMutex;

    // Test embedder factories. Defined at module scope (not nested in a
    // unittest) as plain `Embedder f(EmbedConfig)` functions: each test passes
    // one by reference into the worker actor's ctor (the DI seam), and the
    // address of a module-scope function (`&f`) converts to the plain
    // EmbedderFactory pointer.

    /// Non-thread-safe embedder for the concurrency-isolation test: it writes
    //the input into a per-instance scratch buffer, sleeps to force concurrent
    //overlap, then reads it back.
    private class NtsEmbedder : Embedder {
        private char[] scratch;

        this() {
            scratch = new char[64];
        }

        override void destroy() {
        }

        override string modelName() {
            return "nts";
        }

        override long dimensions() {
            return 16;
        }

        override bool supportsTokenization() {
            return false;
        }

        override EmbedResult embedQuery(string text) {
            return embed(text);
        }

        override EmbedResult embedDocument(string text) {
            return embed(text);
        }

        EmbedResult embed(string text) {
            import core.thread : Thread;
            import core.time : dur;
            import std.algorithm : min;

            int n = min(cast(int) text.length, 16);
            scratch[0 .. n] = text[0 .. n];
            scope (exit)
                Thread.sleep(50.dur!"msecs"); // force overlap
            auto v = new float[16];
            foreach (i, ref f; v)
                f = i < n ? cast(float) scratch[i] : 0f;
            return EmbedResult(v);
        }

        override EmbedResult embedQuery(int[] tokens) {
            return embed(tokens);
        }

        override EmbedResult embedDocument(int[] tokens) {
            return embed(tokens);
        }

        EmbedResult embed(int[] tokens) {
            return EmbedResult(EmbedError("no tokens"));
        }

        override int[] tokenize(string text, bool addSpecial) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            return 1024;
        }
    }

    // Deterministic embedder for the worker integration test: returns an
    // all-ones 8-dim vector; a small batchSize forces multiple chunks.
    private class WkEmbedder : Embedder {
        override void destroy() {
        }

        override string modelName() {
            return "wk";
        }

        override long dimensions() {
            return 8;
        }

        override bool supportsTokenization() {
            return false;
        }

        override EmbedResult embedQuery(string text) {
            return embed(text);
        }

        override EmbedResult embedDocument(string text) {
            return embed(text);
        }

        EmbedResult embed(string text) {
            auto v = new float[8];
            foreach (ref f; v)
                f = 1.0f;
            return EmbedResult(v);
        }

        override EmbedResult embedQuery(int[] tokens) {
            return embed(tokens);
        }

        override EmbedResult embedDocument(int[] tokens) {
            return embed(tokens);
        }

        EmbedResult embed(int[] tokens) {
            return EmbedResult(EmbedError("no tokens"));
        }

        override int[] tokenize(string text, bool addSpecial) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            return 16;
        } // small -> several chunks
    }

    // Embedder for the per-episode failure test: returns a valid all-ones
    // 8-dim vector for normal text, but THROWS for any text carrying the
    // poison marker. A throw (as opposed to an EmbedError) propagates out of
    // addToDatabase so the worker's per-episode catch is exercised for exactly
    // that episode.
    private class PoisonedEmbedder : Embedder {
        override void destroy() {
        }

        override string modelName() {
            return "wk";
        }

        override long dimensions() {
            return 8;
        }

        override bool supportsTokenization() {
            return false;
        }

        override EmbedResult embedQuery(string text) {
            return embed(text);
        }

        override EmbedResult embedDocument(string text) {
            return embed(text);
        }

        EmbedResult embed(string text) {
            import std.string : indexOf;

            if (text.indexOf("POISON") >= 0)
                throw new Exception("poisoned text (per-episode failure test)");
            auto v = new float[8];
            foreach (ref f; v)
                f = 1.0f;
            return EmbedResult(v);
        }

        override EmbedResult embedQuery(int[] tokens) {
            return embed(tokens);
        }

        override EmbedResult embedDocument(int[] tokens) {
            return embed(tokens);
        }

        EmbedResult embed(int[] tokens) {
            return EmbedResult(EmbedError("no tokens"));
        }

        override int[] tokenize(string text, bool addSpecial) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            return 16;
        }
    }

    // Plain-function embedder factories for the actor tests: each test injects its own factory into the worker actor, so no test touches the process-wide "remote" factory slot (a parallel test swapping that slot while another test's worker was starting used to race the worker onto the wrong embedder - or onto a degraded one - for its whole life).
    private Embedder wkEmbedderFactory(EmbedConfig config) {
        return new WkEmbedder();
    }

    private Embedder ntsEmbedderFactory(EmbedConfig config) {
        return new NtsEmbedder();
    }

    private Embedder poisonedEmbedderFactory(EmbedConfig config) {
        return new PoisonedEmbedder();
    }

    /// Test seam: a Logger that captures formatted messages so a test can assert on emitted log lines. Installed via the std.logger `sharedLog` swap (same pattern as TuiLogger in llm.tui); thread-safe because the worker (actor mailbox + detached summarizer) logs from pool threads concurrently with the test thread.
    private class D7LogCapture : logger.Logger {
        import core.sync.mutex;
        import std.array : Appender;

        private {
            Appender!(string[]) lines;
            Mutex mtx;
        }

        this(const logger.LogLevel lvl = logger.LogLevel.all) {
            super(lvl);
            this.mtx = new Mutex;
        }

        override void writeLogMsg(ref LogEntry payload) @trusted {
            mtx.lock_nothrow();
            scope (exit)
                mtx.unlock_nothrow();
            lines.put(payload.msg);
        }

        string[] takeLines() {
            mtx.lock_nothrow();
            scope (exit)
                mtx.unlock_nothrow();
            auto tmp = lines[];
            lines.clear();
            return tmp;
        }
    }

    /// Serializes this module's swap+drain critical sections with the other
    /// capture tests (llm.config, llm.rag.reasoning_index) through test_util's
    /// sharedLogSwapMutex: silly runs unittests in parallel (TaskPool), so
    /// overlapping swap windows would send log lines into the WRONG capture.
    alias d7SharedLogMutex = sharedLogSwapMutex;
    // Per-worker result of the isolation test. Carried back to the test's
    // scoped receiver; the float[] result is checked inside the worker and
    // only a bool verdict crosses the boundary.
    private struct NtsDone {
        int idx;
        bool ok;
    }

    // Isolation worker as an actor: creates its OWN embedder from the injected
    // factory (the per-actor-thread guarantee), embeds its text, and
    // self-verifies the result matches the input -- a shared non-thread-safe
    // instance would corrupt its scratch buffer under concurrent overlap and
    // fail this check. The constructor stores only (the index is a value
    // argument, so no loop-capture hazard), and `go` does the work on its own
    // detached thread and reports the verdict to `outAddr`.
    private final class NtsIsolationActor {
        private {
            int idx_;
            string text_;
            EmbedConfig cfg_;
            EmbedderFactory factory_;
            WeakAddress out_;
        }

        this(int idx, string text, EmbedConfig cfg, EmbedderFactory factory, WeakAddress outAddr) @safe nothrow {
            idx_ = idx;
            text_ = text;
            cfg_ = cfg;
            factory_ = factory;
            out_ = outAddr;
        }

        void go() {
            auto emb = factory_(cfg_);
            if (emb is null) {
                dynSend(out_, "ntsDone", NtsDone(idx_, false));
                return;
            }
            auto v = emb.embedQuery(text_).match!((float[] a) => a, (EmbedError e) => null);
            bool ok = (v !is null && v.length == 16);
            if (ok)
                foreach (j; 0 .. 16)
                    if (v[j] != (j < text_.length ? cast(float) text_[j] : 0f))
                        ok = false;
            dynSend(out_, "ntsDone", NtsDone(idx_, ok));
        }
    }

    // Reasoning-protocol test fakes. SummarizerFn is a plain function pointer (spawn-legal; a delegate carries a hidden context pointer and cannot cross into the actor), so the fakes are module-scope functions with no captures; per-test state goes in statics. Each fake is used by exactly one test below.

    /// Fake: returns a fenced record body; the worker must strip the fences before indexing (stripFences), verbatim otherwise.
    private string riFixedFake(string prompt, string traceText) {
        return "```\nRI_FIXED_ALPHA\n```";
    }

    /// Fake: sleeps 3 s before answering, proving the LLM call runs OFF the mailbox (dedicated timeout): a DiJob queued behind an in-flight RiJob must complete (its chunk FTS-visible) well before the sleep ends. Also used by the deadline test, where the 1.5 s budget is shorter than the sleep.
    private string riSlowFake(string prompt, string traceText) {
        import core.thread : Thread;

        Thread.sleep(3000.dur!"msecs");
        return "SLOWRI record body zeta";
    }

    /// Fake: sleeps 800 ms (drain test: the record must land in the DB during the bounded drain wait, before the drain's checkpoint+close).
    private string riDrainFake(string prompt, string traceText) {
        import core.thread : Thread;

        Thread.sleep(800.dur!"msecs");
        return "DRAINJOIN_GAMMA";
    }

    /// Fake: simulates an LLM failure -> the worker takes the RiDone path (no record indexed, warning logged, mailbox unblocked).
    private string riThrowingFake(string prompt, string traceText) {
        throw new Exception("riThrowingFake: simulated LLM failure (test)");
    }

    /// Fake: records that it was invoked (module-scope flag: a plain function pointer cannot capture state). Proves a degraded worker never spawns a summarizer thread (no LLM spend while degraded).
    private bool riCountFakeInvoked;
    private string riCountFake(string prompt, string traceText) {
        riCountFakeInvoked = true;
        return "counted";
    }

    /// Fake for the SummarizerActor gate smoke tests: returns a non-empty record body (fence-stripped verbatim by the run).
    private string riGateFake(string prompt, string traceText) {
        return "gate record body";
    }

    // Releasable fakes for the drain-protocol edge tests: each holds its own detached summarizer until the test
    // releases it. One release flag per fake: the blocks run in parallel under the test runner, so a shared flag
    // would release every block at once. The spin below is the allowlisted unittest backoff of these blocks
    // (fake-release spin); every block stores its flag before the system shutdown (release-before-shutdown) so the
    // join-all exit cannot hang on a still-spinning summarizer.

    private shared bool riRelPendingFlag;
    private shared bool riRelEarlyFlag;
    private shared bool riRelSkipFlag;
    private shared bool riRelStaleFlag;

    /// Fake: spins until the drain-while-pending test releases the flag, holding the RiRecord in flight so the
    /// drain's pending path is observable; then returns a record body.
    private string riRelPendingFake(string prompt, string traceText) {
        import core.thread : Thread;

        while (!atomicLoad(riRelPendingFlag))
            Thread.sleep(5.dur!"msecs"); // allowlisted unittest backoff: fake-release spin
        return "PENDINGREL record body";
    }

    /// Fake: spins until the early-finalize test releases the flag, then returns a record body; the completion
    /// lands far before the armed deadline.
    private string riRelEarlyFake(string prompt, string traceText) {
        import core.thread : Thread;

        while (!atomicLoad(riRelEarlyFlag))
            Thread.sleep(5.dur!"msecs"); // allowlisted unittest backoff: fake-release spin
        return "EARLYREL record body";
    }

    /// Fake: spins until the gate-skip test releases the flag, then returns a record body; the requester's gate is
    /// already closed when it lands.
    private string riRelSkipFake(string prompt, string traceText) {
        import core.thread : Thread;

        while (!atomicLoad(riRelSkipFlag))
            Thread.sleep(5.dur!"msecs"); // allowlisted unittest backoff: fake-release spin
        return "SKIPREL record body";
    }

    /// Fake: spins until the stale-deadline test releases the flag, then returns a record body; the block re-holds
    /// it for a second record so the first drain's stale ticket fires while a newer drain is pending (seq guard).
    private string riRelStaleFake(string prompt, string traceText) {
        import core.thread : Thread;

        while (!atomicLoad(riRelStaleFlag))
            Thread.sleep(5.dur!"msecs"); // allowlisted unittest backoff: fake-release spin
        return "STALEREL record body";
    }
    // Kill-path / join-semantics fakes: each block owns its flags (the blocks run in parallel
    // under the test runner, so no flag is shared between blocks). The in-flight markers let a
    // bounded pre-shutdown poll wait for the fake to provably start, so the join under test
    // cannot race the spawn and return fast.

    private shared bool riKillInFlight;
    private shared bool riKillRelease;
    private shared bool riJoinInFlight;
    // Unix epoch anchor: exact ms derivation from SysTime differences (a shared long stamp cannot carry a
    // SysTime struct, and toUnixTime() is whole-second only). Start stamps are epoch ms at fake entry;
    // they let the join blocks compute the fake's REMAINING window
    // at poll time -- starvation-proof lower bounds: a starved test thread sees a smaller window.
    private immutable SysTime riEpoch = SysTime(DateTime(1970, 1, 1), UTC());
    private shared long riKillStart;
    private shared long riJoinStart;

    /// Fake: holds the RiJob's summarizer until released or a 400 ms self-timeout expires. The
    /// self-timeout bounds the kill-path shutdown under test: a release-less sys.shutdown() must
    /// join a stuck-but-bounded executor, never hang.
    private string riKillFake(string prompt, string traceText) {
        import core.thread : Thread;
        import std.datetime : Clock;

        atomicStore(riKillStart, (Clock.currTime - riEpoch).total!"msecs");
        auto deadline = Clock.currTime + 400.dur!"msecs";
        atomicStore(riKillInFlight, true);
        while (!atomicLoad(riKillRelease) && Clock.currTime < deadline)
            Thread.sleep(5.dur!"msecs"); // allowlisted unittest backoff: fake-release spin
        return "KILLREL record body";
    }

    /// Fake: fixed 500 ms sleep; the join-semantics block measures sys.shutdown() against it.
    private string riJoinFake(string prompt, string traceText) {
        import core.thread : Thread;
        import std.datetime : Clock;

        atomicStore(riJoinStart, (Clock.currTime - riEpoch).total!"msecs");
        atomicStore(riJoinInFlight, true);
        Thread.sleep(500.dur!"msecs");
        return "JOINSEM record body";
    }

    // Embedder-serialization counters (the two-worker block). D module-scope vars are one shared
    // process-wide instance; `shared` + the atomic ops keep the cross-thread access race-free
    // across the parallel unittest runner's threads.
    private shared long serEmbInstances;
    private shared long serEmbCalls;
    private shared long serEmbViolations;

    /// Serialization-sensing embedder: the mutex-guarded in-call flag must never be observed true
    /// on entry (per-worker concurrency 1, the actor serialization invariant); every call sleeps
    /// long enough for a concurrent call to observe it.
    private class SerEmbedder : Embedder {
        import core.sync.mutex : Mutex;

        private Mutex mtx;
        private bool inCall_;

        this() {
            mtx = new Mutex;
            serEmbInstances.atomicOp!"+="(1);
        }

        override void destroy() {
        }

        override string modelName() {
            return "ser";
        }

        override long dimensions() {
            return 8;
        }

        override bool supportsTokenization() {
            return false;
        }

        override EmbedResult embedQuery(string text) {
            return embed(text);
        }

        override EmbedResult embedDocument(string text) {
            return embed(text);
        }

        EmbedResult embed(string text) {
            import core.thread : Thread;

            mtx.lock_nothrow();
            if (inCall_)
                serEmbViolations.atomicOp!"+="(1);
            inCall_ = true;
            mtx.unlock_nothrow();
            scope (exit) {
                mtx.lock_nothrow();
                inCall_ = false;
                mtx.unlock_nothrow();
            }
            serEmbCalls.atomicOp!"+="(1);
            Thread.sleep(30.dur!"msecs"); // overlap window for a (forbidden) concurrent call
            auto v = new float[8];
            foreach (ref f; v)
                f = 1.0f;
            return EmbedResult(v);
        }

        override EmbedResult embedQuery(int[] tokens) {
            return embed(tokens);
        }

        override EmbedResult embedDocument(int[] tokens) {
            return embed(tokens);
        }

        EmbedResult embed(int[] tokens) {
            return EmbedResult(EmbedError("no tokens"));
        }

        override int[] tokenize(string text, bool addSpecial) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            return 16;
        }
    }

    private Embedder serEmbedderFactory(EmbedConfig config) {
        return new SerEmbedder();
    }

    /// Fast fake for the channel-surface round trip (no sleep: the drain settles in mailbox time).
    private string riChanFake(string prompt, string traceText) {
        return "CHANREC body";
    }
}

/// A non-thread-safe embedder that mimics LlamaEmbedder: it writes the input into a per-instance pre-allocated scratch buffer, sleeps to force concurrent overlap, then reads it back. A SINGLE instance shared across threads would corrupt here; the per-thread instances produced by the worker pattern are isolated.
unittest {
    import std.format : format;

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "nts", dimensions: 16));

    immutable N = 4;
    auto texts = new string[N];
    foreach (i; 0 .. N) {
        string t;
        foreach (j; 0 .. 16)
            t ~= cast(char)('0' + ((i * 7 + j) % 10));
        texts[i] = t;
    }

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;

    // Each worker embeds ITS OWN text with ITS OWN embedder and self-verifies the result against the input. A shared non-thread-safe instance would corrupt its scratch buffer under concurrent overlap and fail the check. The float[] result is checked inside the worker and only a bool verdict is carried back. Detached placement: isolation is the point of the test, pool occupancy must not be a factor.
    foreach (i; 0 .. N) {
        auto a = sys.spawn!(Config.detached, NtsIsolationActor)(i, texts[i],
                cfg, &ntsEmbedderFactory, sup.address());
        dynSend(a, "go");
    }

    int done;
    bool allOk = true;
    foreach (i; 0 .. N)
        assert(sup.receiveTimeout(30.dur!"seconds", (NtsDone d) {
                if (!d.ok)
                    allOk = false;
                ++done;
            }), "a worker did not report its isolation verdict");
    assert(done == N, "expected %s workers to report, got %s".format(N, done));
    assert(allOk, "an embedder instance was not isolated (cross-thread corruption detected)");
}

/// Worker integration: spawn the actor, feed it one DiJob, drain it (the mailbox is FIFO so the drain proves the job was processed), and verify the per-session DB was created and contains indexed chunks.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse, tempDir;
    import std.path : buildPath;
    import std.conv : to;
    import std.format : format;

    auto tmpDir = testArea("worker_integration");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init, "", SummarizerFn(null)), null);

    // One episode. topicName is opaque to the worker (it never parses topic names - the codec round-trip is exercised by dialogue_index tests), but the session id must pass the worker's own validation.
    string sid = "20240101-120000-abcd";
    string topicName = "d_20240101_120000_abcd__t11_11__1000";
    string episodeText;
    foreach (i; 0 .. 12)
        episodeText ~= "turn " ~ i.to!string ~ " content line\n";

    ch.diJob(DiJob(sid, [DiEpisode(topicName.idup, episodeText.idup, 11)]));
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "worker did not drain its mailbox");
    gate.close();

    // Verify the per-session DB has indexed chunks.
    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "per-session DB was not created");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    long countChunks(ref Database d) {
        static immutable sql = "SELECT count(*) FROM TextChunkTbl;";
        auto stmt = d.prepare(sql);
        foreach (ref r; stmt.get.execute)
            return r.peek!long(0);
        return -1;
    }

    auto rowCount = countChunks(db);
    assert(rowCount >= 2, "expected >= 2 indexed chunks, got %s".format(rowCount));

    // FTS5 must be rebuilt by the worker so text search sees the new chunks.
    auto hits = db.queryTextSearch("content", 10);
    assert(hits !is null && hits.length >= 1,
            "expected FTS text search hits after indexing, got none");
    assert(hits[0].text.length > 0, "hit must carry the verbatim chunk text");
}

/// Identical episode text under two different topics must both be indexed. The topic name is the dedup salt, so the two salted identities differ; with content-only dedup the second episode would be dropped as a duplicate and only one source would exist.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.conv : to;
    import std.format : format;

    auto tmpDir = testArea("dialogue_dedup");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init, "", SummarizerFn(null)), null);

    // One job, two episodes: identical text, different topics (turns 11 and 12).
    string sid = "20240101-120000-abcd";
    string episodeText;
    foreach (i; 0 .. 12)
        episodeText ~= "turn " ~ i.to!string ~ " content line\n";
    ch.diJob(DiJob(sid, [
        DiEpisode("d_20240101_120000_abcd__t11_11__1000", episodeText.idup, 11),
        DiEpisode("d_20240101_120000_abcd__t12_12__1000", episodeText.idup, 12),
    ]));
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "worker did not drain its mailbox");
    gate.close();

    // Both topics must be indexed as distinct sources (content-only dedup would leave exactly one).
    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "per-session DB was not created");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;
    assert(db.getSources().length == 2,
            "identical text under two topics must index two sources, got %s".format(
                db.getSources().length));
}

/// A turn split across two compressions re-arrives under the same topic. The worker merges the existing episode text with the new piece (instead of replacing it): after both jobs, one source holds "old\nnew" verbatim and both pieces are text-searchable.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.format : format;

    auto tmpDir = testArea("dialogue_splitmerge");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init, "", SummarizerFn(null)), null);

    // Two jobs, same topic (a turn split across two compression checkpoints).
    string sid = "20240101-120000-abcd";
    string topicName = "d_20240101_120000_abcd__t11_11__1000";
    ch.diJob(DiJob(sid, [
        DiEpisode(topicName.idup, "SPLITUSER exact question alpha".idup, 11)
    ]));
    ch.diJob(DiJob(sid, [
        DiEpisode(topicName.idup, "SPLITASST answer beta".idup, 11)
    ]));
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "worker did not drain its mailbox");
    gate.close();

    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "per-session DB was not created");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    // Exactly one source: the merge replaced the first piece's source, it did not add a second one.
    assert(db.getSources().length == 1,
            "split-turn merge must keep exactly one source, got %s".format(db.getSources().length));

    auto opt = db.getSource(Origin(Topic(topicName)));
    assert(hasValue(opt), "merged source must be findable by topic");
    auto id = opt.match!(a => a.id, (None _) => SourceId.init);
    auto text = db.sourceText(id);
    assert(text == "SPLITUSER exact question alpha\nSPLITASST answer beta",
            "merged text must be old\nnew verbatim, got '%s'".format(text));

    // Both pieces must be text-searchable (the FTS index covers the merge).
    auto h1 = db.queryTextSearch("SPLITUSER", 10);
    assert(h1 !is null && h1.length >= 1, "old piece not text-searchable after the merge");
    auto h2 = db.queryTextSearch("SPLITASST", 10);
    assert(h2 !is null && h2.length >= 1, "new piece not text-searchable after the merge");
}

/// Database.sourceText reconstructs a multi-chunk source exactly - the leading-overlap stripping removes the sliding-window overlap without duplicating or dropping any text.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.conv : to;
    import std.format : format;

    auto tmpDir = testArea("dialogue_multichunk");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init, "", SummarizerFn(null)), null);

    string sid = "20240101-120000-abcd";
    string topicName = "d_20240101_120000_abcd__t11_11__1000";
    string episodeText;
    foreach (i; 0 .. 12)
        episodeText ~= "turn " ~ i.to!string ~ " content line\n";
    ch.diJob(DiJob(sid, [DiEpisode(topicName.idup, episodeText.idup, 11)]));
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "worker did not drain its mailbox");
    gate.close();

    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "per-session DB was not created");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    // Precondition: the episode produced >= 2 chunks (overlapping windows).
    long rowCount = -1;
    {
        auto stmt = db.prepare("SELECT count(*) FROM TextChunkTbl;");
        foreach (ref r; stmt.get.execute)
            rowCount = r.peek!long(0);
    }
    assert(rowCount >= 2, "expected >= 2 chunks for the round-trip test, got %s".format(rowCount));

    auto opt = db.getSource(Origin(Topic(topicName)));
    assert(hasValue(opt), "source must be findable by topic");
    auto id = opt.match!(a => a.id, (None _) => SourceId.init);
    auto text = db.sourceText(id);
    assert(text == episodeText,
            "sourceText must round-trip the original text exactly (no duplicated overlap, no lost text)");
}

/// A pre-existing source with no chunks (empty reconstruction) must not poison the merge - the plain piece is indexed, replacing the empty source (one source, no throw out of the job).
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.format : format;

    auto tmpDir = testArea("dialogue_emptysrc");
    scope (exit)
        tmpDir.cleanup();

    string sid = "20240101-120000-abcd";
    string topicName = "d_20240101_120000_abcd__t11_11__1000";

    // Seed the session DB with an empty source (no chunks) for the topic.
    auto seedOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: false);
    assert(seedOpt.hasValue, "seed DB must open");
    auto seed = seedOpt.match!((Database d) => d, (None _) => Database.init);
    seed.addSource(Source(origin: Origin(Topic(topicName)), checksum: 42.SourceChecksum,
            added: SysTime.init));
    seed.destroy;

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init, "", SummarizerFn(null)), null);

    string piece = "EMPTYFALLBACK plain piece after an empty source";
    ch.diJob(DiJob(sid, [DiEpisode(topicName.idup, piece.idup, 11)]));
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "worker did not drain its mailbox (read failure must not throw)");
    gate.close();

    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "per-session DB was not created");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    assert(db.getSources().length == 1,
            "the empty source must be replaced, not duplicated (got %s)".format(
                db.getSources().length));
    auto h = db.queryTextSearch("EMPTYFALLBACK", 10);
    assert(h !is null && h.length >= 1, "the plain piece was not indexed");
}

/// Two back-to-back jobs for one session (the second arriving well inside the old 1 s coalescing window) must BOTH be text-searchable after the drain - every job that committed chunks rebuilds the FTS index itself, and the drain no longer flushes any deferred rebuilds.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.format : format;

    auto tmpDir = testArea("dialogue_ftsperjob");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init, "", SummarizerFn(null)), null);

    // Two jobs back-to-back (different topics, unique tokens); the second lands far inside the old 1 s coalescing window, so only a per-job rebuild can make its token searchable before the drain's checkpoint+close.
    string sid = "20240101-120000-abcd";
    ch.diJob(DiJob(sid, [
        DiEpisode("d_20240101_120000_abcd__t11_11__1000".idup,
                "FTSJOBONE first job token here".idup, 11)
    ]));
    ch.diJob(DiJob(sid, [
        DiEpisode("d_20240101_120000_abcd__t12_12__1000".idup,
                "FTSJOBTWO second job token here".idup, 12)
    ]));
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "worker did not drain its mailbox");
    gate.close();

    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "per-session DB was not created");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    auto h1 = db.queryTextSearch("FTSJOBONE", 10);
    assert(h1 !is null && h1.length >= 1, "job 1 token not text-searchable");
    auto h2 = db.queryTextSearch("FTSJOBTWO", 10);
    assert(h2 !is null && h2.length >= 1,
            "job 2 token not text-searchable (per-job rebuild missing)");
}

/// Result carried back from the concurrent read-only reader (WAL test) to the test's scoped receiver. A Database cannot cross the actor boundary, so the reader reports a verdict: the committed chunk count it observed and any lock/validity errors.
private struct WkRdResult {
    long count; // committed chunk count observed (-1 = never read a valid count)
    int errors; // lock/validity errors hit while polling
    bool ready; // true = handshake (read-only connection open), false = final verdict
}

/// Concurrent read-only reader (WAL test) as an actor. Opens the session DB file READ-ONLY while the worker holds its WAL write connection, and polls (50 ms backoff) the committed chunk count until it observes the worker's second commit (count >= 4) or times out. It sends a `ready` handshake once its connection is open (so the test thread commits the second job while the reader is definitely open -- a true concurrent writer+reader overlap) and a final `ready=false` verdict at the end. Runs on its own detached thread; the constructor stores only, `run` does the blocking work and reports the verdicts to `outAddr`.
private final class ReaderProbeActor {
    private {
        string dbPath_;
        WeakAddress out_;
    }

    this(string dbPath, WeakAddress outAddr) @safe nothrow {
        dbPath_ = dbPath;
        out_ = outAddr;
    }

    void run() {
        import core.thread : Thread;
        import std.string : indexOf;

        // Step 1: open the DB read-only. openDatabase(readOnly) returns None immediately when the file does not exist yet (no 5 s retry), so poll until the worker has created the file + schema.
        Database db = Database.init;
        bool opened = false;
        foreach (i; 0 .. 300) {
            auto dbOpt = openDatabase(dbPath_.AbsolutePath, "wk", 8, readOnly: true);
            if (dbOpt.hasValue) {
                db = dbOpt.match!((Database d) => d, (None _) => Database.init);
                opened = true;
                break;
            }
            Thread.sleep(50.dur!"msecs"); // poll backoff
        }

        // Handshake: the read-only connection is open (or we give up below). Let the test thread commit the second job while we hold the connection open, so the reader and writer genuinely coexist.
        dynSend(out_, "wkRd", WkRdResult(-1, 0, true));

        if (!opened) {
            dynSend(out_, "wkRd", WkRdResult(-1, 1, false)); // never managed to open
            return;
        }
        scope (exit)
            db.destroy;

        // Step 2: poll the committed chunk count until it reaches >= 4 (both jobs' chunks are visible through this read-only connection) or time out.
        long maxCount = -1;
        int errors = 0;
        bool everValid = false;
        foreach (i; 0 .. 300) {
            long n = -1;
            try {
                auto stmt = db.prepare("SELECT count(*) FROM TextChunkTbl;");
                foreach (ref r; stmt.get.execute)
                    n = r.peek!long(0);
            } catch (Exception e) {
                // A "database is locked" here is the violation under test; any other error (e.g. the table is not committed yet) is transient -- keep polling.
                string m = (e.msg is null) ? "" : e.msg;
                if (m.indexOf("locked") >= 0)
                    errors++;
            }
            if (n >= 0) {
                everValid = true;
                if (n > maxCount)
                    maxCount = n;
                if (maxCount >= 4)
                    break;
            }
            Thread.sleep(50.dur!"msecs"); // poll backoff
        }
        if (!everValid)
            errors += 2; // never saw a valid committed count through the reader
        dynSend(out_, "wkRd", WkRdResult(maxCount, errors, false)); // final verdict
    }
}

// Embedder factory for the degraded-worker test: always throws, so it throws
// when injected and the worker enters the degraded path (embedder is null).
// Module-scope so its address can be passed to the worker actor as the DI seam
// (a plain function pointer).
private Embedder throwingEmbedderFactory(EmbedConfig config) {
    throw new Exception("simulated embedder creation failure (degraded-worker test)");
}

// WAL lets the worker (single writer) and a concurrent read-only reader
// coexist on the same session DB file without "database is locked". The reader
// opens the file read-only while the worker holds its WAL write connection,
// must observe the worker's first commit, and must then see the worker's
// second commit land (polling for visibility) without a lock error. This
// follows the production pattern: the worker drains exactly once, at dispose,
// AFTER all its DiJobs -- it does not checkpoint mid-stream.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.conv : to;
    import std.format : format;

    auto tmpDir = testArea("dialogue_rw");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init, "", SummarizerFn(null)), null);

    string sid = "20240101-120000-abcd";
    string topicName = "d_20240101_120000_abcd__t11_11__1000";
    string episodeText;
    foreach (i; 0 .. 12)
        episodeText ~= "turn " ~ i.to!string ~ " content line\n";

    // Writer: first job (fire-and-forget, like production onCheckpoint). This opens the worker's WAL connection and creates the schema + first commit.
    ch.diJob(DiJob(sid, [DiEpisode(topicName.idup, episodeText.idup, 11)]));

    // Concurrent read-only reader on the same file (detached: it polls with sleeps and must not occupy the pool).
    string dbPath = (tmpDir ~ (sid ~ ".db")).idup;
    auto rp = sys.spawn!(Config.detached, ReaderProbeActor)(dbPath, sup.address());
    dynSend(rp, "run");

    // Wait for the reader's handshake: its read-only connection is now open.
    WkRdResult readyMsg;
    assert(sup.receiveTimeout(30.dur!"seconds", (WkRdResult r) { readyMsg = r; }),
            "reader did not report its read-only connection is open");
    assert(readyMsg.ready, "first reader message must be the ready handshake");

    // Writer: second job committed WHILE the reader connection is open. With WAL the reader keeps running and will see this commit; without WAL it would hit "database is locked" (caught and reported by the reader).
    string episodeText2;
    foreach (i; 0 .. 12)
        episodeText2 ~= "turn " ~ i.to!string ~ " content line two\n";
    ch.diJob(DiJob(sid, [DiEpisode(topicName.idup, episodeText2.idup, 12)]));

    // Wait for the reader's final verdict (it polled until it saw both commits).
    WkRdResult finMsg;
    assert(sup.receiveTimeout(30.dur!"seconds", (WkRdResult r) { finMsg = r; }),
            "reader final verdict not received");
    assert(!finMsg.ready, "second reader message must be the final verdict");
    assert(finMsg.errors == 0,
            "read-only reader hit a lock/validity error while the writer was committing: %s".format(
                finMsg.errors));
    assert(finMsg.count >= 4,
            "reader did not observe both commits (expected >= 4 chunks, saw %s)".format(
                finMsg.count));

    // Single final drain (production dispose pattern): flush dirty FTS and close the write connection. The reader is closed now, so the checkpoint is clean.
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "worker did not drain its mailbox");
    gate.close();

    // Independent verification: a fresh read-only connection sees all committed chunks and the file is in WAL mode.
    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "per-session DB missing");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;
    long total = -1;
    {
        auto stmt = db.prepare("SELECT count(*) FROM TextChunkTbl;");
        foreach (ref r; stmt.get.execute)
            total = r.peek!long(0);
    }
    assert(total >= 4, "fresh connection sees < 4 committed chunks: %s".format(total));
    string journal = "";
    {
        auto stmt = db.prepare("PRAGMA journal_mode;");
        foreach (ref r; stmt.get.execute)
            journal = r.peek!string(0);
    }
    assert(journal == "wal", "expected wal journal mode, got '%s'".format(journal));
}

/// Resilience (writer side): a pre-corrupted session DB must not crash the worker or starve other sessions. The corrupted session's job is skipped (openDatabase keeps failing and returns none), but a second session's job in the same mailbox is still indexed, and the worker still answers the drain (DiDrained).
unittest {
    import std.file : mkdirRecurse, rmdirRecurse, write;
    import std.conv : to;
    import std.format : format;

    auto tmpDir = testArea("dialogue_corrupt");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    // Pre-corrupt session A's DB file with junk bytes so openDatabase fails.
    string sidA = "20240101-120000-abcd";
    string sidB = "20240101-120001-ef01";
    ubyte[] junk;
    foreach (i; 0 .. 128)
        junk ~= cast(ubyte)(i + 1);
    write((tmpDir ~ (sidA ~ ".db")).AbsolutePath, junk);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init, "", SummarizerFn(null)), null);

    // Job for the corrupted session A (skipped), then a valid session B.
    string epA;
    foreach (i; 0 .. 12)
        epA ~= "turn " ~ i.to!string ~ " content line\n";
    ch.diJob(DiJob(sidA, [
        DiEpisode("d_20240101_120000_abcd__t11_11__1000", epA.idup, 11)
    ]));
    string epB;
    foreach (i; 0 .. 12)
        epB ~= "session B turn " ~ i.to!string ~ " line\n";
    ch.diJob(DiJob(sidB, [
        DiEpisode("d_20240101_120001_ef01__t11_11__1000", epB.idup, 11)
    ]));
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    assert(sup.receiveTimeout(60.dur!"seconds", (DiDrained _) {}),
            "worker did not drain after a corrupted session DB (crashed or hung)");
    gate.close();

    // Session B's DB must have indexed chunks (other session's state is intact).
    auto dbOpt = openDatabase((tmpDir ~ (sidB ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "session B DB missing (worker aborted after the corrupted session)");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;
    long rowCount = -1;
    {
        auto stmt = db.prepare("SELECT count(*) FROM TextChunkTbl;");
        foreach (ref r; stmt.get.execute)
            rowCount = r.peek!long(0);
    }
    assert(rowCount >= 2, "session B should have >= 2 indexed chunks, got %s".format(rowCount));
}

/// Resilience (degraded worker): when the injected embedder factory throws, the worker degrades gracefully -- it sends the degraded notice exactly once (from onSpawn, before any mailbox processing), skips every DiJob, and still answers diDrain so disposal never hangs.
unittest {
    import std.conv : to;
    import std.format : format;

    auto tmpDir = testArea("dialogue_degraded");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, sup.address(),
            tmpDir.workArea, cfg, ragCfg, &throwingEmbedderFactory,
            SummaryModelConfig.init, "", SummarizerFn(null)), null);

    // Two DiJobs (both must be skipped) and a diDrain (must be answered).
    string sid = "20240101-120000-abcd";
    string ep;
    foreach (i; 0 .. 12)
        ep ~= "turn " ~ i.to!string ~ " content line\n";
    ch.diJob(DiJob(sid, [
        DiEpisode("d_20240101_120000_abcd__t11_11__1000", ep.idup, 11)
    ]));
    ch.diJob(DiJob(sid, [
        DiEpisode("d_20240101_120000_abcd__t12_12__1000", ep.idup, 12)
    ]));
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));

    // The degraded notice is sent from onSpawn, before the first mailbox message is processed, so it always precedes the drain answer in the supervisor's mailbox (exactly once: one onSpawn per spawn).
    string reason;
    assert(sup.receiveTimeout(30.dur!"seconds", (string s) { reason = s; }),
            "degraded worker did not report its degraded state");
    assert(reason == "no embedder available", "unexpected degraded reason: %s".format(reason));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "degraded worker did not answer diDrain (disposal would hang)");
    // A duplicate notice would have been posted before the drain answer (same sender, FIFO) and therefore already sit in the supervisor's mailbox: the negative receive below enforces the exactly-once property (one onSpawn per spawn).
    assert(!sup.receiveTimeout(100.dur!"msecs", (string _) {}),
            "degraded notice must be sent exactly once");
    gate.close();
}

/// Resilience (per-episode): a poisoned episode (the embedder THROWS for that text) must not abort the rest of the job. The bad episode is skipped (warningf + ++failures) and the other episodes in the SAME job are still indexed.
unittest {
    auto tmpDir = testArea("dialogue_poison");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea, cfg,
            ragCfg, &poisonedEmbedderFactory, SummaryModelConfig.init, "", SummarizerFn(null)),
            null);

    string sid = "20240101-120000-abcd";
    // Three episodes in one job: the first is poisoned (the embedder throws for it); the other two are normal and must still be indexed.
    string poison = "POISON marker this episode must fail to index";
    string good1 = "alpha episode one normal content here";
    string good2 = "beta episode two normal content here";
    ch.diJob(DiJob(sid, [
        DiEpisode("d_20240101_120000_abcd__t1_1__1000", poison.idup, 1),
        DiEpisode("d_20240101_120000_abcd__t2_2__1000", good1.idup, 2),
        DiEpisode("d_20240101_120000_abcd__t3_3__1000", good2.idup, 3),
    ]));
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "worker did not drain after a poisoned episode (crashed)");
    gate.close();

    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "session DB was not created");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    // The normal episodes must be indexed (the bad episode did not abort them).
    auto h1 = db.queryTextSearch("alpha", 10);
    assert(h1 !is null && h1.length >= 1,
            "good episode 1 was not indexed (the bad episode aborted the job)");
    auto h2 = db.queryTextSearch("beta", 10);
    assert(h2 !is null && h2.length >= 1,
            "good episode 2 was not indexed (the bad episode aborted the job)");

    // The poisoned episode must NOT be indexed (its embedding threw).
    auto hp = db.queryTextSearch("POISON", 10);
    assert(hp is null || hp.length == 0,
            "poisoned episode was indexed despite the embedder throwing");
}

/// One completed DiJob must produce exactly ONE worker trace line carrying all observability fields (session id, episode/chunk/failure counts, turn range min turnStart - max turnEnd), with no per-chunk spam. Captures the line via the std.logger sharedLog seam (restored on exit, with the global log level raised to trace for the duration only). The capture is process-global and parallel worker tests emit their own trace lines, so this test uses a unique per-process session id and anchors the capture filter on its quoted form.
unittest {
    import std.algorithm : canFind;
    import std.string : startsWith;
    import std.conv : to;
    import std.format : format;

    auto tmpDir = testArea("dialogue_d7");
    scope (exit)
        tmpDir.cleanup();
    synchronized (d7SharedLogMutex) {

        // Capture all log output (the worker actor's included) and enable trace level for the trace line; both are restored on exit.
        auto prevLog = logger.sharedLog;
        auto prevLevel = logger.globalLogLevel;
        auto cap = cast(shared) new D7LogCapture();
        logger.sharedLog = cap;
        logger.globalLogLevel = logger.LogLevel.trace;
        scope (exit) {
            logger.globalLogLevel = prevLevel;
            logger.sharedLog = prevLog;
        }

        auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
                modelName: "wk", dimensions: 8));
        auto ragCfg = RagConfig(windowOverlapPercent: 10);

        auto sys = makeSystem;
        scope (exit)
            sys.shutdown();
        auto sup = scopedActor;
        auto gate = new CompletionGate;

        auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys,
                WeakAddress.init, tmpDir.workArea, cfg,
                ragCfg, &wkEmbedderFactory, SummaryModelConfig.init, "", SummarizerFn(null)), null);

        // One job with two single-turn episodes (turns 3 and 7): the trace line must report the range 3-7 (min turnStart - max turnEnd over the job). Unique per-process session id (valid IdPattern form: YYYYMMDD-HHMMSS-4hex): the capture filter anchors on its quoted form, which no parallel test's line can match. A module-scope counter plus the wall clock make collisions with the fixed sids other tests use (~"abcd") negligible.
        static uint d7Seq;
        long nowMillis = Clock.currTime.toUnixTime * 1000;
        uint salt = cast(uint)(nowMillis) * 2654435761u + ++d7Seq;
        string sid = format("20240101-120000-%04x", salt & 0xFFFFu);
        string hex = sid[16 .. $];
        string ep3;
        foreach (i; 0 .. 12)
            ep3 ~= "episode three turn " ~ i.to!string ~ " line\n";
        string ep7;
        foreach (i; 0 .. 12)
            ep7 ~= "episode seven turn " ~ i.to!string ~ " line\n";
        ch.diJob(DiJob(sid, [
            DiEpisode("d_20240101_120000_" ~ hex ~ "__t3_3__1000", ep3.idup, 3),
            DiEpisode("d_20240101_120000_" ~ hex ~ "__t7_7__1000", ep7.idup, 7),
        ]));
        ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
        assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
                "worker did not drain its mailbox");
        gate.close();

        // The drain proves the job completed, so the trace line is already captured. Anchor on this test's quoted session id: parallel worker tests share this global capture and log their own trace lines (their own sids).
        string[] d7;
        foreach (l; (cast() cap).takeLines())
            if (l.startsWith("dialogue worker: session '" ~ sid ~ "'"))
                d7 ~= l;
        assert(d7.length == 1,
                "expected exactly one trace line per completed job, got %s: %s".format(d7.length,
                    d7));
        string line = d7[0];
        assert(line.canFind(sid), "trace line missing session id: " ~ line);
        assert(line.canFind("2 episodes"), "trace line missing episode count: " ~ line);
        assert(line.canFind("chunks,"), "trace line missing chunk count: " ~ line);
        assert(line.canFind("0 failures"), "trace line missing failure count: " ~ line);
        assert(line.canFind("turns 3-7"), "trace line missing the 3-7 turn range: " ~ line);

        // No per-chunk spam: several chunks were indexed (verified below) yet exactly one trace line was emitted.
        auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
        assert(dbOpt.hasValue, "session DB missing");
        auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
        scope (exit)
            db.destroy;
        long rowCount = -1;
        {
            auto stmt = db.prepare("SELECT count(*) FROM TextChunkTbl;");
            foreach (ref r; stmt.get.execute)
                rowCount = r.peek!long(0);
        }
        assert(rowCount >= 2, "expected >= 2 indexed chunks, got %s".format(rowCount));
    }
}

/// Fixed-text fake summarizer: an RiJob must produce a retrievable r_
//reasoning record in the session DB: the fence-stripped text indexed verbatim
//as a single chunk, FTS-searchable, and the topic decoding to the job's
//session/turn range with Kind.reasoning.
unittest {
    import std.format : format;

    auto tmpDir = testArea("dialogue_p2_fixed");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init,
            "fake prompt", &riFixedFake), null);
    string sid = "20240101-120000-ab01";
    ch.riJob(RiJob(sid, "trace text alpha beta for the fixed fake".idup, 4, 8));
    // early finalize: the detached summarizer's riRecord can only be enqueued
    // after riJob was processed -- in practice it lands behind diDrain, so the
    // drain takes the pending path and answers when the record lands, before
    // the default budget elapses (a pre-empting completion would hit the
    // immediate-finalize path instead; same observable behavior: the record is
    // indexed before the checkpoint+close either way).
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "worker did not answer diDrain after RiJob");
    gate.close();

    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "session DB missing after drain");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    // Exactly one chunk (the record is shorter than the 16-char window), and exactly the fence-stripped record text, verbatim.
    string chunkTexts;
    int chunkCount = 0;
    {
        auto stmt = db.prepare("SELECT text FROM TextChunkTbl;");
        foreach (ref r; stmt.get.execute) {
            chunkCount++;
            chunkTexts ~= r.peek!string(0) ~ "\n";
        }
    }
    assert(chunkCount == 1, "expected exactly 1 chunk for the record, got %s".format(chunkCount));
    assert(chunkTexts == "RI_FIXED_ALPHA\n",
            "record must be indexed fence-stripped, verbatim; got: %s".format(chunkTexts));

    // FTS surfaces it under an r_ topic that decodes to the RiJob metadata.
    auto hits = db.queryTextSearch("RI_FIXED_ALPHA", 10);
    assert(hits.length >= 1, "record must be FTS-searchable");
    string topicName = hits[0].origin.match!((Topic t) => t.name, (_) => "");
    auto metaOpt = decodeTopicName(topicName);
    assert(metaOpt.hasValue, "r_ topic must decode, got: " ~ topicName);
    auto meta = metaOpt.match!((EpisodeMeta m) => m, (None _) => EpisodeMeta());
    assert(meta.kind == Kind.reasoning, "kind must be reasoning, got %s".format(meta.kind));
    assert(meta.sessionId == sid, "sessionId mismatch: " ~ meta.sessionId);
    assert(meta.turnStart == 4 && meta.turnEnd == 8,
            "turn range mismatch: %s-%s".format(meta.turnStart, meta.turnEnd));
}

/// Slow fake summarizer: the LLM call runs OFF the mailbox (dedicated timeout): a DiJob queued behind an in-flight RiJob must complete (its chunk FTS-visible) well before the fake's 3 s sleep ends. A mailbox-blocked design would delay it by the full sleep.
unittest {
    import core.thread : Thread;
    import std.conv : to;
    import std.format : format;

    auto tmpDir = testArea("dialogue_p2_slow");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init,
            "fake prompt", &riSlowFake), null);
    string sid = "20240101-120000-ab02";
    string ep;
    foreach (i; 0 .. 12)
        ep ~= "fastdi turn " ~ i.to!string ~ " line\n";

    auto t0 = Clock.currTime;
    ch.riJob(RiJob(sid, "trace text for the slow fake".idup, 1, 2));
    ch.diJob(DiJob(sid, [
        DiEpisode("d_20240101_120000_ab02__t1_1__1000", ep.idup, 1)
    ]));

    // Poll read-only for the DiJob's chunk (documented unittest backoff spin,
    // 50 ms). The RiJob's ensureDb creates the DB file before the summarizer
    // thread spawns, so it appears quickly; openDatabase(readOnly) returns
    // None until then (no blocking retry).
    bool diVisible = false;
    auto deadline = t0 + 5.dur!"seconds";
    while (Clock.currTime < deadline) {
        auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
        if (dbOpt.hasValue) {
            auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
            scope (exit)
                db.destroy;
            if (db.queryTextSearch("FASTDI", 10).length >= 1) {
                diVisible = true;
                break;
            }
        }
        Thread.sleep(50.dur!"msecs");
    }
    auto elapsed = Clock.currTime - t0;
    assert(diVisible, "DiJob chunk never became FTS-visible within 5 s");
    assert(elapsed < 1000.dur!"msecs",
            "DiJob was delayed by the in-flight RiJob (mailbox blocked?): %s".format(elapsed));

    // drain-while-pending / early finalize: the 3 s fake is still in flight,
    // so the drain registers the waiter and arms the default budget; the reply
    // arrives when the record lands (job count -> 0), with the record indexed
    // before the checkpoint+close.
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}), "worker did not answer diDrain");
    gate.close();

    auto dbOpt2 = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt2.hasValue, "session DB missing after drain");
    auto db2 = dbOpt2.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db2.destroy;
    assert(db2.queryTextSearch("SLOWRI", 10).length >= 1,
            "the in-flight RiJob record must be indexed by the drain join");
}

/// Throwing fake summarizer: the LLM failure takes the RiDone path: no record indexed, and the DiJob queued in the same mailbox is unaffected.
unittest {
    import std.string : startsWith;
    import std.conv : to;
    import std.format : format;

    auto tmpDir = testArea("dialogue_p2_throw");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init,
            "fake prompt", &riThrowingFake), null);
    string sid = "20240101-120000-ab03";
    string ep;
    foreach (i; 0 .. 12)
        ep ~= "gooddi turn " ~ i.to!string ~ " line\n";

    ch.riJob(RiJob(sid, "trace text for the throwing fake".idup, 3, 5));
    ch.diJob(DiJob(sid, [
        DiEpisode("d_20240101_120000_ab03__t3_3__1000", ep.idup, 3)
    ]));
    // early finalize: the fake fails immediately, so the completion is a
    // RiDone (no record); in practice it lands behind diDrain, so when it
    // arrives the job count reaches zero and the drain finalizes then --
    // before the default budget elapses (a pre-empting completion would hit
    // the immediate-finalize path; same observable behavior).
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}), "worker did not answer diDrain");
    gate.close();

    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "session DB missing after drain");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;

    // The DiJob indexed normally (one d_ source); the failed RiJob left no r_ source behind.
    int dSources = 0;
    int rSources = 0;
    foreach (src; db.getSources)
        src.origin.match!((Topic t) {
            if (t.name.startsWith("d_"))
                dSources++;
            else if (t.name.startsWith("r_"))
                rSources++;
        }, (_) {});
    assert(dSources == 1, "the DiJob episode must be indexed, got %s d_ source(s)".format(dSources));
    assert(rSources == 0,
            "a throwing summarizer must not index a record, got %s r_ source(s)".format(rSources));
    assert(db.queryTextSearch("GOODDI", 10).length >= 1, "the DiJob chunk must be FTS-searchable");
}

/// Degraded worker: an RiJob must be skipped WITHOUT spawning a summarizer thread (no LLM spend while degraded): the counting fake must never be invoked.
unittest {
    import std.format : format;

    auto tmpDir = testArea("dialogue_p2_degraded");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    riCountFakeInvoked = false;
    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, sup.address(), tmpDir.workArea, cfg,
            ragCfg, &throwingEmbedderFactory, SummaryModelConfig.init,
            "fake prompt", &riCountFake), null);
    string sid = "20240101-120000-ab04";

    // The degraded notice doubles as the startup wait: the worker sends it once its (throwing) factory is exhausted, before any mailbox message is processed.
    string reason;
    assert(sup.receiveTimeout(30.dur!"seconds", (string s) { reason = s; }),
            "worker did not report its degraded state");
    assert(reason == "no embedder available", "unexpected degraded reason: %s".format(reason));

    ch.riJob(RiJob(sid, "trace text for the degraded worker".idup, 2, 2));
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "degraded worker did not answer diDrain (disposal would hang)");
    gate.close();

    assert(!riCountFakeInvoked,
            "degraded worker must never invoke the summarizer (no LLM spend while degraded)");
}

// diDrain with an in-flight slow fake: the drain's bounded deadline must wait
// for the RiRecord, index it (recordJob) BEFORE the checkpoint+close, and then
// answer DiDrained: the record survives in the closed DB.
unittest {
    import std.format : format;

    auto tmpDir = testArea("dialogue_p2_drainjoin");
    scope (exit)
        tmpDir.cleanup();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init,
            "fake prompt", &riDrainFake), null);
    string sid = "20240101-120000-ab05";
    ch.riJob(RiJob(sid, "trace text for the drain-join fake".idup, 6, 9));
    // Immediately request drain: the fake sleeps 800 ms, so its RiRecord is
    // still in flight -- the drain must consume it, not skip it. The reply
    // arrives when the record lands (job count -> 0), with the record indexed
    // before the checkpoint+close.
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}), "worker did not answer diDrain");
    gate.close();

    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "session DB missing after drain");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;
    assert(db.queryTextSearch("DRAINJOIN_GAMMA", 10).length >= 1,
            "the in-flight record must be indexed before the drain's checkpoint+close");
    string chunkTexts;
    int chunkCount = 0;
    {
        auto stmt = db.prepare("SELECT text FROM TextChunkTbl;");
        foreach (ref r; stmt.get.execute) {
            chunkCount++;
            chunkTexts ~= r.peek!string(0) ~ "\n";
        }
    }
    assert(chunkCount == 1, "expected exactly 1 chunk, got %s".format(chunkCount));
    assert(chunkTexts == "DRAINJOIN_GAMMA\n", "record text mismatch: %s".format(chunkTexts));
}

// Drain deadline exceeded: with an in-flight fake that cannot finish within
// the per-drain DiDrain.budget override (1500 ms here), the drain must give up
// with a warning, still answer DiDrained, and never block on the fake. The
// late completion is consumed by the still-running worker afterwards (accepted
// shutdown record-loss window; its state is not asserted).
unittest {
    import core.thread : Thread;
    import std.string : startsWith;
    import std.format : format;

    auto tmpDir = testArea("dialogue_p2_deadline");
    scope (exit)
        tmpDir.cleanup();
    Duration lateWait;
    synchronized (d7SharedLogMutex) {

        // Capture the worker actor's log output; restored on exit.
        auto prevLog = logger.sharedLog;
        auto prevLevel = logger.globalLogLevel;
        auto cap = cast(shared) new D7LogCapture();
        logger.sharedLog = cap;
        logger.globalLogLevel = logger.LogLevel.trace;
        scope (exit) {
            logger.globalLogLevel = prevLevel;
            logger.sharedLog = prevLog;
        }

        auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
                modelName: "wk", dimensions: 8));
        auto ragCfg = RagConfig(windowOverlapPercent: 10);

        auto sys = makeSystem;
        scope (exit)
            sys.shutdown();
        auto sup = scopedActor;
        auto gate = new CompletionGate;

        auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys,
                WeakAddress.init, tmpDir.workArea, cfg,
                ragCfg, &wkEmbedderFactory, SummaryModelConfig.init, "fake prompt", &riSlowFake),
                null);
        string sid = "20240101-120000-ab06";

        ch.riJob(RiJob(sid, "trace text for the deadline fake".idup, 7, 7));
        auto t0 = Clock.currTime;
        ch.diDrain(DiDrain(sup.address(), gate, 1500.dur!"msecs"));
        assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
                "worker must answer diDrain even when the join hits its deadline");
        gate.close();
        auto elapsed = Clock.currTime - t0;
        assert(elapsed < 4000.dur!"msecs",
                "drain blocked on the in-flight fake instead of the deadline: %s".format(elapsed));

        // bounded deadline: with the job count still > 0 at the deadline the
        // worker warns and finalizes anyway (the reply is not dropped; the
        // late record just misses this drain). The warning must have been
        // emitted by the worker.
        string capturedAll;
        bool warned = false;
        foreach (l; (cast() cap).takeLines()) {
            capturedAll ~= l ~ "\n";
            if (l.startsWith("dialogue worker: drain deadline"))
                warned = true;
        }
        assert(warned,
                "expected a 'drain deadline' warning from the worker; captured:\n" ~ capturedAll);
        lateWait = 3500.dur!"msecs" - (Clock.currTime - t0);
    }
    // Let the late RiRecord (fake sleeps 3000 ms) be consumed by the still-running worker BEFORE the test area is cleaned up (documented unittest backoff sleep: the late recordJob may reopen the session DB; accepted shutdown record-loss window -- its state is deliberately not asserted).
    if (lateWait > Duration.zero)
        Thread.sleep(lateWait);
}

@("SummarizerActor delivers through an open completion gate")
unittest {
    auto sup = scopedActor;
    auto gate = new CompletionGate;
    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    sys.spawn!(Config.detached, SummarizerActor)(SummaryModelConfig(), "p",
            "trace", "topic", &riGateFake, gate, sup.address());
    bool got;
    assert(sup.receiveTimeout(30.dur!"seconds", (RiRecord r) {
            assert(r.topicName == "topic");
            got = true;
        }));
    assert(got);
}

@("SummarizerActor skips delivery on a closed gate")
unittest {
    auto sup = scopedActor;
    auto gate = new CompletionGate;
    gate.close(); // the waiter stopped waiting before the summarizer finished
    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    sys.spawn!(Config.detached, SummarizerActor)(SummaryModelConfig(), "p",
            "trace", "topic", &riGateFake, gate, sup.address());
    assert(!sup.receiveTimeout(2.dur!"seconds", (RiRecord _) {}),
            "no record may arrive on a closed gate");
}

@("DialogueWorkerActor: diJob + riJob + drain round-trip")
unittest {
    import std.conv : to;
    import std.format : format;
    import std.string : startsWith;

    auto tmpDir = testArea("dialogue_actor_roundtrip");
    scope (exit)
        tmpDir.cleanup();
    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init,
            "fake prompt", &riGateFake), null);

    string sid = "20240101-120000-aa01";
    string topicName = "d_20240101_120000_aa01__t11_11__1000";
    string episodeText;
    foreach (i; 0 .. 12)
        episodeText ~= "turn " ~ i.to!string ~ " content line\n";

    ch.diJob(DiJob(sid, [DiEpisode(topicName.idup, episodeText.idup, 11)]));
    ch.riJob(RiJob(sid, "trace text for the actor round-trip".idup, 1, 2));
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));

    // Ordering: the test thread enqueued diJob, riJob and diDrain in order, and the detached summarizer's riRecord can only be enqueued after riJob was processed -- in practice it lands behind diDrain, so the drain finds the summarizer in flight, takes the pending path, and the reply comes from the early finalize when riRecord lands (a pre-empting completion would hit the immediate-finalize path; same observable behavior).
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}), "worker did not answer diDrain");
    gate.close(); // the waiter stopped waiting: a late reply must be skipped, not delivered

    // Independent verification: a fresh read-only connection sees both jobs committed before the drain's checkpoint+close.
    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "session DB missing after drain");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;
    int dSources = 0;
    int rSources = 0;
    foreach (src; db.getSources)
        src.origin.match!((Topic t) {
            if (t.name.startsWith("d_"))
                dSources++;
            else if (t.name.startsWith("r_"))
                rSources++;
        }, (_) {});
    assert(dSources == 1, "the DiJob episode must be indexed, got %s d_ source(s)".format(dSources));
    assert(rSources == 1,
            "the RiJob record must be indexed before the drain's close, got %s r_ source(s)".format(
                rSources));
}

@("DialogueWorkerActor: degraded path reaches the owner")
unittest {
    import std.format : format;

    auto tmpDir = testArea("dialogue_actor_degraded");
    scope (exit)
        tmpDir.cleanup();
    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    sys.spawn!DialogueWorkerActor(&sys, sup.address(), tmpDir.workArea, cfg,
            ragCfg, &throwingEmbedderFactory, SummaryModelConfig.init,
            "fake prompt", &riCountFake);

    string reason;
    assert(sup.receiveTimeout(30.dur!"seconds", (string s) { reason = s; }),
            "worker did not report its degraded state");
    assert(reason == "no embedder available", "unexpected degraded reason: %s".format(reason));
}

@("DialogueWorkerActor: idle drain answers immediately")
unittest {
    auto tmpDir = testArea("dialogue_actor_idle_drain");
    scope (exit)
        tmpDir.cleanup();
    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init,
            "fake prompt", &riGateFake), null);

    // No jobs were ever sent: the worker is idle (outstanding == 0). The drain must not wait
    // for its budget deadline -- it must answer under the gate as soon as it is processed.
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "idle worker did not answer diDrain");
    gate.close();
}

// TODO: this test seems a bit too complex
/// Drain-while-pending: with an RiJob in flight (the held fake), a diDrain must NOT answer early: the reply waits
/// for the in-flight completion, the worker keeps answering plain jobs meanwhile (the drain does not block its
/// mailbox), and only after the record lands does DiDrained arrive with both commits visible (rule: reply only
/// after completion).
@("DialogueWorkerActor: drain-while-pending answers only after completion")
unittest {
    auto tmpDir = testArea("dialogue_drainpending");
    scope (exit)
        tmpDir.cleanup();
    auto sys = makeSystem;
    scope (exit) {
        // release-before-shutdown: a still-spinning detached summarizer would block the join-all exit
        atomicStore(riRelPendingFlag, true);
        sys.shutdown();
    }
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init,
            "fake prompt", &riRelPendingFake), null);
    string sid = "20240101-120000-ab15";
    ch.riJob(RiJob(sid, "trace text for the drain-pending fake".idup, 1, 2));

    // The held fake keeps the RiRecord in flight: the drain must find the job count > 0 and WAIT -- no DiDrained
    // within this window (200 ms is far past the mailbox round-trip, far before any release).
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    assert(!sup.receiveTimeout(200.dur!"msecs", (DiDrained _) {}),
            "drain-while-pending: DiDrained must wait for the in-flight completion, not answer early");

    // While the drain is pending the worker must stay responsive: this diJob is processed before the record can
    // land (mailbox FIFO; the record is only enqueued after the release below), so its chunk must be indexed
    // while the drain is still pending.
    string diTopic = "d_20240101_120000_ab15__t11_11__1000";
    ch.diJob(DiJob(sid, [
        DiEpisode(diTopic.idup, "PENDINGDI episode indexed while drain pending".idup, 11)
    ]));

    // Release the held fake: the record lands, the job count drops to 0, the early finalize indexes it, answers
    // DiDrained and closes the DB.
    atomicStore(riRelPendingFlag, true);
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "worker did not answer diDrain after the in-flight completion landed");
    gate.close();

    // Independent verification: a fresh read-only connection sees both commits before the drain's checkpoint+close:
    // the diJob chunk (indexed while the drain was pending) and the record (indexed by the early finalize).
    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "session DB missing after drain");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;
    assert(db.queryTextSearch("PENDINGDI", 10).length >= 1,
            "the diJob must have been indexed while the drain was pending");
    assert(db.queryTextSearch("PENDINGREL", 10).length >= 1,
            "the in-flight record must be indexed before the drain's checkpoint+close");
}

/// Early finalize: when the in-flight completion lands before the armed deadline (default budget 330 s here), the
/// drain must finalize on the job-count-to-0 transition -- not wait for the deadline -- and a post-drain diJob must
/// still commit (the store reopens; legacy parity).
@("DialogueWorkerActor: early finalize on completion before the deadline") unittest {
    auto tmpDir = testArea("dialogue_earlyfinalize");
    scope (exit)
        tmpDir.cleanup();
    auto sys = makeSystem;
    scope (exit) {
        // release-before-shutdown: a still-spinning detached summarizer would block the join-all exit
        atomicStore(riRelEarlyFlag, true);
        sys.shutdown();
    }
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init,
            "fake prompt", &riRelEarlyFake), null);
    string sid = "20240101-120000-ab16";
    ch.riJob(RiJob(sid, "trace text for the early-finalize fake".idup, 1, 2));
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));

    // Release immediately: the record lands within the fake's spin granularity plus a mailbox hop -- far before the
    // armed deadline (default budget 330 s). The small bound pins the early finalize: a worker that waited for the
    // deadline would miss it by orders of magnitude.
    atomicStore(riRelEarlyFlag, true);
    assert(sup.receiveTimeout(5.dur!"seconds", (DiDrained _) {}),
            "early finalize: DiDrained must arrive on completion, well before the armed deadline");
    gate.close();

    // Legacy parity: a diJob after the finalize must still commit -- the store reopens on demand.
    string diTopic = "d_20240101_120000_ab16__t11_11__1000";
    ch.diJob(DiJob(sid, [
        DiEpisode(diTopic.idup, "EARLYDI episode commits after reopen".idup, 11)
    ]));
    auto gate2 = new CompletionGate;
    ch.diDrain(DiDrain(sup.address(), gate2, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "post-drain drain did not answer (worker unhealthy after finalize)");
    gate2.close();

    auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
    assert(dbOpt.hasValue, "session DB missing after the second drain");
    auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db.destroy;
    assert(db.queryTextSearch("EARLYDI", 10).length >= 1,
            "a post-drain diJob must commit (the store reopens after finalize)");
}

/// Requester gate skip: if the drain's requester stops waiting (gate closed) before the in-flight completion
/// lands, the reply must be DROPPED -- not sent to the address -- and the worker must stay healthy (a fresh drain
/// with a new gate completes normally).
@("DialogueWorkerActor: closed requester gate skips the drain reply")
unittest {
    auto tmpDir = testArea("dialogue_gateskip");
    scope (exit)
        tmpDir.cleanup();
    auto sys = makeSystem;
    scope (exit) {
        // release-before-shutdown: a still-spinning detached summarizer would block the join-all exit
        atomicStore(riRelSkipFlag, true);
        sys.shutdown();
    }
    auto sup = scopedActor;
    auto gate = new CompletionGate;

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init,
            "fake prompt", &riRelSkipFake), null);
    string sid = "20240101-120000-ab17";
    ch.riJob(RiJob(sid, "trace text for the gate-skip fake".idup, 1, 2));
    ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
    gate.close(); // the requester stopped waiting before the summarizer finished

    // Release: the completion lands and the worker finalizes (checkpoint+close), but the closed gate must make it
    // skip the send -- the waiter never sees DiDrained (the skip, not a crash, is the behavior under test).
    atomicStore(riRelSkipFlag, true);
    assert(!sup.receiveTimeout(500.dur!"msecs", (DiDrained _) {}),
            "gate skip: a closed requester gate must drop the reply, not deliver it");

    // Worker health: a fresh drain with a new gate and a new waiter completes normally.
    auto gate2 = new CompletionGate;
    ch.diDrain(DiDrain(sup.address(), gate2, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "worker must stay healthy after a skipped reply");
    gate2.close();
}

/// Stale deadline no-op: force the early finalize with a small per-drain budget override (1000 ms) -- the early
/// finalize clears the drain state before the armed deadline ticket fires, so the stale ticket must be a no-op: the
/// first waiter gets exactly ONE DiDrained. The block then re-holds the fake and re-arms a second drain (new seq,
/// record in flight) before the first ticket fires: the seq guard (seq != drainSeq_) must skip the stale ticket
/// without consuming the newer drain's waiter, and the second drain answers on the second completion. A new drain
/// after the stale deadline still completes.
@("DialogueWorkerActor: stale deadline ticket is a no-op")
unittest {
    auto tmpDir = testArea("dialogue_staledeadline");
    scope (exit)
        tmpDir.cleanup();
    auto sys = makeSystem;
    scope (exit) {
        // release-before-shutdown: a still-spinning detached summarizer would block the join-all exit
        atomicStore(riRelStaleFlag, true);
        sys.shutdown();
    }
    auto sup = scopedActor;
    auto gate = new CompletionGate;
    auto gate2 = new CompletionGate;

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init,
            "fake prompt", &riRelStaleFake), null);
    string sid = "20240101-120000-ab18";
    ch.riJob(RiJob(sid, "trace text for the stale-deadline fake".idup, 1, 2));
    // Small per-drain budget override: the armed deadline ticket (1000 ms) is expected to go stale -- the early
    // finalize must clear the drain state before it fires.
    ch.diDrain(DiDrain(sup.address(), gate, 1000.dur!"msecs"));

    // Release immediately (early-finalize pattern): the first drain is answered on completion.
    atomicStore(riRelStaleFlag, true);
    assert(sup.receiveTimeout(5.dur!"seconds", (DiDrained _) {}),
            "early finalize must answer the first drain");
    gate.close();

    // Re-hold the fake and re-arm a second drain (new seq, its record in flight) before the first ticket fires:
    // when the stale ticket lands, the seq guard must skip it -- a worker without the guard would re-finalize on
    // the stale ticket and answer the second drain's waiter early, inside the window below. The second drain's own
    // budget (2000 ms) outruns that window, so only the first drain's stale ticket can fire inside it.
    atomicStore(riRelStaleFlag, false);
    ch.riJob(RiJob(sid, "trace text for the stale-deadline fake, second hold".idup, 1, 3));
    ch.diDrain(DiDrain(sup.address(), gate2, 2000.dur!"msecs"));
    // The stale ticket (armed 1000 ms after the first drain) fires inside this window while the second drain is
    // pending: no DiDrained may arrive. The second gate stays OPEN deliberately, so an early (wrong) answer would
    // be observable here and fail.
    assert(!sup.receiveTimeout(1200.dur!"msecs", (DiDrained _) {}),
            "stale deadline: the seq guard must skip the stale ticket while a newer drain is pending");

    // Second release: the second drain is answered on the second completion (the stale ticket consumed nothing).
    atomicStore(riRelStaleFlag, true);
    assert(sup.receiveTimeout(5.dur!"seconds", (DiDrained _) {}),
            "early finalize must answer the second drain after the stale ticket fired");
    gate2.close();

    // A new drain after the stale deadline still completes (the drain state was fully cleared, not half-consumed).
    auto gate3 = new CompletionGate;
    ch.diDrain(DiDrain(sup.address(), gate3, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "a new drain after the stale deadline must still complete");
    gate3.close();
}

// Kill path: a stuck-but-bounded summarizer must not hang the release-less shutdown.
// System.shutdown runs stopDetached (the join of the detached summarizer, bounded by the fake's
// 400 ms self-timeout) BEFORE the pool actors stop, so the worker is still alive with its gate
// open while the join runs: the completion is delivered to the live worker's mailbox and the
// message is discarded at worker teardown. The gate-skip branch (runSummarizer's "worker gone
// before completion; record lost" warning) is not exercised here -- it stays covered by reading.
// The scope(exit) release guard keeps an ABNORMAL teardown (an assert failure mid-block) from
// hanging the runner: the flag is released before the idempotent re-shutdown.
@("DialogueWorkerActor: kill-path shutdown joins a stuck summarizer and returns")
unittest {
    import core.thread : Thread;
    import std.datetime : Clock;
    import std.format : format;

    auto tmpDir = testArea("dialogue_killpath");
    scope (exit)
        tmpDir.cleanup();
    auto sys = makeSystem;
    scope (exit) {
        // release-before-shutdown: the spin guard, so no teardown path hangs on the fake
        atomicStore(riKillRelease, true);
        sys.shutdown();
    }

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init,
            "fake prompt", &riKillFake), null);
    string sid = "20240101-120000-ab19";
    ch.riJob(RiJob(sid, "kill-path trace".idup, 1, 1));

    // Bounded in-flight poll: the detached summarizer must be running before the shutdown under
    // test, or the join could race the spawn and return fast (mylib in-flight idiom).
    auto t0 = Clock.currTime;
    while (!atomicLoad(riKillInFlight) && Clock.currTime - t0 < 5.dur!"seconds")
        Thread.sleep(5.dur!"msecs"); // allowlisted unittest backoff: bounded in-flight poll
    assert(atomicLoad(riKillInFlight), "the fake must be in flight before the kill-path shutdown");

    // Starvation-proof lower bound: the fake's 400 ms window minus the time already elapsed
    // since its start stamp, with 50 ms slack for the poll tick and teardown (a starved test
    // thread that sees the flag late simply measures a smaller remaining window).
    long elapsedSinceStart = (Clock.currTime - riEpoch).total!"msecs" - atomicLoad(riKillStart);
    long remainingMs = 400L - elapsedSinceStart;
    if (remainingMs < 0L)
        remainingMs = 0L;
    long floorMs = remainingMs - 50L; // slack: one poll tick + join teardown
    if (floorMs < 0L)
        floorMs = 0L;

    // The kill path: shutdown WITHOUT releasing the stuck job.
    auto sh0 = Clock.currTime;
    sys.shutdown();
    auto elapsed = Clock.currTime - sh0;
    assert(elapsed >= floorMs.dur!"msecs",
            "join-all must wait for the in-flight summarizer (floor %s ms): %s".format(floorMs,
                elapsed));
    assert(elapsed <= 15.dur!"seconds",
            "kill-path shutdown must stay bounded by the fake's runtime: %s".format(elapsed));
}

// Join semantics: join-all includes in-flight detached summarizers -- the shutdown must block for
// the REMAINING part of the fake's fixed 500 ms sleep (computed from the fake's start stamp at
// poll time, minus 50 ms slack -- starvation-proof: a starved test thread sees a smaller
// remaining window) and nothing outlives it (the executor is never daemon; the process exits the
// suite cleanly).
@("DialogueWorkerActor: shutdown joins in-flight detached summarizers")
unittest {
    import core.thread : Thread;
    import std.datetime : Clock;
    import std.format : format;

    auto tmpDir = testArea("dialogue_joinsem");
    scope (exit)
        tmpDir.cleanup();
    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wk", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmpDir.workArea,
            cfg, ragCfg, &wkEmbedderFactory, SummaryModelConfig.init,
            "fake prompt", &riJoinFake), null);
    string sid = "20240101-120000-ab20";
    ch.riJob(RiJob(sid, "join-semantics trace".idup, 1, 1));

    auto t0 = Clock.currTime;
    while (!atomicLoad(riJoinInFlight) && Clock.currTime - t0 < 5.dur!"seconds")
        Thread.sleep(5.dur!"msecs"); // allowlisted unittest backoff: bounded in-flight poll
    assert(atomicLoad(riJoinInFlight), "the fake must be in flight before the measured shutdown");

    long elapsedSinceStart = (Clock.currTime - riEpoch).total!"msecs" - atomicLoad(riJoinStart);
    long remainingMs = 500L - elapsedSinceStart;
    if (remainingMs < 0L)
        remainingMs = 0L;
    long floorMs = remainingMs - 50L; // slack: one poll tick + join teardown
    if (floorMs < 0L)
        floorMs = 0L;

    auto sh0 = Clock.currTime;
    sys.shutdown();
    auto elapsed = Clock.currTime - sh0;
    assert(elapsed >= floorMs.dur!"msecs",
            "join-all must include the in-flight summarizer (floor %s ms): %s".format(floorMs,
                elapsed));
    assert(elapsed <= 15.dur!"seconds", "join-all must stay bounded: %s".format(elapsed));
}

// Embedder serialization (the actor serialization invariant): one shared factory serves two
// workers; each worker's instance must never see a concurrent call (in-call flag false on every
// entry), and the process-wide counter must equal the number of calls: one chunk per short
// episode plus one chunk per record, each on its own worker's mailbox.
@("DialogueWorkerActor: embedder calls serialize per worker")
unittest {
    import std.format : format;

    auto tmp1 = testArea("dialogue_ser1");
    scope (exit)
        tmp1.cleanup();
    auto tmp2 = testArea("dialogue_ser2");
    scope (exit)
        tmp2.cleanup();
    auto sys = makeSystem;
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "ser", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);

    auto ch1 = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmp1.workArea, cfg,
            ragCfg, &serEmbedderFactory, SummaryModelConfig.init, "fake prompt", &riChanFake), null);
    auto ch2 = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys, WeakAddress.init, tmp2.workArea, cfg,
            ragCfg, &serEmbedderFactory, SummaryModelConfig.init, "fake prompt", &riChanFake), null);

    // Same session id on purpose: the workers own different directories, so there is no store
    // contention, and each embedder instance stays single-use (its own worker).
    string sid = "20240101-120000-ab21";
    ch1.diJob(DiJob(sid, [
        DiEpisode("d_20240101_120000_ab21__t1_1__1000".idup, "SERW1 ep body".idup, 1)
    ]));
    ch2.diJob(DiJob(sid, [
        DiEpisode("d_20240101_120000_ab21__t2_2__1000".idup, "SERW2 ep body".idup, 2)
    ]));
    ch1.riJob(RiJob(sid, "ser worker one trace".idup, 3, 4));
    ch2.riJob(RiJob(sid, "ser worker two trace".idup, 5, 6));

    auto gate1 = new CompletionGate;
    auto gate2 = new CompletionGate;
    ch1.diDrain(DiDrain(sup.address(), gate1, Duration.zero));
    ch2.diDrain(DiDrain(sup.address(), gate2, Duration.zero));
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "worker 1 did not answer the drain");
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
            "worker 2 did not answer the drain");
    gate1.close();
    gate2.close();

    assert(atomicLoad(serEmbInstances) == 2, "expected one embedder instance per worker");
    assert(atomicLoad(serEmbViolations) == 0,
            "a concurrent embed call observed inCall_ true on entry");
    assert(atomicLoad(serEmbCalls) == 4,
            "expected exactly four embed calls (two episodes + two records): %s".format(
                atomicLoad(serEmbCalls)));

    // Both workers committed their episode (per-directory DBs, as in the other blocks).
    auto db1Opt = openDatabase((tmp1 ~ (sid ~ ".db")).AbsolutePath, "ser", 8, readOnly: true);
    assert(db1Opt.hasValue, "worker 1 session DB missing after drain");
    auto db1 = db1Opt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db1.destroy;
    assert(db1.queryTextSearch("SERW1", 10).length >= 1, "worker 1 episode not indexed");
    auto db2Opt = openDatabase((tmp2 ~ (sid ~ ".db")).AbsolutePath, "ser", 8, readOnly: true);
    assert(db2Opt.hasValue, "worker 2 session DB missing after drain");
    auto db2 = db2Opt.match!((Database d) => d, (None _) => Database.init);
    scope (exit)
        db2.destroy;
    assert(db2.queryTextSearch("SERW2", 10).length >= 1, "worker 2 episode not indexed");
}

// Channel surface: the typed Channel!DialogueWorkerAPI must carry the worker protocol with the
// current payload shapes -- the calls below compile only if the interface and the payloads agree
// (the protocol-shape regression). The runtime half round-trips every method: diJob indexes the
// episode, riJob delivers the fake's record, diDrain answers, and the post-drain riRecord/riDone
// prove the store reopens and the completion path handles unpaired sends (they underflow the
// signed job counter, which no later drain observes -- the worker is killed by the guard
// shutdown, so the underflow is the deliberate transport-test artifact, not state this suite
// asserts on).
// The surface is FIVE methods (diJob, riJob, riRecord, riDone, diDrain): the internal drainDeadline
// self-message is deliberately not on the channel (the deadline is an internal scheduling ticket,
// not the external protocol -- see the interface doc).
@("DialogueWorkerActor: channel surface carries the worker protocol")
unittest {
    import core.thread : Thread;
    import std.datetime : Clock;
    import std.string : startsWith;

    auto tmpDir = testArea("dialogue_channelsurface");
    scope (exit)
        tmpDir.cleanup();
    synchronized (d7SharedLogMutex) {

        // Capture the worker's trace output (the riDone skip line); restored on exit.
        auto prevLog = logger.sharedLog;
        auto prevLevel = logger.globalLogLevel;
        auto cap = cast(shared) new D7LogCapture();
        logger.sharedLog = cap;
        logger.globalLogLevel = logger.LogLevel.trace;
        scope (exit) {
            logger.globalLogLevel = prevLevel;
            logger.sharedLog = prevLog;
        }

        auto sys = makeSystem;
        scope (exit)
            sys.shutdown();
        auto sup = scopedActor;
        auto gate = new CompletionGate;

        auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
                modelName: "wk", dimensions: 8));
        auto ragCfg = RagConfig(windowOverlapPercent: 10);

        auto ch = Channel!DialogueWorkerAPI(sys.spawn!DialogueWorkerActor(&sys,
                WeakAddress.init, tmpDir.workArea, cfg,
                ragCfg, &wkEmbedderFactory, SummaryModelConfig.init, "fake prompt", &riChanFake),
                null);
        string sid = "20240101-120000-ab22";

        // Every channel method with the current payload shapes (compile check).
        ch.diJob(DiJob(sid, [
            DiEpisode("d_20240101_120000_ab22__t1_1__1000".idup, "CHANEPI episode body".idup, 1)
        ]));
        ch.riJob(RiJob(sid, "channel-surface trace".idup, 2, 3));
        ch.diDrain(DiDrain(sup.address(), gate, Duration.zero));
        assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}),
                "the drain must answer after the surface round trip");
        gate.close();

        // Post-drain completions: the store reopens (legacy parity), riRecord is indexed, and
        // riDone is logged with its reason. Bounded wait: the worker is a separate mailbox.
        ch.riRecord(RiRecord("r_20240101_120000_ab22__t4_4__1000", "CHANDIRECT record body"));
        ch.riDone(RiDone("r_20240101_120000_ab22__t5_5__1000", "CHANFAIL"));
        string capturedAll;
        bool skipped = false;
        auto t0 = Clock.currTime;
        while (!skipped && Clock.currTime - t0 < 5.dur!"seconds") {
            foreach (l; (cast() cap).takeLines()) {
                capturedAll ~= l ~ "\n";
                if (l.startsWith(
                        "dialogue worker: reasoning record 'r_20240101_120000_ab22__t5_5__1000' skipped: CHANFAIL"))
                    skipped = true;
            }
            if (!skipped)
                Thread.sleep(5.dur!"msecs"); // allowlisted unittest backoff: bounded log wait
        }
        assert(skipped, "riDone must log the skip with the reason; captured:\n" ~ capturedAll);

        // diJob + riJob + riRecord evidence in the committed store.
        auto dbOpt = openDatabase((tmpDir ~ (sid ~ ".db")).AbsolutePath, "wk", 8, readOnly: true);
        assert(dbOpt.hasValue, "session DB missing after the channel surface round trip");
        auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
        scope (exit)
            db.destroy;
        assert(db.queryTextSearch("CHANEPI", 10).length >= 1, "diJob episode not indexed");
        assert(db.queryTextSearch("CHANREC", 10).length >= 1, "riJob record not indexed");
        assert(db.queryTextSearch("CHANDIRECT", 10).length >= 1, "post-drain riRecord not indexed");
    }
}
