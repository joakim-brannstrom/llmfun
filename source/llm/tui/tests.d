module llm.tui.tests;

// Behavioral tests for TextUserInterfaceActor: the message flow (query
// forwarding, /stop, session action, render branch - driven directly on
// the test thread via the package-state seams in package.d) and the
// headless terminate handshake (real my.actor System).
//
// Lives in its own module instead of a unittest block in package.d because
// LDC does not register module-scope unittests of package.d index modules in
// __traits(getUnitTests, module); silly's test discovery (which drives
// `dub test`) iterates that trait, so a package.d unittest compiles but is
// never executed. Follows the codebase convention (llm.session.tests,
// llm.app_agent.tests, llm.tool_call.io.tests).

// The join uses a core.sync latch (Mutex + Condition, the pattern
// established in llm.pipeline) instead of a legacy send/receive mailbox so
// the tui package stays free of the deprecated concurrency module (no such
// imports anywhere under source/llm/tui/).
import core.sync : Condition, Mutex;
import std.datetime : Duration, SysTime, Clock, dur;
import std.logger;

import my.actor.registration : implActor;
import my.actor;
import my.path : Path;

import llm.session : SessionId;
import llm.tui;
import llm.utility : clearStopAgent, isStopAgentTriggered;
import llmfun_tui;

/// One-shot lock-guarded latch: fire() signals, await(dt) blocks until
/// fire() or the deadline (true when signaled, false on timeout).
private final class FiredLatch {
    private Mutex m;
    private Condition c;
    private bool fired;

    this() {
        m = new Mutex();
        c = new Condition(m);
    }

    void fire() {
        m.lock();
        fired = true;
        m.unlock();
        c.notify();
    }

    bool await(Duration dt) {
        m.lock();
        bool ok = fired || c.wait(dt);
        m.unlock();
        return ok;
    }
}

private class RecordingTUIListener : TUIListener {
    private FiredLatch latch;
    // Call records. The manual-drive tests dispatch and read them on the
    // test thread; the System-driven terminate test only fires the latch.
    string[] queries;
    SessionId[] selects;
    int news;
    SessionId[] renameIds;
    string[] renameTitles;
    SessionId[] deletes;
    int terminated;
    string startupFailure;

    this(FiredLatch l) {
        latch = l;
    }

    void userQuery(string s) {
        queries ~= s;
    }

    void sessionSelect(SessionId id) {
        selects ~= id;
    }

    void sessionNew() {
        news++;
    }

    void sessionRename(SessionId id, string title) {
        renameIds ~= id;
        renameTitles ~= title;
    }

    void sessionDelete(SessionId id) {
        deletes ~= id;
    }

    void uiTerminated() {
        terminated++;
        latch.fire();
    }

    void uiStartupFailed(string reason) {
        startupFailure = reason;
    }
}

/// Drives an actor instance directly on the test thread: own address +
/// ActorShell, no System. spawn! returns a TypedAddress and my.actor
/// exposes no path from an address back to the instance, so tests needing
/// instance-state setup (query_, the session stash - the
/// package seams in package.d) drive the shell themselves. The test thread
/// is the actor thread (no field synchronization), and process() is called
/// with a frozen fake clock (starts at 0, +1msecs per call) so the delayed
/// self-tick armed at real-time +10msecs can never come due mid-test.
private final class DrivenActor(T) {
    private ActorShell kernel;
    private SysTime fake;

    this(T instance) {
        this.kernel = ActorShell(makeAddress());
        implActor(instance, &this.kernel);
    }

    /// The driven actor's address (dynSend target / TypedAddress wrap).
    @property WeakAddress address() @safe {
        return kernel.address;
    }

    /// One shell tick at the frozen fake time.
    void process() {
        fake += 1.dur!"msecs";
        kernel.process(fake);
    }

    /// One shell tick at an explicit time. Unlike process(), a repeating
    /// self-tick armed at the real-time schedule moment becomes due once
    /// `now` passes it — used to prove a tick was (not) armed.
    void processAt(SysTime now) {
        kernel.process(now);
    }

