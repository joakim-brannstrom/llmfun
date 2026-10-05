/// Integration and prompt-data guards for the Agent turn policy.
module llm.agent.tests;

import std.algorithm : canFind;
import std.file : exists;
import std.sumtype : match;
import std.typecons : nullable;

import llm.agent;
import llm.chat : Chat, Message, turnIdOf;
import llm.metric.monitor : MetricMonitor;

unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;

    // stdTime (100ns resolution) keeps two runs in the same second from colliding on the temp dir.
    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_turnid_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    auto agent = new Agent("integration", llmConf, null, null);

    agent.setSystemPrompt("sys");
    agent.addUserQuery("first question"); // opens turn 1
    agent.addContinue(); // nudge continues turn 1
    agent.addUserQuery("second question"); // opens turn 2
    agent.addKeepReasoning(); // nudge continues turn 2
    agent.addContinueMessage("You stopped without calling 'pipelineOutput'."); // retry nudge continues turn 2

    assert(agent.chat.nextTurnId() == 2);
    auto msgs = agent.chat.getMessages;
    assert(msgs.length == 6);
    assert(turnIdOf(msgs[0]) == 0, "system prompt belongs to no turn");
    assert(turnIdOf(msgs[1]) == 1);
    assert(turnIdOf(msgs[2]) == 1);
    assert(turnIdOf(msgs[3]) == 2);
    assert(turnIdOf(msgs[4]) == 2);
    assert(turnIdOf(msgs[5]) == 2, "retry nudge continues turn 2, never opens one");

    // The retry nudge is harness traffic: it appears in neither projection and did not fragment the turn sequence.
    auto dialogue = agent.chat.getDialogueHistory();
    assert(dialogue.length == 2, "only the two real user queries are dialogue");
    assert(turnIdOf(dialogue[0]) == 1 && turnIdOf(dialogue[1]) == 2);
    assert(agent.chat.getReasoningTrace().length == 0, "harness nudges are not trace");
}

// Fail-fast: a configured nudge file that is missing fails the AGENT
// CONSTRUCTION with the same "Prompt file not found" error a missing AGENT.md
// produces — before any request is wasted. The override names a file that
// exists nowhere in the fixture prompt dir.
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_nudges_missing_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.nudges.keepReasoning.softNudges = ["NO_SUCH_NUDGE.md"];

    bool thrown;
    try {
        auto a = new Agent("integration", llmConf, null, null);
        assert(a.nudges_.keepReasoning.softNudges == ["NO_SUCH_NUDGE.md"],
                "construction must throw before this line");
    } catch (Exception e) {
        thrown = true;
        assert(e.msg == "Prompt file not found: NO_SUCH_NUDGE.md", e.msg);
    }
    assert(thrown, "construction must fail on a missing configured nudge file");
}

// Wholesale override + eager load at model switch: resetModel swaps the
// ENTIRE policy — fields the model block does not restate fall back to struct
// defaults, NOT to the global customization — and the model's own templates are
// eagerly loaded from the construction prompt dir.
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import std.typecons : nullable;

    import llm.config : CodeModelConfig, NudgeConfig, NudgeKind;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_nudges_switch_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);
    write(tmpDir ~ "/MODEL_NUDGE.md", "MODEL-NUDGE-MARKER");

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.nudges.recovery.hardStrikes = 9; // global customization the model block does NOT restate

    CodeModelConfig modelCfg;
    modelCfg.modelName = "nudged-model";
    modelCfg.contextSize = 8192;
    NudgeConfig modelNudges;
    modelNudges.keepReasoning.softNudges = ["MODEL_NUDGE.md"];
    modelCfg.nudges = modelNudges.nullable;
    llmConf.codeModels ~= modelCfg;

    auto a = new Agent("integration", llmConf, null, null);

    // Construction resolved the active model from the global default.
    assert(a.nudges_.keepReasoning.softNudges.length == 0);
    assert(a.nudges_.recovery.hardStrikes == 9);
    assert(a.nudgeTexts_.soft[NudgeKind.keepReasoning][0].canFind(
            "[SYSTEM NUDGE - NOT USER INPUT]"));

    a.resetModel(llmConf.codeModels[1]);

    // Wholesale swap: the model's block IS the entire policy.
    assert(a.nudges_ == modelNudges);
    // ...so the global recovery customization is gone (struct default back).
    assert(a.nudges_.recovery.hardStrikes == 1);
    // ...and the model's own soft template was eagerly loaded from promptDir_.
    assert(a.nudgeTexts_.soft[NudgeKind.keepReasoning][0] == "MODEL-NUDGE-MARKER");
}

// The trigger rule lives in prompt data, not code, so in-tree removal of the section would silently change the main agent's behavior. This guard loads the real llmfun/config/prompt/AGENT.md through the production getPrompt/getBasePrompt path (FlatVfs) and asserts the section heading and the exact tool name are present. A missing file makes getBasePrompt throw, which fails the test.
unittest {
    const agentPromptFile = "llmfun/config/prompt/AGENT.md";
    assert(agentPromptFile.exists, "in-tree AGENT.md missing; getBasePrompt would throw at startup");
    auto llmConf = makeAgentTestConfig("llmfun/config/prompt");
    auto prompt = llmConf.getPrompt(null, "AGENT.md");
    assert(prompt.canFind("# Dialogue History Retrieval"),
            "trigger-rule section missing from the composed main-agent prompt");
    assert(prompt.canFind("queryDialogueHistory"),
            "The trigger rule must name the queryDialogueHistory tool");
}

// The reasoning-history rule also lives in prompt data, not code, so in-tree removal of the section would silently change the main agent's behavior. Same guard shape as the dialogue test above: load the real llmfun/config/prompt/AGENT.md through the production getPrompt path and assert the section heading, the exact tool name, and the anti-anchoring warning are present.
unittest {
    const agentPromptFile = "llmfun/config/prompt/AGENT.md";
    assert(agentPromptFile.exists, "in-tree AGENT.md missing; getBasePrompt would throw at startup");
    auto llmConf = makeAgentTestConfig("llmfun/config/prompt");
    auto prompt = llmConf.getPrompt(null, "AGENT.md");
    assert(prompt.canFind("# Reasoning History Retrieval"),
            "Reasoning History Retrieval section missing from the composed main-agent prompt");
    assert(prompt.canFind("queryReasoningHistory"),
            "Reasoning rule must name the queryReasoningHistory tool");
    assert(prompt.canFind("PAST THOUGHTS, NOT ground truth"),
            "Reasoning rule must state that results are past thoughts, not ground truth");
}

// The five shipped nudge templates are prompt data, not code, so in-tree
// removal (of a file, or of the harness marker / tool name it must carry)
// would silently strip the nudge ladder of its functional traffic or let a
// nudge leak into the dialogue/trace projections. These guards load each real
// llmfun/config/prompt/NUDGE_*.md through the production readPromptFile path
// (FlatVfs) and assert exactly what the nudge files require. A missing file makes
// readPromptFile throw, which fails the test.
unittest {
    foreach (name; [
        "NUDGE_KEEP_REASONING_SOFT.md", "NUDGE_KEEP_REASONING_HARD.md",
        "NUDGE_RECOVERY_SOFT.md", "NUDGE_RECOVERY_HARD.md", "NUDGE_COMPRESSION.md"
    ]) {
        const nudgeFile = "llmfun/config/prompt/" ~ name;
        assert(nudgeFile.exists, "in-tree nudge file " ~ name ~ " missing");
    }

    auto llmConf = makeAgentTestConfig("llmfun/config/prompt");

    // Hard nudges must name the exact tool the ladder demands.
    auto keepHard = llmConf.readPromptFile("NUDGE_KEEP_REASONING_HARD.md");
    assert(keepHard.canFind("taskDone"), "keep-reasoning hard nudge must name the taskDone tool");
    auto recoveryHard = llmConf.readPromptFile("NUDGE_RECOVERY_HARD.md");
    assert(recoveryHard.canFind("Call taskDone now."),
            "recovery hard nudge must tell the agent to call taskDone");

    // The compression nudge must name the requestCompression tool and carry
    // the harness marker, like the soft nudges.
    auto compression = llmConf.readPromptFile("NUDGE_COMPRESSION.md");
    assert(compression.canFind("requestCompression"),
            "compression nudge must name the requestCompression tool");
    assert(compression.canFind("[SYSTEM NUDGE - NOT USER INPUT]"),
            "compression nudge must carry the SYSTEM NUDGE marker");

    // Soft nudges must carry the harness markers that keep them out of the
    // dialogue/trace projections.
    auto keepSoft = llmConf.readPromptFile("NUDGE_KEEP_REASONING_SOFT.md");
    assert(keepSoft.canFind("[SYSTEM NUDGE - NOT USER INPUT]"),
            "keep-reasoning soft nudge must carry the SYSTEM NUDGE marker");
    auto recoverySoft = llmConf.readPromptFile("NUDGE_RECOVERY_SOFT.md");
    assert(recoverySoft.canFind("[SYSTEM RECOVERY - NOT USER INPUT]"),
            "recovery soft nudge must carry the SYSTEM RECOVERY marker");
}

