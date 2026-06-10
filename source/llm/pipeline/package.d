module llm.pipeline;

import logger = std.logger;
import core.sync : Mutex, Condition;
import std.algorithm : filter, max, map, count;
import std.array : join, appender;
import std.conv : to;
import std.datetime : Clock, SysTime, Duration, dur;
import std.format : format;
import std.range;
import std.typecons : Nullable;
import std.json : JSONValue;

import llm.agent_pool : AgentExecutionPool;
import llm.chat : Chat, Message, Role;
import llm.config : LlmConfig;
import llm.metric.monitor : MetricMonitor;
import llm.pipeline.graph : StopNode;
import llm.pipeline.graph;
import llm.rag.rag : RAG;
import llm.summary_agent : SummaryAgent;
import llm.tool_call.pipeline : PipelineControlContext;
import llm.types : IBasicAgent, IAgent, ProcessResult;

public import llm.pipeline.graph;

struct AgentName {
    string value;
    bool opEquals(const AgentName s) @safe {
        return value == s.value;
    }

    bool opEquals(const string s) @safe {
        return value == s;
    }
}

struct NodeConfig {
    uint maxRetries = 3;
}

/// Node in the pipeline graph; wraps an agent and tracks execution state.
class PipelineNode : Node {
    string id_;
    IBasicAgent agent;
    bool executed;
    NodeContext ctx;
    NodeConfig conf;

    this(string id_, IBasicAgent agent, NodeConfig conf) {
        this.id_ = id_;
        this.agent = agent;
        this.ctx = new NodeContext;
        this.agent.setPipelineContext(ctx);
        this.conf = conf;
    }

    override string id() const {
        return id_;
    }

    void addUserQuery(string query) {
        agent.addUserQuery(query);
    }

    string output() @safe pure nothrow const @nogc {
        return ctx.output;
    }

    void clearOutput() @safe pure nothrow @nogc {
        ctx.output = null;
    }

    static class NodeContext : PipelineControlContext {
        string output;
        override void setPipelineOutput(string output) {
            this.output = output;
        }
    }
}

/// Wrapper that ensures an agent calls pipelineOutput before completing.
/// Implements the retry loop: if node output is empty after a run, re-prompt
/// the agent and retry up to conf.maxRetries times.
class PipelineAgent : IBasicAgent {
    IBasicAgent wrappedAgent;
    PipelineNode node;

    this(IBasicAgent wrappedAgent, PipelineNode node)
    in (wrappedAgent !is null)
    in (node !is null) {
        this.wrappedAgent = wrappedAgent;
        this.node = node;
    }

    override string id() {
        return wrappedAgent.id();
    }

    override void addUserQuery(string query) {
        wrappedAgent.addUserQuery(query);
    }

    override void setPipelineContext(PipelineControlContext ctx) {
        wrappedAgent.setPipelineContext(ctx);
    }

    override ProcessResult runToCompletion(void delegate(ProcessResult) step = null,
            SummaryAgent.ProgressCallback compressCallback = null, bool delegate() interrupt = null) {
        ProcessResult result;
        uint attempts = 0;
        node.clearOutput;

        while (true) {
            result = wrappedAgent.runToCompletion(step, compressCallback, interrupt);

            if (!node.output.empty) {
                return result; // Agent produced output
            }

            attempts++;
            if (attempts >= node.conf.maxRetries) {
                logger.warningf(
                        "[PipelineAgent] Agent '%s' failed to produce output after %s attempts (1 initial + %s retries)",
                        wrappedAgent.id(), attempts + 1, node.conf.maxRetries);
                return result;
            }

            wrappedAgent.addUserQuery("You stopped without calling 'pipelineOutput'. Please continue your work, "
                    ~ "or call 'pipelineOutput' followed by 'taskDone' if you're finished.");
        }
    }
}

alias TransitionCondition = bool delegate(string nodeOutput, Node from, Node to);

class PipelineEdge : Edge {
    string fromId_;
    string toId_;
    TransitionCondition condition;
    uint maxLoops_;

    string output;

    this(string from, string to, TransitionCondition cond = null, uint maxLoops_ = 0) {
        this.fromId_ = from;
        this.toId_ = to;
        this.condition = cond;
        this.maxLoops_ = maxLoops_;
    }

    override string fromId() const {
        return fromId_;
    }

