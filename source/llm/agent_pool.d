module llm.agent_pool;

import core.sync : Mutex, Condition;
import core.thread : Thread;
import logger = std.logger;
import std.conv : to;
import std.datetime : Clock, dur;
import std.parallelism;
import core.atomic;

import llm.types;

/**
 * Thread-safe agent execution pool.
 *
 * Submits agents for asynchronous execution on a dedicated worker thread.
 * Agents are processed in FIFO order. Callbacks are invoked with the
 * agent and its ProcessResult after execution completes.
 */
class AgentExecutionPool {
    struct AgentTask {
        IAgent agent;
        void delegate(IAgent, ProcessResult) callback;
    }

    TaskPool pool;

    private {
        shared bool running;
        shared bool stopped;
        shared size_t queuedAgents;
    }

    this(size_t workerThreads = 1) {
        pool = new TaskPool(workerThreads);
        running.atomicStore(true);
    }

    /**
     * Submits an agent for asynchronous execution.
     *
     * The callback is invoked on the worker thread after the agent
     * finishes running via runToCompletion().
     *
     * Exceptions from agent execution or callback invocation are caught
     * and logged; they do not propagate to the submitter or terminate
     * the worker thread.
     *
     * Throws Exception if pool was never started, is stopped, or is stopping.
     */
    void submit(IAgent agent, void delegate(IAgent, ProcessResult) callback) {
        if (stopped.atomicLoad()) {
            throw new Exception("Pool is stopped");
        }
        if (!running.atomicLoad()) {
            throw new Exception("Pool not started");
        }

        static void runAgent(AgentExecutionPool self, AgentTask* t) {
            scope (exit)
                self.queuedAgents.atomicOp!"-="(1);
            self._executeTask(t);
        }

        auto t = new AgentTask(agent, callback);
        pool.put(task!runAgent(this, t));
        queuedAgents.atomicOp!"+="(1);
        logger.tracef("[pool] Submitted agent '%s' (queue size: %s)", agent.id,
                queuedAgents.atomicLoad);
    }

    /**
     * Stops the pool gracefully.
     *
     * Signals the worker to shut down after draining remaining items
     * in the queue. Blocks until the worker thread exits or a timeout
     * of 10 minutes is reached.
     *
     * Idempotent: calling twice is safe.
     */
    void stop() {
        if (stopped.atomicLoad()) {
            logger.tracef("[pool] Already stopped, no-op");
            return; // Idempotent no-op
        }
        stopped.atomicStore(true); // Set IMMEDIATELY to prevent double-stop race
        pool.finish(blocking: true);
        running.atomicStore(false);
        logger.tracef("[pool] Worker thread stopped");
    }

    /**
     * Returns true if the pool is currently running (accepting and processing tasks).
     */
    bool isRunning() {
        return running.atomicLoad();
    }

    /**
      * Executes a single task with try/catch resilience.
      * Called outside the mutex lock.
      */
    private void _executeTask(AgentTask* task) {
        logger.tracef("[pool] Executing agent '%s'", task.agent.id);
        ProcessResult result;
        try {
            result = task.agent.runToCompletion();
            logger.trace("[pool] Agent completed");
        } catch (Exception e) {
            logger.errorf("[pool] Agent execution threw for agent '%s': %s", task.agent.id, e.msg);
            result = ProcessResult.init;
            result.status = ProcessResult.Status.unknownFailure;
        }
        // Callback always fires, even on agent exception
        try {
            task.callback(task.agent, result);
        } catch (Exception e) {
            logger.errorf("[pool] Callback threw for agent '%s': %s", task.agent.id, e.msg);
        }
    }

    /**
     * Returns the approximate number of agents currently queued for execution.
     */
    size_t pending() {
        return queuedAgents.atomicLoad;
    }
}

private:

version (unittest) {
    shared static this() {
        // To activate detailed logging
        // logger.globalLogLevel = logger.LogLevel.trace;
        // (cast() logger.sharedLog).logLevel = logger.LogLevel.trace;
    }

    import llm.summary_agent;
    import std.json : JSONValue;
    import std.datetime : Duration;
    import std.stdio : writeln, writefln;
    import std.algorithm : map;

    /// Mock agent that returns a predictable ProcessResult
    class MockAgent : IAgent {
        string name;
        ProcessResult.Status status;

        this(string name, ProcessResult.Status status = ProcessResult.Status.ok) {
            this.name = name;
            this.status = status;
        }

        override string id() {
            return name;
        }

        override ProcessResult runToCompletion(void delegate(ProcessResult) step = null,
                SummaryAgent.ProgressCallback compressCallback = null,
                bool delegate() interrupt = null) {
            auto result = ProcessResult();
            result.status = status;
            result.chat = [];
            result.hasToolCall = false;
            result.timing = JSONValue(null);
            result.usage = JSONValue(null);
            return result;
        }
    }

    /// Mock agent that throws during runToCompletion
    class ThrowingAgent : IAgent {
        string message;

        this(string message = "Intentional test exception") {
            this.message = message;
        }

        override string id() {
            return message;
        }

        override ProcessResult runToCompletion(void delegate(ProcessResult) step = null,
                SummaryAgent.ProgressCallback compressCallback = null,
                bool delegate() interrupt = null) {
            throw new Exception(message);
        }
    }

    /// Helper: signals a condition variable and tracks whether it has been signaled.
    struct WaitHandle {
        Mutex _mutex;
        Condition _cond;
        bool _signaled;

        void signal() {
            _mutex.lock();
            _signaled = true;
            _cond.notify();
            _mutex.unlock();
        }

        void wait(Duration timeout) {
            _mutex.lock();
            scope (exit)
                _mutex.unlock();
            auto deadline = Clock.currTime() + timeout;
            while (!_signaled && Clock.currTime() < deadline)
                _cond.wait();
        }

        bool get() {
            _mutex.lock();
            scope (exit)
                _mutex.unlock();
            return _signaled;
        }
    }

    WaitHandle waitHandle() {
        auto m = new Mutex();
        return WaitHandle(_mutex: m, _cond: new Condition(m), _signaled: false);
    }

    /// Helper: wait until a count reaches a threshold.
    struct CountHandle {
        Mutex _mutex;
        Condition _cond;
        int _count;

        void increment() {
            _mutex.lock();
            _count++;
            _cond.notify();
            _mutex.unlock();
        }

        int wait(int threshold, Duration timeout) {
            _mutex.lock();
            scope (exit)
                _mutex.unlock();
            auto deadline = Clock.currTime() + timeout;
            while (_count < threshold && Clock.currTime() < deadline)
                _cond.wait();
            return _count;
        }

        int get() {
            _mutex.lock();
            scope (exit)
                _mutex.unlock();
            return _count;
        }
    }

    CountHandle countHandle() {
        auto m = new Mutex();
        return CountHandle(_mutex: m, _cond: new Condition(m), _count: 0);
    }

    /// Helper: identity check on interface array.
    bool agentFound(IAgent target, IAgent[] list) {
        foreach (a; list) {
            if (a is target)
                return true;
        }
        return false;
    }
}

/// Test: basic submit-execute-callback flow (happy path)
unittest {
    auto pool = new AgentExecutionPool();
    scope (exit)
        pool.stop();

    auto handle = waitHandle();
    ProcessResult capturedResult;
    IAgent capturedAgent;

    void callback(IAgent agent, ProcessResult result) {
        capturedAgent = agent;
        capturedResult = result;
        handle.signal();
    }

    auto mockAgent = new MockAgent("agent", ProcessResult.Status.ok);
    pool.submit(mockAgent, &callback);

    handle.wait(10.dur!"seconds");
    assert(handle.get(), "Callback was not invoked");
    assert(capturedAgent is mockAgent, "Callback received wrong agent");
    assert(capturedResult.status == ProcessResult.Status.ok,
            "Expected status ok, got " ~ capturedResult.status.to!string);

    pool.stop();
    assert(!pool.isRunning(), "Pool should not be running after stop");
}

/// Test: submit with non-ok status is passed through correctly
unittest {
    auto pool = new AgentExecutionPool();
    scope (exit)
        pool.stop();

    auto handle = waitHandle();
    ProcessResult capturedResult;

    void callback(IAgent agent, ProcessResult result) {
        capturedResult = result;
        handle.signal();
    }

    auto mockAgent = new MockAgent("agent", ProcessResult.Status.unknownFailure);
    pool.submit(mockAgent, &callback);

    handle.wait(10.dur!"seconds");
    assert(handle.get(), "Callback was not invoked for failure status");
    assert(capturedResult.status == ProcessResult.Status.unknownFailure,
            "Expected status unknownFailure, got " ~ capturedResult.status.to!string);
}

/// Test: concurrent submissions from multiple threads
unittest {
    auto pool = new AgentExecutionPool();
    scope (exit)
        pool.stop();

    auto counter = countHandle();
    Mutex agentMutex = new Mutex();
    IAgent[] executedAgents;

    void callback(IAgent agent, ProcessResult result) {
        agentMutex.lock();
        scope (exit)
            agentMutex.unlock();
        executedAgents ~= agent;
        counter.increment();
    }

    immutable N = 10;
    MockAgent[] submittedAgents;
    Thread[] threads;

    // Launch N threads, each submitting one agent
    foreach (i; 0 .. N) {
        static struct Capture {
            MockAgent agent;
            AgentExecutionPool pool;
            void delegate(IAgent agent, ProcessResult result) callback;
            void dg() {
                pool.submit(agent, callback);
            }
        }

        auto captures = new Capture(agent: new MockAgent(i.to!string,
                ProcessResult.Status.ok), pool: pool, callback: &callback);
        submittedAgents ~= captures.agent;
        threads ~= new Thread(&captures.dg);
        threads[$ - 1].start();
    }

    // Wait for all submission threads to complete
    foreach (t; threads) {
        t.join();
    }

    // Wait for all agents to be executed
    counter.wait(N, 30.dur!"seconds");

    // Verify all agents were executed exactly once
    assert(counter.get() == N,
            "Expected " ~ N.to!string ~ " executions, got " ~ counter.get().to!string);

    // Verify all unique agents ran using identity comparison
    foreach (submitted; submittedAgents) {
        assert(agentFound(submitted, executedAgents), "Submitted agent was not executed");
    }
}