version (unittest) {
    import llm.config : LlmConfig;

    /// Test seam for the runToCompletion loop: `Agent.process` is public and
    /// virtual (not final, not private), so a subclass can feed the loop canned
    /// ProcessResults without any HTTP endpoint. The canned results need only
    /// the fields the loop reads — status and hasToolCall.
    class CannedProcessAgent : Agent {
        ProcessResult[] canned;
        size_t served;

        this(string name, LlmConfig llmConf) {
            super(name, llmConf, null, null);
        }

        /// Wires a monitor so the handleToolCalls feedback gating can be
        /// exercised directly (the default ctors pass null, which disables the
        /// feedback warnings entirely).
        this(string name, LlmConfig llmConf, MetricMonitor monitor) {
            import my.filter : ReFilter;

            super(name, llmConf, null, monitor, null, ReFilter.init);
        }

        override ProcessResult process(bool delegate() interrupt) @trusted nothrow {
            return canned[served++];
        }
    }
}

// The default ladder through the real runToCompletion loop: strikes 1-2 emit the shipped
// soft keep-reasoning nudge (repeat-last), strike 3 the hard one, strike 4
// exhausts the default 2+1 ladder and fails the turn with
// Status.agentStuckInLoop. The CannedProcessAgent seam feeds the loop without
// any HTTP endpoint. Also carries the harness-traffic acceptance: the nudges
// are userQuery:false — invisible in the dialogue/trace projections, and they
// never open or fragment a turn.
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_escalation_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto agent = new CannedProcessAgent("integration", makeAgentTestConfig(tmpDir));
    agent.addUserQuery("question"); // opens turn 1; nudges continue it
    agent.canned = [
        ProcessResult(status: ProcessResult.Status.needMoreThinking),
        ProcessResult(status: ProcessResult.Status.needMoreThinking),
        ProcessResult(status: ProcessResult.Status.needMoreThinking),
        ProcessResult(status: ProcessResult.Status.needMoreThinking)
    ];

    auto result = agent.runToCompletion();

    assert(result.status == ProcessResult.Status.agentStuckInLoop,
            "strike 4 exhausts the default 2+1 ladder and fails the turn");

    auto msgs = agent.chat.getMessages;
    assert(msgs.length == 4); // query + 3 nudges (strike 4 adds none)
    assert(msgs[1].match!((Message m) => m.content.canFind("[SYSTEM NUDGE - NOT USER INPUT]"),
            (_) => false), "strike 1: soft keep-reasoning nudge");
    assert(msgs[2].match!((Message m) => m.content.canFind("[SYSTEM NUDGE - NOT USER INPUT]"),
            (_) => false), "strike 2: same soft file (repeat-last)");
    assert(msgs[3].match!((Message m) => m.content.canFind("[SYSTEM OVERRIDE"),
            (_) => false), "strike 3: hard keep-reasoning nudge");
    foreach (m; msgs[1 .. $])
        assert(m.match!((Message m) => !m.isUserQuery, (_) => false),
                "nudges are harness traffic, not user queries");
    foreach (m; msgs[1 .. $])
        assert(turnIdOf(m) == 1, "nudges continue turn 1, never open one");

    assert(agent.chat.getDialogueHistory.length == 1, "only the real query");
    assert(agent.chat.getReasoningTrace.length == 0, "nudges are not trace");
    assert(agent.keepReasoningStrikes == 4, "the exhausted strike still counts");
    assert(agent.continueStrikes == 0);
}

// Wholesale per-model override: the model block restates only softStrikes:1
// — everything else (hardStrikes, file lists) falls back to struct defaults, so
// strike 1 uses the shipped soft nudge and strike 2 the shipped hard one.
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;

    import llm.config : CodeModelConfig, NudgeConfig;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_escalation_model_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    CodeModelConfig modelCfg;
    modelCfg.modelName = "nudged-model";
    modelCfg.contextSize = 8192;
    NudgeConfig modelNudges;
    modelNudges.keepReasoning.softStrikes = 1;
    modelCfg.nudges = modelNudges.nullable;
    llmConf.codeModels ~= modelCfg;

    auto agent = new CannedProcessAgent("integration", llmConf);
    agent.resetModel(llmConf.codeModels[1]); // wholesale swap

    agent.canned = [
        ProcessResult(status: ProcessResult.Status.needMoreThinking),
        ProcessResult(status: ProcessResult.Status.needMoreThinking),
        ProcessResult(status: ProcessResult.Status.needMoreThinking)
    ];

    auto result = agent.runToCompletion();

    assert(result.status == ProcessResult.Status.agentStuckInLoop,
            "strike 3 exhausts the softStrikes:1 ladder (1 soft + 1 hard)");
    auto msgs = agent.chat.getMessages;
    assert(msgs.length == 2); // 2 nudges
    assert(msgs[0].match!((Message m) => m.content.canFind("[SYSTEM NUDGE - NOT USER INPUT]"),
            (_) => false), "strike 1: soft (per-model ladder)");
    assert(msgs[1].match!((Message m) => m.content.canFind("[SYSTEM OVERRIDE"),
            (_) => false), "strike 2: hard (phase transition)");
    assert(agent.keepReasoningStrikes == 3);
}

// `enabled: false` short-circuits to true — no counter increment: the
// loop runs on with no nudge injected and ends via the MaxConsecutiveSameStatus
// backstop (4 identical needMoreThinking results), not via ladder exhaustion.
// The fixture omits the keep-reasoning files entirely: if the disabled kind's
// ladder were touched, the missing-key AA access would throw.
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_escalation_disabled_%d_%d",
            now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    write(tmpDir ~ "/NUDGE_RECOVERY_SOFT.md", "soft");
    write(tmpDir ~ "/NUDGE_RECOVERY_HARD.md", "hard");
    write(tmpDir ~ "/NUDGE_COMPRESSION.md", "compression");

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.nudges.keepReasoning.enabled = false;

    auto agent = new CannedProcessAgent("integration", llmConf);
    agent.canned = [
        ProcessResult(status: ProcessResult.Status.needMoreThinking),
        ProcessResult(status: ProcessResult.Status.needMoreThinking),
        ProcessResult(status: ProcessResult.Status.needMoreThinking),
        ProcessResult(status: ProcessResult.Status.needMoreThinking)
    ];

    auto result = agent.runToCompletion();

    assert(result.status == ProcessResult.Status.agentStuckInLoop,
            "the turn ends via the loop-safety backstop");
    assert(agent.chat.getMessages.length == 0, "no nudge message was injected");
    assert(agent.keepReasoningStrikes == 0, "no counter increment");
    assert(agent.continueStrikes == 0);
}

// Pin: a pipeline-retry message added via addContinueMessage consumes no
// strikes and never routes through the ladder — the retry path bypasses the
// escalation mechanism entirely (caller-supplied text).
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_w3_bypass_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto agent = new CannedProcessAgent("integration", makeAgentTestConfig(tmpDir));
    agent.addUserQuery("question"); // opens turn 1

    agent.addContinueMessage("You stopped without calling 'pipelineOutput'.");

    assert(agent.keepReasoningStrikes == 0 && agent.continueStrikes == 0,
            "the retry nudge consumes no strikes");
    auto msgs = agent.chat.getMessages;
    assert(msgs.length == 2); // query + retry nudge
    assert(msgs[1].match!((Message m) => m.content == "You stopped without calling 'pipelineOutput'.",
            (_) => false), "caller text verbatim, no template");
    assert(turnIdOf(msgs[1]) == 1, "the retry nudge continues the open turn");
    assert(agent.chat.getDialogueHistory.length == 1, "not in the dialogue projection");
    assert(agent.chat.getReasoningTrace.length == 0, "not in the trace projection");
}

// Regression: an ok round WITH tool calls is a fresh round — it injects no
// nudge and resets the strike counters (per-turn lifecycle), so a healthy
// multi-round turn never drifts toward ladder exhaustion. The soft marker on
// the second keep-reasoning nudge proves the reset: without it, strike 2 would
// emit the hard template.
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_ok_toolcall_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto agent = new CannedProcessAgent("integration", makeAgentTestConfig(tmpDir));
    agent.addUserQuery("question"); // opens turn 1
    agent.canned = [
        ProcessResult(status: ProcessResult.Status.needMoreThinking), // strike 1: soft
        ProcessResult(status: ProcessResult.Status.ok, hasToolCall: true), // fresh round: reset
        ProcessResult(status: ProcessResult.Status.needMoreThinking), // strike 1 AGAIN: soft, not hard
        ProcessResult(status: ProcessResult.Status.networkFailure) // ends the turn
    ];

    auto result = agent.runToCompletion();

    assert(result.status == ProcessResult.Status.networkFailure,
            "the turn ends with the canned failure, not a nudge backstop");
    auto msgs = agent.chat.getMessages;
    assert(msgs.length == 3); // query + 2 keep-reasoning nudges (ok round adds none)
    assert(msgs[1].match!((Message m) => m.content.canFind("[SYSTEM NUDGE - NOT USER INPUT]"),
            (_) => false), "strike 1: soft keep-reasoning nudge");
    assert(msgs[2].match!((Message m) => m.content.canFind("[SYSTEM NUDGE - NOT USER INPUT]"),
            (_) => false), "strike reset by the ok round: soft again, not hard");
    assert(agent.keepReasoningStrikes == 1 && agent.continueStrikes == 0,
            "only the final needMoreThinking round consumed a strike");
}