    /// Full exit: tick 1 - SystemExitMsg(kill) runs the user onExit first
    /// (zombie: ui = TextUserInterface.init tears down the C state and
    /// restores the global logger the actor ctor swapped in), then
    /// forceShutdown; tick 2 - finishShutdown; tick 3 - stopped (DownMsg
    /// to monitors, instance dtor - ui already .init - address shut down).
    void kill() {
        sendExit(address, ExitReason.kill);
        process();
        process();
        process();
    }
}

@("test TUI actor")
unittest {
    // makeSystem() without a pool: the system owns (and on shutdown finishes)
    // its pool, so no pool threads outlive the test and block process exit.
    auto sys = makeSystem();
    // Idempotent shutdown on every exit path (including the assert failure
    // below), so no pool thread outlives the test and blocks process exit.
    scope (exit)
        sys.shutdown();
    auto latch = new FiredLatch();
    auto listener = sys.spawn!RecordingTUIListener(latch);

    // Headless TUI: real state, no backend (the C frame calls no-op). The
    // 4-arg ctor takes the raw state so `ui` owns it exactly once.
    auto tui = sys.spawn!TextUserInterfaceActor(TypedAddress!TUIListener(listener.addr),
            Path.init, 80, tuiCreateState());

    // spawn! instantiates TextUserInterfaceActor, compile-gating the hooks
    // (onSpawn/onExit/onException/onError) and the full TUICommands surface.
    // Send uiTerminate through a Channel!TUICommands targeting the actor.
    auto tuiCmd = Channel!TUICommands(tui, null);
    tuiCmd.uiTerminate();

    // The recording listener must report uiTerminated within 5s.
    assert(latch.await(5.dur!"seconds"),
            "recording listener did not receive uiTerminated within 5s");
}

/// Builds the manual-drive fixture used by the message-flow tests: a
/// RecordingTUIListener and a headless TextUserInterfaceActor (real C
/// state, no backend) on their own shells; the actor shell is
/// bootstrapped unless `bootstrap` is false (onSpawn ran, first tick
/// queued) - the listener shell's first tick runs on the first drive().
/// The startup-failure test passes false to inject `startupError` before
/// onSpawn. The caller must kill() both.
private final class TuiDriveFixture {
    FiredLatch latch;
    RecordingTUIListener listener;
    DrivenActor!RecordingTUIListener dListener;
    TextUserInterfaceActor actor;
    DrivenActor!TextUserInterfaceActor dActor;

    this(bool bootstrap = true) {
        latch = new FiredLatch();
        listener = new RecordingTUIListener(latch);
        dListener = new DrivenActor!RecordingTUIListener(listener);
        // 3-arg ctor: raw C state (owned once by `ui`), no backend, so the
        // C render path no-ops (headless).
        actor = new TextUserInterfaceActor(TypedAddress!TUIListener(dListener.address.lock),
                Path.init, 80, tuiCreateState());
        dActor = new DrivenActor!TextUserInterfaceActor(actor);

        // Bootstrap: onSpawn queues the first continuation tick AND the
        // fixture's process() dispatches it - the first frame renders here
        // (updateCycle 1); the mailbox then holds exactly the re-armed tick.
        if (bootstrap)
            dActor.process();
    }

    /// One uiMsg round trip through both shells.
    void drive() {
        dynSend(dActor.address, "uiMsg", UiRuntimeStatus(TuiPhase.thinking, "x"));
        dActor.process();
        dListener.process();
    }

    void teardown() {
        dActor.kill();
        dListener.kill();
    }
}

@("TUI actor: query forwarding")
unittest {
    auto f = new TuiDriveFixture();

    // Package seam: a pending user query, set while the actor is paused
    // between process() calls (same thread - no race).
    f.actor.ui.query_ = "hello";
    f.drive();

    assert(f.listener.queries == ["hello"], "listener should have userQuery(\"hello\")");
    assert(f.actor.ui.query_ is null, "query_ should be popped (empty after)");
    f.teardown();
}