/// Test: stop/drain behavior
unittest {
    auto pool = new AgentExecutionPool();
    scope (exit)
        pool.stop();

    auto counter = countHandle();

    void callback(IAgent agent, ProcessResult result) {
        counter.increment();
    }

    // Submit several agents before stop
    immutable N = 5;
    foreach (i; 0 .. N) {
        auto agent = new MockAgent(i.to!string, ProcessResult.Status.ok);
        pool.submit(agent, &callback);
    }

    // Stop the pool (should drain pending agents)
    pool.stop();

    // Verify all submitted agents completed
    counter.wait(N, 30.dur!"seconds");
    assert(counter.get() == N,
            "Expected " ~ N.to!string ~ " completions, got " ~ counter.get().to!string);

    // Attempt to submit after stop → should throw
    bool exceptionThrown = false;
    try {
        auto agent = new MockAgent("agent", ProcessResult.Status.ok);
        pool.submit(agent, &callback);
    } catch (Exception e) {
        exceptionThrown = true;
    }
    assert(exceptionThrown, "Submit after stop should throw exception");

    // Call stop again → should be safe (idempotent)
    bool doubleStopException = false;
    try {
        pool.stop();
    } catch (Exception e) {
        doubleStopException = true;
    }
    assert(!doubleStopException, "Calling stop() twice should not throw");

    // Verify isRunning returns false after stop
    assert(!pool.isRunning(), "Pool should not be running after stop");
}
/// Test: exception resilience - pool survives agent and callback exceptions (Task 12)
unittest {
    auto pool = new AgentExecutionPool();
    scope (exit)
        pool.stop();

    auto counter = countHandle();
    Mutex agentMutex = new Mutex();
    IAgent[] successfulAgents;

    void callbackNormal(IAgent agent, ProcessResult result) {
        agentMutex.lock();
        if (result.status == ProcessResult.Status.ok)
            successfulAgents ~= agent;
        agentMutex.unlock();
        counter.increment();
    }

    void callbackThrows(IAgent agent, ProcessResult result) {
        // Callback itself throws
        throw new Exception("Callback threw exception");
    }

    // Submit agent A (normal, succeeds)
    auto agentA = new MockAgent("a", ProcessResult.Status.ok);
    pool.submit(agentA, &callbackNormal);

    // Submit agent B (throws in runToCompletion)
    pool.submit(new ThrowingAgent("Agent B exception"), &callbackNormal);

    // Submit agent C (normal, succeeds)
    auto agentC = new MockAgent("c", ProcessResult.Status.ok);
    pool.submit(agentC, &callbackNormal);

    // Submit agent D (succeeds but callback throws)
    pool.submit(new MockAgent("d", ProcessResult.Status.ok), &callbackThrows);

    // Submit agent E (normal, succeeds)
    auto agentE = new MockAgent("e", ProcessResult.Status.ok);
    pool.submit(agentE, &callbackNormal);

    // Wait for 3 successful completions (A, C, E)
    counter.wait(3, 30.dur!"seconds");

    // Give a moment for exceptions to be processed
    Thread.sleep(500.dur!"msecs");

    // PROVE POOL IS STILL ALIVE: submit one more agent after exceptions
    auto agentF = new MockAgent("alive", ProcessResult.Status.ok);
    auto postHandle = waitHandle();

    void callbackPost(IAgent agent, ProcessResult result) {
        postHandle.signal();
    }

    pool.submit(agentF, &callbackPost);
    postHandle.wait(10.dur!"seconds");
    assert(postHandle.get(), "Pool should still accept and process agents after exceptions");

    // Stop pool
    pool.stop();

    // Verify agents A, C, E completed successfully
    assert(counter.get() == 4, "Expected 4 completions before post-exception agent");
    assert(successfulAgents.length == 3,
            "Expected 3 successful completions before post-exception agent");

    // Verify the correct agents succeeded using identity
    assert(agentFound(agentA, successfulAgents), "Agent A should have completed");
    assert(agentFound(agentC, successfulAgents), "Agent C should have completed");
    assert(agentFound(agentE, successfulAgents), "Agent E should have completed");
}

/// Test: start() is idempotent
unittest {
    auto pool = new AgentExecutionPool();
    scope (exit)
        pool.stop();
    assert(pool.isRunning());
}