// The ok-without-tool-call path drives the recovery ladder through the real
// loop: the nudge is injected, the counter advances, and the
// projections stay nudge-free.
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_ok_continue_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto agent = new CannedProcessAgent("integration", makeAgentTestConfig(tmpDir));
    agent.addUserQuery("question"); // opens turn 1
    agent.canned = [
        ProcessResult(status: ProcessResult.Status.ok), // no tool call: recovery strike 1
        ProcessResult(status: ProcessResult.Status.networkFailure) // ends the turn
    ];

    auto result = agent.runToCompletion();

    assert(result.status == ProcessResult.Status.networkFailure,
            "the loop ends with the canned failure, not a nudge backstop");
    auto msgs = agent.chat.getMessages;
    assert(msgs.length == 2); // query + 1 recovery nudge
    assert(msgs[1].match!((Message m) => m.content.canFind("[SYSTEM RECOVERY - NOT USER INPUT]"),
            (_) => false), "ok without tool call: soft recovery nudge");
    assert(agent.continueStrikes == 1 && agent.keepReasoningStrikes == 0);
    assert(agent.chat.getDialogueHistory.length == 1, "nudge not in the dialogue projection");
    assert(agent.chat.getReasoningTrace.length == 0, "not in the trace projection");
    foreach (m; msgs[1 .. $])
        assert(m.match!((Message m) => !m.isUserQuery, (_) => false),
                "the nudge is harness traffic, not a user query");
    foreach (m; msgs[1 .. $])
        assert(turnIdOf(m) == 1, "the nudge continues the open turn");
}

// The compression nudge is config-gated end to end: below the configured
// threshold nothing fires; at/above it the shipped template is emitted with
// {{context_percent}} substituted (rendered %.1f, e.g. "83.0%"), and the
// one-shot compressNudgeSent keeps later rounds in the same compress cycle
// silent.
unittest {
    import std.array : replicate;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_compression_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    // Below the threshold: no nudge.
    auto below = new CannedProcessAgent("integration", makeAgentTestConfig(tmpDir));
    below.addUserQuery("x".replicate(12_000)); // 6000/8192 = 73.2%
    below.syncContextFromChat(); // the loop's gates read prevStat
    below.canned = [ProcessResult(status: ProcessResult.Status.networkFailure)];
    below.runToCompletion();
    assert(below.chat.getMessages.length == 1, "below the threshold: no nudge");

    // At/above the threshold: the shipped template fires once with the
    // rendered percentage.
    auto agent = new CannedProcessAgent("integration", makeAgentTestConfig(tmpDir));
    agent.addUserQuery("x".replicate(13_600)); // 6800/8192 = 83.0%
    agent.syncContextFromChat();
    agent.canned = [ProcessResult(status: ProcessResult.Status.networkFailure)];
    auto result = agent.runToCompletion();
    assert(result.status == ProcessResult.Status.networkFailure,
            "the turn ends with the canned failure, not a compression backstop");
    auto msgs = agent.chat.getMessages;
    assert(msgs.length == 2); // query + compression nudge
    assert(msgs[1].match!((Message m) => m.content.canFind("[SYSTEM NUDGE - NOT USER INPUT]")
            && m.content.canFind("83.0%") && m.content.canFind("requestCompression"), (_) => false),
            "at/above threshold: shipped template with %.1f percent");
    assert(msgs[1].match!((Message m) => !m.content.canFind("{{"),
            (_) => false), "no placeholder survives substitution");

    // One-shot: further rounds in the same compress cycle stay silent.
    agent.canned = [ProcessResult(status: ProcessResult.Status.networkFailure)];
    agent.served = 0; // the canned index persists across runs
    agent.runToCompletion();
    agent.served = 0; // each run consumes one canned entry
    agent.runToCompletion();
    assert(agent.chat.getMessages.length == 2, "one-shot per compress cycle");
}

// The per-model compression threshold is a wholesale override: the
// model's own threshold governs. At the same 83% usage a model with 0.9 stays
// silent where the global default 0.8 fires, and a model with 0.6 fires
// earlier than the global default would.
unittest {
    import std.array : replicate;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;

    import llm.config : CodeModelConfig, NudgeConfig;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_compression_model_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    CodeModelConfig modelCfg;
    modelCfg.modelName = "threshold-model";
    modelCfg.contextSize = 8192;
    NudgeConfig modelNudges;
    modelNudges.compression.threshold = 0.9;
    modelCfg.nudges = modelNudges.nullable;
    llmConf.codeModels ~= modelCfg;

    // threshold 0.9: silent at 83% (the global 0.8 would fire here).
    auto agent = new CannedProcessAgent("integration", llmConf);
    agent.resetModel(llmConf.codeModels[1]); // wholesale swap
    agent.addUserQuery("x".replicate(13_600)); // 83.0%
    agent.syncContextFromChat();
    agent.canned = [ProcessResult(status: ProcessResult.Status.networkFailure)];
    agent.runToCompletion();
    assert(agent.chat.getMessages.length == 1,
            "per-model threshold 0.9: silent at 83% where the global 0.8 fires");

    // threshold 0.6: fires at the same usage — earlier than the global 0.8.
    auto eager = new CannedProcessAgent("integration", makeAgentTestConfig(tmpDir));
    eager.addUserQuery("x".replicate(13_600));
    eager.syncContextFromChat();
    eager.canned = [ProcessResult(status: ProcessResult.Status.networkFailure)];
    eager.runToCompletion();
    assert(eager.chat.getMessages.length == 2,
            "per-model threshold 0.6 fires at 83% (the global 0.8 would not yet)");
    // Exact-at boundary (strict >): with threshold 0.75 the trigger point is
    // exactly 6144 tokens — at it the nudge stays silent, one token above
    // fires.
    CodeModelConfig boundCfg;
    boundCfg.modelName = "boundary-model";
    boundCfg.contextSize = 8192;
    NudgeConfig boundNudges;
    boundNudges.compression.threshold = 0.75;
    boundCfg.nudges = boundNudges.nullable;
    llmConf.codeModels ~= boundCfg;

    auto atBoundary = new CannedProcessAgent("integration", llmConf);
    atBoundary.resetModel(llmConf.codeModels[2]);
    atBoundary.addUserQuery("x".replicate(12_288)); // exactly 6144/8192 = 75.0%
    atBoundary.syncContextFromChat();
    atBoundary.canned = [
        ProcessResult(status: ProcessResult.Status.networkFailure)
    ];
    atBoundary.runToCompletion();
    assert(atBoundary.chat.getMessages.length == 1, "exactly at the threshold: no nudge (strict >)");

    auto pastBoundary = new CannedProcessAgent("integration", llmConf);
    pastBoundary.resetModel(llmConf.codeModels[2]);
    pastBoundary.addUserQuery("x".replicate(12_290)); // 6145/8192 tokens
    pastBoundary.syncContextFromChat();
    pastBoundary.canned = [
        ProcessResult(status: ProcessResult.Status.networkFailure)
    ];
    pastBoundary.runToCompletion();
    assert(pastBoundary.chat.getMessages.length == 2,
            "one token past the threshold: the nudge fires");
}

// `compression.enabled: false` silences the nudge at any usage: the
// guard returns before the threshold check, and loadNudgeTexts loads no
// template for the disabled kind (nudgeTexts_.compression stays empty — even a
// guard bug could not substitute anything).
unittest {
    import std.array : replicate;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_compression_off_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.nudges.compression.enabled = false;

    auto agent = new CannedProcessAgent("integration", llmConf);
    agent.addUserQuery("x".replicate(13_600)); // 83.0%
    agent.syncContextFromChat();
    agent.canned = [ProcessResult(status: ProcessResult.Status.networkFailure)];
    agent.runToCompletion();

    assert(agent.chat.getMessages.length == 1, "disabled compression: no nudge at any usage");
}