    override string toId() const {
        return toId_;
    }

    override uint maxLoops() const {
        return maxLoops_;
    }

    override bool canTraverse(Node from, Node to) const {
        if (condition) {
            return condition((cast(PipelineNode) from).output, from, to);
        }
        return true;
    }

    override void traverse() {
    }

    override void reset() {
    }
}

/// Result from a single agent in the pipeline
struct PipelineAgentResult {
    string agentName;
    string output; // last assistant text
    bool success;
    long durationMs;
}

/// Complete result from running the pipeline
struct PipelineResult {
    PipelineAgentResult[] agentResults;
    string finalOutput; // last agent's output
    bool allSuccess;
    long totalDurationMs;
    string[] executionOrder; /// ordered list of node IDs as they were executed
    bool wasInterrupted; /// indicates whether the pipeline was interrupted before completion
}

/// Format a PipelineResult as a human-readable string for terminal output.
string prettyPrint(PipelineResult result) {
    auto a = appender!string();
    const sep = "────────────────────────────────────────────────────────────";
    const indent = "  ";

    a.put("\n");
    a.put(sep ~ "\n");
    a.put("  Pipeline Result\n");
    a.put(sep ~ "\n");

    // Summary line
    a.put(format("%sStatus: %s\n", indent, result.allSuccess
            ? "✓ All succeeded" : "✗ Some failed"));
    a.put(format("%sTotal duration: %s ms\n", indent, result.totalDurationMs));
    a.put(format("%sAgents executed: %s\n", indent, result.agentResults.length));
    if (result.wasInterrupted)
        a.put(format("%sInterrupted: Yes\n", indent));
    a.put("\n");

    // Execution order
    if (result.executionOrder.length > 0) {
        a.put(format("%sExecution order: %s\n", indent, result.executionOrder.join(" → ")));
        a.put("\n");
    }

    // Per-agent results
    a.put(sep ~ "\n");
    a.put("  Agent Results\n");
    a.put(sep ~ "\n");

    foreach (i, agent; result.agentResults) {
        a.put("\n");
        a.put(format("%sAgent #%s: %s\n", indent, i + 1, agent.agentName));
        a.put(format("%s  Status: %s\n", indent, agent.success ? "✓ Success" : "✗ Failed"));
        a.put(format("%s  Duration: %s ms\n", indent, agent.durationMs));

        // Truncate long outputs for readability
        auto outputPreview = agent.output;
        if (outputPreview.length > 500) {
            const originalLen = outputPreview.length;
            outputPreview = outputPreview[0 .. 500] ~ "\n  ... (truncated, "
                ~ originalLen.to!string ~ " chars total)";
        }

        if (!outputPreview.empty) {
            a.put(indent ~ "  Output:\n");
            foreach (line; outputPreview.split("\n")) {
                a.put(indent ~ "    " ~ line ~ "\n");
            }
        } else {
            a.put(indent ~ "  Output: (empty)\n");
        }
    }

    a.put("\n" ~ sep ~ "\n");
    a.put("  Final Output\n");
    a.put(sep ~ "\n");

    if (!result.finalOutput.empty) {
        a.put(result.finalOutput ~ "\n");
    } else {
        a.put("  (empty)\n");
    }

    a.put(sep ~ "\n");
    return a.data;
}

/// Chains multiple agents together, passing output from one to the next
struct Pipeline {
    Graph graph;
    bool isFirst = true;

    // Thread-safety members for pool-based execution
    private {
        Mutex _mutex;
        Condition _doneCondition;
        bool _pipelineDone;
        size_t _pending;
        size_t _workerThreads = 1;
        AgentExecutionPool _pool;

        // Result tracking for pool-based execution
        PipelineAgentResult[] _agentResults;
        string[] _executionOrder;
        string _lastOutput;
        bool _allSuccess = true;
        bool delegate() _interrupt;
        bool _wasInterrupted;
        SysTime[string] _nodeStartTimes;
    }

    this(Graph graph) {
        this.graph = graph;
        isFirst = false;
        _mutex = new Mutex();
        _doneCondition = new Condition(_mutex);
    }

    void add(PipelineNode node) {
        graph.add(node);
        if (isFirst)
            graph.setStartNode(node.id);
        isFirst = false;
    }