@("TUI actor: /stop handling")
unittest {
    auto f = new TuiDriveFixture();

    clearStopAgent(); // normalize the process-wide flag before asserting
    f.actor.ui.query_ = "/stop";
    f.drive();

    // /stop is consumed locally: nothing reaches the listener, the
    // stop-agent flag is raised and the status line tells the user.
    assert(f.listener.queries.length == 0, "/stop must not be forwarded");
    assert(isStopAgentTriggered(), "/stop must raise the stop-agent flag");
    assert(f.actor.ui.runtimeStatus_.detail == "Stopping agent");
    assert(f.actor.ui.runtimeStatus_.phase == TuiPhase.waiting);
    clearStopAgent(); // hygiene: do not leak the flag into other tests
    f.teardown();
}

@("TUI actor: session action forwarding")
unittest {
    auto f = new TuiDriveFixture();

    // Package seam: a pending session action stash.
    f.actor.pendingAction = TuiSessionAction_Rename;
    f.actor.pendingActionId = "id1";
    f.actor.pendingActionTitle = "new title";
    f.drive();

    assert(f.listener.renameIds.length == 1 && f.listener.renameIds[0] == SessionId("id1"),
            "listener should have sessionRename(\"id1\", ...)");
    assert(f.listener.renameTitles == ["new title"], "rename title mismatch");
    assert(f.listener.queries.length == 0, "no query expected");
    // the stash is cleared after the forward
    assert(f.actor.pendingAction == TuiSessionAction_None, "stash should be cleared");
    assert(f.actor.pendingActionId is null && f.actor.pendingActionTitle is null);
    f.teardown();
}

@("TUI actor: renders happen on ticks; a uiMsg dispatch never renders")
unittest {
    auto f = new TuiDriveFixture();

    f.drive(); // pops the re-armed tick: peek sees the queued message -> non-parking render
    assert(f.actor.updateCycle == 2,
            "render must have run (the bootstrap tick rendered first, this drive's tick saw the message)");

    f.drive(); // pops the message: NO render - a uiMsg dispatch only mutates state
    assert(f.actor.updateCycle == 2, "a uiMsg dispatch must not render - the follow-up tick does");
    f.dActor.process(); // pops the re-armed tick: parked render; re-arms
    assert(f.actor.updateCycle == 3, "the follow-up tick renders once");
    f.teardown();
}

@("TUI actor: parked render still runs the polls")
unittest {
    auto f = new TuiDriveFixture();

    // The bootstrap tick was already dispatched by the fixture; a bare
    // process() pops the re-armed one: the mailbox is clean at postProcess ->
    // render(!false) parks inside the C call (headless: no backend, the
    // parked entry returns immediately). The updateCycle advance proves the
    // parked pass returned and the polls after it ran.
    f.dActor.process();
    assert(f.actor.updateCycle == 2, "parked render must return and run the polls");
    f.teardown();
}

@("TUI phase ordinals are the ABI's numbers")
unittest {
    // The C++ side pins this same literal table (test_status_chips.cpp, the
    // "phase ordinals are the ABI table" expect): `TuiRuntimeStatus.phase`
    // crosses as a raw int, so a reorder of either enum must break the other
    // side's pin instead of silently rewording the row.
    assert(TuiPhase.idle == 0);
    assert(TuiPhase.thinking == 1);
    assert(TuiPhase.tooling == 2);
    assert(TuiPhase.writing == 3);
    assert(TuiPhase.waiting == 4);
    assert(TuiPhase.error == 5);
    assert(TuiPhase.offline == 6);
}

@("TUI actor: hasPending peeks the mailbox without consuming")
unittest {
    auto f = new TuiDriveFixture();

    // Queue a message without driving: the package seam must see it, be
    // non-destructive, and keep seeing it while the re-armed tick is in front
    // (the bootstrap tick was already dispatched; each process pops the tick,
    // peeks the queued message, renders non-parking, and re-arms).
    dynSend(f.dActor.address, "uiMsg", UiRuntimeStatus(TuiPhase.thinking, "x"));
    assert(f.actor.hasPending(), "a queued message must flag pending");
    assert(f.actor.hasPending(), "peek must be non-destructive");
    f.dActor.process(); // pops the re-armed tick: peek sees the message, renders
    assert(f.actor.hasPending(), "the peeked message is still queued");
    assert(f.actor.pendingCount() == 2, "the peeked message + the re-armed tick");
    f.dActor.process(); // pops the message: consumed, no re-queue for externals
    assert(f.actor.pendingCount() == 1, "the message is gone - exactly the re-armed tick remains");
    f.teardown();
}

