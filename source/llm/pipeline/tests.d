/// Unit tests for pipeline output propagation, interruption and the PipelineAgent retry loop. Relocated from llm/pipeline/package.d: unittest blocks in a package.d module compile and link but are never discovered by the `dub test` runner (dub's generated dub_test_root registers no package.d modules), so these assertions never executed there.
module llm.pipeline.tests;

import core.time : dur;
import std.array : empty;
import std.datetime : Clock;

import llm.pipeline;
import llm.summary_agent : SummaryAgent;
import llm.tool_call.pipeline : PipelineControlContext;
import llm.types : IBasicAgent, IStreamCallback, ProcessResult;

version (unittest) {
    import core.thread : Thread;
    import std.json : JSONValue;
    import std.datetime : Clock, SysTime, Duration;

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

        override void setStreamUpdate(IStreamCallback _) {
        }

        override void addUserQuery(string query) {
            _hasQuery = true;
        }

        override void addContinueMessage(string msg) {
            _hasQuery = true;
        }

        override void setPipelineContext(PipelineControlContext ctx) {
        }

        override ProcessResult runToCompletion(void delegate(ProcessResult) step = null,
                SummaryAgent.ProgressCallback compressCallback = null,
                bool delegate() interrupt = null) {
            Thread.sleep(_sleepDuration);
            return ProcessResult.init;
        }
    }

    /// Mock agent that captures input queries and sets a configurable output via pipelineOutput. Used to test output propagation between pipeline steps.
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

        override void setStreamUpdate(IStreamCallback _) {
        }

        override void addUserQuery(string query) {
            _receivedQueries ~= query;
        }

        override void addContinueMessage(string msg) {
            _receivedQueries ~= msg;
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

            return ProcessResult.init;
        }
    }

    /// Mock agent for testing PipelineAgent retry behavior. Configurable to produce output on a specific call number (0 = never).
    class ControllableMockAgent : IBasicAgent {
        string _id;
        uint _runCount;
        uint _addUserQueryCount;
        uint _addContinueMessageCount;
        uint _produceOutputOnCall; // 0 = never, 1 = on first call, 2 = on second call, etc.
        PipelineControlContext _ctx;

        this(string id, uint produceOutputOnCall) {
            _id = id;
            _produceOutputOnCall = produceOutputOnCall;
        }

        override string id() {
            return _id;
        }

        override void setStreamUpdate(IStreamCallback _) {
        }

        override void addUserQuery(string query) {
            _addUserQueryCount++;
        }

        override void addContinueMessage(string msg) {
            _addContinueMessageCount++;
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

            return ProcessResult.init;
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

    auto result = pipeline.run(null);

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

    auto result = pipeline.run(interruptDelegate);

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
    auto result = pipeline.run(null);

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
    auto result = pipeline.run(throwingDelegate);

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
    assert(mockAgent._addContinueMessageCount == 0,
            "No retry nudge should have been sent when output is produced");
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
    assert(mockAgent._addContinueMessageCount == maxRetries - 1,
            "Retry nudge should have been sent maxRetries-1 times (continuing the turn, H1)");
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
    assert(mockAgent._addContinueMessageCount == 1,
            "Retry nudge should have been sent exactly once (continuing the turn, H1)");
    assert(!node.output.empty, "Node output should be set after second attempt");
}