    /// Add a node to the pipeline graph.
    void addNode(string id, IBasicAgent agent, NodeConfig conf) {
        add(new PipelineNode(id, agent, conf));
    }

    void add(PipelineEdge edge) {
        graph.add(edge);
    }

    /// Add a directed edge to the pipeline graph.
    void add(string fromId, string toId, TransitionCondition cond = null, uint maxLoops = 0) {
        graph.add(new PipelineEdge(fromId, toId, cond, maxLoops));
    }
    /// Configure the number of worker threads for pool-based execution.
    /// Default is 1.
    void setWorkerThreads(size_t n) {
        _workerThreads = n;
    }

    /// Get the current number of worker threads.
    size_t getWorkerThreads() const {
        return _workerThreads;
    }

    /// Log the pipeline's graph structure for debugging.
    string toString() {
        return graph.toString;
    }

    /// Run the pipeline using pool-based, event-driven execution.
    PipelineResult run(bool delegate() interrupt = null) @trusted {
        logger.tracef("[pipeline] run(workerThreads=%s)", _workerThreads);

        SysTime startTime = Clock.currTime;
        auto result = runGraph(interrupt);
        SysTime endTime = Clock.currTime;
        result.totalDurationMs = (endTime - startTime).total!"msecs";

        logger.tracef("[pipeline] Pipeline finished: total=%s ms success=%s agents=%d",
                result.totalDurationMs, result.allSuccess ? "true" : "false",
                result.agentResults.length);
        return result;
    }

    /// Callback invoked by the pool when an agent finishes execution.
    /// This is the heart of the event-driven execution model.
    /// All shared state access runs under _mutex to prevent races between pool threads.
    /// Pool submissions happen OUTSIDE the mutex to prevent deadlock.
    private void onAgentComplete(PipelineNode node, ProcessResult result) {
        string statusStr = result.status == ProcessResult.Status.ok ? "ok" : "failure";
        logger.tracef("[pipeline] callback(nodeId='%s') status=%s", node.id, statusStr);

        struct PendingTask {
            PipelineNode pn;
        }

        PendingTask[] tasksToSubmit;
        bool shouldSignal;

        {
            _mutex.lock();
            scope (exit)
                _mutex.unlock();

            // Early bail-out if already interrupted (safe: just reading a bool)
            if (_wasInterrupted) {
                return;
            }

            // Capture the agent result (store output, track success/failure)

            bool success = result.status == ProcessResult.Status.ok;
            string output = node.output();

            if (!success) {
                logger.warningf("[pipeline] Agent '%s' failed with status: %s",
                        node.id, result.status);
                _allSuccess = false;
            }

            // Track execution order
            _executionOrder ~= node.id;

            // Store last output
            _lastOutput = output;

            // Compute duration for this node
            long durationMs = 0;
            if (node.id in _nodeStartTimes) {
                Duration elapsed = Clock.currTime - _nodeStartTimes[node.id];
                durationMs = elapsed.total!"msecs";
                _nodeStartTimes.remove(node.id);
            }

            // Store per-agent result
            _agentResults ~= PipelineAgentResult(agentName: node.id, output: output,
                    success: success, durationMs: durationMs);

            // Mark node as executed
            node.executed = true;

            // Complete the node in the graph and collect ready nodes
            Node[] readyNodes;
            graph.completeNode(node.id, output, readyNodes);
            string readyList = readyNodes.map!(n => n.id).array().join(", ");
            logger.tracef("[pipeline] completeNode(nodeId='%s') → ready=[%s]",
                    node.id, readyList);

            // Collect tasks to submit (without actually submitting yet)
            foreach (rn; readyNodes) {
                auto pn = cast(PipelineNode) rn;
                if (cast(StopNode) rn !is null) {
                    continue;
                } else if (!pn) {
                    logger.warningf("[pipeline] Ready node '%s' is not a PipelineNode, skipping",
                            rn.id);
                    continue;
                }

                // Capture fresh input before clearing (completeNode just appended it)
                auto inputs = graph.getNodeInputs(pn.id);

                // Clear accumulated inputs if this node is being re-executed (loop edge)
                // to prevent stale inputs contaminating the new run
                if (pn.executed) {
                    graph.clearNodeInputs(pn.id);
                }

                // Build the query for this node
                string query;
                if (pn.executed) {
                    // Re-execution: frame as a new turn so the agent knows it needs fresh output
                    query = "=== NEW TURN ===\n"
                        ~ "You are being re-invoked with new input data. This is a fresh execution turn.\n"
                        ~ "Please analyze the new input below and produce a new output using pipelineOutput.\n"
                        ~ "Your previous output is less relevant.\n\n" ~ "New input:\n" ~ inputs.join(
                                "\n");
                } else {
                    // First execution: use all accumulated inputs
                    query = inputs.join("\n");
                }

                // Prepare the agent with its inputs
                pn.addUserQuery(query);

                // Record start time for duration tracking
                _nodeStartTimes[pn.id] = Clock.currTime;
                // Increment pending before submitting (so it doesn't hit zero prematurely)
                _pending++;

                // Queue the task for submission outside the mutex
                tasksToSubmit ~= PendingTask(pn);
                logger.tracef("[pipeline] submit(nodeId='%s') pending=%s", pn.id, _pending);
            }

            // Decrement AFTER submitting new work, then check for pipeline completion
            if (_pending > 0) {
                _pending--;
                if (_pending == 0) {
                    _pipelineDone = true;
                    logger.tracef("[pipeline] pending=0 → done");
                }
            }

            // Capture signal flag under mutex to avoid reading _pipelineDone outside lock
            // (prevents deadlock if callback re-enters during pool.submit)
            shouldSignal = _pipelineDone;
        }

        // Check interrupt delegate OUTSIDE the mutex before submitting new tasks
        // (prevents deadlock if delegate blocks while holding mutex)
        bool delegate() localInterrupt;
        {
            _mutex.lock();
            scope (exit)
                _mutex.unlock();
            localInterrupt = _interrupt;
        }

        bool interrupted = false;
        if (localInterrupt) {
            try {
                interrupted = localInterrupt();
            } catch (Exception e) {
                logger.tracef("interrupt callback failed: %s", e.msg);
                // If interrupt check throws, treat as not interrupted
                interrupted = false;
            }
        }

        if (interrupted) {
            _mutex.lock();
            scope (exit)
                _mutex.unlock();
            _wasInterrupted = true;
            _pipelineDone = true;
            _pending = 0; // Cancel all pending tasks
            shouldSignal = true;
            // Don't submit new tasks — pipeline is interrupted
            tasksToSubmit = null;
        }

        // Submit tasks OUTSIDE the mutex to prevent deadlock
        // (callbacks re-enter onAgentComplete which locks the same mutex)
        foreach (t; tasksToSubmit) {
            auto wrappedAgent = new PipelineAgent(t.pn.agent, t.pn);
            _pool.submit(wrappedAgent, (IAgent _, ProcessResult r) {
                this.onAgentComplete(t.pn, r);
            });
        }

        // Signal completion using flag captured under mutex
        if (shouldSignal) {
            _mutex.lock();
            scope (exit)
                _mutex.unlock();
            _doneCondition.notify();
        }
    }