@("TUI actor: uiMsg dispatches send no self-ticks")
unittest {
    auto f = new TuiDriveFixture();

    // The bootstrap tick re-arms after each dispatch, so the mailbox always
    // holds exactly the one re-armed tick - a leftover self-tick sent from a
    // uiMsg handler would pile up on top of it.
    f.drive(); // pops the re-armed tick: peek sees the message -> non-parking render; re-arm
    assert(f.actor.updateCycle == 2);
    assert(f.actor.pendingCount() == 2, "the external message + the re-armed tick");
    f.drive(); // pops the message: NO render (a uiMsg dispatch runs no postProcess), no re-arm
    assert(f.actor.updateCycle == 2, "a uiMsg dispatch must not render - the follow-up tick does");
    assert(f.actor.pendingCount() == 2,
            "an external dispatch must not enqueue a tick - the second external + the re-armed tick remain");
    f.teardown();
}

@("TUI actor: external messages never enqueue ticks")
unittest {
    auto f = new TuiDriveFixture();

    // The ONLY send site is the uiTick tail, which re-arms after every
    // dispatch. Driving external messages must never ADD a tick beyond
    // that re-arm - a postProcess tail-send would permanently add one per
    // external message.
    f.drive(); // pops the re-armed tick: peek sees the message -> non-parking render; re-arm
    assert(f.actor.updateCycle == 2);
    assert(f.actor.pendingCount() == 2, "the external message + the re-armed tick");
    f.drive(); // pops the message: NO render (a uiMsg dispatch runs no postProcess); no re-arm
    assert(f.actor.updateCycle == 2, "a uiMsg dispatch must not render - the follow-up tick does");
    assert(f.actor.pendingCount() == 2, "the second external + the re-armed tick");
    f.dActor.process(); // pops the re-armed tick: parked render; re-arms
    assert(f.actor.updateCycle == 3, "the follow-up tick renders once");
    assert(f.actor.pendingCount() == 2, "the external + the re-armed tick remain");
    f.dActor.process(); // pops the external: no render, no re-arm
    assert(f.actor.updateCycle == 3, "the last external does not render");
    assert(f.actor.pendingCount() == 1, "the burst drained: exactly the re-armed tick remains");
    f.teardown();
}

@("TUI actor: the re-armed tick keeps exactly one tick in flight")
unittest {
    auto f = new TuiDriveFixture();

    // The mailbox must hold at most one SELF tick in flight plus whatever
    // externals are still queued - the re-arm never accumulates (a
    // postProcess tail-send leaks one tick per external message). The
    // bootstrap tick is already queued by onSpawn and dispatched by the
    // fixture; an extra uiTick here plays an in-flight one.
    dynSend(f.dActor.address, "uiTick");
    f.dActor.process(); // pops the bootstrap tick: parks; re-arms
    assert(f.actor.updateCycle == 2, "the bootstrap tick rendered");
    assert(f.actor.pendingCount() == 2,
            "the re-armed tick + the external one - at most one self tick in flight");
    f.dActor.process(); // pops the external tick: parks (the first chain's
    // re-arm is queued); re-arms
    assert(f.actor.updateCycle == 3);
    assert(f.actor.pendingCount() == 2, "a manual uiTick send (out of contract - production has no second "
            ~ "sender) forks a second re-arm chain: two self-ticks, never a third");
    f.teardown();
}