// Feedback warnings: gating only — the text stays owned by
// FeedbackEngine. The legacy constants are gone: intervalSecs and
// minToolCalls drive the two gates. The first-warning sentinel
// (toolCallWarnCounter == -1) fires regardless of minToolCalls once the
// interval has passed - the sentinel start is SysTime.init.
unittest {
    import std.datetime : Clock, SysTime;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import my.path : Path;
    import std.path : buildPath;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_feedback_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto monitor = new MetricMonitor(buildPath(tmpDir, "metrics.jsonl").Path);

    size_t feedbackWarnings(CannedProcessAgent a) {
        size_t n;
        foreach (msg; a.chat.getMessages)
            n += msg.match!((Message m) => m.content.canFind("[SYSTEM MONITORING NOTE]")
                    ? 1 : 0, (_) => 0);
        return n;
    }

    StreamResponse.ToolCall[long] calls;
    calls[0] = StreamResponse.ToolCall(id: "1", name: "no_such_tool", arguments: "{}");

    // Sentinel: the first warning fires regardless of minToolCalls — the
    // interval has passed (lastToolCallWarning starts at SysTime.init).
    auto llmConf = makeAgentTestConfig(tmpDir);
    auto agent = new CannedProcessAgent("integration", llmConf, monitor);
    agent.handleToolCalls(null, calls);
    assert(agent.chat.getMessages.length == 3); // warning + tool call + tool response
    assert(feedbackWarnings(agent) == 1, "sentinel: the first warning fires");

    // Both gates closed by default (900 s interval, 50 min tool calls): the
    // second call stays warning-silent.
    agent.handleToolCalls(null, calls);
    assert(agent.chat.getMessages.length == 5);
    assert(feedbackWarnings(agent) == 1,
            "the second warning gates on intervalSecs AND minToolCalls");

    // minToolCalls gate alone blocks: intervalSecs 0 passes the clock gate
    // (rewound), but the counter is below the minimum.
    auto llmConfMin = makeAgentTestConfig(tmpDir);
    llmConfMin.nudges.feedback.intervalSecs = 0;
    auto gated = new CannedProcessAgent("integration", llmConfMin, monitor);
    gated.handleToolCalls(null, calls); // sentinel fires
    assert(feedbackWarnings(gated) == 1);
    gated.lastToolCallWarning = SysTime.init; // rewind: only minToolCalls gates now
    gated.handleToolCalls(null, calls);
    assert(feedbackWarnings(gated) == 1, "minToolCalls 50: the counter gate blocks");

    // minToolCalls boundary (strict >): with intervalSecs 0 the clock gate
    // always passes after the rewind, so the counter gate is exercised alone —
    // counter == minToolCalls is still blocked, minToolCalls + 1 fires.
    auto llmConfBound = makeAgentTestConfig(tmpDir);
    llmConfBound.nudges.feedback.intervalSecs = 0;
    llmConfBound.nudges.feedback.minToolCalls = 2;
    auto bound = new CannedProcessAgent("integration", llmConfBound, monitor);
    bound.handleToolCalls(null, calls); // sentinel fires
    assert(feedbackWarnings(bound) == 1);
    bound.lastToolCallWarning = SysTime.init; // keep the clock gate open
    foreach (i; 0 .. 2) // gates see counters 1, 2 — 2 == minToolCalls blocks
        bound.handleToolCalls(null, calls);
    assert(feedbackWarnings(bound) == 1, "counter == minToolCalls: equality blocks");
    bound.handleToolCalls(null, calls); // the gate sees 3 > 2: fires
    assert(feedbackWarnings(bound) == 2, "counter just above minToolCalls fires");

    // Both gates open (intervalSecs 0, minToolCalls 0): the second warning
    // fires (rewound start; the counter advanced past the minimum).
    auto llmConfOpen = makeAgentTestConfig(tmpDir);
    llmConfOpen.nudges.feedback.intervalSecs = 0;
    llmConfOpen.nudges.feedback.minToolCalls = 0;
    auto open = new CannedProcessAgent("integration", llmConfOpen, monitor);
    open.handleToolCalls(null, calls); // sentinel fires
    assert(feedbackWarnings(open) == 1);
    open.lastToolCallWarning = SysTime.init; // rewind
    open.handleToolCalls(null, calls);
    assert(feedbackWarnings(open) == 2, "intervalSecs 0 + minToolCalls 0: the second warning fires");

    // enabled: false silences the warnings at any usage.
    auto llmConfOff = makeAgentTestConfig(tmpDir);
    llmConfOff.nudges.feedback.enabled = false;
    auto off = new CannedProcessAgent("integration", llmConfOff, monitor);
    off.handleToolCalls(null, calls);
    assert(feedbackWarnings(off) == 0, "enabled: false — no warnings at all");
}

// The Agent ctor builds the broker tool pool at the agent seam.

@("broker pool wiring: the per-agent filter builds the pool - include '.*' "
        ~ "- pool == registry in registry order, convenience ctor - unfiltered "
        ~ "pool, kill switch on and off")
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import std.range : empty;

    import llm.tool_call : getFunctions;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_broker_pool_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolFilter.include = [".*"]; // match every tool name

    // Production shape: the filter is passed explicitly (app_agent does), so
    // the pool follows the filter the agent was actually given.
    auto agent = new Agent("integration", llmConf, null, null, null, llmConf.toolFilter.to());

    auto reg = getFunctions();
    assert(agent.toolCtx.pool.length == reg.length,
            "include '.*' ⇒ the pool is the whole registry");
    foreach (i, f; agent.toolCtx.pool)
        assert(f.name == reg[i].name, "pool order must match registry order");
    assert(agent.toolCtx.brokerEnabled, "kill switch defaults to on");
    assert(agent.toolCtx.broker.activated.empty, "fresh broker: no activations");

    // Convenience-ctor agents get an unfiltered member filter (ReFilter.init),
    // so their pool is unfiltered too — the pool can never disagree with the
    // request path.
    auto unfiltered = new Agent("integration", llmConf, null, null);
    assert(unfiltered.toolCtx.pool.length == reg.length);

    // Kill switch off: the flag reaches the context.
    llmConf.toolBroker.enabled = false;
    auto off = new Agent("integration", llmConf, null, null);
    assert(!off.toolCtx.brokerEnabled);
}