    /// @deprecated: Replaced by pool-based execution in Task 5. Retained for potential future use.
    private auto runToCompletion(PipelineNode node) {
        while (true) {
            auto res = node.agent.runToCompletion();
            if (node.output.empty) {
                node.agent.addUserQuery("You stopped without calling 'pipelineOutput'. Please continue your work, or call 'pipelineOutput' followed by 'taskDone' if you're finished");
            } else {
                return res;
            }
        }
    }

    PipelineResult runGraph(bool delegate() interrupt) {
        // Reset result tracking state
        _agentResults = null;
        _executionOrder = null;
        _lastOutput = null;
        _allSuccess = true;

        // Create the execution pool
        auto pool = new AgentExecutionPool(_workerThreads);
        scope (exit) {
            pool.stop();
        }

        // Reset graph state
        graph.restart;

        // Initialize interrupt/tracking state under mutex
        {
            _mutex.lock();
            scope (exit)
                _mutex.unlock();
            _wasInterrupted = false;
            _interrupt = interrupt;
            _pipelineDone = false;
            _pending = 0;
        }

        // Scope exit: clear interrupt delegate under mutex
        scope (exit) {
            _mutex.lock();
            scope (exit)
                _mutex.unlock();
            _interrupt = null;
        }

        // Get start node
        auto startNode = cast(PipelineNode) graph.startNode;
        if (!startNode) {
            logger.errorf("[pipeline] No start node found in graph");
            _allSuccess = false;
            _pipelineDone = true;
            return buildResult();
        }

        // Submit start node to pool
        {
            _mutex.lock();
            scope (exit)
                _mutex.unlock();

            _pending = 1;
            _pool = pool;
            _nodeStartTimes[startNode.id] = Clock.currTime;
            logger.tracef("[pipeline] submit(nodeId='%s') pending=%s", startNode.id, _pending);
            auto wrappedAgent = new PipelineAgent(startNode.agent, startNode);
            _pool.submit(wrappedAgent, (IAgent _, ProcessResult r) {
                this.onAgentComplete(startNode, r);
            });
        }

        // Copy delegate to local variable under mutex (prevents torn reads)
        bool delegate() localInterrupt;
        {
            _mutex.lock();
            scope (exit)
                _mutex.unlock();
            localInterrupt = _interrupt;
        }

        // Wait for all agents to complete using timed wait with interrupt check
        while (true) {
            bool done;
            {
                _mutex.lock();
                scope (exit)
                    _mutex.unlock();
                done = _pipelineDone;
            }
            if (done) {
                break;
            }

            // Timed wait for signal (held under mutex as required by Condition)
            _mutex.lock();
            scope (exit)
                _mutex.unlock();
            auto signalled = _doneCondition.wait(dur!"msecs"(250));

            // Check if pipeline completed during wait
            {
                // TODO: seems strange
                _mutex.lock();
                scope (exit)
                    _mutex.unlock();
                if (_pipelineDone)
                    break;
            }

            // Check interrupt outside mutex (delegate may block)
            if (localInterrupt && localInterrupt()) {
                _mutex.lock();
                scope (exit)
                    _mutex.unlock();
                _wasInterrupted = true;
                _pipelineDone = true;
                break;
            }
        }

        // Build and return result
        return buildResult();
    }
    // Safely stop the pool, catching any exceptions (used in scope(exit))
    private void stopPool(AgentExecutionPool pool) {
        try {
            pool.stop();
        } catch (Exception e) {
            logger.warningf("[pipeline] pool.stop() failed: %s", e.msg);
        }
    }