@("TUI actor: a burst plus a tick drains down to exactly the re-armed tick")
unittest {
    auto f = new TuiDriveFixture();

    // At the final state (onSpawn queues the bootstrap tick and the fixture
    // dispatches it): the burst drains via the single re-arm chain - uiMsg
    // dispatches neither render nor re-arm, the ticks do.
    dynSend(f.dActor.address, "uiMsg", UiRuntimeStatus(TuiPhase.thinking, "m1"));
    dynSend(f.dActor.address, "uiMsg", UiRuntimeStatus(TuiPhase.thinking, "m2"));
    dynSend(f.dActor.address, "uiTick");
    f.dActor.process(); // pops the bootstrap tick (front): peek [m1, m2, tick] -> non-parking render; re-arm
    assert(f.actor.updateCycle == 2);
    assert(f.actor.pendingCount() == 4, "m1 + m2 + the external tick + the re-armed tick");
    f.dActor.process(); // pops m1: NO render (a uiMsg dispatch runs no postProcess); no re-arm
    assert(f.actor.updateCycle == 2, "a uiMsg dispatch must not render - the follow-up tick does");
    assert(f.actor.pendingCount() == 3);
    f.dActor.process(); // pops m2: still no render, no re-arm
    assert(f.actor.updateCycle == 2);
    assert(f.actor.pendingCount() == 2);
    f.dActor.process(); // pops the external tick: parked render (only the re-armed
    // tick is left); re-arms (the second chain)
    assert(f.actor.updateCycle == 3);
    assert(f.actor.pendingCount() == 2,
            "two re-arm chains - one per self-tick source; never a third");
    f.dActor.process(); // pops the re-armed tick: parked (the other re-armed tick queued); re-arms
    assert(f.actor.updateCycle == 4, "each parked pass still advances the cycle");
    assert(f.actor.pendingCount() == 2);
    f.teardown();
}

@("TUI actor: startup failure dispatch")
unittest {
    // No bootstrap: the failure must be injected before onSpawn runs.
    auto f = new TuiDriveFixture(false);
    f.actor.startupError = "boom";
    f.dActor.process(); // onSpawn: dispatch the failure, do not arm the tick
    f.dListener.process(); // deliver the channel message to the listener

    assert(f.listener.startupFailure == "boom", "listener must receive uiStartupFailed(reason)");
    assert(f.listener.terminated == 0, "no terminate expected");

    // No tick was sent: with one, the due processAt below would dispatch it
    // and run uiTick -> postProcess and render (updateCycle would advance).
    f.dActor.processAt(Clock.currTime + 1.dur!"seconds");
    assert(f.actor.updateCycle == 0, "a failed startup must not arm the frame tick");

    f.teardown();
}

@("TUI actor: user-terminated render runs the exit handshake")
unittest {
    auto f = new TuiDriveFixture();

    f.actor.ui.userTerminated_ = true;
    f.drive(); // render -> sees the flag -> uiTerminate

    assert(f.listener.terminated == 1, "uiTerminated must be dispatched once");

    // A later message must not re-run the handshake (running is false; the
    // exit is already on its way).
    f.drive();
    assert(f.listener.terminated == 1, "no duplicate uiTerminated");

    f.teardown();
}

@("TUI actor: an in-flight tick after uiTerminate skips the work and re-arms nothing")
unittest {
    auto f = new TuiDriveFixture();

    // The `running` guard at the top of uiTick is the chain stopper.
    // Terminate first (the re-armed tick - the bootstrap tick was already
    // dispatched by the fixture ctor - runs the exit handshake), then dispatch
    // one more tick: it skips the work and re-arms nothing - which is exactly
    // what ends the chain (the single send site at the uiTick tail never runs
    // again).
    f.actor.ui.userTerminated_ = true;
    f.dActor.process(); // the re-armed tick: parked render -> sees the flag -> uiTerminate
    f.dListener.process(); // deliver the uiTerminated channel message
    assert(f.listener.terminated == 1, "uiTerminated must be dispatched once");
    assert(f.actor.updateCycle == 1, "the setup tick's render; the in-test tick contributed 0");
    assert(!f.actor.hasPending(), "the chain stopped");

    dynSend(f.dActor.address, "uiTick"); // an in-flight tick
    f.dActor.process();
    assert(f.actor.updateCycle == 1, "the skipped tick must not render");
    assert(f.listener.terminated == 1, "no duplicate uiTerminated");
    assert(f.actor.pendingCount() == 0, "the skipped tick re-armed nothing");
    f.teardown();
}