@("broker pool wiring: a neverHide tool excluded by toolFilter is unioned back with a ctor warning")
unittest {
    import core.sync.mutex : Mutex;
    import logger = std.logger;
    import std.algorithm : canFind;
    import std.array : Appender;
    import std.conv : to;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;

    import llm.agent.nudges : sharedLogSwapMutex;
    import llm.tool_call : getFunctions;

    final class AgentLogCapture : logger.Logger {
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

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_broker_neverhide_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    // A filter that matches nothing hides the WHOLE registry — except the
    // default neverHideTools (["taskDone"]), which is unioned back.
    llmConf.toolFilter.include = ["^no_such_tool_pattern$"];

    synchronized (sharedLogSwapMutex) {
        auto prevLog = logger.sharedLog;
        auto cap = cast(shared) new AgentLogCapture();
        logger.sharedLog = cap;
        scope (exit)
            logger.sharedLog = prevLog;

        auto agent = new Agent("integration", llmConf, null, null, null, llmConf.toolFilter.to());

        auto pool = agent.toolCtx.pool;
        assert(pool.length == 1 && pool[0].name == "taskDone",
                "neverHide tools are unioned back: " ~ pool.length.to!string);
        assert(agent.toolCtx.brokerEnabled);

        assert(canFind((cast() cap).takeLines(), "neverHide tool 'taskDone' excluded by toolFilter; fix the config"),
                "the ctor must warn about the excluded neverHide tool");
    }
}

@("agent owns the tools array: the listToolTags entry carries the composed description")
unittest {
    import std.algorithm : canFind, countUntil;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import std.string : indexOf;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_t7_composed_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.toolTagDescriptions = [
        "workarea": "Workarea tools.",
        "rag": "RAG knowledge base tools.",
    ];
    auto agent = new Agent("integration", llmConf, null, null, null, llmConf.toolFilter.to());

    auto idx = agent.tools.countUntil!(e => e["function"]["name"].str == "listToolTags");
    assert(idx >= 0, "the discovery tool is in the day-one array");

    auto desc = agent.tools[idx]["function"]["description"].str;
    assert(canFind(desc, "List available tool tags"),
            "the UDA base text must survive the composition:\n" ~ desc);
    assert(canFind(desc, "workarea (Workarea tools.)"),
            "configured workarea description missing:\n" ~ desc);
    assert(canFind(desc, "rag (RAG knowledge base tools.)"),
            "configured rag description missing:\n" ~ desc);
    assert(desc.indexOf("rag") < desc.indexOf("workarea"),
            "zero activation counts: alphabetical tag order expected:\n" ~ desc);

    // Rebuild-at-activation (the change point): the raw selectTools output
    // carries the bare UDA text, so the delegate must recompose — otherwise the
    // configured vocabulary disappears from the model's view after the first
    // activation.
    agent.toolCtx.rebuildTools();
    desc = agent.tools[idx]["function"]["description"].str;
    assert(canFind(desc, "workarea (Workarea tools.)"),
            "the composed description must survive a rebuild:\n" ~ desc);
    assert(canFind(desc, "rag (RAG knowledge base tools.)"),
            "the composed description must survive a rebuild:\n" ~ desc);
}

@("agent owns the tools array: a configured-but-empty tag description warns once")
unittest {
    import core.sync.mutex : Mutex;
    import logger = std.logger;
    import std.algorithm : canFind;
    import std.array : Appender, join;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;

    import llm.agent.nudges : sharedLogSwapMutex;

    final class AgentLogCapture : logger.Logger {
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

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_t7_emptydesc_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.toolTagDescriptions = ["agent_t7_empty_desc_probe": ""];

    synchronized (sharedLogSwapMutex) {
        auto prevLog = logger.sharedLog;
        auto cap = cast(shared) new AgentLogCapture();
        logger.sharedLog = cap;
        scope (exit)
            logger.sharedLog = prevLog;

        auto agent = new Agent("integration", llmConf, null, null, null, llmConf.toolFilter.to());

        auto captured = (cast() cap).takeLines();
        assert(captured.canFind!(l => canFind(l, "has an empty configured description")),
                "the empty tag description must warn:\n" ~ captured.join("\n"));
        assert(captured.canFind!(l => canFind(l, "agent_t7_empty_desc_probe")),
                "the warning must name the offending tag:\n" ~ captured.join("\n"));
    }
}

// The instructive refusal at the dispatch site — the three
// refusal paths and the visible-dispatch control. Registry-absent and
// config-excluded (⇒ not in the pool) messages stay unchanged; the
// hidden-but-pooled case (tagged, never activated) names the recovery path, and the
// refusal flows through the normal tool-result recording path (ToolMessage +
// ToolResponse land in the chat, like a real result).
version (unittest) {
    import std.json : JSONValue;

    import llm.tool_call : Context, ExecuteFuncResult;

    /// Empty params struct for the refusal fixture (nothing to decode).
    private struct T8FixtureParams {
    }

    /// (Context, JSONValue)-shaped callback matching the RegFunction.callback type.
    private ExecuteFuncResult t8FixtureCallback(Context ctx, JSONValue args) {
        return ExecuteFuncResult("ok", true);
    }
}

@("tool dispatch refusals: tier-1 unknown, tier-2 config-excluded, tier-3 in-pool-not-visible is instructive, visible tool dispatches")
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import std.sumtype : match;

    import llm.chat : ToolResponse;
    import llm.tool_call : RegFunction, addFunction, toParams;
    import llm.tool_call.broker : activateTag;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_t8_refusals_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    // Tier-3 fixture: a tagged tool registered before agent construction, so it
    // is in the pool (the default test filter matches every name) but not in
    // the day-one tools array (tagged, never activated).
    addFunction(RegFunction(name: "agent_t8_tier3_fixture", desc: "tier3 fixture tool", params: toParams!T8FixtureParams, callback: &t8FixtureCallback,
            tags: ["workarea"]));

    // Runs one tool call through the dispatch site and returns the ToolResponse
    // it recorded (the last one; every call records exactly one).
    ToolResponse dispatch(Agent agent, string name) {
        StreamResponse.ToolCall[long] calls;
        calls[0] = StreamResponse.ToolCall(id: "1", name: name, arguments: "{}");
        agent.handleToolCalls(null, calls);
        ToolResponse rval;
        foreach (msg; agent.chat.getMessages)
            msg.match!((ToolResponse r) { rval = r; }, (_) {});
        return rval;
    }

    auto agent = new Agent("integration", makeAgentTestConfig(tmpDir), null, null);

    // Tier-1: the tool is absent from the registry entirely (⇒ absent from the
    // pool) — the opaque unknown-tool refusal, unchanged.
    auto unknown = dispatch(agent, "agent_t8_no_such_tool");
    assert(unknown.content == "error: unknown tool agent_t8_no_such_tool", unknown.content);
    assert(!unknown.success());
    assert(unknown.toolName == "agent_t8_no_such_tool");

    // Tier-2: the tool exists but the config filter excludes it (⇒ not in the
    // pool either) — the not-available refusal, unchanged.
    auto llmConf2 = makeAgentTestConfig(tmpDir);
    llmConf2.toolFilter.include = ["^no_such_tool_pattern$"];
    auto excluded = new Agent("integration", llmConf2, null, null, null, llmConf2.toolFilter.to());
    auto unavailable = dispatch(excluded, "writeFile");
    assert(unavailable.content == "error: tool 'writeFile' is not available to this agent",
            unavailable.content);
    assert(!unavailable.success());
    assert(unavailable.toolName == "writeFile");

    // Tier-3: the tool is in the pool but hidden from this agent (tagged, never
    // activated) — the instructive refusal names the recovery path.
    auto hidden = dispatch(agent, "agent_t8_tier3_fixture");
    assert(hidden.content == "error: tool 'agent_t8_tier3_fixture' is not visible to this agent in the current session (visibility resets when chat history is loaded or the session changes)" ~ "; discover tools with `listToolTags`, then make them visible with `loadToolTag`.",
            hidden.content);
    assert(!hidden.success());
    assert(hidden.toolName == "agent_t8_tier3_fixture");
    // The refusal took the recording path: the tool-call message and the tool
    // response both landed in the chat (a broken-call continue adds neither).
    // Two dispatches ran on this agent (tier-1 above + this refusal), each
    // recording one ToolMessage + one ToolResponse.
    assert(agent.chat.getMessages.length == 4);

    // A visible tool still dispatches normally: activate the tag at the
    // activation change point, rebuild the tools array, dispatch again.
    activateTag(agent.toolCtx.broker, agent.toolCtx.pool, "workarea");
    agent.toolCtx.rebuildTools();
    auto ok = dispatch(agent, "agent_t8_tier3_fixture");
    assert(ok.content == "ok", ok.content);
    assert(ok.success());
}

// Compression-point pruning — the scan/apply split inside Agent.compress
// The scan reads the intact pre-compression chat; the
// apply (broker state + tools-array rebuild) runs only when the compression
// actually rewrote history (originalLength != newLength || purgedCount > 0).

version (unittest) {
    import std.json : JSONValue;

    import llm.tool_call : Context, ExecuteFuncResult;

    /// Empty params struct for the prune fixtures (nothing to decode).
    private struct T9FixtureParams {
    }

    /// (Context, JSONValue)-shaped callback matching the RegFunction.callback type.
    private ExecuteFuncResult t9FixtureCallback(Context ctx, JSONValue args) {
        return ExecuteFuncResult("ok", true);
    }
}

// TODO: this test call a real llm which it shall NEVER DO
// @("compression-point pruning: an unused activated tool is pruned at a rewriting compression, a used one survives, alwaysOn/neverHide are never pruned")
// unittest {
//     import std.array : replicate;
//     import std.datetime : Clock;
//     import std.file : mkdirRecurse, write;
//     import std.format : format;
//     import std.json : JSONValue;
//     import std.range : empty;
//
//     import llm.tool_call : RegFunction, addFunction, toParams;
//     import llm.tool_call.broker : activateTag;
//
//     auto now = Clock.currTime();
//     auto tmpDir = format("llmfun_test/agent_t9_prune_%d_%d", now.toUnixTime(), now.stdTime);
//     mkdirRecurse(tmpDir);
//     scope (exit)
//         cleanupAgentTestDir(tmpDir);
//     write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
//     writeDefaultNudgeFiles(tmpDir);
//
//     // Two workarea-tagged fixtures registered BEFORE agent construction, so both
//     // are in the pool (the pool is built at ctor time from the live registry).
//     addFunction(RegFunction(name: "agent_t9_used_fixture", desc: "t9 used fixture", params: toParams!T9FixtureParams,
//             callback: &t9FixtureCallback, tags: ["workarea"]));
//     addFunction(RegFunction(name: "agent_t9_unused_fixture", desc: "t9 unused fixture", params: toParams!T9FixtureParams, callback: &t9FixtureCallback,
//             tags: ["workarea"]));
//
//     string[] namesOf(JSONValue[] ts) {
//         string[] n;
//         foreach (t; ts)
//             n ~= t["function"]["name"].str;
//         return n;
//     }
//
//     auto agent = new Agent("integration", makeAgentTestConfig(tmpDir), null, null);
//
//     // Day one: tagged fixtures are hidden (never activated).
//     assert(!namesOf(agent.tools).canFind("agent_t9_used_fixture"));
//     assert(!namesOf(agent.tools).canFind("agent_t9_unused_fixture"));
//
//     // Activation change point: activate the tag, rebuild the tools array.
//     activateTag(agent.toolCtx.broker, agent.toolCtx.pool, "workarea");
//     agent.toolCtx.rebuildTools();
//     assert(namesOf(agent.tools).canFind("agent_t9_used_fixture"));
//     assert(namesOf(agent.tools).canFind("agent_t9_unused_fixture"));
//     assert(namesOf(agent.tools).canFind("taskDone"), "neverHide stays in the head");
//
//     // Usage: a real tool CALL through the dispatch site — the only thing
//     // that counts. The unused fixture never gets a call.
//     StreamResponse.ToolCall[long] calls;
//     calls[0] = StreamResponse.ToolCall(id: "1", name: "agent_t9_used_fixture", arguments: "{}");
//     // The system prompt must be set BEFORE the tool traffic: Chat.setSystemPrompt
//     // replaces history[0], and in a chat whose only message is the tool call
//     // that would wipe the ToolMessage (and the scan would prune the used tool).
//     agent.setSystemPrompt("sys");
//     agent.handleToolCalls(null, calls);
//
//     // A chat long enough to compress with a REWRITE even though the (offline,
//     // always-failing) summary produces nothing: system + an oversized newest
//     // candidate (it alone exceeds the X token budget, so the
//     // newest-first X-fill stops with X empty and the whole candidate pool, the
//     // tool-call pair and the oversized message, goes to the failed summary) +
//     // five small kept messages. 9 -> 6 keeps the apply gate true
//     // (originalLength != newLength).
//     agent.addUserQuery("x".replicate(9000)); // ~4500 tokens > TokenBudget (4096)
//     agent.addUserQuery("s1");
//     agent.addContinue();
//     agent.addUserQuery("s2");
//     agent.addContinue();
//     agent.addUserQuery("s3");
//
//     auto res = agent.compress(0.9, true);
//     assert(res.compressed, "the chat must actually compress (rewrite) for the D39 gate");
//     assert(res.originalLength > res.newLength,
//             "the failed summary rewrites the history: the oversized candidate is dropped (D39 gate input)");
//
//     // The prune applied: the unused activated fixture is gone from the next
//     // tools array, the used one survives (its call sits in the verbatim epoch),
//     // alwaysOn (untagged) tools and the neverHide taskDone are untouched.
//     auto names = namesOf(agent.tools);
//     assert(!names.canFind("agent_t9_unused_fixture"), "unused activated tool must be pruned");
//     assert(names.canFind("agent_t9_used_fixture"), "a used tool survives the prune");
//     assert(names.canFind("taskDone"), "neverHide tools are never pruned");
//     foreach (f; agent.toolCtx.pool)
//         if (f.tags.empty)
//             assert(names.canFind(f.name), "alwaysOn tools are never pruned");
//     assert(!agent.toolCtx.broker.activated.canFind("agent_t9_unused_fixture"),
//             "the activation list loses the pruned name");
//     assert(agent.toolCtx.broker.activated.canFind("agent_t9_used_fixture"),
//             "the used tool stays activated");
// }

@("compression-point pruning: a no-op compression (history not rewritten) leaves the broker state and the tools array untouched")
unittest {
    import std.algorithm : canFind;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import std.json : JSONValue;

    import llm.tool_call : RegFunction, addFunction, toParams;
    import llm.tool_call.broker : activateTag;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_t9_noop_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    addFunction(RegFunction(name: "agent_t9_noop_fixture", desc: "t9 noop fixture", params: toParams!T9FixtureParams,
            callback: &t9FixtureCallback, tags: ["workarea"]));
    addFunction(RegFunction(name: "agent_t9_noop_unused", desc: "t9 noop unused fixture", params: toParams!T9FixtureParams,
            callback: &t9FixtureCallback, tags: ["workarea"]));

    string[] namesOf(JSONValue[] ts) {
        string[] n;
        foreach (t; ts)
            n ~= t["function"]["name"].str;
        return n;
    }

    auto agent = new Agent("integration", makeAgentTestConfig(tmpDir), null, null);

    activateTag(agent.toolCtx.broker, agent.toolCtx.pool, "workarea");
    agent.toolCtx.rebuildTools();
    assert(namesOf(agent.tools).canFind("agent_t9_noop_fixture"));

    // A used tool: a no-op compression must not prune it either — nothing is
    // pruned at all when the history is not rewritten.
    StreamResponse.ToolCall[long] calls;
    calls[0] = StreamResponse.ToolCall(id: "1", name: "agent_t9_noop_fixture", arguments: "{}");
    agent.handleToolCalls(null, calls);

    // 3 messages: below the summary.compress floor (1 + KeepLast), so the
    // forced compression no-ops: originalLength/newLength stay .init and the
    // The rule (0 != 0 || 0 > 0) is false.
    agent.setSystemPrompt("sys");
    agent.addUserQuery("q1");
    agent.addUserQuery("q2");

    auto before = JSONValue(agent.tools).toString;
    auto res = agent.compress(0.9, true);
    assert(!res.compressed, "the chat is too short: summary.compress no-ops");
    assert(res.originalLength == 0 && res.newLength == 0 && res.purgedCount == 0,
            "no-rewrite fields stay .init, so the D39 gate is false");
    auto after = JSONValue(agent.tools).toString;
    assert(before == after, "a no-op compression must not touch the tools array");
    assert(agent.toolCtx.broker.activated.canFind("agent_t9_noop_fixture"),
            "no prune: the used tool stays activated");
    assert(namesOf(agent.tools).canFind("agent_t9_noop_fixture"));
    assert(namesOf(agent.tools).canFind("agent_t9_noop_unused"),
            "the D39 gate skipped the apply: an unused activated tool survives a no-op");
    assert(agent.toolCtx.broker.activated.canFind("agent_t9_noop_unused"),
            "the activation list keeps the unused tool when history is not rewritten");
}

version (unittest) {
    import std.json : JSONValue;

    import llm.tool_call : Context, ExecuteFuncResult;

    /// Empty params struct for the broker-event fixtures (nothing to decode).
    private struct T10FixtureParams {
    }

    /// (Context, JSONValue)-shaped callback matching the RegFunction.callback type.
    private ExecuteFuncResult t10FixtureCallback(Context ctx, JSONValue args) {
        return ExecuteFuncResult("ok", true);
    }
}

// TODO: this test call a real llm which it shall NEVER DO
// @("broker metrics: the tools_request estimate, the tier-3 refusal, the discovery miss+hit pair with activation, and the prune count all land in the agent's MetricMonitor JSONL")
// unittest {
//     import std.algorithm : canFind, count, filter, map;
//     import std.array : array, replicate;
//     import std.datetime : Clock;
//     import std.file : mkdirRecurse, readText, write;
//     import std.format : format;
//     import std.json : JSONOptions, JSONValue, parseJSON;
//     import std.path : buildPath;
//     import std.range : empty;
//     import std.string : splitLines;
//
//     import my.path : Path;
//
//     import llm.common.config : ApproxTokenSize;
//     import llm.metric.monitor : MetricMonitor;
//     import llm.tool_call : RegFunction, addFunction, toParams;
//     import llm.tool_call.discovery : ListToolTagsParams, listToolTags;
//
//     auto now = Clock.currTime();
//     auto tmpDir = format("llmfun_test/agent_t10_metrics_%d_%d", now.toUnixTime(), now.stdTime);
//     mkdirRecurse(tmpDir);
//     scope (exit)
//         cleanupAgentTestDir(tmpDir);
//     write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
//     writeDefaultNudgeFiles(tmpDir);
//
//     // Three workarea-tagged fixtures registered BEFORE agent construction, so
//     // all are in the pool (the pool is built at ctor time from the live
//     // registry): the tier-3 refusal target plus the used/unused prune pair.
//     addFunction(RegFunction(name: "agent_t10_tier3_fixture", desc: "t10 tier3 fixture", params: toParams!T10FixtureParams,
//             callback: &t10FixtureCallback, tags: ["workarea"]));
//     addFunction(RegFunction(name: "agent_t10_used_fixture", desc: "t10 used fixture", params: toParams!T10FixtureParams, callback: &t10FixtureCallback,
//             tags: ["workarea"]));
//     addFunction(RegFunction(name: "agent_t10_unused_fixture", desc: "t10 unused fixture", params: toParams!T10FixtureParams,
//             callback: &t10FixtureCallback, tags: ["workarea"]));
//
//     // The agent's own JSONL sink: a real MetricMonitor on a fresh file (the
//     // feedback gating tolerates null monitors, but the metrics sites need a
//     // real sink). A real Agent (not CannedProcessAgent) so process() runs the
//     // real request site; the empty server type never dials out.
//     auto dataFile = buildPath(tmpDir, "monitor.jsonl").Path;
//     auto monitor = new MetricMonitor(dataFile);
//     auto agent = new Agent("integration", makeAgentTestConfig(tmpDir), monitor, null);
//
//     string[] namesOf(JSONValue[] ts) {
//         string[] n;
//         foreach (t; ts)
//             n ~= t["function"]["name"].str;
//         return n;
//     }
//
//     // Per-kind JSONL reader over the agent's monitor file (re-read per call:
//     // events accumulate as the test runs).
//     JSONValue[] byKind(string kind) {
//         return readText(dataFile).splitLines
//             .map!(a => parseJSON(a))
//             .filter!(j => "kind" in j && j["kind"].str == kind)
//             .array;
//     }
//
//     // 1. tools_request: the per-request tools size + schema token estimate.
//     // The event fires BEFORE the requester dials (the request itself fails
//     // offline with an unknown endpoint, which process() reports as
//     // unknownFailure).
//     agent.process(null);
//     auto reqs = byKind("tools_request");
//     assert(reqs.length == 1);
//     assert(reqs[0]["agent"].str == "integration");
//     assert(reqs[0]["toolsCount"].integer == cast(long) agent.tools.length);
//     long schemaTokens;
//     foreach (t; agent.tools)
//         schemaTokens += t.toString(JSONOptions.doNotEscapeSlashes).length;
//     assert(reqs[0]["schemaTokens"].integer == schemaTokens / ApproxTokenSize,
//             "schemaTokens uses the ApproxTokenSize heuristic");
//
//     // 2. tool_refusal: the tier-3 instructive refusal (the fixture is in the
//     // pool but not yet activated, so it is not in agent.tools).
//     StreamResponse.ToolCall[long] calls;
//     calls[0] = StreamResponse.ToolCall(id: "1", name: "agent_t10_tier3_fixture", arguments: "{}");
//     agent.handleToolCalls(null, calls);
//     auto refusals = byKind("tool_refusal");
//     assert(refusals.length == 1);
//     assert(refusals[0]["tool"].str == "agent_t10_tier3_fixture");
//     assert(refusals[0]["tier"].integer == 3);
//     assert(refusals[0]["agent"].str == "integration");
//
//     // 3. tag_discovery / tag_activation: the miss records known=false and
//     // nothing else; the hit activates the tag's visible tools (the fixtures)
//     // and records the activated count (the tag's pool size).
//     auto miss = listToolTags(agent.toolCtx, ListToolTagsParams(tag: "nope"));
//     assert(!miss.success);
//     auto discoveries = byKind("tag_discovery");
//     assert(discoveries.length == 1);
//     assert(discoveries[0]["tag"].str == "nope");
//     assert(!discoveries[0]["known"].boolean);
//
//     auto hit = listToolTags(agent.toolCtx, ListToolTagsParams(tag: "workarea"));
//     assert(hit.success);
//     auto activations = byKind("tag_activation");
//     assert(activations.length == 1);
//     assert(activations[0]["tag"].str == "workarea");
//     assert(activations[0]["toolsActivated"].integer == cast(
//             long) agent.toolCtx.pool.count!(f => f.tags.canFind("workarea")));
//     assert(byKind("tag_discovery").length == 2, "the miss and the hit both record");
//
//     // 4. broker_prune: a rewriting compression prunes the unused activated
//     // fixture; the used one survives (the same recipe as the prune
//     // test). The system prompt must be set BEFORE the tool traffic.
//     StreamResponse.ToolCall[long] usedCalls;
//     usedCalls[0] = StreamResponse.ToolCall(id: "1", name: "agent_t10_used_fixture", arguments: "{}");
//     agent.setSystemPrompt("sys");
//     agent.handleToolCalls(null, usedCalls);
//
//     agent.addUserQuery("x".replicate(9000)); // ~4500 tokens > TokenBudget (4096)
//     agent.addUserQuery("s1");
//     agent.addContinue();
//     agent.addUserQuery("s2");
//     agent.addContinue();
//     agent.addUserQuery("s3");
//
//     auto res = agent.compress(0.9, true);
//     assert(res.compressed, "the chat must actually compress (rewrite) for the D39 gate");
//     assert(res.originalLength > res.newLength);
//
//     auto prunes = byKind("broker_prune");
//     assert(prunes.length == 1);
//
//     // The prune count is registry-state-dependent: other modules' tests leak
//     // workarea-tagged fixtures that my workarea hit activates, so the scan
//     // prunes them too. The invariant: the unused fixture is among the pruned;
//     // the used one and the refusal fixture (whose refusal is delivered as a
//     // tool result, which counts as a use) survive.
//     assert(prunes[0]["pruned"].integer >= 1, "the unused activated fixture is pruned");
//
//     // The used fixture and the tier-3-refused fixture survive; the unused
//     // one is gone.
//     auto names = namesOf(agent.tools);
//     assert(names.canFind("agent_t10_used_fixture"), "a used tool survives the prune");
//     assert(names.canFind("agent_t10_tier3_fixture"),
//             "the tier-3 refusal is a use: the fixture survives");
//     assert(!names.canFind("agent_t10_unused_fixture"), "the unused fixture is pruned");
//
//     // 5. broker_seed: a restore that actually seeds - the hand-built
//     // chat.load + seedBrokerFromChat seam the seed-on-load tests use. After
//     // the prune above, the unused fixture is the only pool tool whose name
//     // no longer sits in the activation list, so a history proving its use
//     // re-activates exactly it: "seeded": 1 (the used and refused fixtures
//     // are already activated, and activation is sticky).
//     agent.chat.load(parseJSON(`{
//         "messages": [
//             {"role": "assistant", "content": null, "reasoning_content": "",
//              "tool_calls": [{"id": "2", "type": "function",
//                              "function": {"name": "agent_t10_unused_fixture", "arguments": "{}"}}]},
//             {"role": "tool", "content": "out", "tool_call_id": "2",
//              "name": "agent_t10_unused_fixture"}
//         ]
//     }`));
//     agent.seedBrokerFromChat();
//
//     auto seeds = byKind("broker_seed");
//     assert(seeds.length == 1, "a restore that seeds lands exactly one broker_seed event");
//     assert(seeds[0]["seeded"].integer == 1);
//     assert(seeds[0]["agent"].str == "integration");
//
//     // Idempotency (D7): a second seed of the same chat fires nothing.
//     agent.seedBrokerFromChat();
//     assert(byKind("broker_seed").length == 1, "a no-op seed records no event");
//
// }

// --- Seed-on-load: the load-path inverse of the compression-time prune ---

version (unittest) {
    import std.json : JSONValue;

    import llm.tool_call : Context, ExecuteFuncResult;

    /// Empty params struct for the seed-on-load fixtures (nothing to decode).
    private struct T11FixtureParams {
    }

    /// (Context, JSONValue)-shaped callback matching the RegFunction.callback type.
    private ExecuteFuncResult t11FixtureCallback(Context ctx, JSONValue args) {
        return ExecuteFuncResult("ok", true);
    }
}

@("seed-on-load: a loaded chat's structured tool call re-activates the workarea-tagged fixture and the rebuilt tools array carries it")
unittest {
    import std.algorithm : canFind;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import std.json : JSONValue, parseJSON;
    import std.range : empty;

    import llm.tool_call : RegFunction, addFunction, toParams;
    import llm.tool_call.broker : BrokerState;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_t11_seed_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    // Workarea-tagged fixture registered BEFORE agent construction, so it is
    // in the pool (the pool is built at ctor time from the live registry) but
    // not in the day-one tools array (tagged, never activated).
    addFunction(RegFunction(name: "agent_t11_seed_fixture", desc: "t11 seed fixture", params: toParams!T11FixtureParams, callback: &t11FixtureCallback,
            tags: ["workarea"]));

    string[] namesOf(JSONValue[] ts) {
        string[] n;
        foreach (t; ts)
            n ~= t["function"]["name"].str;
        return n;
    }

    auto agent = new Agent("integration", makeAgentTestConfig(tmpDir), null, null);

    // Fresh-broker pin, kept explicit: the case seeds from scratch.
    agent.toolCtx.broker = BrokerState.init;
    assert(agent.toolCtx.broker.activated.empty);

    // A chat doc holding one structured call + tool response pair for the
    // fixture: the same save shape the session-restore path parses back.
    agent.chat.load(parseJSON(`{
        "messages": [
            {"role": "assistant", "content": null, "reasoning_content": "",
             "tool_calls": [{"id": "1", "type": "function",
                             "function": {"name": "agent_t11_seed_fixture", "arguments": "{}"}}]},
            {"role": "tool", "content": "out", "tool_call_id": "1",
             "name": "agent_t11_seed_fixture"}
        ]
    }`));
    agent.seedBrokerFromChat();

    assert(agent.toolCtx.broker.activated.canFind("agent_t11_seed_fixture"),
            "the tool the history proves was used is re-activated");
    assert(namesOf(agent.tools).canFind("agent_t11_seed_fixture"),
            "the rebuilt tools array carries the seeded tool");
}

@(
        "seed-on-load: an empty chat doc is a no-op - the activation list and the tools array stay untouched")
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import std.json : JSONValue, parseJSON;
    import std.range : empty;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_t11_empty_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto agent = new Agent("integration", makeAgentTestConfig(tmpDir), null, null);

    auto before = JSONValue(agent.tools).toString;
    agent.chat.load(parseJSON(`{"messages": []}`));
    agent.seedBrokerFromChat();

    assert(agent.toolCtx.broker.activated.empty, "nothing to seed: no activations");
    assert(JSONValue(agent.tools).toString == before,
            "an empty chat doc must not touch the tools array");
}