    /// Build PipelineResult from tracked execution data.
    /// Must be called after pipeline completes (under no lock or after unlock).
    private PipelineResult buildResult() {
        PipelineResult result;
        result.agentResults = _agentResults.dup;
        result.executionOrder = _executionOrder.dup;
        result.finalOutput = _lastOutput;
        result.allSuccess = _allSuccess;
        result.wasInterrupted = _wasInterrupted;
        result.totalDurationMs = 0; // Set by caller (run method)
        return result;
    }
}

/// Fluent builder for constructing graph-based pipelines.
struct PipelineBuilder {
    PipelineNode[] nodes;
    PipelineEdge[] edges;
    string startId;
    string stopId;
    size_t workerThreads_ = 1;

    /// Set the number of worker threads for pool-based execution.
    /// Returns the builder for fluent chaining.
    PipelineBuilder workerThreads(size_t n) {
        workerThreads_ = n;
        return this;
    }

    /// Add a node to the pipeline.
    PipelineBuilder addNode(string id, IBasicAgent agent, NodeConfig conf = NodeConfig.init) {
        nodes ~= new PipelineNode(id, agent, conf);
        return this;
    }

    /// Add an edge with no condition and no loop limit.
    PipelineBuilder addEdge(string fromId, string toId) {
        edges ~= new PipelineEdge(fromId, toId, null, 0u);
        return this;
    }

    /// Add an edge with a transition condition.
    PipelineBuilder addEdge(string fromId, string toId, TransitionCondition condition) {
        edges ~= new PipelineEdge(fromId, toId, condition, 0u);
        return this;
    }

    /// Add an edge with a loop limit.
    PipelineBuilder addEdge(string fromId, string toId, uint maxLoops) {
        edges ~= new PipelineEdge(fromId, toId, null, maxLoops);
        return this;
    }

    /// Add an edge with both condition and loop limit.
    PipelineBuilder addEdge(string fromId, string toId, TransitionCondition condition, uint maxLoops) {
        edges ~= new PipelineEdge(fromId, toId, condition, maxLoops);
        return this;
    }

    PipelineBuilder startNode(string id) {
        startId = id;
        return this;
    }

    PipelineBuilder stopNode(string id) {
        stopId = id;
        return this;
    }

