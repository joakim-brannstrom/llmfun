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

import my.actor;
import my.actor.registration : implActor;

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
/// instance-state setup (query_, the session stash, nextUpdate - the
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
    // 3-arg ctor takes the raw state so `ui` owns it exactly once.
    auto tui = sys.spawn!TextUserInterfaceActor(
            TypedAddress!TUIListener(listener.addr), 80, tuiCreateState());

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
/// armed) - the listener shell's first tick runs on the first drive().
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
                80, tuiCreateState());
        dActor = new DrivenActor!TextUserInterfaceActor(actor);

        // Bootstrap: onSpawn runs and arms the repeating self-tick at
        // real-time + UpdateInterval (never due under the frozen clock).
        if (bootstrap)
            dActor.process();
    }

    /// One uiMsg round trip through both shells.
    void drive() {
        dynSend(dActor.address, "uiMsg", UiStatusText("x"));
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
    assert(f.actor.ui.statusText == "Stopping agent");
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

@("TUI actor: render branch")
unittest {
    auto f = new TuiDriveFixture();

    // Package seam: arm the render branch one tick in the past (headless:
    // the C backend no-ops, this exercises the poll/render bookkeeping).
    auto armed = Clock.currTime - 1.dur!"msecs";
    f.actor.nextUpdate = armed;
    f.drive();

    auto advanced = f.actor.nextUpdate - armed;
    assert(advanced >= 9.dur!"msecs", "render branch should have run (nextUpdate advanced)");
    assert(advanced <= 30.dur!"msecs", "at most one render per drive (no double advance)");
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

    // No frame tick was armed: with one, the due tick below would run
    // uiTick -> postProcess and render (updateCycle would advance).
    f.dActor.processAt(Clock.currTime + 1.dur!"seconds");
    assert(f.actor.updateCycle == 0, "a failed startup must not arm the frame tick");

    f.teardown();
}

@("TUI actor: user-terminated render runs the exit handshake")
unittest {
    auto f = new TuiDriveFixture();

    // Package seam: a frame reported user-terminated (GUI window close).
    f.actor.ui.userTerminated_ = true;
    f.actor.nextUpdate = Clock.currTime - 1.dur!"msecs";
    f.drive(); // render branch -> sees the flag -> uiTerminate

    assert(f.listener.terminated == 1, "uiTerminated must be dispatched once");

    // A later message must not re-run the handshake (running is false; the
    // exit is already on its way).
    f.actor.nextUpdate = Clock.currTime - 1.dur!"msecs";
    f.drive();
    assert(f.listener.terminated == 1, "no duplicate uiTerminated");

    f.teardown();
}