@("chat header decoration")
unittest {
    import core.time : Duration;
    import std.datetime : dur;
    import my.path : Path;
    import llm.chat : MessageTime, NoProcessingTime;

    auto u = TextUserInterface(tuiCreateState()); // state freed by ~this

    // Undecorated pass-through: nr = 0 equals shortSummary EXACTLY
    // (no prefix, no suffix, no extra whitespace).
    u.addChatMessage("hello world", null, TuiChatMessageType_Assistant, 0,
            MessageTime(NoProcessingTime()));
    assert(u.lastChatSummary == "hello world");
    u.addChatMessage("", null, TuiChatMessageType_Assistant, 0, MessageTime(NoProcessingTime()));
    assert(u.lastChatSummary == "");
    // 100-char and multi-line inputs keep shortSummary semantics
    char[100] long1;
    long1[] = 'x';
    auto longMsg = cast(string) long1;
    u.addChatMessage(longMsg, null, TuiChatMessageType_Assistant, 0,
            MessageTime(NoProcessingTime()));
    assert(u.lastChatSummary.length == 100);
    u.addChatMessage("first line\nsecond line", null,
            TuiChatMessageType_Assistant, 0, MessageTime(NoProcessingTime()));
    assert(u.lastChatSummary == "first line");

    // Decorated: prefix + body + suffix
    u.addChatMessage("hello world", null, TuiChatMessageType_User, 1,
            MessageTime(NoProcessingTime()));
    assert(u.lastChatSummary == "[1] hello world");
    u.addChatMessage("hello world", "thinking", TuiChatMessageType_Assistant,
            2, MessageTime(500.dur!"msecs"));
    assert(u.lastChatSummary == "[2] hello world [500ms]");
    u.addChatMessage(longMsg, null, TuiChatMessageType_Assistant, 12,
            MessageTime(1200.dur!"msecs"));
    assert(u.lastChatSummary == "[12] " ~ longMsg ~ " [1.2s]");
    u.addChatMessage("first line\nsecond line", null,
            TuiChatMessageType_ToolCall, 3, MessageTime(60000.dur!"msecs"));
    assert(u.lastChatSummary == "[3] first line [1m 0s]");

    // Empty summary edge cases
    u.addChatMessage("", null, TuiChatMessageType_User, 1, MessageTime(NoProcessingTime()));
    assert(u.lastChatSummary == "[1]"); // no trailing space
    u.addChatMessage("", null, TuiChatMessageType_Assistant, 1, MessageTime(0.dur!"msecs"));
    assert(u.lastChatSummary == "[1]  [0ms]"); // double space, explicit 0 shows [0ms]

    // Wrapper policy on timeSuffix (direct)
    assert(timeSuffix(MessageTime(NoProcessingTime())) == "");
    assert(timeSuffix(MessageTime(0.dur!"msecs")) == " [0ms]");
    assert(timeSuffix(MessageTime(-1200.dur!"msecs")) == ""); // negative: no suffix
}

@("TUI logger: records, a copied drain and a bounded queue")
unittest {
    // The logger is the pipe between the runtime and the C++ ring: it captures the entry's
    // *fields* (no formatted line), hands out a **copy** of what it captured (a slice of the
    // appender's own buffer is not a snapshot - it is memory the next `put` writes into), and
    // bounds what it cannot drain in time while *counting* the loss (task-31 review I1/I2).
    import std.algorithm.searching : endsWith;
    import std.conv : text;

    auto log = new TuiLogger(LogLevel.all);

    log.info("first record");
    auto first = log.drainEntries();
    assert(first.length == 1, "one log call is one record");
    assert(first[0].message == "first record", "the message arrives unformatted");

    log.info("second record");
    assert(first[0].message == "first record",
            "the drained array is a copy: a later write cannot mutate it");

    // The queue is bounded, and what it cannot keep is reported as one record. The bound is
    // exact: 20 000 kept + 1 synthetic overflow record (an `<=` would also pass a future
    // off-by-one in `MaxQueued`).
    foreach (i; 0 .. 20_050)
        log.info("spam");
    auto drained = log.drainEntries();
    assert(drained.length == 20_001,
            "the queue keeps exactly MaxQueued records plus the one overflow report (got "
            ~ drained.length.text ~ ")");
    assert(drained[$ - 1].message.endsWith("records (the frame loop did not drain in time)"),
            "the overflow is reported, not swallowed");
    // ... and the counter resets, so the report is not repeated on the next drain.
    log.info("after the flood");
    auto after = log.drainEntries();
    assert(after.length == 1, "the synthetic record is emitted once");
}