    /// Build and return a Pipeline. Validates graph integrity.
    Pipeline build() {
        if (startId.empty)
            throw new Exception("PipelineBuilder.build: No start node configured");
        if (stopId.empty)
            throw new Exception("PipelineBuilder.build: No stop node configured");
        auto g = new Graph;

        // Validate no duplicate node IDs
        bool[string] seen;
        foreach (ref n; nodes) {
            if (n.id in seen) {
                throw new Exception("PipelineBuilder.build: duplicate node ID '" ~ n.id ~ "'");
            }
            seen[n.id] = true;
        }
        if (stopId !in seen) {
            g.add(new StopNode(stopId));
            seen[stopId] = true;
        }

        // Validate edge references and self-loops
        foreach (ref e; edges) {
            if (e.fromId !in seen) {
                throw new Exception("PipelineBuilder.build: source node '" ~ e.fromId
                        ~ "' not found");
            }
            if (e.toId !in seen) {
                throw new Exception("PipelineBuilder.build: target node '" ~ e.toId ~ "' not found");
            }
            if (e.fromId == e.toId) {
                logger.warningf("[pipeline] Self-loop detected on node '%s'", e.fromId);
            }
        }

        bool[string] reachable;
        reachable[startId] = true;
        string[] reachQueue = nodes.filter!(a => a.id == startId)
            .map!"a.id"
            .array;

        size_t rqi = 0;
        while (rqi < reachQueue.length) {
            string current = reachQueue[rqi++];
            foreach (ref e; edges) {
                if ((e.fromId == current) && (e.toId !in reachable)) {
                    reachable[e.toId] = true;
                    reachQueue ~= e.toId;
                }
            }
        }
        foreach (ref n; nodes) {
            if (n.id !in reachable) {
                logger.warningf("[pipeline] Unreachable node: '%s'", n.id);
            }
        }

        if (stopId !in reachable) {
            throw new Exception("PipelineBuilder.build: stop node is not reachable");
        }

        foreach (n; nodes) {
            g.add(n);
        }
        foreach (e; edges) {
            g.add(e);
        }
        g.setStartNode(startId);
        g.setStopNode(stopId);

        auto p = Pipeline(g);
        p.setWorkerThreads(workerThreads_);
        return p;
    }
}

PipelineBuilder pipelineBuilder() {
    return PipelineBuilder.init;
}

version (unittest) {
    import core.thread : Thread;
    import core.time : dur;
    import std.json : JSONValue;
    import std.datetime : Clock, SysTime, Duration;
    import llm.summary_agent : SummaryAgent;

    /// Mock IBasicAgent that sleeps for a configurable duration before returning
    class SlowMockAgent : IBasicAgent {
        string _id;
        Duration _sleepDuration;
        bool _hasQuery;

        this(string id, Duration sleepDuration) {
            _id = id;
            _sleepDuration = sleepDuration;
        }

        override string id() {
            return _id;
        }

        override void addUserQuery(string query) {
            _hasQuery = true;
        }

        override void setPipelineContext(PipelineControlContext ctx) {
        }

        override ProcessResult runToCompletion(void delegate(ProcessResult) step = null,
                SummaryAgent.ProgressCallback compressCallback = null,
                bool delegate() interrupt = null) {
            Thread.sleep(_sleepDuration);
            ProcessResult result;
            result.status = ProcessResult.Status.ok;
            result.chat = [];
            result.hasToolCall = false;
            result.timing = JSONValue(null);
            result.usage = JSONValue(null);
            return result;
        }
    }

    /// Mock agent that captures input queries and sets a configurable output via pipelineOutput.
    /// Used to test output propagation between pipeline steps.
    class PropagatingMockAgent : IBasicAgent {
        string _id;
        string _outputToSet;
        string[] _receivedQueries;
        PipelineControlContext _ctx;

        this(string id, string outputToSet) {
            _id = id;
            _outputToSet = outputToSet;
        }

        override string id() {
            return _id;
        }

        override void addUserQuery(string query) {
            _receivedQueries ~= query;
        }

        override void setPipelineContext(PipelineControlContext ctx) {
            _ctx = ctx;
        }

        override ProcessResult runToCompletion(void delegate(ProcessResult) step = null,
                SummaryAgent.ProgressCallback compressCallback = null,
                bool delegate() interrupt = null) {
            // Simulate calling pipelineOutput – this is how agents propagate output downstream
            if (_ctx)
                _ctx.setPipelineOutput(_outputToSet);

            ProcessResult result;
            result.status = ProcessResult.Status.ok;
            result.chat = [];
            result.hasToolCall = false;
            result.timing = JSONValue(null);
            result.usage = JSONValue(null);
            return result;
        }
    }

    /// Mock agent for testing PipelineAgent retry behavior.
    /// Configurable to produce output on a specific call number (0 = never).
    class ControllableMockAgent : IBasicAgent {
        string _id;
        uint _runCount;
        uint _addUserQueryCount;
        uint _produceOutputOnCall; // 0 = never, 1 = on first call, 2 = on second call, etc.
        PipelineControlContext _ctx;

        this(string id, uint produceOutputOnCall) {
            _id = id;
            _produceOutputOnCall = produceOutputOnCall;
        }

        override string id() {
            return _id;
        }

        override void addUserQuery(string query) {
            _addUserQueryCount++;
        }

        override void setPipelineContext(PipelineControlContext ctx) {
            _ctx = ctx;
        }

        override ProcessResult runToCompletion(void delegate(ProcessResult) step = null,
                SummaryAgent.ProgressCallback compressCallback = null,
                bool delegate() interrupt = null) {
            _runCount++;

            // Set output on the configured call number
            if (_produceOutputOnCall > 0 && _runCount == _produceOutputOnCall) {
                if (_ctx) {
                    _ctx.setPipelineOutput("output_from_mock");
                }
            }

            ProcessResult result;
            result.status = ProcessResult.Status.ok;
            result.chat = [];
            result.hasToolCall = false;
            result.timing = JSONValue(null);
            result.usage = JSONValue(null);
            return result;
        }
    }

}