@("seed-on-load: the kill switch off makes the seed a no-op even when the history proves a use")
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import std.json : JSONValue, parseJSON;
    import std.range : empty;

    import llm.tool_call : RegFunction, addFunction, toParams;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_t11_killswitch_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    addFunction(RegFunction(name: "agent_t11_killswitch_fixture", desc: "t11 kill-switch fixture", params: toParams!T11FixtureParams,
            callback: &t11FixtureCallback, tags: ["workarea"]));

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.enabled = false;
    auto agent = new Agent("integration", llmConf, null, null);

    // The history proves a use, but the gate is brokerEnabled's first
    // conjunct: the seed must stay a no-op.
    agent.chat.load(parseJSON(`{
        "messages": [
            {"role": "assistant", "content": null, "reasoning_content": "",
             "tool_calls": [{"id": "1", "type": "function",
                             "function": {"name": "agent_t11_killswitch_fixture", "arguments": "{}"}}]},
            {"role": "tool", "content": "out", "tool_call_id": "1",
             "name": "agent_t11_killswitch_fixture"}
        ]
    }`));
    auto before = JSONValue(agent.tools).toString;
    agent.seedBrokerFromChat();

    assert(agent.toolCtx.broker.activated.empty, "kill switch off: nothing is seeded (D6)");
    assert(JSONValue(agent.tools).toString == before,
            "kill switch off: the tools array is untouched by the seed");
}