@("TUI logger: a 5000-record burst is not truncated (no stop-when-full cap)")
unittest {
    import std.conv : text;

    // The 1000-entry stop-when-full cap D used to have is gone: the C++ ring is the single
    // bounded store, so a burst that fits between two frames has to arrive whole - a cap here
    // would drop the *newest* records, which is the wrong end of a log.
    auto log = new TuiLogger(LogLevel.all);
    foreach (i; 0 .. 5_000)
        log.info("burst");
    auto drained = log.drainEntries();
    assert(drained.length == 5_000,
            "a 5000-record burst arrives whole (got " ~ drained.length.text ~ ")");
    assert(drained[0].message == "burst" && drained[$ - 1].message == "burst",
            "arrival order is preserved across the burst");
}

@("TUI log levels: the five-step scale, with no level invented for D")
unittest {
    // This std.logger has no `debug` member at all (all, trace, info, warning, error,
    // critical, fatal, off), so TuiLogLevel_Debug never comes from D: trace is the developer
    // level. `critical` and `fatal` fold into error - the UI has one rendering for "something
    // failed", and a user-facing history wants that, not the runtime's severity taxonomy.
    assert(toTuiLevel(LogLevel.trace) == TuiLogLevel_Trace);
    assert(toTuiLevel(LogLevel.info) == TuiLogLevel_Info);
    assert(toTuiLevel(LogLevel.warning) == TuiLogLevel_Warn);
    assert(toTuiLevel(LogLevel.error) == TuiLogLevel_Error);
    assert(toTuiLevel(LogLevel.critical) == TuiLogLevel_Error);
    assert(toTuiLevel(LogLevel.fatal) == TuiLogLevel_Error);
    // `all` is a filter value, never a message's level, and `off` silences the logger: neither
    // has a rendering of its own, so they land on the documented ends of the scale.
    assert(toTuiLevel(LogLevel.all) == TuiLogLevel_Trace);
    assert(toTuiLevel(LogLevel.off) == TuiLogLevel_Info);
    // An out-of-range value off the wire is info, never a guess.
    assert(toTuiLevel(cast(LogLevel) 100) == TuiLogLevel_Info);
    assert(toTuiLevel(cast(LogLevel) 0) == TuiLogLevel_Trace);
}

@("TUI log record: fields map positionally, the message splits at its first line")
unittest {
    // `TuiLogRecord`'s `module` and `function` fields are D keywords, so the record is built
    // and read positionally; the static asserts in `makeLogRecord` pin the layout this test
    // relies on.
    string view(String s) {
        return s.data is null ? "" : cast(string) s.data[0 .. s.len];
    }

    auto record = makeLogRecord(TuiLogRecordData(LogLevel.error, 1_782_000_000_123L,
            412, "read_file failed\n  at pool.d:412\n  at agent.d:88",
            "llm.agent.pool", "processTurn", "/src/llm/agent/pool.d"), TuiLogAction_Retry, "turn-7");

    assert(record.level == TuiLogLevel_Error, "error keeps its severity");
    assert(record.action == TuiLogAction_Retry, "the action crosses as an id, never a label");
    assert(record.timestampMs == 1_782_000_000_123L, "the sender's clock is kept");
    assert(record.line == 412, "the source line is kept");
    assert(record.reserved == 0, "the reserved word stays zero");
    assert(view(record.tupleof[5]) == "read_file failed", "the first line is the message");
    assert(view(record.tupleof[6]) == "  at pool.d:412\n  at agent.d:88",
            "the rest of a multi-line entry is the detail");
    assert(view(record.tupleof[7]) == "llm.agent.pool", "the module crosses");
    assert(view(record.tupleof[8]) == "processTurn", "the function crosses");
    assert(view(record.tupleof[9]) == "/src/llm/agent/pool.d", "the file crosses");
    assert(view(record.tupleof[10]) == "turn-7", "the action payload crosses");

    // A single-line entry has no detail: the Activity row must not hold the message twice.
    auto single = makeLogRecord(TuiLogRecordData(LogLevel.info, 0, 0,
            "one line", null, null, null), TuiLogAction_None, null);
    assert(view(single.tupleof[5]) == "one line");
    assert(view(single.tupleof[6]).length == 0, "a single-line message has no detail");
    assert(single.timestampMs == 0, "a record with no clock stays 0: the UI stamps the arrival");
}