// Test: output from agent-A propagates as input to agent-B
unittest {
    auto outputA = "output_from_A";
    auto agentA = new PropagatingMockAgent("agentA", outputA);
    auto agentB = new PropagatingMockAgent("agentB", "output_from_B");

    auto pipeline = pipelineBuilder().addNode("agentA", agentA).addNode("agentB",
            agentB).addEdge("agentA", "agentB").startNode("agentA").stopNode("agentB").build();

    auto result = pipeline.run("initial query", null);

    assert(result.allSuccess, "Pipeline should succeed");
    assert(result.agentResults.length == 2, "Both agents should have run");

    // Agent A should output what it was configured to output
    assert(result.agentResults[0].output == outputA,
            "Agent A output should match configured output");

    // Agent B should have received A's output as its input query
    assert(agentB._receivedQueries.length == 1, "Agent B should have received exactly one query");
    assert(agentB._receivedQueries[0] == outputA,
            "Agent B should have received A's output as input");
}

// test interrupt of pipeline
unittest {
    auto agentA = new SlowMockAgent("agentA", dur!"msecs"(1));
    auto agentB = new SlowMockAgent("agentB", dur!"msecs"(300));
    auto agentC = new SlowMockAgent("agentC", dur!"msecs"(300));

    // dfmt off
    auto pipeline = pipelineBuilder()
        .addNode("agentA", agentA)
        .addNode("agentB", agentB)
        .addNode("agentC", agentC)
        .addEdge("agentA", "agentB")
        .addEdge("agentB", "agentC")
        .startNode("agentA")
        .stopNode("agentC")
        .build();
    // dfmt on

    // Interrupt delegate that fires before all agents finish
    bool shouldInterrupt = false;
    auto startTime = Clock.currTime;
    auto interruptDelegate = () {
        if (!shouldInterrupt) {
            if ((Clock.currTime - startTime) > 100.dur!"msecs") {
                shouldInterrupt = true;
            }
        }
        return shouldInterrupt;
    };

    auto result = pipeline.run("test query", interruptDelegate);

    // Verify interrupt was detected
    assert(result.wasInterrupted, "Pipeline should have been interrupted");

    // Verify partial results: at least one agent completed
    assert(result.agentResults.length >= 1,
            "At least one agent should have completed before interrupt");

    // Verify not all agents completed (interrupt was early enough)
    assert(result.agentResults.length < 3, "Not all agents should have completed due to interrupt");
}