@("seed-on-load: only alwaysOn (untagged) calls in the history - the activation list and the tools array stay untouched")
unittest {
    import std.algorithm : canFind;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import std.json : JSONValue, parseJSON;
    import std.range : empty;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_t11_alwayson_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    string[] namesOf(JSONValue[] ts) {
        string[] n;
        foreach (t; ts)
            n ~= t["function"]["name"].str;
        return n;
    }

    auto agent = new Agent("integration", makeAgentTestConfig(tmpDir), null, null);

    // A history whose only structured call targets taskDone: untagged
    // (alwaysOn) and neverHide - the seeder must skip both (D2).
    agent.chat.load(parseJSON(`{
        "messages": [
            {"role": "assistant", "content": null, "reasoning_content": "",
             "tool_calls": [{"id": "1", "type": "function",
                             "function": {"name": "taskDone", "arguments": "{}"}}]}
        ]
    }`));
    auto before = JSONValue(agent.tools).toString;
    agent.seedBrokerFromChat();

    assert(agent.toolCtx.broker.activated.empty,
            "an alwaysOn/neverHide call never enters the activation list (D2)");
    assert(JSONValue(agent.tools).toString == before,
            "an alwaysOn-only history must not touch the tools array");
    assert(namesOf(agent.tools).canFind("taskDone"), "alwaysOn stays in the head");
}

@(
        "seed-on-load: a second seed of the same chat is a no-op - nothing newly activated, no rebuild (D7)")
unittest {
    import std.algorithm : canFind;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import std.json : JSONValue, parseJSON;
    import std.range : empty;

    import llm.tool_call : RegFunction, addFunction, toParams;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_t11_idem_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    addFunction(RegFunction(name: "agent_t11_idem_fixture", desc: "t11 idempotency fixture", params: toParams!T11FixtureParams,
            callback: &t11FixtureCallback, tags: ["workarea"]));

    string[] namesOf(JSONValue[] ts) {
        string[] n;
        foreach (t; ts)
            n ~= t["function"]["name"].str;
        return n;
    }

    auto agent = new Agent("integration", makeAgentTestConfig(tmpDir), null, null);

    agent.chat.load(parseJSON(`{
        "messages": [
            {"role": "assistant", "content": null, "reasoning_content": "",
             "tool_calls": [{"id": "1", "type": "function",
                             "function": {"name": "agent_t11_idem_fixture", "arguments": "{}"}}]},
            {"role": "tool", "content": "out", "tool_call_id": "1",
             "name": "agent_t11_idem_fixture"}
        ]
    }`));
    // Counting rebuild hook installed BEFORE the first seed: the first seed
    // (something newly activated) must fire exactly one rebuild, the second
    // (nothing newly activated) none (D7). The wrapper forwards to the
    // default rebuild so the tools array still gets recomposed.
    size_t rebuilds;
    auto defaultRebuild = agent.toolCtx.rebuildTools;
    agent.toolCtx.rebuildTools = delegate() @safe {
        rebuilds++;
        defaultRebuild();
    };
    agent.seedBrokerFromChat(); // first seed: activates + rebuilds
    assert(rebuilds == 1, "the first seed fires exactly one rebuild");
    assert(agent.toolCtx.broker.activated.canFind("agent_t11_idem_fixture"));

    agent.seedBrokerFromChat(); // second seed: sticky activation, seeded == 0
    assert(rebuilds == 1, "the second seed must fire no rebuild (D7)");
    assert(agent.toolCtx.broker.activated.canFind("agent_t11_idem_fixture"),
            "the activation is sticky");
    assert(namesOf(agent.tools).canFind("agent_t11_idem_fixture"), "the seeded tool stays visible");
}

@("seed-on-load end to end: agent.loadHistory seeds the broker from the restored session file")
unittest {
    import std.algorithm : canFind;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import std.json : JSONValue;

    import my.path : Path;

    import llm.tool_call : RegFunction, addFunction, toParams;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_t11_load_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    addFunction(RegFunction(name: "agent_t11_load_fixture", desc: "t11 load fixture", params: toParams!T11FixtureParams, callback: &t11FixtureCallback,
            tags: ["workarea"]));

    string[] namesOf(JSONValue[] ts) {
        string[] n;
        foreach (t; ts)
            n ~= t["function"]["name"].str;
        return n;
    }

    auto agent = new Agent("integration", makeAgentTestConfig(tmpDir), null, null);

    // The history file in the save format, written AFTER construction (the
    // ctor does not auto-load): one structured call + tool response pair.
    write(tmpDir ~ "/integration_history.json", `{
        "messages": [
            {"role": "assistant", "content": null, "reasoning_content": "",
             "tool_calls": [{"id": "1", "type": "function",
                             "function": {"name": "agent_t11_load_fixture", "arguments": "{}"}}]},
            {"role": "tool", "content": "out", "tool_call_id": "1",
             "name": "agent_t11_load_fixture"}
        ]
    }`);
    agent.loadHistory(tmpDir.Path);

    assert(agent.toolCtx.broker.activated.canFind("agent_t11_load_fixture"),
            "the restored session re-activated the used tool");
    assert(namesOf(agent.tools).canFind("agent_t11_load_fixture"),
            "the rebuilt tools array carries the seeded tool");
}

// --- MCP tools route through the broker: connect change point ---

@("An MCP tool registered at runtime enters the pool at the connect change point, the discovery description includes its tag after the recompose, and activation makes it visible")
unittest {
    import logger = std.logger;
    import core.sync.mutex : Mutex;
    import std.algorithm : canFind, filter;
    import std.array : Appender, array;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import std.json : parseJSON;

    import llm.agent.nudges : sharedLogSwapMutex;
    import llm.mcp_server.registration : registerMcpTool;
    import llm.tool_call.discovery : ListToolTagsParams, listToolTags;

    final class AgentLogCapture : logger.Logger {
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

    enum tag = "mcp_ext_server_t12";

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_mcp_connect_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.enabled = true;

    llmConf.toolBroker.toolTagDescriptions = [
        tag: "tags inherited from a connected MCP server"
    ];

    auto agent = new Agent("integration", llmConf, null, null);
    assert(!agent.toolCtx.pool.canFind!(f => f.name == "mcp_ext_t12_e2e"),
            "not registered yet — the ctor pools the registry as it is");

    // Runtime registration (the MCP client would call this when a server
    // connects), then the connect change point: the pool is recomputed from
    // the live registry and the array + discovery description rebuilt through
    // toolCtx.rebuildTools (the MCP connect hook). The tag vocabulary is config-side and read
    // at rebuild time; it is preset before construction, so the recomposed
    // description carries the tag — if the hook did not recompose, the raw
    // selectTools output would carry the bare UDA text and the tag would
    // vanish.
    registerMcpTool("mcp_ext_t12_e2e", "ext tool for the e2e recompose test",
            [], (ctx, args) => ExecuteFuncResult("ok", true), [tag]);
    agent.onMcpServerConnected();

    assert(agent.toolCtx.pool.canFind!(f => f.name == "mcp_ext_t12_e2e" && f.tags == [
        tag
    ]), "the connect hook must recompute the pool from the live registry");
    assert(!agent.tools.canFind!(e => e["function"]["name"].str == "mcp_ext_t12_e2e"),
            "a tagged tool is hidden until activation");

    auto listDesc = agent.tools.filter!(e => e["function"]["name"].str == "listToolTags")
        .array[0]["function"]["description"].str;
    assert(listDesc.canFind("\n\nTags: " ~ tag), listDesc);

    // Discovery loop: list the tag (activates it), then the harness-side
    // rebuild — the MCP tool becomes visible in the array.
    auto resp = listToolTags(agent.toolCtx, ListToolTagsParams(tag));
    assert(resp.success, resp.msg);
    agent.toolCtx.rebuildTools();
    assert(agent.tools.canFind!(e => e["function"]["name"].str == "mcp_ext_t12_e2e"),
            "the MCP tool becomes visible after its tag is activated");
}

@("Kill switch off: the MCP connect hook rebuilds the legacy tools array, so an MCP tool becomes visible without activation")
unittest {
    import std.algorithm : canFind;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;

    import llm.mcp_server.registration : registerMcpTool;

    enum tag = "mcp_ext_server_t12_legacy";

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/agent_mcp_killswitch_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupAgentTestDir(tmpDir);
    write(tmpDir ~ "/SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.enabled = false;

    auto agent = new Agent("integration", llmConf, null, null);
    assert(!agent.toolCtx.brokerEnabled);
    assert(!agent.tools.canFind!(e => e["function"]["name"].str == "mcp_ext_t12_legacy"),
            "not registered yet");

    registerMcpTool("mcp_ext_t12_legacy", "ext tool, kill switch off", [],
            (ctx, args) => ExecuteFuncResult("ok", true), [tag]);
    agent.onMcpServerConnected();

    assert(agent.tools.canFind!(e => e["function"]["name"].str == "mcp_ext_t12_legacy"),
            "the legacy path emits registry ∩ toolFilter regardless of tags");
}