// null delegate (no-op)
unittest {
    auto agentA = new SlowMockAgent("agentA", dur!"msecs"(1));
    auto agentB = new SlowMockAgent("agentB", dur!"msecs"(1));
    auto agentC = new SlowMockAgent("agentC", dur!"msecs"(1));

    // dfmt off
    auto pipeline = pipelineBuilder()
        .addNode("agentA", agentA)
        .addNode("agentB", agentB)
        .addNode("agentC", agentC)
        .addEdge("agentA", "agentB")
        .addEdge("agentB", "agentC")
        .startNode("agentA")
        .stopNode("agentC")
        .build();
    // dfmt on

    // Run with null interrupt delegate — should behave identically to pre-interrupt
    auto result = pipeline.run("test query", null);

    // All agents complete normally
    assert(result.agentResults.length == 3,
            "All agents should have completed with null interrupt delegate");

    // wasInterrupted must be false
    assert(!result.wasInterrupted,
            "wasInterrupted should be false when interrupt delegate is null");
}

// throwing delegate
unittest {
    auto agentA = new SlowMockAgent("agentA", dur!"msecs"(1));
    auto agentB = new SlowMockAgent("agentB", dur!"msecs"(1));
    auto agentC = new SlowMockAgent("agentC", dur!"msecs"(1));

    // dfmt off
    auto pipeline = pipelineBuilder()
        .addNode("agentA", agentA)
        .addNode("agentB", agentB)
        .addNode("agentC", agentC)
        .addEdge("agentA", "agentB")
        .addEdge("agentB", "agentC")
        .startNode("agentA")
        .stopNode("agentC")
        .build();
    // dfmt on

    // Delegate that always throws — should be treated as "not interrupted"
    bool delegate() throwingDelegate = () {
        throw new Exception("interrupt delegate threw");
    };

    // Pipeline should not crash and should complete normally
    auto result = pipeline.run("test query", throwingDelegate);

    // Throwing delegate must not crash the pipeline
    assert(result.agentResults.length == 3,
            "All agents should have completed despite throwing interrupt delegate");

    // wasInterrupted must be false (throwing delegate treated as not interrupted)
    assert(!result.wasInterrupted, "wasInterrupted should be false when interrupt delegate throws");
}

// Test PipelineAgent with agent that produces output immediately
unittest {
    auto mockAgent = new ControllableMockAgent("instantOutput", 1); // produces output on call 1
    auto node = new PipelineNode("testNode", mockAgent, NodeConfig.init);
    auto wrapper = new PipelineAgent(mockAgent, node);
    auto result = wrapper.runToCompletion();

    assert(result.status == ProcessResult.Status.ok, "Result should be ok");
    assert(mockAgent._runCount == 1, "Wrapped agent should have been called exactly once");
    assert(mockAgent._addUserQueryCount == 0, "No retry prompt should have been sent");
    assert(!node.output.empty, "Node output should be set");
}

// Test PipelineAgent with agent that never produces output
unittest {
    const uint maxRetries = 3;
    auto mockAgent = new ControllableMockAgent("noOutput", 0); // never produces output
    auto node = new PipelineNode("testNode", mockAgent, NodeConfig(maxRetries: maxRetries));

    auto wrapper = new PipelineAgent(mockAgent, node);
    auto result = wrapper.runToCompletion();

    assert(result.status == ProcessResult.Status.ok, "Result should still be ok (safety valve)");
    assert(mockAgent._runCount == maxRetries,
            "Wrapped agent should have been called exactly maxRetries times");
    assert(mockAgent._addUserQueryCount == maxRetries - 1,
            "Retry prompt should have been sent maxRetries-1 times");
    assert(node.output.empty, "Node output should still be empty");
}

// Test PipelineAgent with agent that produces output on second retry
unittest {
    auto mockAgent = new ControllableMockAgent("secondTryOutput", 2); // produces output on call 2
    auto node = new PipelineNode("testNode", mockAgent, NodeConfig.init);

    auto wrapper = new PipelineAgent(mockAgent, node);
    auto result = wrapper.runToCompletion();

    assert(result.status == ProcessResult.Status.ok, "Result should be ok");
    assert(mockAgent._runCount == 2, "Wrapped agent should have been called exactly twice");
    assert(mockAgent._addUserQueryCount == 1, "Retry prompt should have been sent exactly once");
    assert(!node.output.empty, "Node output should be set after second attempt");
}
